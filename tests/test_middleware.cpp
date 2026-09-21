#include "conn_client.hpp"
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

namespace {

http::request<http::string_body> get_req(std::string target, bool keep) {
    http::request<http::string_body> req{http::verb::get, std::move(target), 11};
    req.set(http::field::host, "127.0.0.1");
    req.keep_alive(keep);
    return req;
}

} // namespace

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

// group の MW はマッチしたルートにだけ付く。隣の group とトップレベルへは漏れない。
TEST(Mw, GroupMwSkipsSiblingGroup) {
    std::string trace;
    TestServer srv([&](hayate::App &app) {
        auto mark = [&trace](std::string tag) {
            return [&trace, tag](hayate::Request &req,
                                 hayate::Next next) -> asio::awaitable<hayate::Response> {
                trace += tag;
                co_return co_await next(req);
            };
        };
        auto ok = [](hayate::Request &) { return hayate::Response::text("ok"); };
        app.group("/a", [&](hayate::Router &r) {
            r.use(mark("A"));
            r.get("/x", ok);
        });
        app.group("/b", [&](hayate::Router &r) {
            r.use(mark("B"));
            r.get("/x", ok);
        });
        app.get("/top", ok);
    });
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/b/x").status, 200);
    EXPECT_EQ(trace, "B");
    trace.clear();
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/top").status, 200);
    EXPECT_EQ(trace, "");
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/a/x").status, 200);
    EXPECT_EQ(trace, "A");
}

namespace {

// 通った MW の印を trace に足す。
auto marker(std::string &trace) {
    return [&trace](std::string tag) {
        return [&trace, tag](hayate::Request &req,
                             hayate::Next next) -> asio::awaitable<hayate::Response> {
            trace += tag;
            co_return co_await next(req);
        };
    };
}

} // namespace

// group のパスの 404 / 405 はどのルートにもマッチしていない。通るのは App の MW だけ。
TEST(Mw, GroupMwSkipsGroup404And405) {
    std::string trace;
    TestServer srv([&](hayate::App &app) {
        auto mark = marker(trace);
        app.use(mark("P"));
        app.group("/a", [&](hayate::Router &r) {
            r.use(mark("A"));
            r.get("/x", [](hayate::Request &) { return hayate::Response::text("ok"); });
        });
    });
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::post, "/a/x").status, 405);
    EXPECT_EQ(trace, "P");
    trace.clear();
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/a/nope").status, 404);
    EXPECT_EQ(trace, "P");
}

TEST(Mw, NestedGroupOrder) {
    std::string trace;
    TestServer srv([&](hayate::App &app) {
        auto mark = marker(trace);
        app.use(mark("P"));
        app.group("/o", [&](hayate::Router &r) {
            r.use(mark("O"));
            r.group("/i", [&](hayate::Router &rr) {
                rr.use(mark("I"));
                rr.get("/x", [&trace](hayate::Request &) {
                    trace += "H";
                    return hayate::Response::text("ok");
                });
            });
        });
    });
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/o/i/x").status, 200);
    EXPECT_EQ(trace, "POIH");
}

// use() は呼んだ位置に関係なく、その App / Router の全ルートに付く。
TEST(Mw, UseAfterRouteApplies) {
    std::string trace;
    TestServer srv([&](hayate::App &app) {
        auto mark = marker(trace);
        auto ok = [](hayate::Request &) { return hayate::Response::text("ok"); };
        app.get("/x", ok);
        app.group("/g", [&](hayate::Router &r) {
            r.get("/y", ok);
            r.use(mark("G"));
        });
        app.use(mark("P"));
    });
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/x").status, 200);
    EXPECT_EQ(trace, "P");
    trace.clear();
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/g/y").status, 200);
    EXPECT_EQ(trace, "PG");
}

// 例外を 500 にした後も接続は閉じない。同じ接続の次の要求が通る。
TEST(Mw, KeepAliveAfter500) {
    TestServer srv([](hayate::App &app) {
        app.get("/boom",
                [](hayate::Request &) -> hayate::Response { throw std::runtime_error("boom"); });
        app.get("/ok", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    Conn c(srv.port());
    ASSERT_FALSE(c.connect_error());
    ASSERT_FALSE(c.write(get_req("/boom", true)));
    http::response<http::string_body> first;
    ASSERT_FALSE(c.read(first));
    EXPECT_EQ(first.result_int(), 500);
    EXPECT_TRUE(first.keep_alive());

    ASSERT_FALSE(c.write(get_req("/ok", false)));
    http::response<http::string_body> second;
    const auto ec = c.read(second);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(second.result_int(), 200);
    EXPECT_EQ(second.body(), "ok");
}

// 同じ T の set は上書き。
TEST(Request, ExtensionOverwrite) {
    hayate::Request req;
    req.set<std::string>("first");
    req.set<std::string>("second");
    ASSERT_NE(req.get<std::string>(), nullptr);
    EXPECT_EQ(*req.get<std::string>(), "second");
}

// Extension の寿命は Request。keep-alive の次の要求には残らない。
TEST(Mw, ExtensionDoesNotLeakAcrossRequests) {
    TestServer srv([](hayate::App &app) {
        app.use([](hayate::Request &req, hayate::Next next) -> asio::awaitable<hayate::Response> {
            if (const auto v = req.header("X-Set"); !v.empty()) {
                req.set<std::string>(std::string(v));
            }
            co_return co_await next(req);
        });
        app.get("/", [](hayate::Request &req) {
            auto *v = req.get<std::string>();
            return hayate::Response::text(v ? *v : "none");
        });
    });
    Conn c(srv.port());
    ASSERT_FALSE(c.connect_error());
    auto first_req = get_req("/", true);
    first_req.set("X-Set", "one");
    ASSERT_FALSE(c.write(first_req));
    http::response<http::string_body> first;
    ASSERT_FALSE(c.read(first));
    EXPECT_EQ(first.body(), "one");

    ASSERT_FALSE(c.write(get_req("/", false)));
    http::response<http::string_body> second;
    ASSERT_FALSE(c.read(second));
    EXPECT_EQ(second.body(), "none");
}
