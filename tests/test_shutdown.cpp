#include "conn_client.hpp"
#include "http_client.hpp"
#include "test_server.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace http = boost::beast::http;
namespace net = boost::asio;

namespace {

// サーバー側の期限は client の期限（5 s）よりずっと長くし、閉じられたら stop()
// のせいだと分かるようにする。
void long_timeouts(hayate::App &app) {
    app.limits().read_timeout = std::chrono::seconds(30);
    app.limits().idle_timeout = std::chrono::seconds(30);
    app.limits().write_timeout = std::chrono::seconds(30);
}

hayate::Response ok(hayate::Request &) { return hayate::Response::text("ok"); }

// client の期限切れではなく、サーバーが閉じたこと。
void expect_closed_by_server(Conn &c) {
    http::response<http::string_body> res;
    const auto ec = c.read(res, std::chrono::seconds(5));
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, boost::beast::error::timeout) << "connection outlived stop()";
}

// backlog は来た順に accept される。後から来た要求が通ったなら、先の接続も accept 済み。
void wait_accepted(std::uint16_t port) {
    ASSERT_EQ(http_call("127.0.0.1", port, http::verb::get, "/").status, 200);
}

} // namespace

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
    // stop() は admin strand に post するだけ。後から来た accept の完了はその後ろに並ぶので、
    // 拒否か応答なしの EOF のどちらかになる。期限切れは「固まった」なので拒否と数えない。
    auto refused = http_call("127.0.0.1", srv.port(), http::verb::get, "/slow", {}, {},
                             std::chrono::seconds(2));
    EXPECT_TRUE(refused.error);
    EXPECT_FALSE(refused.timed_out) << refused.error_message;
    EXPECT_NE(refused.status, 200u);
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
    Conn c(srv.port());
    ASSERT_FALSE(c.connect_error());
    auto one = [&](std::string target) {
        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::response<http::string_body> res;
        if (c.write(req) || c.read(res)) {
            return std::string("<failed>");
        }
        return res.body();
    };
    EXPECT_EQ(one("/a"), "a");
    EXPECT_EQ(one("/b"), "b");
}

// 要求を待っているだけの接続は、stop() ですぐ閉じる。idle_timeout まで serve() を止めない。
TEST(Shutdown, ClosesIdleKeepAlive) {
    TestServer srv([](hayate::App &app) {
        long_timeouts(app);
        app.get("/", ok);
    });
    Conn c(srv.port());
    http::request<http::string_body> req{http::verb::get, "/", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(true);
    ASSERT_FALSE(c.write(req));
    http::response<http::string_body> res;
    ASSERT_FALSE(c.read(res));
    ASSERT_TRUE(res.keep_alive());
    srv.app().stop();
    expect_closed_by_server(c);
}

TEST(Shutdown, ClosesBeforeFirstRequest) {
    TestServer srv([](hayate::App &app) {
        long_timeouts(app);
        app.get("/", ok);
    });
    Conn c(srv.port());
    ASSERT_FALSE(c.connect_error());
    wait_accepted(srv.port());
    srv.app().stop();
    expect_closed_by_server(c);
}

// ヘッダが途中までなら、要求はまだ完成していない。待たずに閉じる。
TEST(Shutdown, ClosesPartialHeader) {
    TestServer srv([](hayate::App &app) {
        long_timeouts(app);
        app.get("/", ok);
    });
    Conn c(srv.port());
    ASSERT_FALSE(c.write_raw("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"));
    wait_accepted(srv.port());
    srv.app().stop();
    expect_closed_by_server(c);
}

// ヘッダが揃った要求は完了させ、停止中なので Connection: close を付けて閉じる。
TEST(Shutdown, InFlightResponseSaysClose) {
    std::promise<void> entered;
    std::atomic<bool> release{false};
    TestServer srv([&](hayate::App &app) {
        long_timeouts(app);
        app.get("/slow", [&](hayate::Request &) -> net::awaitable<hayate::Response> {
            entered.set_value();
            // io スレッドを塞がずに待つ。塞ぐと stop() が admin strand で処理されない。
            net::steady_timer t(co_await net::this_coro::executor);
            while (!release.load()) {
                t.expires_after(std::chrono::milliseconds(1));
                co_await t.async_wait(net::use_awaitable);
            }
            co_return hayate::Response::text("done");
        });
    });
    Conn c(srv.port(), std::chrono::seconds(5));
    http::request<http::string_body> req{http::verb::get, "/slow", 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(true);
    ASSERT_FALSE(c.write(req));
    entered.get_future().wait();
    srv.app().stop();
    // shutting は acceptor を閉じる前に、同じ admin strand の処理で立つ。拒否されたら立っている。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool refused = false;
    while (!refused && std::chrono::steady_clock::now() < deadline) {
        Conn probe(srv.port(), std::chrono::milliseconds(200));
        refused = static_cast<bool>(probe.connect_error());
    }
    ASSERT_TRUE(refused);
    release = true;
    http::response<http::string_body> res;
    ASSERT_FALSE(c.read(res, std::chrono::seconds(5)));
    EXPECT_EQ(res.body(), "done");
    EXPECT_FALSE(res.keep_alive());
    expect_closed_by_server(c);
}
