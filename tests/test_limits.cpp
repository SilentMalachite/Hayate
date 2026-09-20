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
