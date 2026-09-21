#include "conn_client.hpp"
#include "detail/open_file.hpp"
#include "http_client.hpp"
#include "temp_dir.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
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

// 包含判定が「root の直後が区切りか」だと、root が `/` のときに配下が全部外れる。
TEST(Static, RootSlashServesDescendant) {
    TempDir base;
    {
        std::ofstream out(base.dir / "a.txt");
        out << "root";
    }
    const auto abs = fs::canonical(base.dir / "a.txt").string();
    TestServer srv([](hayate::App &app) { app.get("/assets/*path", hayate::files("/")); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets" + abs);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "root");
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
    Conn c(srv.port(), std::chrono::seconds(5));
    auto send = [&](const std::string &target) -> std::string {
        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::response<http::string_body> res;
        if (c.write(req, std::chrono::seconds(5)) || c.read(res, std::chrono::seconds(5))) {
            return {};
        }
        return res.body();
    };
    // 1 本目を Content-Length ちょうどで閉じていなければ 2 本目が読めない。
    EXPECT_EQ(send("/assets/big.bin").size(), big.size());
    EXPECT_EQ(send("/assets/small.txt"), "hi");
}

namespace {

void write_text(const fs::path &p, std::string_view s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

// 書き込み側の pipe を閉じる。TestServer より後に宣言して先に壊すと、read で待つワーカーが
// EOF で抜け、プールの join が固まらない。
struct PipeEnd {
    int fd{-1};
    ~PipeEnd() { close(); }
    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

// pipe の読み側を FileSource にする。stat の無い「まだ書かれていないファイル」を作れる。
hayate::Response pipe_response(int read_fd, std::uint64_t size,
                               std::shared_ptr<boost::asio::thread_pool> pool) {
    auto file = std::make_shared<hayate::detail::OpenFile>();
    file->f.native_handle(read_fd);
    return hayate::Response::file({"stream.bin", size, std::move(pool), std::move(file)});
}

} // namespace

// 最後の要素が symlink なら、root 内を指していても開かない。canonical を渡したはずの名前が
// 差し替えられた印なので、O_NOFOLLOW で断る。
TEST(Static, OpenVerifiedRefusesFinalSymlink) {
    TempDir t;
    // macOS の /var は /private/var への symlink。files() と同じく canonical の root を渡す。
    const auto root = fs::canonical(t.dir);
    write_text(root / "real.txt", "real");
    fs::create_symlink(root / "real.txt", root / "link.txt");
    const auto real = hayate::detail::open_verified(root, root / "real.txt");
    ASSERT_TRUE(real.has_value());
    EXPECT_EQ(real->size, 4u);
    EXPECT_FALSE(hayate::detail::open_verified(root, root / "link.txt").has_value());
}

// 途中のディレクトリが root 外への symlink なら、開いた fd の実パスで弾く。
TEST(Static, OpenVerifiedRefusesEscapeThroughDirectory) {
    TempDir t;
    TempDir outside;
    const auto root = fs::canonical(t.dir);
    write_text(fs::canonical(outside.dir) / "secret.txt", "secret");
    fs::create_directory_symlink(fs::canonical(outside.dir), root / "dir");
    EXPECT_FALSE(hayate::detail::open_verified(root, root / "dir" / "secret.txt").has_value());
}

// 文字列の前置一致ではなくパス要素で比べる。
TEST(Static, ContainedComparesPathElements) {
    using hayate::detail::contained;
    EXPECT_TRUE(contained("/x/pub", "/x/pub"));
    EXPECT_TRUE(contained("/x/pub", "/x/pub/a"));
    EXPECT_FALSE(contained("/x/pub", "/x/public"));
    EXPECT_FALSE(contained("/x/pub", "/x/public/a"));
    EXPECT_FALSE(contained("/x/pub", "/x"));
}

// 前置一致で見ると、root `pub` の隣の `public` が root 内に見える。
TEST(Static, SiblingWithSharedPrefixIs404) {
    TempDir t;
    fs::create_directory(t.dir / "pub");
    fs::create_directory(t.dir / "public");
    write_text(t.dir / "pub" / "a.txt", "a");
    write_text(t.dir / "public" / "x.txt", "x");
    TestServer srv([&](hayate::App &app) {
        app.get("/assets/*path", hayate::files((t.dir / "pub").string()));
    });
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/a.txt").status, 200);
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/../public/x.txt");
    EXPECT_EQ(r.status, 404);
    EXPECT_NE(r.body, "x");
}

// %2F は区切りにならず、param は絶対パスになる。root を外れるので 404。
TEST(Static, EncodedAbsolutePathIs404) {
    TempDir root;
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/%2Fetc%2Fhosts");
    EXPECT_EQ(r.status, 404);
}

// ディレクトリに index.html を足した後の名前も、root 内か確かめ直す。
TEST(Static, IndexSymlinkEscapingRootIs404) {
    TempDir root;
    TempDir outside;
    write_text(outside.dir / "index.html", "secret");
    fs::create_directory(root.dir / "dir");
    fs::create_symlink(outside.dir / "index.html", root.dir / "dir" / "index.html");
    TestServer srv(
        [&](hayate::App &app) { app.get("/assets/*path", hayate::files(root.dir.string())); });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/dir");
    EXPECT_EQ(r.status, 404);
    EXPECT_NE(r.body, "secret");
}

// stat より縮んだら Content-Length を満たせない。ヘッダは送った後なので、閉じるしかない。
TEST(Static, ShrinkDuringSendClosesEarly) {
    TempDir root;
    write_text(root.dir / "a.bin", std::string(200 * 1024, 'a'));
    std::promise<void> in_gate;
    std::promise<void> release;
    auto gate_hit = std::make_shared<std::atomic<bool>>(false);
    TestServer srv([&](hayate::App &app) {
        app.use([&](hayate::Request &req,
                    hayate::Next next) -> boost::asio::awaitable<hayate::Response> {
            auto res = co_await next(req);
            // stat と open は済み、まだ送っていない。
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
        done.set_value(http_call("127.0.0.1", srv.port(), http::verb::get, "/assets/a.bin", {}, {},
                                 std::chrono::seconds(10)));
    });
    in_gate.get_future().wait();
    fs::resize_file(root.dir / "a.bin", 1000);
    release.set_value();
    auto r = done.get_future().get();
    caller.join();
    EXPECT_TRUE(r.error);
    EXPECT_FALSE(r.timed_out) << r.error_message;
}

// stat より伸びても、送るのは stat のサイズちょうど。keep-alive も続く。
TEST(Static, GrowDuringSendSendsStatSize) {
    TempDir root;
    const std::string original(100 * 1024, 'o');
    write_text(root.dir / "a.bin", original);
    std::promise<void> in_gate;
    std::promise<void> release;
    auto gate_hit = std::make_shared<std::atomic<bool>>(false);
    TestServer srv([&](hayate::App &app) {
        app.use([&](hayate::Request &req,
                    hayate::Next next) -> boost::asio::awaitable<hayate::Response> {
            auto res = co_await next(req);
            if (!gate_hit->exchange(true)) {
                in_gate.set_value();
                release.get_future().wait();
            }
            co_return res;
        });
        app.get("/assets/*path", hayate::files(root.dir.string()));
    });
    struct Result {
        std::string first;
        unsigned second_status{};
    };
    std::promise<Result> done;
    std::thread caller([&] {
        Result out;
        Conn c(srv.port(), std::chrono::seconds(10));
        auto req = [](bool keep) {
            http::request<http::string_body> r{http::verb::get, "/assets/a.bin", 11};
            r.set(http::field::host, "127.0.0.1");
            r.keep_alive(keep);
            return r;
        };
        http::response<http::string_body> first;
        http::response<http::string_body> second;
        if (!c.connect_error() && !c.write(req(true)) && !c.read(first, std::chrono::seconds(10))) {
            out.first = first.body();
            if (!c.write(req(false)) && !c.read(second, std::chrono::seconds(10))) {
                out.second_status = second.result_int();
            }
        }
        done.set_value(std::move(out));
    });
    in_gate.get_future().wait();
    {
        std::ofstream out(root.dir / "a.bin", std::ios::binary | std::ios::app);
        out << std::string(50 * 1024, 'g');
    }
    release.set_value();
    auto r = done.get_future().get();
    caller.join();
    EXPECT_EQ(r.first.size(), original.size());
    EXPECT_EQ(r.first, original);
    EXPECT_EQ(r.second_status, 200u);
}

// 本体は 64 KiB ずつ読んで送る（SPEC「静的ファイル」）。全部読んでから送る実装は、
// pipe の 2 チャンク目を待って何も送らない。1 チャンク目を書いた時点で本文が届くこと。
TEST(Static, StreamsBeforeFileEnds) {
    constexpr std::size_t kChunk = 64 * 1024;
    std::array<int, 2> fds{};
    ASSERT_EQ(::pipe(fds.data()), 0);
    const int read_fd = fds[0];
    TestServer srv([&](hayate::App &app) {
        // プールは App だけが持つ（files() と同じ）。io_context より先に join される。
        auto pool = std::make_shared<boost::asio::thread_pool>(1);
        app.get("/stream", [read_fd, pool](hayate::Request &) {
            return pipe_response(read_fd, 3 * kChunk, pool);
        });
    });
    PipeEnd writer{fds[1]};

    Conn c(srv.port());
    ASSERT_FALSE(c.connect_error());
    http::request<http::string_body> req{http::verb::get, "/stream", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(false);
    ASSERT_FALSE(c.write(req));
    // pipe の容量は 1 チャンクより小さいことがある。ワーカーが読み切るまでここで待つ。
    const std::string first(kChunk, 'h');
    ASSERT_EQ(::write(writer.fd, first.data(), first.size()), static_cast<ssize_t>(first.size()));

    http::response_parser<http::string_body> parser;
    parser.body_limit(4 * kChunk);
    boost::beast::flat_buffer buf;
    const auto ec =
        c.run(std::chrono::seconds(5), [&]() -> boost::asio::awaitable<boost::system::error_code> {
            while (!parser.is_done() && parser.get().body().empty()) {
                auto [rec, n] =
                    co_await http::async_read_some(c.stream(), buf, parser, boost::asio::as_tuple);
                (void)n;
                if (rec) {
                    co_return rec;
                }
            }
            co_return boost::system::error_code{};
        });
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(parser.get().result_int(), 200);
    EXPECT_FALSE(parser.is_done());
    ASSERT_FALSE(parser.get().body().empty());
    EXPECT_EQ(parser.get().body().front(), 'h');
    writer.close();
}

// 送出中のチャンク読みが read_timeout を超えたら閉じる。ヘッダは送った後なので直せない。
TEST(Static, ChunkReadDeadlineCloses) {
    constexpr std::size_t kChunk = 64 * 1024;
    std::array<int, 2> fds{};
    ASSERT_EQ(::pipe(fds.data()), 0);
    const int read_fd = fds[0];
    TestServer srv([&](hayate::App &app) {
        app.limits().read_timeout = std::chrono::milliseconds(200);
        auto pool = std::make_shared<boost::asio::thread_pool>(1);
        app.get("/stream", [read_fd, pool](hayate::Request &) {
            return pipe_response(read_fd, 2 * kChunk, pool);
        });
    });
    PipeEnd writer{fds[1]};
    Conn c(srv.port());
    ASSERT_FALSE(c.connect_error());
    http::request<http::string_body> req{http::verb::get, "/stream", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(false);
    ASSERT_FALSE(c.write(req));
    // 1 チャンク目だけ書く。pipe が詰まるので、ワーカーが読むまで別スレッドで待つ。
    std::thread feed([fd = writer.fd] {
        const std::string first(kChunk, 'f');
        (void)::write(fd, first.data(), first.size());
    });
    http::response_parser<http::string_body> parser;
    parser.body_limit(4 * kChunk);
    // ヘッダと 1 チャンク目は届き、2 チャンク目の前に EOF。期限切れ（固まった）ではない。
    const auto ec = c.read(parser, std::chrono::seconds(5));
    feed.join();
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, boost::beast::error::timeout) << ec.message();
    EXPECT_TRUE(parser.is_header_done());
    EXPECT_EQ(parser.get().result_int(), 200);
    EXPECT_FALSE(parser.is_done());
    writer.close();
}
