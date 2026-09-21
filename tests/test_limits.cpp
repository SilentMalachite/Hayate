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

} // namespace

TEST(Limits, OversizeHeaderIs431) {
    TestServer srv([](hayate::App &app) {
        app.limits().max_header_bytes = 64;
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    Conn c(srv.port());
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set("X-Big", std::string(200, 'a'));
    ASSERT_FALSE(c.write(req));
    http::response<http::string_body> res;
    const auto ec = c.read(res);
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
    Conn c(srv.port());
    ASSERT_FALSE(c.write_raw("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"));
    http::response<http::string_body> res;
    // client の期限切れではなく、サーバーが閉じたこと。
    const auto ec = c.read(res, std::chrono::seconds(5));
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, boost::beast::error::timeout);
}

TEST(Limits, IdleTimeoutClosesKeepAlive) {
    TestServer srv([](hayate::App &app) {
        app.limits().idle_timeout = std::chrono::milliseconds(50);
        app.limits().read_timeout = std::chrono::seconds(5);
        app.get("/a", [](hayate::Request &) { return hayate::Response::text("a"); });
    });
    Conn c(srv.port());
    http::request<http::string_body> req{http::verb::get, "/a", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(true);
    ASSERT_FALSE(c.write(req));
    http::response<http::string_body> res;
    ASSERT_FALSE(c.read(res));
    EXPECT_EQ(res.result_int(), 200);
    EXPECT_EQ(res.body(), "a");
    http::response<http::string_body> idle;
    auto t0 = std::chrono::steady_clock::now();
    const auto ec = c.read(idle, std::chrono::seconds(8));
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, boost::beast::error::timeout);
    EXPECT_LT(ms, 1000) << "idle timeout must fire before read_timeout";
}

TEST(Limits, MaxConnectionsRefusesNew) {
    TestServer srv([](hayate::App &app) {
        app.limits().max_connections = 1;
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    Conn hold(srv.port());
    ASSERT_FALSE(hold.connect_error());
    // TCP は backlog で繋がる。サーバーは accept した直後に切るので、書き込みの失敗は見ない。
    Conn extra(srv.port());
    (void)extra.write_raw("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    http::response<http::string_body> res;
    const auto ec = extra.read(res);
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, boost::beast::error::timeout);
}

TEST(Limits, ReadTimeoutAppliesToKeepAliveRequests) {
    TestServer srv([](hayate::App &app) {
        app.limits().read_timeout = std::chrono::milliseconds(50);
        app.limits().idle_timeout = std::chrono::seconds(5);
        app.get("/a", [](hayate::Request &) { return hayate::Response::text("a"); });
    });
    Conn c(srv.port());
    http::request<http::string_body> req{http::verb::get, "/a", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(true);
    ASSERT_FALSE(c.write(req));
    http::response<http::string_body> res;
    ASSERT_FALSE(c.read(res));
    ASSERT_EQ(res.result_int(), 200);

    // 2 本目を途中まで送る。窓は read_timeout であって idle_timeout ではない。
    ASSERT_FALSE(
        c.write_raw("POST /a HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10\r\n\r\nab"));
    http::response<http::string_body> second;
    auto t0 = std::chrono::steady_clock::now();
    const auto ec = c.read(second, std::chrono::seconds(8));
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, boost::beast::error::timeout);
    EXPECT_LT(ms, 1000) << "read timeout must govern the body of a keep-alive request";
}

// accept が一度失敗しただけで accept ループを終えると、fd が戻っても誰も繋げない。
TEST(Limits, AcceptSurvivesFdExhaustion) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    // 使い切った後はクライアントも fd を作れない。2 本とも先に取っておく。
    Conn starved(srv.port(), Conn::deferred);
    Conn later(srv.port(), Conn::deferred);
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(false);
    {
        FdHog hog;
        ASSERT_FALSE(starved.connect());
        ASSERT_FALSE(starved.write(req));
        // サーバーの accept は EMFILE になる。macOS はその接続を RST で捨て、Linux は
        // backlog に残す。どちらでも枯渇中に応答は来ない。
        http::response<http::string_body> res;
        EXPECT_TRUE(starved.read(res, std::chrono::milliseconds(200)));
    }
    ASSERT_FALSE(later.connect());
    ASSERT_FALSE(later.write(req));
    http::response<http::string_body> res;
    const auto ec = later.read(res);
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
