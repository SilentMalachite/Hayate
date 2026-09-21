#include "http_client.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>
#include <hayate/hayate.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace http = boost::beast::http;

TEST(Mw, OnionOrder) {
    std::string trace;
    TestServer srv([&](hayate::App &app) {
        app.use([&](hayate::Request &req, hayate::Next next) -> asio::awaitable<hayate::Response> {
            trace += "A>";
            auto res = co_await next(req);
            trace += "<A";
            co_return res;
        });
        app.use([&](hayate::Request &req, hayate::Next next) -> asio::awaitable<hayate::Response> {
            trace += "B>";
            auto res = co_await next(req);
            trace += "<B";
            co_return res;
        });
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(trace, "A>B><B<A");
}

TEST(Mw, ShortCircuitSkipsLater) {
    std::string trace;
    TestServer srv([&](hayate::App &app) {
        app.use([&](hayate::Request &, hayate::Next) -> asio::awaitable<hayate::Response> {
            trace += "A";
            co_return hayate::Response::text("short");
        });
        app.use([&](hayate::Request &req, hayate::Next next) -> asio::awaitable<hayate::Response> {
            trace += "B";
            co_return co_await next(req);
        });
        app.get("/", [&](hayate::Request &) {
            trace += "H";
            return hayate::Response::text("h");
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(r.body, "short");
    EXPECT_EQ(trace, "A");
}

TEST(Mw, InvokedOn404) {
    bool entered = false;
    TestServer srv([&](hayate::App &app) {
        app.use([&](hayate::Request &req, hayate::Next next) -> asio::awaitable<hayate::Response> {
            entered = true;
            co_return co_await next(req);
        });
        app.get("/x", [](hayate::Request &) { return hayate::Response::text("x"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/missing");
    EXPECT_EQ(r.status, 404);
    EXPECT_TRUE(entered);
}

TEST(Mw, ExtensionRoundtrip) {
    TestServer srv([](hayate::App &app) {
        app.use([](hayate::Request &req, hayate::Next next) -> asio::awaitable<hayate::Response> {
            req.set<std::string>("rid-1");
            co_return co_await next(req);
        });
        app.get("/", [](hayate::Request &req) {
            auto *id = req.get<std::string>();
            return hayate::Response::text(id ? *id : "none");
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(r.body, "rid-1");
}

TEST(Mw, ThrowingMiddlewareIs500) {
    TestServer srv([](hayate::App &app) {
        app.use([](hayate::Request &, hayate::Next) -> asio::awaitable<hayate::Response> {
            throw std::runtime_error("boom");
            co_return hayate::Response::text("unreachable");
        });
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_FALSE(r.error);
    EXPECT_EQ(r.status, 500);
}

TEST(Mw, ThrowingAfterNextIs500) {
    TestServer srv([](hayate::App &app) {
        app.use([](hayate::Request &req, hayate::Next next) -> asio::awaitable<hayate::Response> {
            co_await next(req);
            throw std::runtime_error("boom");
        });
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_FALSE(r.error);
    EXPECT_EQ(r.status, 500);
}

TEST(Mw, ThrowingHandlerIs500) {
    TestServer srv([](hayate::App &app) {
        app.get("/",
                [](hayate::Request &) -> hayate::Response { throw std::runtime_error("boom"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_FALSE(r.error);
    EXPECT_EQ(r.status, 500);
}

namespace {

// dispatch は要求ごとに MW を合成し直し、そこでコピーする。立てた後のコピーで投げる。
struct ThrowOnCopy {
    std::shared_ptr<std::atomic<bool>> armed;

    explicit ThrowOnCopy(std::shared_ptr<std::atomic<bool>> a) : armed(std::move(a)) {}
    ThrowOnCopy(const ThrowOnCopy &o) : armed(o.armed) {
        if (armed->load()) {
            throw std::runtime_error("copy");
        }
    }
    ThrowOnCopy(ThrowOnCopy &&) = default;

    asio::awaitable<hayate::Response> operator()(hayate::Request &req, hayate::Next next) const {
        co_return co_await next(req);
    }
};

} // namespace

TEST(Mw, CopyThrowIs500) {
    auto armed = std::make_shared<std::atomic<bool>>(false);
    TestServer srv([&](hayate::App &app) {
        app.use(ThrowOnCopy{armed});
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    armed->store(true);
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_FALSE(r.error);
    EXPECT_EQ(r.status, 500);
}
