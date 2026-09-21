#include "http_client.hpp"
#include "temp_dir.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace http = boost::beast::http;
namespace fs = std::filesystem;

TEST(Static, ServesExistingFile) {
    TempDir root;
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
    TempDir root;
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/nope.txt");
    EXPECT_EQ(r.status, 404);
}

TEST(Static, PathTraversalIs404) {
    TempDir root;
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

// root 内の symlink が root 外を指すなら、存在を漏らさず 404。
TEST(Static, SymlinkEscapingRootIs404) {
    TempDir root;
    const auto secret = root.dir.parent_path() / ("hayate_secret_" + root.dir.filename().string());
    {
        std::ofstream out(secret);
        out << "secret";
    }
    std::error_code link_ec;
    fs::create_symlink(secret, root.dir / "link.txt", link_ec);
    ASSERT_FALSE(link_ec);
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/link.txt");
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(r.body.find("secret"), std::string::npos);
    std::error_code ec;
    fs::remove(secret, ec);
}

// 検査を通ったファイルが、送出前に root 外への symlink へ差し替えられても、
// 送るのは検査したバイト列であること。
TEST(Static, SwapAfterStatStillServesVerifiedBytes) {
    TempDir root;
    const auto secret = root.dir.parent_path() / ("hayate_secret_" + root.dir.filename().string());
    {
        std::ofstream out(secret);
        out << "secret";
    }
    {
        std::ofstream out(root.dir / "a.txt");
        out << "public";
    }
    std::promise<void> in_gate;
    std::promise<void> release;
    auto gate_hit = std::make_shared<std::atomic<bool>>(false);
    TestServer srv([&](hayate::App &app) {
        app.use([&](hayate::Request &req,
                    hayate::Next next) -> boost::asio::awaitable<hayate::Response> {
            auto res = co_await next(req);
            // ハンドラは終わり、まだ送っていない。ここで差し替えられる。
            if (!gate_hit->exchange(true)) {
                in_gate.set_value();
                release.get_future().wait();
            }
            co_return res;
        });
        app.get("/assets/*path", hayate::files(root.dir.string()));
    });
    std::promise<HttpCall> done;
    std::thread caller([&] {
        done.set_value(http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/a.txt", {}, {},
                                 std::chrono::seconds(10)));
    });
    in_gate.get_future().wait();
    std::error_code ec;
    fs::remove(root.dir / "a.txt", ec);
    fs::create_symlink(secret, root.dir / "a.txt", ec);
    ASSERT_FALSE(ec);
    release.set_value();
    auto r = done.get_future().get();
    caller.join();
    EXPECT_EQ(r.body, "public");
    EXPECT_NE(r.body, "secret");
    fs::remove(secret, ec);
}

// FIFO は通常ファイルではない。書き手が居なくても待たされずに 404。
TEST(Static, FifoIsNotServedAndDoesNotHang) {
    TempDir root;
    const auto fifo = root.dir / "pipe";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    const auto began = std::chrono::steady_clock::now();
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/pipe", {}, {},
                       std::chrono::seconds(3));
    EXPECT_EQ(r.status, 404);
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(3));
}

TEST(Static, IndexHtml) {
    TempDir root;
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
    TempDir root;
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
    TempDir root;
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
    TempDir root;
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
    TempDir parent;
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

// 未作成の相対 root が相対のまま残ると、後で作っても候補（絶対パス）が root 外扱いになる。
// libstdc++ の weakly_canonical で起きる。libc++ では今も通る（回帰の檻）。
TEST(Static, RelativeRootCreatedLater) {
    TempDir base;
    struct CwdGuard {
        fs::path saved = fs::current_path();
        ~CwdGuard() {
            std::error_code ec;
            fs::current_path(saved, ec);
        }
    } guard;
    fs::current_path(base.dir);
    TestServer srv([](hayate::App &app) { app.get("/assets/*path", hayate::files("public")); });
    fs::create_directories(base.dir / "public");
    {
        std::ofstream out(base.dir / "public" / "a.txt");
        out << "late";
    }
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/a.txt");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "late");
}

TEST(Static, PercentEncodedFileName) {
    TempDir root;
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
    TempDir root;
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
    TempDir root;
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
    TempDir root;
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
    TempDir root;
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
    TempDir root;
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
    TempDir root;
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
