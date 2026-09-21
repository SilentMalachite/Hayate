#include "conn_client.hpp"
#include "http_client.hpp"
#include "metrics_text.hpp"
#include "test_server.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace http = boost::beast::http;
namespace net = boost::asio;

namespace {

// プロセスの fd を使い切り、サーバーの accept を EMFILE で失敗させる。
// ctest はテストごとに別プロセスなので、limit を下げても他のテストに漏れない。
class FdHog {
  public:
    FdHog() {
        ::getrlimit(RLIMIT_NOFILE, &saved_);
        rlimit low = saved_;
        low.rlim_cur = std::min<rlim_t>(saved_.rlim_cur, 512);
        ::setrlimit(RLIMIT_NOFILE, &low);
        for (;;) {
            const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fd == -1) {
                break;
            }
            fds_.push_back(fd);
        }
    }
    ~FdHog() {
        for (const int fd : fds_) {
            ::close(fd);
        }
        ::setrlimit(RLIMIT_NOFILE, &saved_);
    }
    FdHog(const FdHog &) = delete;
    FdHog &operator=(const FdHog &) = delete;

  private:
    rlimit saved_{};
    std::vector<int> fds_;
};

// 期限つきで 1 本読む。期限切れは閉じずに cancel するだけなので、後で読み直せる。
boost::system::error_code read_within(net::io_context &ioc, boost::beast::tcp_stream &s,
                                      boost::beast::flat_buffer &buf,
                                      http::response<http::string_body> &res,
                                      std::chrono::milliseconds limit) {
    boost::system::error_code out;
    net::steady_timer timer(ioc, limit);
    http::async_read(s, buf, res, [&](boost::system::error_code ec, std::size_t) {
        out = ec;
        timer.cancel();
    });
    timer.async_wait([&](boost::system::error_code ec) {
        if (!ec) {
            s.cancel();
        }
    });
    ioc.restart();
    ioc.run();
    return out;
}

} // namespace

