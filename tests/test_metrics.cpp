#include "conn_client.hpp"
#include "http_client.hpp"
#include "metrics_text.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace http = boost::beast::http;

namespace {

void setup(hayate::App &app) {
    app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    app.get("/metrics", hayate::metrics(app));
}

} // namespace

TEST(Metrics, CountsRequests) {
    TestServer srv(setup);
    http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(value_of(r.body, "hayate_requests_total"), 2);
}

TEST(Metrics, CountsStatusClasses) {
    TestServer srv(setup);
    http_call("127.0.0.1", srv.port(), http::verb::get, "/nope");
    auto first = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_EQ(value_of(first.body, "hayate_responses_total{class=\"4xx\"}"), 1);
    EXPECT_EQ(value_of(first.body, "hayate_responses_total{class=\"2xx\"}"), 0);
    EXPECT_EQ(value_of(first.body, "hayate_responses_total{class=\"5xx\"}"), 0);
    // 1 本目のスクレイプ（200）は自分の出力には入らず、次に現れる。
    auto second = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_EQ(value_of(second.body, "hayate_responses_total{class=\"2xx\"}"), 1);
}

TEST(Metrics, CountsAcceptedConnections) {
    TestServer srv(setup);
    http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    // http_call は 1 回 1 接続。スクレイプ自身の接続も accept 済み。
    EXPECT_EQ(value_of(r.body, "hayate_connections_accepted_total"), 3);
}

TEST(Metrics, ContentType) {
    TestServer srv(setup);
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_NE(r.content_type.find("text/plain"), std::string::npos);
    EXPECT_NE(r.content_type.find("version=0.0.4"), std::string::npos);
}

TEST(Metrics, RejectedOverMaxConnections) {
    TestServer srv([](hayate::App &app) {
        app.limits({.max_connections = 1});
        setup(app);
    });
    namespace net = boost::asio;
    // A を keep-alive で握ったままにする。これで open = 1。
    Conn a(srv.port(), std::chrono::seconds(5));
    auto send = [&](const std::string &target) -> std::string {
        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::response<http::string_body> res;
        if (a.write(req, std::chrono::seconds(5)) || a.read(res, std::chrono::seconds(5))) {
            return {};
        }
        return res.body();
    };
    EXPECT_EQ(send("/"), "ok");

    // B は上限に当たる。EOF を見た時点で拒否カウンタは進んでいる。
    {
        Conn b(srv.port());
        std::array<char, 16> sink{};
        const auto ec =
            b.run(std::chrono::seconds(2), [&]() -> net::awaitable<boost::system::error_code> {
                auto [rec, n] = co_await b.stream().async_read_some(
                    net::buffer(sink), net::as_tuple(net::use_awaitable));
                (void)n;
                co_return rec;
            });
        EXPECT_TRUE(static_cast<bool>(ec)) << "B should be closed by the server";
        EXPECT_NE(ec, boost::beast::error::timeout);
    }

    const auto body = send("/metrics");
    EXPECT_EQ(value_of(body, "hayate_connections_rejected_total"), 1);
    EXPECT_EQ(value_of(body, "hayate_connections_open"), 1);
}

// 送れないヘッダは 500 に差し替える。metrics もハンドラの 200 ではなく送った 500 で数える。
TEST(Metrics, OversizedHeaderCountsAs5xx) {
    TestServer srv([](hayate::App &app) {
        app.get("/big", [](hayate::Request &) {
            auto res = hayate::Response::text("ok");
            res.set_header("X-Big", std::string(70000, 'a'));
            return res;
        });
        app.get("/metrics", hayate::metrics(app));
    });
    http_call("127.0.0.1", srv.port(), http::verb::get, "/big");
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_EQ(value_of(r.body, "hayate_responses_total{class=\"5xx\"}"), 1);
    EXPECT_EQ(value_of(r.body, "hayate_responses_total{class=\"2xx\"}"), 0);
}

// 上限超過の 413 / 431 は Router に届かないが、応答は返るので数える。
TEST(Metrics, LimitErrorsAreCounted) {
    TestServer srv([](hayate::App &app) {
        // スクレイプの要求は通る大きさにする。
        app.limits().max_header_bytes = 256;
        app.limits().max_body_bytes = 4;
        setup(app);
        app.post("/echo", [](hayate::Request &) { return hayate::Response::text("echo"); });
    });
    auto big_header = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                                std::chrono::seconds(2), {{"X-Pad", std::string(1024, 'a')}});
    EXPECT_EQ(big_header.status, 431);
    auto big_body = http_call("127.0.0.1", srv.port(), http::verb::post, "/echo", "hello");
    EXPECT_EQ(big_body.status, 413);
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(value_of(r.body, "hayate_requests_total"), 2);
    EXPECT_EQ(value_of(r.body, "hayate_responses_total{class=\"4xx\"}"), 2);
}

// threads(4) で並行に数えても取りこぼさない。
TEST(Metrics, ExactUnderConcurrency) {
    TestServer srv([](hayate::App &app) {
        app.threads(4);
        setup(app);
    });
    constexpr int kClients = 8;
    constexpr int kPerClient = 8;
    std::latch start(kClients + 1);
    std::vector<std::future<int>> oks;
    std::vector<std::thread> threads;
    oks.reserve(kClients);
    threads.reserve(kClients);
    for (int c = 0; c < kClients; ++c) {
        std::promise<int> p;
        oks.emplace_back(p.get_future());
        threads.emplace_back([&srv, &start, p = std::move(p)]() mutable {
            start.arrive_and_wait();
            int ok = 0;
            for (int i = 0; i < kPerClient; ++i) {
                auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                                   std::chrono::seconds(10));
                ok += r.status == 200 ? 1 : 0;
            }
            p.set_value(ok);
        });
    }
    start.arrive_and_wait();
    int total = 0;
    for (auto &f : oks) {
        total += f.get();
    }
    for (auto &t : threads) {
        t.join();
    }
    ASSERT_EQ(total, kClients * kPerClient);
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(value_of(r.body, "hayate_requests_total"), total);
    EXPECT_EQ(value_of(r.body, "hayate_responses_total{class=\"2xx\"}"), total);
    // スクレイプ自身の接続も accept 済み。connections_open は閉じる順が client の読みと
    // 前後するので見ない。
    EXPECT_EQ(value_of(r.body, "hayate_connections_accepted_total"), total + 1);
}
