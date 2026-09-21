#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <string>

namespace http = boost::beast::http;
using hayate::Json;

namespace {

hayate::Response ok(hayate::Request &) { return hayate::Response::text("ok"); }

} // namespace

TEST(Openapi, ListsRegisteredRoutes) {
    hayate::App app;
    app.get("/", ok);
    app.get("/users", ok);
    app.get("/users/:id", ok);
    const auto doc = hayate::openapi(app);
    EXPECT_EQ(doc.value("openapi", ""), "3.1.0");
    const auto &paths = doc.at("paths");
    EXPECT_TRUE(paths.contains("/"));
    EXPECT_TRUE(paths.contains("/users"));
    EXPECT_TRUE(paths.contains("/users/{id}"));
}

TEST(Openapi, PathParamBecomesParameter) {
    hayate::App app;
    app.get("/users/:id", ok);
    const auto doc = hayate::openapi(app);
    const auto &params = doc.at("paths").at("/users/{id}").at("get").at("parameters");
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(params[0].value("name", ""), "id");
    EXPECT_EQ(params[0].value("in", ""), "path");
    EXPECT_TRUE(params[0].value("required", false));
    EXPECT_EQ(params[0].at("schema").value("type", ""), "string");
    EXPECT_FALSE(params[0].contains("x-hayate-wildcard"));
}

TEST(Openapi, MergesMethodsOnOnePath) {
    hayate::App app;
    app.get("/users", ok);
    app.post("/users", ok);
    const auto doc = hayate::openapi(app);
    const auto &item = doc.at("paths").at("/users");
    EXPECT_TRUE(item.contains("get"));
    EXPECT_TRUE(item.contains("post"));
    EXPECT_EQ(item.size(), 2u);
}

// /users/{id} と /users/{name} は OpenAPI では同じテンプレート。別キーにすると不正な文書になる。
TEST(Openapi, SameShapeDifferentParamNamesMerge) {
    hayate::App app;
    app.get("/users/:id", ok);
    app.post("/users/:name", ok);
    const auto doc = hayate::openapi(app);
    const auto &paths = doc.at("paths");
    EXPECT_EQ(paths.size(), 1u) << paths.dump();
    ASSERT_TRUE(paths.contains("/users/{id}"));
    const auto &item = paths.at("/users/{id}");
    EXPECT_TRUE(item.contains("get"));
    ASSERT_TRUE(item.contains("post"));
    EXPECT_EQ(item.at("post").at("parameters").at(0).value("name", ""), "id");
}

TEST(Openapi, UnregisteredMethodIsAbsent) {
    hayate::App app;
    app.get("/users", ok);
    // 一時オブジェクトの部分式に参照を束ねると寿命が延びない。先に受ける。
    const auto doc = hayate::openapi(app);
    const auto &item = doc.at("paths").at("/users");
    EXPECT_FALSE(item.contains("post"));
    EXPECT_TRUE(item.at("get").at("responses").contains("default"));
}

TEST(Openapi, WildcardIsMarked) {
    hayate::App app;
    app.get("/assets/*path", ok);
    const auto doc = hayate::openapi(app);
    const auto &params = doc.at("paths").at("/assets/{path}").at("get").at("parameters");
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(params[0].value("name", ""), "path");
    EXPECT_TRUE(params[0].value("x-hayate-wildcard", false));
}

TEST(Openapi, GroupPrefixIsIncluded) {
    hayate::App app;
    app.group("/api", [](hayate::Router &r) {
        r.get("/me", ok);
        r.get("/items/:id", ok);
    });
    const auto doc = hayate::openapi(app);
    const auto &paths = doc.at("paths");
    EXPECT_TRUE(paths.contains("/api/me"));
    EXPECT_TRUE(paths.contains("/api/items/{id}"));
}

TEST(Openapi, InfoFromArgument) {
    hayate::App app;
    app.get("/", ok);
    const auto def = hayate::openapi(app);
    EXPECT_EQ(def.at("info").value("title", ""), "hayate");
    EXPECT_EQ(def.at("info").value("version", ""), "0.1.0");
    const auto named = hayate::openapi(app, {.title = "my api", .version = "2.3"});
    EXPECT_EQ(named.at("info").value("title", ""), "my api");
    EXPECT_EQ(named.at("info").value("version", ""), "2.3");
}

TEST(Openapi, ServedAsJson) {
    TestServer srv([](hayate::App &app) {
        app.get("/users/:id", ok);
        app.get("/openapi.json",
                [&app](hayate::Request &) { return hayate::Response::json(hayate::openapi(app)); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/openapi.json");
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(r.content_type.find("application/json"), std::string::npos);
    const auto doc = Json::parse(r.body, nullptr, false);
    ASSERT_FALSE(doc.is_discarded());
    EXPECT_EQ(doc.value("openapi", ""), "3.1.0");
    // 自分自身も登録済みルートなので出る。
    EXPECT_TRUE(doc.at("paths").contains("/openapi.json"));
    EXPECT_TRUE(doc.at("paths").contains("/users/{id}"));
}
