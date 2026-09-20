#include "http_client.hpp"
#include "test_server.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <future>
#include <thread>

namespace http = boost::beast::http;
namespace net = boost::asio;

TEST(Shutdown, FinishesInFlightAndRefusesNew) {
    std::promise<void> in_handler;
    std::promise<void> release;
    TestServer srv([&](hayate::App &app) {
        app.threads(2);
        app.get("/slow", [&](hayate::Request &) {
            in_handler.set_value();
            release.get_future().wait();
            return hayate::Response::text("done");
        });
    });
    std::promise<HttpCall> slow_done;
    std::thread slow([&] {
        slow_done.set_value(http_call("127.0.0.1", srv.port(), http::verb::get, "/slow", {}, {},
                                      std::chrono::seconds(5)));
    });
    in_handler.get_future().wait();
    srv.app().stop();
    auto refused = http_call("127.0.0.1", srv.port(), http::verb::get, "/slow", {}, {},
                             std::chrono::milliseconds(200));
    EXPECT_TRUE(refused.error || refused.status == 0);
    release.set_value();
    auto finished = slow_done.get_future().get();
    EXPECT_EQ(finished.status, 200);
    EXPECT_EQ(finished.body, "done");
    slow.join();
}

TEST(Conn, KeepAliveTwoRequests) {
    TestServer srv([](hayate::App &app) {
        app.get("/a", [](hayate::Request &) { return hayate::Response::text("a"); });
        app.get("/b", [](hayate::Request &) { return hayate::Response::text("b"); });
    });
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    auto one = [&](std::string target) {
        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::write(stream, req);
        boost::beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        return res.body();
    };
    EXPECT_EQ(one("/a"), "a");
    EXPECT_EQ(one("/b"), "b");
}
