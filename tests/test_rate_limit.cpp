#include "conn_client.hpp"
#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>

namespace http = boost::beast::http;

TEST(RateLimit, AllowsUnderLimit) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::rate_limit({.max = 10, .window = std::chrono::seconds(60)}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto a = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto b = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(a.status, 200) << a.error_message;
    EXPECT_EQ(b.status, 200) << b.error_message;
}

TEST(RateLimit, OverLimitIs429) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::rate_limit({.max = 2, .window = std::chrono::seconds(60)}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto a = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto b = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto c = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(a.status, 200) << a.error_message;
    EXPECT_EQ(b.status, 200) << b.error_message;
    EXPECT_EQ(c.status, 429) << c.error_message;
    EXPECT_FALSE(c.retry_after.empty());
}

TEST(RateLimit, PeerIsLoopback) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.peer()));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    EXPECT_EQ(r.status, 200) << r.error_message;
    EXPECT_TRUE(r.body.find("127.0.0.1") != std::string::npos ||
                r.body.find("::1") != std::string::npos)
        << r.body;
}

TEST(RateLimit, KeepAliveRequestsShareOnePeerKey) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::rate_limit({.max = 1, .window = std::chrono::seconds(60)}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    Conn c(srv.port());
    // 失敗は 0。既定の応答は 200 なので、失敗をそのまま返すと通ったように見える。
    auto send = [&]() -> unsigned {
        http::request<http::string_body> req{http::verb::get, "/", 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::response<http::string_body> res;
        if (c.write(req) || c.read(res)) {
            return 0;
        }
        return res.result_int();
    };
    EXPECT_EQ(send(), 200u);
    EXPECT_EQ(send(), 429u);
}

// 拒否中も数え続けると、同じ窓で 2^32 回目に 0 へ戻って通ってしまう。
TEST(RateLimit, RejectedHitsDoNotWrap) {
    hayate::mw::detail::RateWindow w;
    w.count = std::numeric_limits<std::uint32_t>::max();
    EXPECT_FALSE(hayate::mw::detail::admit(w, 1));
    EXPECT_EQ(w.count, std::numeric_limits<std::uint32_t>::max());
}

// キーは接続の peer。X-Forwarded-For は client が好きに書けるので見ない。
TEST(RateLimit, ForwardedForIsIgnored) {
    TestServer srv([](hayate::App &app) {
        app.use(hayate::mw::rate_limit({.max = 1, .window = std::chrono::seconds(60)}));
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    auto a = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                       std::chrono::seconds(2), {{"X-Forwarded-For", "203.0.113.1"}});
    auto b = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                       std::chrono::seconds(2), {{"X-Forwarded-For", "203.0.113.2"}});
    EXPECT_EQ(a.status, 200) << a.error_message;
    EXPECT_EQ(b.status, 429) << b.error_message;
}

namespace {

using hayate::mw::detail::hit;
using hayate::mw::detail::RateTable;
using std::chrono::milliseconds;

// 時計を進めずに窓の境界を踏むため、時刻は作り物にする。
const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);

} // namespace

// 窓が過ぎたら数え直す。
TEST(RateLimit, WindowRollsOver) {
    RateTable t;
    const hayate::mw::RateLimit cfg{.max = 1, .window = milliseconds(1000)};
    EXPECT_FALSE(hit(t, "p", t0, cfg).has_value());
    EXPECT_TRUE(hit(t, "p", t0 + milliseconds(999), cfg).has_value());
    EXPECT_FALSE(hit(t, "p", t0 + milliseconds(1000), cfg).has_value());
    EXPECT_TRUE(hit(t, "p", t0 + milliseconds(1001), cfg).has_value());
}

// 窓の切れた peer は次の要求で消える。窓の中の peer は残る。
TEST(RateLimit, SweepDropsExpiredPeers) {
    RateTable t;
    const hayate::mw::RateLimit cfg{.max = 10, .window = milliseconds(1000)};
    hit(t, "a", t0, cfg);
    hit(t, "b", t0 + milliseconds(500), cfg);
    ASSERT_EQ(t.by_peer.size(), 2u);
    hit(t, "c", t0 + milliseconds(1200), cfg);
    EXPECT_EQ(t.by_peer.count("a"), 0u);
    EXPECT_EQ(t.by_peer.count("b"), 1u);
    EXPECT_EQ(t.by_peer.count("c"), 1u);
}

// Retry-After は窓の残りを秒に切り上げる。最小 1。
TEST(RateLimit, RetryAfterRoundsUp) {
    struct Case {
        milliseconds left;
        std::uint32_t want;
    };
    for (const auto &c :
         {Case{milliseconds(300), 1}, Case{milliseconds(999), 1}, Case{milliseconds(1000), 1},
          Case{milliseconds(1001), 2}, Case{milliseconds(60000), 60}}) {
        RateTable t;
        const hayate::mw::RateLimit cfg{.max = 1, .window = milliseconds(60000)};
        ASSERT_FALSE(hit(t, "p", t0, cfg).has_value());
        const auto got = hit(t, "p", t0 + (cfg.window - c.left), cfg);
        ASSERT_TRUE(got.has_value()) << c.left.count();
        EXPECT_EQ(*got, c.want) << c.left.count();
    }
}
