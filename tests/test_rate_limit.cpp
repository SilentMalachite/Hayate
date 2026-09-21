#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>

namespace http = boost::beast::http;

TEST(RateLimit, AllowsUnderLimit) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::rate_limit({.max = 10, .window = std::chrono::seconds(60)}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto a = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto b = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(a.status, 200) << a.error_message;
    EXPECT_EQ(b.status, 200) << b.error_message;
}

TEST(RateLimit, OverLimitIs429) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::rate_limit({.max = 2, .window = std::chrono::seconds(60)}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto a = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto b = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto c = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(a.status, 200) << a.error_message;
    EXPECT_EQ(b.status, 200) << b.error_message;
    EXPECT_EQ(c.status, 429) << c.error_message;
    EXPECT_FALSE(c.retry_after.empty());
}

TEST(RateLimit, PeerIsLoopback) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.peer()));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(r.status, 200) << r.error_message;
    EXPECT_TRUE(r.body.find("127.0.0.1") != std::string::npos ||
                r.body.find("::1") != std::string::npos)
        << r.body;
}

TEST(RateLimit, KeepAliveRequestsShareOnePeerKey) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::rate_limit({.max = 1, .window = std::chrono::seconds(60)}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    namespace net = boost::asio;
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(2));
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    boost::beast::flat_buffer buf;
    auto send = [&] {
        stream.expires_after(std::chrono::seconds(2));
        http::request<http::string_body> req{http::verb::get, "/", 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::write(stream, req);
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        return res.result_int();
    };
    EXPECT_EQ(send(), 200u);
    EXPECT_EQ(send(), 429u);
}

// 拒否中も数え続けると、同じ窓で 2^32 回目に 0 へ戻って通ってしまう。
TEST(RateLimit, RejectedHitsDoNotWrap) {
    hayate::mw::detail::RateWindow w;
    w.count = std::numeric_limits<std::uint32_t>::max();
    EXPECT_FALSE(hayate::mw::detail::admit(w, 1));
    EXPECT_EQ(w.count, std::numeric_limits<std::uint32_t>::max());
}
