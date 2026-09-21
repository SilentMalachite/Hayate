#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

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

TEST(Json, TypeMismatchIs400) {
    TestServer srv([](hayate::App &app) {
        app.post("/s", [](hayate::Request &req) {
            auto body = req.json<std::string>();
            if (!body.ok()) {
                return hayate::Response::from_error(body.error());
            }
            return hayate::Response::text(body.value());
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), boost::beast::http::verb::post, "/s",
                       R"({"id":"7"})", "application/json");
    EXPECT_EQ(r.status, 400);
}

TEST(Json, TypeOkString) {
    TestServer srv([](hayate::App &app) {
        app.post("/s", [](hayate::Request &req) {
            auto body = req.json<std::string>();
            if (!body.ok()) {
                return hayate::Response::from_error(body.error());
            }
            return hayate::Response::text(body.value());
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), boost::beast::http::verb::post, "/s", R"("hello")",
                       "application/json");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "hello");
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

// json<T>() も、壊れた本文は型の不一致ではなく bad_json の 400。
TEST(Json, TypedBadBodyIs400) {
    TestServer srv([](hayate::App &app) {
        app.post("/s", [](hayate::Request &req) {
            auto body = req.json<std::string>();
            if (!body.ok()) {
                return hayate::Response::from_error(body.error());
            }
            return hayate::Response::text(body.value());
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), boost::beast::http::verb::post, "/s", "{",
                       "application/json");
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body, "broken json");
}

// 名前は RFC 9110 の token。token でない名前と空の名前は捨てる。値の前後の空白は削る。
TEST(Response, SetHeaderTokenAndTrim) {
    auto r = hayate::Response::text("x");
    auto count = [&] {
        std::size_t n = 0;
        r.for_each_header([&](std::string_view, std::string_view) { ++n; });
        return n;
    };
    r.set_header("X-!#$%&'*+-.^_`|~9", "v");
    EXPECT_EQ(r.header("X-!#$%&'*+-.^_`|~9"), "v");
    const auto before = count();
    r.set_header("", "v");
    for (const char *bad : {"X(", "X,", "X/", "X:", "X;", "X=", "X@", "X[", "X{", "X\"", "X y"}) {
        r.set_header(bad, "v");
    }
    EXPECT_EQ(count(), before);
    r.set_header("X-Pad", "  a b  ");
    EXPECT_EQ(r.header("X-Pad"), "a b");
}
