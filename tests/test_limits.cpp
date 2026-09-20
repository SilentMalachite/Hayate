#include "http_client.hpp"
#include "test_server.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>

namespace http = boost::beast::http;
namespace net = boost::asio;

TEST(Limits, OversizeHeaderIs431) {
    TestServer srv([](hayate::App &app) {
        app.limits().max_header_bytes = 64;
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set("X-Big", std::string(200, 'a'));
    http::write(stream, req);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    boost::system::error_code ec;
    http::read(stream, buf, res, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(res.result_int(), 431);
}

TEST(Limits, OversizeBodyIs413) {
    TestServer srv([](hayate::App &app) {
        app.limits().max_body_bytes = 8;
        app.post("/x", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::post, "/x", "0123456789", "text/plain");
    EXPECT_EQ(r.status, 413);
}

TEST(Limits, ReadTimeoutCloses) {
    TestServer srv([](hayate::App &app) {
        app.limits().read_timeout = std::chrono::milliseconds(50);
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    const char *partial = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    net::write(stream.socket(), net::buffer(partial, std::strlen(partial)));
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    boost::system::error_code ec;
    http::read(stream, buf, res, ec);
    EXPECT_TRUE(ec);
}

TEST(Limits, IdleTimeoutClosesKeepAlive) {
    TestServer srv([](hayate::App &app) {
        app.limits().idle_timeout = std::chrono::milliseconds(50);
        app.limits().read_timeout = std::chrono::seconds(5);
        app.get("/a", [](hayate::Request &) { return hayate::Response::text("a"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    http::request<http::string_body> req{http::verb::get, "/a", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(true);
    http::write(stream, req);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);
    EXPECT_EQ(res.result_int(), 200);
    EXPECT_EQ(res.body(), "a");
    stream.expires_after(std::chrono::seconds(8));
    http::response<http::string_body> idle;
    boost::system::error_code ec;
    auto t0 = std::chrono::steady_clock::now();
    http::read(stream, buf, idle, ec);
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_TRUE(ec);
    EXPECT_LT(ms, 1000) << "idle timeout must fire before read_timeout";
}

TEST(Limits, MaxConnectionsRefusesNew) {
    TestServer srv([](hayate::App &app) {
        app.limits().max_connections = 1;
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream hold(ioc);
    hold.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    boost::beast::tcp_stream extra(ioc);
    extra.expires_after(std::chrono::seconds(1));
    boost::system::error_code ec;
    extra.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()), ec);
    extra.socket().write_some(net::buffer(std::string("GET / HTTP/1.1\r\nHost: x\r\n\r\n")), ec);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(extra, buf, res, ec);
    EXPECT_TRUE(ec);
}

TEST(Limits, ReadTimeoutAppliesToKeepAliveRequests) {
    TestServer srv([](hayate::App &app) {
        app.limits().read_timeout = std::chrono::milliseconds(50);
        app.limits().idle_timeout = std::chrono::seconds(5);
        app.get("/a", [](hayate::Request &) { return hayate::Response::text("a"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    http::request<http::string_body> req{http::verb::get, "/a", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(true);
    http::write(stream, req);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);
    ASSERT_EQ(res.result_int(), 200);

    // 2 本目を途中まで送る。窓は read_timeout であって idle_timeout ではない。
    const char *partial = "POST /a HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10\r\n\r\nab";
    net::write(stream.socket(), net::buffer(partial, std::strlen(partial)));
    stream.expires_after(std::chrono::seconds(8));
    http::response<http::string_body> second;
    boost::system::error_code ec;
    auto t0 = std::chrono::steady_clock::now();
    http::read(stream, buf, second, ec);
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_TRUE(ec);
    EXPECT_LT(ms, 1000) << "read timeout must govern the body of a keep-alive request";
}
