#include "conn_client.hpp"
#include "http_client.hpp"
#include "https_client.hpp"
#include "temp_dir.hpp"
#include "test_cert.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace http = boost::beast::http;
namespace fs = std::filesystem;

TEST(Tls, HandshakeAndGet) {
    TempCert cert;
    TestServer srv([&](hayate::App &app) {
        app.tls({.cert_file = cert.cert().string(), .key_file = cert.key().string()});
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = https_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(r.status, 200) << r.error_message;
    EXPECT_EQ(r.body, "ok");
}

TEST(Tls, KeepAliveOverTls) {
    TempCert cert;
    TestServer srv([&](hayate::App &app) {
        app.tls({.cert_file = cert.cert().string(), .key_file = cert.key().string()});
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto ctx = test_client_ctx();
    TlsConn c(srv.port(), ctx, std::chrono::seconds(5));
    ASSERT_FALSE(c.connect_error()) << c.connect_error().message();
    auto send = [&]() -> std::string {
        http::request<http::string_body> req{http::verb::get, "/", 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::response<http::string_body> res;
        if (c.write(req, std::chrono::seconds(5)) || c.read(res, std::chrono::seconds(5))) {
            return {};
        }
        return res.body();
    };
    EXPECT_EQ(send(), "ok");
    EXPECT_EQ(send(), "ok");
}

TEST(Tls, PlainTextClientIsRejected) {
    TempCert cert;
    TestServer srv([&](hayate::App &app) {
        app.tls({.cert_file = cert.cert().string(), .key_file = cert.key().string()});
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    // 平文の要求は ClientHello として壊れている。応答は返らない。
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(r.status, 0u);
    EXPECT_TRUE(static_cast<bool>(r.error));
}

TEST(Tls, MissingCertFileThrows) {
    hayate::App app;
    EXPECT_THROW(
        app.tls({.cert_file = "/nonexistent/server.pem", .key_file = "/nonexistent/server.key"}),
        std::exception);
}

TEST(Tls, StaticFileOverTls) {
    TempCert cert;
    TempDir tmp;
    const auto &dir = tmp.dir;
    // 一様な中身だと、チャンクの取り違えや重複を見逃す。
    std::string want;
    want.reserve(200 * 1024);
    for (std::size_t i = 0; want.size() < 200 * 1024; ++i) {
        want += "hayate-" + std::to_string(i) + "\n";
    }
    {
        std::ofstream out(dir / "big.bin", std::ios::binary);
        out << want;
    }
    TestServer srv([&](hayate::App &app) {
        app.tls({.cert_file = cert.cert().string(), .key_file = cert.key().string()});
        app.get("/assets/*path", hayate::files(dir.string()));
    });
    auto r = https_call("127.0.0.1", srv.port(), http::verb::get, "/assets/big.bin",
                        std::chrono::seconds(10), {"Content-Length"});
    EXPECT_EQ(r.status, 200) << r.error_message;
    EXPECT_EQ(r.body.size(), want.size());
    EXPECT_EQ(r.body, want);
    EXPECT_EQ(r.extra["Content-Length"], std::to_string(want.size()));
}

namespace {

namespace net = boost::asio;
namespace beast = boost::beast;
using TlsStream = beast::ssl_stream<beast::tcp_stream>;

// 非同期で回す。Beast の期限は非同期 I/O にしか効かない。
template <typename Op> boost::system::error_code run_async(net::io_context &ioc, Op op) {
    boost::system::error_code out;
    net::co_spawn(ioc, [&]() -> net::awaitable<void> { out = co_await op(); }, net::detached);
    ioc.restart();
    ioc.run();
    return out;
}

boost::system::error_code connect_and_handshake(net::io_context &ioc, TlsStream &s,
                                                std::uint16_t port) {
    return run_async(ioc, [&]() -> net::awaitable<boost::system::error_code> {
        auto &tcp = beast::get_lowest_layer(s);
        tcp.expires_after(std::chrono::seconds(5));
        const net::ip::tcp::endpoint ep(net::ip::make_address("127.0.0.1"), port);
        auto [cec] = co_await tcp.async_connect(ep, net::as_tuple);
        if (cec) {
            co_return cec;
        }
        auto [hec] = co_await s.async_handshake(net::ssl::stream_base::client, net::as_tuple);
        co_return hec;
    });
}

void serve_tls(hayate::App &app, const TempCert &cert) {
    app.tls({.cert_file = cert.cert().string(), .key_file = cert.key().string()});
    app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
}

} // namespace

// 対でない証明書と鍵は、設定時に落とす。
TEST(Tls, MismatchedKeyThrows) {
    TempCert a;
    TempCert b;
    hayate::App app;
    EXPECT_THROW(app.tls({.cert_file = a.cert().string(), .key_file = b.key().string()}),
                 std::exception);
}

// OpenSSL は鍵を種類ごとのスロットに入れるので、種類が違うと読み込みでは照合されない。
TEST(Tls, MismatchedKeyTypeThrows) {
    TempCert ec;
    TempCert rsa(TempCert::Key::rsa);
    hayate::App app;
    EXPECT_THROW(app.tls({.cert_file = ec.cert().string(), .key_file = rsa.key().string()}),
                 std::exception);
}

// ClientHello を送らない相手は、ハンドシェイクの窓（read_timeout）で切る。
TEST(Tls, HandshakeTimeoutCloses) {
    TempCert cert;
    TestServer srv([&](hayate::App &app) {
        app.limits().read_timeout = std::chrono::milliseconds(200);
        serve_tls(app, cert);
    });
    Conn c(srv.port());
    ASSERT_FALSE(c.connect_error());
    std::array<char, 16> sink{};
    const auto ec =
        c.run(std::chrono::seconds(5), [&]() -> net::awaitable<boost::system::error_code> {
            auto [rec, n] = co_await c.stream().async_read_some(net::buffer(sink), net::as_tuple);
            (void)n;
            co_return rec;
        });
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, beast::error::timeout) << ec.message();
}

// close_notify を返さない相手を待ち続けない。待つのは write_timeout まで。
TEST(Tls, ShutdownTimeoutCloses) {
    TempCert cert;
    TestServer srv([&](hayate::App &app) {
        app.limits().write_timeout = std::chrono::milliseconds(200);
        serve_tls(app, cert);
    });
    net::io_context ioc;
    auto ctx = test_client_ctx();
    TlsStream s{ioc, ctx};
    auto ec = connect_and_handshake(ioc, s, srv.port());
    ASSERT_FALSE(ec) << ec.message();
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(false);
    http::response<http::string_body> res;
    beast::flat_buffer buf;
    ec = run_async(ioc, [&]() -> net::awaitable<boost::system::error_code> {
        beast::get_lowest_layer(s).expires_after(std::chrono::seconds(5));
        auto [wec, wn] = co_await http::async_write(s, req, net::as_tuple);
        (void)wn;
        if (wec) {
            co_return wec;
        }
        auto [rec, rn] = co_await http::async_read(s, buf, res, net::as_tuple);
        (void)rn;
        co_return rec;
    });
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(res.body(), "ok");
    // TLS 層を通さずに読み、こちらの close_notify は送らない。窓が切れれば EOF。
    std::array<char, 1024> sink{};
    ec = run_async(ioc, [&]() -> net::awaitable<boost::system::error_code> {
        auto &tcp = beast::get_lowest_layer(s);
        tcp.expires_after(std::chrono::seconds(5));
        for (;;) {
            auto [rec, n] = co_await tcp.async_read_some(net::buffer(sink), net::as_tuple);
            (void)n;
            if (rec) {
                co_return rec;
            }
        }
    });
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, beast::error::timeout) << ec.message();
}

// 最低 TLS 1.2。1.2 までしか話さない client は繋がる。
TEST(Tls, Tls12ClientConnects) {
    TempCert cert;
    TestServer srv([&](hayate::App &app) { serve_tls(app, cert); });
    net::io_context ioc;
    auto ctx = test_client_ctx();
    ASSERT_EQ(SSL_CTX_set_max_proto_version(ctx.native_handle(), TLS1_2_VERSION), 1);
    TlsStream s{ioc, ctx};
    const auto ec = connect_and_handshake(ioc, s, srv.port());
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(SSL_version(s.native_handle()), TLS1_2_VERSION);
}

// 1.1 までしか話さない client は断る。OpenSSL 3 は既定の security level でも 1.1 を断るので、
// これは SPEC の約束の確認で、no_tlsv1_1 を外したことまでは検出できない。
TEST(Tls, Tls11ClientIsRejected) {
    TempCert cert;
    TestServer srv([&](hayate::App &app) { serve_tls(app, cert); });
    net::io_context ioc;
    auto ctx = test_client_ctx();
    SSL_CTX_set_security_level(ctx.native_handle(), 0);
    ASSERT_EQ(SSL_CTX_set_cipher_list(ctx.native_handle(), "DEFAULT@SECLEVEL=0"), 1);
    ASSERT_EQ(SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_VERSION), 1);
    ASSERT_EQ(SSL_CTX_set_max_proto_version(ctx.native_handle(), TLS1_1_VERSION), 1);
    TlsStream s{ioc, ctx};
    const auto ec = connect_and_handshake(ioc, s, srv.port());
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, beast::error::timeout) << ec.message();
}

// ハンドシェイクを始めない接続も、stop() ですぐ閉じる。read_timeout まで待たない。
TEST(Tls, StopClosesPendingHandshake) {
    TempCert cert;
    TestServer srv([&](hayate::App &app) {
        app.tls({.cert_file = cert.cert().string(), .key_file = cert.key().string()});
        app.limits().read_timeout = std::chrono::seconds(30);
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    Conn c(srv.port());
    ASSERT_FALSE(c.connect_error());
    // backlog は来た順に accept される。後の要求が通れば、c も accept 済み。
    ASSERT_EQ(https_call("127.0.0.1", srv.port(), http::verb::get, "/").status, 200);
    srv.app().stop();
    http::response<http::string_body> res;
    const auto ec = c.read(res, std::chrono::seconds(5));
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, boost::beast::error::timeout);
}

// 停止中は close_notify を送るだけで、相手の返事を待たない。待つと write_timeout まで
// serve() が戻らない。
TEST(Tls, StopClosesIdleKeepAlive) {
    TempCert cert;
    auto srv = std::make_unique<TestServer>([&](hayate::App &app) {
        app.tls({.cert_file = cert.cert().string(), .key_file = cert.key().string()});
        app.limits().idle_timeout = std::chrono::seconds(30);
        app.limits().write_timeout = std::chrono::seconds(30);
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto ctx = test_client_ctx();
    TlsConn c(srv->port(), ctx, std::chrono::seconds(5));
    ASSERT_FALSE(c.connect_error()) << c.connect_error().message();
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(true);
    ASSERT_FALSE(c.write(req));
    http::response<http::string_body> res;
    ASSERT_FALSE(c.read(res));
    srv->app().stop();
    http::response<http::string_body> after;
    // close_notify を受けたなら end_of_stream。無しで切られたら stream_truncated になる。
    EXPECT_EQ(c.read(after, std::chrono::seconds(5)), http::error::end_of_stream);
    // c は close_notify を返さないまま開いている。それでも serve() はすぐ戻る。
    const auto t0 = std::chrono::steady_clock::now();
    srv.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
}
