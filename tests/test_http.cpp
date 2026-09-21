#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace http = boost::beast::http;
namespace net = boost::asio;

namespace {

// keep-alive で同じ接続を使い回すための素の接続。
struct Conn {
    net::io_context ioc;
    boost::beast::tcp_stream stream{ioc};
    boost::beast::flat_buffer buf;
    explicit Conn(std::uint16_t port) {
        stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    }
};

http::request<http::string_body> make_req(http::verb v, std::string target, bool keep) {
    http::request<http::string_body> req{v, std::move(target), 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(keep);
    return req;
}

std::string text_of(hayate::Request &req) {
    const auto b = req.body();
    return {reinterpret_cast<const char *>(b.data()), b.size()};
}

} // namespace

// listen だけして accept しない。接続は backlog で成立し、応答は永遠に来ない。
// Beast の期限は同期 I/O に効かないので、クライアントが非同期でないと固まる。
TEST(HttpClient, TimesOutOnSilentServer) {
    net::io_context ioc;
    net::ip::tcp::acceptor silent(ioc, {net::ip::make_address("127.0.0.1"), 0});
    const auto port = silent.local_endpoint().port();
    const auto began = std::chrono::steady_clock::now();
    auto r =
        http_call("127.0.0.1", port, http::verb::get, "/", {}, {}, std::chrono::milliseconds(200));
    EXPECT_TRUE(r.error);
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(2));
}

// HEAD への応答に本文があると、読み手は残りを次の応答として読んで壊れる。
TEST(Http, HeadGetsNoBody) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) { return hayate::Response::text("root"); });
    });
    Conn c(srv.port());
    http::write(c.stream, make_req(http::verb::head, "/", true));
    http::response_parser<http::empty_body> head;
    head.skip(true);
    http::read(c.stream, c.buf, head);
    EXPECT_EQ(head.get().result_int(), 405);

    http::write(c.stream, make_req(http::verb::get, "/", false));
    http::response<http::string_body> res;
    boost::system::error_code ec;
    http::read(c.stream, c.buf, res, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(res.result_int(), 200);
    EXPECT_EQ(res.body(), "root");
}

// 100 を待つクライアントに何も返さないと、互いに待って read_timeout で切れる。
TEST(Http, ExpectContinueGets100) {
    TestServer srv([](hayate::App &app) {
        auto l = app.limits();
        l.read_timeout = std::chrono::milliseconds(500);
        app.limits(l);
        app.post("/echo",
                 [](hayate::Request &req) { return hayate::Response::text(text_of(req)); });
    });
    Conn c(srv.port());
    auto req = make_req(http::verb::post, "/echo", false);
    req.set(http::field::expect, "100-continue");
    req.body() = "hello";
    req.prepare_payload();
    http::request_serializer<http::string_body> sr{req};
    http::write_header(c.stream, sr);

    http::response<http::empty_body> interim;
    boost::system::error_code ec;
    http::read(c.stream, c.buf, interim, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(interim.result(), http::status::continue_);

    http::write(c.stream, sr);
    http::response<http::string_body> res;
    http::read(c.stream, c.buf, res, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(res.result_int(), 200);
    EXPECT_EQ(res.body(), "hello");
}

// 上限を超える本文は、100 で送らせてから断るのではなく、先に 413。
TEST(Http, ExpectContinueOverLimitIs413Without100) {
    TestServer srv([](hayate::App &app) {
        auto l = app.limits();
        l.max_body_bytes = 4;
        l.read_timeout = std::chrono::milliseconds(500);
        app.limits(l);
        app.post("/echo",
                 [](hayate::Request &req) { return hayate::Response::text(text_of(req)); });
    });
    Conn c(srv.port());
    auto req = make_req(http::verb::post, "/echo", false);
    req.set(http::field::expect, "100-continue");
    req.body() = "hello";
    req.prepare_payload();
    http::request_serializer<http::string_body> sr{req};
    http::write_header(c.stream, sr);

    http::response<http::string_body> res;
    boost::system::error_code ec;
    http::read(c.stream, c.buf, res, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(res.result_int(), 413);
}

// ハンドラが close を付けたのに接続を続けると、ヘッダと挙動が食い違う。
TEST(Http, HandlerCloseIsHonored) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) {
            auto res = hayate::Response::text("bye");
            res.set_header("Connection", "close");
            return res;
        });
    });
    Conn c(srv.port());
    http::write(c.stream, make_req(http::verb::get, "/", true));
    http::response<http::string_body> first;
    http::read(c.stream, c.buf, first);
    EXPECT_EQ(first.result_int(), 200);
    EXPECT_FALSE(first.keep_alive());

    boost::system::error_code ec;
    http::write(c.stream, make_req(http::verb::get, "/", true), ec);
    http::response<http::string_body> second;
    if (!ec) {
        http::read(c.stream, c.buf, second, ec);
    }
    EXPECT_TRUE(ec) << "サーバーは 1 本目の後に閉じているはず";
}

// サーバーが閉じると決めたら、ハンドラの keep-alive は応答に出さない。
TEST(Http, ServerCloseOverridesHandlerKeepAlive) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) {
            auto res = hayate::Response::text("hi");
            res.set_header("Connection", "keep-alive");
            return res;
        });
    });
    Conn c(srv.port());
    http::write(c.stream, make_req(http::verb::get, "/", false));
    http::response<http::string_body> res;
    http::read(c.stream, c.buf, res);
    EXPECT_EQ(res.result_int(), 200);
    EXPECT_FALSE(res.keep_alive());
}

// 204 は本文を持たない。`Content-Length: 0` も付けない（RFC 9110 §8.6）。
TEST(Http, NoContentHasNoContentLength) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) { return hayate::Response::no_content(); });
    });
    Conn c(srv.port());
    http::write(c.stream, make_req(http::verb::get, "/", false));
    http::response<http::string_body> res;
    http::read(c.stream, c.buf, res);
    EXPECT_EQ(res.result_int(), 204);
    EXPECT_EQ(res.count(http::field::content_length), 0u);
}

// 本文付きの 204 を Beast に渡すと投げ、応答なしで切れる。本文は捨てて送る。
TEST(Http, NoContentDropsHandlerBody) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) {
            auto res = hayate::Response::text("x");
            res.status(204);
            return res;
        });
    });
    Conn c(srv.port());
    boost::system::error_code ec;
    http::write(c.stream, make_req(http::verb::get, "/", false), ec);
    http::response<http::string_body> res;
    if (!ec) {
        http::read(c.stream, c.buf, res, ec);
    }
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(res.result_int(), 204);
    EXPECT_EQ(res.count(http::field::content_length), 0u);
    EXPECT_TRUE(res.body().empty());
}

// Beast はヘッダ 1 本が 64 KiB 弱を超えると投げる。投げると応答なしで切れる。
TEST(Http, OversizedHeaderIs500) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) {
            auto res = hayate::Response::text("ok");
            res.set_header("X-Big", std::string(70000, 'a'));
            return res;
        });
    });
    Conn c(srv.port());
    boost::system::error_code ec;
    http::write(c.stream, make_req(http::verb::get, "/", false), ec);
    http::response<http::string_body> res;
    if (!ec) {
        http::read(c.stream, c.buf, res, ec);
    }
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(res.result_int(), 500);
    EXPECT_EQ(res.count("X-Big"), 0u);
}
