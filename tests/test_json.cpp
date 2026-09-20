#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

TEST(Response, JsonFactorySetsContentType) {
    auto res = hayate::Response::json({{"id", "7"}});
    EXPECT_EQ(res.status(), 200);
    EXPECT_EQ(res.body(), "{\"id\":\"7\"}");
    EXPECT_EQ(res.header("content-type"), "application/json");
}

TEST(Response, TextAndNoContent) {
    auto t = hayate::Response::text("hello");
    EXPECT_EQ(t.status(), 200);
    EXPECT_EQ(t.body(), "hello");
    auto n = hayate::Response::no_content();
    EXPECT_EQ(n.status(), 204);
    EXPECT_TRUE(n.body().empty());
}

TEST(Response, FromErrorUsesHttpStatus) {
    auto res = hayate::Response::from_error({"bad_json", "x", 400});
    EXPECT_EQ(res.status(), 400);
}

TEST(Json, PostEcho) {
    TestServer srv([](hayate::App &app) {
        app.post("/echo", [](hayate::Request &req) {
            auto body = req.json();
            if (!body.ok()) {
                return hayate::Response::from_error(body.error());
            }
            return hayate::Response::json(body.value());
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), boost::beast::http::verb::post, "/echo",
                       R"({"id":"7"})", "application/json");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(hayate::Json::parse(r.body)["id"], "7");
}

TEST(Json, BadBodyIs400) {
    TestServer srv([](hayate::App &app) {
        app.post("/echo", [](hayate::Request &req) {
            auto body = req.json();
            if (!body.ok()) {
                return hayate::Response::from_error(body.error());
            }
            return hayate::Response::json(body.value());
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), boost::beast::http::verb::post, "/echo", "{",
                       "application/json");
    EXPECT_EQ(r.status, 400);
}
