#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

namespace http = boost::beast::http;

TEST(Cors, GetAddsAllowOrigin) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors());
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                       std::chrono::seconds(2), {{"Origin", "https://app.example"}});
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.allow_origin, "*");
}

TEST(Cors, PreflightOptionsIs204) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors());
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call(
        "127.0.0.1", srv.port(), http::verb::options, "/", {}, {}, std::chrono::seconds(2),
        {{"Origin", "https://app.example"}, {"Access-Control-Request-Method", "GET"}});
    EXPECT_EQ(r.status, 204);
    EXPECT_EQ(r.allow_origin, "*");
    EXPECT_NE(r.allow_methods.find("GET"), std::string::npos);
    EXPECT_NE(r.allow_headers.find("Content-Type"), std::string::npos);
    EXPECT_TRUE(r.body.empty());
}

// 下流が付けた Vary を上書きすると、キャッシュが別言語・別 Cookie の応答を配る。
TEST(Cors, KeepsExistingVary) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors({.origin = "https://app.example"}));
        app.get("/", [](hayate::Request &) {
            auto res = hayate::Response::text("ok");
            res.set_header("Vary", "Accept-Language");
            return res;
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                       std::chrono::seconds(2), {{"Origin", "https://app.example"}});
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(r.vary.find("Accept-Language"), std::string::npos) << r.vary;
    EXPECT_NE(r.vary.find("Origin"), std::string::npos) << r.vary;
}

// 固定 origin では Origin の有無で応答が変わる。Origin 無しの応答にも Vary が要る。
TEST(Cors, FixedOriginVariesWithoutOriginHeader) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors({.origin = "https://app.example"}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.allow_origin.empty());
    EXPECT_EQ(r.vary, "Origin");
}

TEST(Cors, NoOriginOmitsHeader) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors());
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.allow_origin.empty());
}

TEST(Cors, NotFoundStillHasOrigin) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors());
        app.get("/x", [](hayate::Request &) { return hayate::Response::text("x"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/missing", {}, {},
                       std::chrono::seconds(2), {{"Origin", "https://app.example"}});
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(r.allow_origin, "*");
}

TEST(Cors, CustomOrigin) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors({.origin = "https://app.example"}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                       std::chrono::seconds(2), {{"Origin", "https://app.example"}});
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.allow_origin, "https://app.example");
}

TEST(Cors, SpecificOriginSetsVary) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors({.origin = "https://app.example"}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                       std::chrono::seconds(2), {{"Origin", "https://app.example"}});
    EXPECT_EQ(r.allow_origin, "https://app.example");
    EXPECT_EQ(r.vary, "Origin");
}

TEST(Cors, WildcardOriginHasNoVary) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors());
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                       std::chrono::seconds(2), {{"Origin", "https://app.example"}});
    EXPECT_EQ(r.vary, "");
}

TEST(Cors, PreflightSpecificOriginSetsVary) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors({.origin = "https://app.example"}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::options, "/", {}, {},
                       std::chrono::seconds(2), {{"Origin", "https://app.example"}});
    EXPECT_EQ(r.status, 204);
    EXPECT_EQ(r.vary, "Origin");
}
