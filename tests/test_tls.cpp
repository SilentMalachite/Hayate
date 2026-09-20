#include "http_client.hpp"
#include "https_client.hpp"
#include "test_cert.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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
    namespace net = boost::asio;
    namespace beast = boost::beast;
    net::io_context ioc;
    auto ctx = test_client_ctx();
    beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
    beast::get_lowest_layer(stream).connect(
        net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    stream.handshake(net::ssl::stream_base::client);
    beast::flat_buffer buf;
    auto send = [&] {
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(5));
        http::request<http::string_body> req{http::verb::get, "/", 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::write(stream, req);
        http::response<http::string_body> res;
        http::read(stream, buf, res);
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
    const auto dir = fs::temp_directory_path() / "hayate_tls_static";
    fs::create_directories(dir);
    const std::string want(200 * 1024, 'z');
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
    EXPECT_EQ(r.extra["Content-Length"], std::to_string(want.size()));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
