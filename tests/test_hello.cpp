#include "http_client.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>

TEST(Hello, GetRootReturns200) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) { return hayate::Response::text("hello"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), boost::beast::http::verb::get, "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "hello");
}