TEST(Limits, OversizeHeaderIs431) {
    TestServer srv([](hayate::App &app) {
        app.limits().max_header_bytes = 64;
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set("X-Big", std::string(200, 'a'));
    http::write(stream, req);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    boost::system::error_code ec;
    http::read(stream, buf, res, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(res.result_int(), 431);
}

TEST(Limits, OversizeBodyIs413) {
    TestServer srv([](hayate::App &app) {
        app.limits().max_body_bytes = 8;
        app.post("/x", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::post, "/x", "0123456789", "text/plain");
    EXPECT_EQ(r.status, 413);
}

TEST(Limits, ReadTimeoutCloses) {
    TestServer srv([](hayate::App &app) {
        app.limits().read_timeout = std::chrono::milliseconds(50);
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    const char *partial = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    net::write(stream.socket(), net::buffer(partial, std::strlen(partial)));
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    boost::system::error_code ec;
    http::read(stream, buf, res, ec);
    EXPECT_TRUE(ec);
}

TEST(Limits, IdleTimeoutClosesKeepAlive) {
    TestServer srv([](hayate::App &app) {
        app.limits().idle_timeout = std::chrono::milliseconds(50);
        app.limits().read_timeout = std::chrono::seconds(5);
        app.get("/a", [](hayate::Request &) { return hayate::Response::text("a"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    http::request<http::string_body> req{http::verb::get, "/a", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(true);
    http::write(stream, req);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);
    EXPECT_EQ(res.result_int(), 200);
    EXPECT_EQ(res.body(), "a");
    stream.expires_after(std::chrono::seconds(8));
    http::response<http::string_body> idle;
    boost::system::error_code ec;
    auto t0 = std::chrono::steady_clock::now();
    http::read(stream, buf, idle, ec);
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_TRUE(ec);
    EXPECT_LT(ms, 1000) << "idle timeout must fire before read_timeout";
}

TEST(Limits, MaxConnectionsRefusesNew) {
    TestServer srv([](hayate::App &app) {
        app.limits().max_connections = 1;
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream hold(ioc);
    hold.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    boost::beast::tcp_stream extra(ioc);
    extra.expires_after(std::chrono::seconds(1));
    boost::system::error_code ec;
    extra.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()), ec);
    extra.socket().write_some(net::buffer(std::string("GET / HTTP/1.1\r\nHost: x\r\n\r\n")), ec);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(extra, buf, res, ec);
    EXPECT_TRUE(ec);
}

TEST(Limits, ReadTimeoutAppliesToKeepAliveRequests) {
    TestServer srv([](hayate::App &app) {
        app.limits().read_timeout = std::chrono::milliseconds(50);
        app.limits().idle_timeout = std::chrono::seconds(5);
        app.get("/a", [](hayate::Request &) { return hayate::Response::text("a"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    http::request<http::string_body> req{http::verb::get, "/a", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(true);
    http::write(stream, req);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);
    ASSERT_EQ(res.result_int(), 200);

    // 2 本目を途中まで送る。窓は read_timeout であって idle_timeout ではない。
    const char *partial = "POST /a HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10\r\n\r\nab";
    net::write(stream.socket(), net::buffer(partial, std::strlen(partial)));
    stream.expires_after(std::chrono::seconds(8));
    http::response<http::string_body> second;
    boost::system::error_code ec;
    auto t0 = std::chrono::steady_clock::now();
    http::read(stream, buf, second, ec);
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_TRUE(ec);
    EXPECT_LT(ms, 1000) << "read timeout must govern the body of a keep-alive request";
}

// accept が一度失敗しただけで accept ループを終えると、fd が戻っても誰も繋げない。
TEST(Limits, AcceptSurvivesFdExhaustion) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    const net::ip::tcp::endpoint ep(net::ip::make_address("127.0.0.1"), srv.port());
    // 使い切った後はクライアントも fd を作れない。2 本とも先に取っておく。
    net::io_context ioc;
    boost::beast::tcp_stream starved(ioc);
    boost::beast::tcp_stream later(ioc);
    starved.socket().open(net::ip::tcp::v4());
    later.socket().open(net::ip::tcp::v4());
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(false);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    {
        FdHog hog;
        starved.socket().connect(ep);
        http::write(starved, req);
        // サーバーの accept は EMFILE になる。macOS はその接続を RST で捨て、Linux は
        // backlog に残す。どちらでも枯渇中に応答は来ない。
        EXPECT_TRUE(read_within(ioc, starved, buf, res, std::chrono::milliseconds(200)));
    }
    later.socket().connect(ep);
    http::write(later, req);
    buf.clear();
    res = {};
    const auto ec = read_within(ioc, later, buf, res, std::chrono::seconds(2));
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(res.result_int(), 200);
}

// 読まない client に書き続けると、write_timeout で切る。切らないと接続と本文を握り続ける。
TEST(Limits, WriteTimeoutClosesStalledReader) {
    // client の受信窓とサーバーの送信バッファ（自動調整の上限を含む）を合わせても収まらない大きさ。
    constexpr std::size_t kBody = 16u * 1024 * 1024;
    TestServer srv([](hayate::App &app) {
        app.limits().write_timeout = std::chrono::milliseconds(200);
        app.get("/big",
                [](hayate::Request &) { return hayate::Response::text(std::string(kBody, 'x')); });
        app.get("/metrics", hayate::metrics(app));
    });
    Conn c(srv.port(), std::chrono::seconds(2), 4096);
    ASSERT_FALSE(c.connect_error());
    http::request<http::string_body> req{http::verb::get, "/big", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(false);
    ASSERT_FALSE(c.write(req));

    // 期限で切れれば、開いている接続はスクレイプ自身の 1 本になる。時間ではなく状態を待つ。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    long long open = -1;
    while (open != 1 && std::chrono::steady_clock::now() < deadline) {
        auto m = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
        open = value_of(m.body, "hayate_connections_open");
    }
    ASSERT_EQ(open, 1) << "stalled connection was not closed";

    // カーネルに残った分を読み切ると、Content-Length に届く前に EOF。
    std::size_t total = 0;
    std::array<char, 64 * 1024> chunk{};
    const auto ec =
        c.run(std::chrono::seconds(5), [&]() -> net::awaitable<boost::system::error_code> {
            for (;;) {
                auto [rec, n] =
                    co_await c.stream().async_read_some(net::buffer(chunk), net::as_tuple);
                total += n;
                if (rec) {
                    co_return rec;
                }
            }
        });
    EXPECT_NE(ec, boost::beast::error::timeout);
    EXPECT_LT(total, kBody);
}
