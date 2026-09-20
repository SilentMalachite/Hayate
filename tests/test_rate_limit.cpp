#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <chrono>

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
