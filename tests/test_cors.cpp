#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <string_view>

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

// Vary は既存の値を残して Origin を足す。既に Origin（大小無視）か * があれば触らない。
TEST(Cors, AddVaryMergesTokens) {
    struct Case {
        const char *before;
        const char *after;
    };
    for (const auto &c :
         {Case{"", "Origin"}, Case{"Accept-Language", "Accept-Language, Origin"},
          Case{"X-Origin", "X-Origin, Origin"}, Case{"*", "*"}, Case{"Accept, *", "Accept, *"},
          Case{"Origin", "Origin"}, Case{"origin", "origin"},
          // タブは set_header が CTL として落とすので、ここへ届くのは空白だけ。
          Case{"Accept-Language ,  Origin", "Accept-Language ,  Origin"}}) {
        auto res = hayate::Response::text("ok");
        if (*c.before != '\0') {
            res.set_header("Vary", c.before);
        }
        hayate::mw::detail::add_vary(res, "Origin");
        EXPECT_EQ(res.header("Vary"), std::string_view(c.after))
            << "before: \"" << c.before << "\"";
    }
}

// 404 / 405 も Origin の有無で変わらない応答ではない。固定 origin なら Vary が要る。
TEST(Cors, FixedOriginVariesOnErrorsWithoutOrigin) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors({.origin = "https://app.example"}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto missing = http_call("127.0.0.1", srv.port(), http::verb::get, "/missing");
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.vary, "Origin");
    EXPECT_TRUE(missing.allow_origin.empty());
    auto wrong_method = http_call("127.0.0.1", srv.port(), http::verb::post, "/");
    EXPECT_EQ(wrong_method.status, 405);
    EXPECT_EQ(wrong_method.vary, "Origin");
    EXPECT_TRUE(wrong_method.allow_origin.empty());
}

// 空の Origin は Origin 無しと同じ。Access-Control-* を付けず、preflight にもしない。
TEST(Cors, EmptyOriginIsAbsent) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::cors());
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto get = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                         std::chrono::seconds(2), {{"Origin", ""}});
    EXPECT_EQ(get.status, 200);
    EXPECT_TRUE(get.allow_origin.empty());
    // origin が * なら Vary は要らない。
    EXPECT_TRUE(get.vary.empty()) << get.vary;
    auto options = http_call("127.0.0.1", srv.port(), http::verb::options, "/", {}, {},
                             std::chrono::seconds(2), {{"Origin", ""}});
    EXPECT_NE(options.status, 204);
    EXPECT_TRUE(options.allow_origin.empty());
    EXPECT_TRUE(options.allow_methods.empty());
}
