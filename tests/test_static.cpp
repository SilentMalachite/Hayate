#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace http = boost::beast::http;
namespace fs = std::filesystem;

namespace {

struct StaticDir {
    fs::path dir;
    StaticDir() {
        dir =
            fs::temp_directory_path() /
            ("hayate_static_" + std::to_string(std::hash<std::string>{}(
                                    __FILE__ + std::to_string(reinterpret_cast<uintptr_t>(this)))));
        fs::create_directories(dir);
    }
    ~StaticDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

TEST(Static, ServesExistingFile) {
    StaticDir root;
    {
        std::ofstream out(root.dir / "hello.txt");
        out << "hi";
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/hello.txt");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "hi");
    EXPECT_NE(r.content_type.find("text/plain"), std::string::npos);
}

TEST(Static, MissingIs404) {
    StaticDir root;
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/nope.txt");
    EXPECT_EQ(r.status, 404);
}

TEST(Static, PathTraversalIs404) {
    StaticDir root;
    const auto secret = root.dir.parent_path() / ("hayate_secret_" + root.dir.filename().string());
    {
        std::ofstream out(secret);
        out << "secret";
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get,
                       "/assets/../" + secret.filename().string());
    EXPECT_EQ(r.status, 404);
    std::error_code ec;
    fs::remove(secret, ec);
}

TEST(Static, IndexHtml) {
    StaticDir root;
    {
        std::ofstream out(root.dir / "index.html");
        out << "<h1>ok</h1>";
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "<h1>ok</h1>");
    EXPECT_NE(r.content_type.find("text/html"), std::string::npos);
}

TEST(Static, HtmlContentType) {
    StaticDir root;
    {
        std::ofstream out(root.dir / "page.html");
        out << "<p>x</p>";
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/page.html");
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(r.content_type.find("text/html"), std::string::npos);
}

TEST(Static, OversizeFileIs404) {
    StaticDir root;
    {
        std::ofstream out(root.dir / "big.txt");
        out << std::string(64, 'x');
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string(), 16)); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/big.txt");
    EXPECT_EQ(r.status, 404);
}

TEST(Static, UnderLimitFileIsServed) {
    StaticDir root;
    {
        std::ofstream out(root.dir / "small.txt");
        out << "hi";
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string(), 16)); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/small.txt");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "hi");
}

TEST(Static, TrailingSlashRootCreatedLater) {
    // 登録時に root が無いと weakly_canonical が末尾 / を残す。それでも配れること。
    StaticDir parent;
    const auto late = parent.dir / "late";
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(late.string() + "/")); });
    fs::create_directories(late);
    {
        std::ofstream out(late / "hello.txt");
        out << "hi";
    }
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/hello.txt");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "hi");
}
