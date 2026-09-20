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

TEST(Static, PercentEncodedFileName) {
    StaticDir root;
    {
        std::ofstream out(root.dir / "my file.txt");
        out << "hi";
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/my%20file.txt");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "hi");
}

TEST(Static, EncodedTraversalIs404) {
    StaticDir root;
    const auto secret = root.dir.parent_path() / ("hayate_secret_" + root.dir.filename().string());
    {
        std::ofstream out(secret);
        out << "secret";
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get,
                       "/assets/..%2F" + secret.filename().string());
    EXPECT_EQ(r.status, 404) << r.body;
    std::error_code ec;
    fs::remove(secret, ec);
}

TEST(Static, NulInPathIs404) {
    StaticDir root;
    {
        std::ofstream out(root.dir / "hello.txt");
        out << "hi";
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/hello.txt%00.png");
    EXPECT_EQ(r.status, 404) << r.body;
}

TEST(Static, LargeFileIsStreamedWhole) {
    StaticDir root;
    // 64 KiB チャンクを 3 本 + 端数。ループが複数回まわる大きさ。
    std::string want;
    want.reserve(200 * 1024);
    for (std::size_t i = 0; want.size() < 200 * 1024; ++i) {
        want += "hayate-" + std::to_string(i) + "\n";
    }
    {
        std::ofstream out(root.dir / "big.bin", std::ios::binary);
        out << want;
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/big.bin", {}, {},
                       std::chrono::seconds(5), {}, {"Content-Length", "Transfer-Encoding"});
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body.size(), want.size());
    EXPECT_EQ(r.body, want);
    EXPECT_EQ(r.extra["Content-Length"], std::to_string(want.size()));
    EXPECT_TRUE(r.extra["Transfer-Encoding"].empty());
}

TEST(Static, EmptyFileIsServed) {
    StaticDir root;
    {
        std::ofstream out(root.dir / "empty.txt", std::ios::binary);
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/empty.txt", {}, {},
                       std::chrono::seconds(2), {}, {"Content-Length"});
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body.empty());
    EXPECT_EQ(r.extra["Content-Length"], "0");
}

TEST(Static, DefaultHasNoSizeCap) {
    StaticDir root;
    // 旧既定（1 MiB）なら 404 になっていた大きさ。
    const std::string want(2 * 1024 * 1024, 'x');
    {
        std::ofstream out(root.dir / "huge.bin", std::ios::binary);
        out << want;
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/huge.bin", {}, {},
                       std::chrono::seconds(10));
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body.size(), want.size());
}

TEST(Static, KeepAliveAfterStreamedFile) {
    StaticDir root;
    const std::string big(200 * 1024, 'y');
    {
        std::ofstream out(root.dir / "big.bin", std::ios::binary);
        out << big;
    }
    {
        std::ofstream out(root.dir / "small.txt");
        out << "hi";
    }
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    namespace net = boost::asio;
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(5));
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    boost::beast::flat_buffer buf;
    auto send = [&](const std::string &target) {
        stream.expires_after(std::chrono::seconds(5));
        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::write(stream, req);
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        return res.body();
    };
    // 1 本目を Content-Length ちょうどで閉じていなければ 2 本目が読めない。
    EXPECT_EQ(send("/assets/big.bin").size(), big.size());
    EXPECT_EQ(send("/assets/small.txt"), "hi");
}
