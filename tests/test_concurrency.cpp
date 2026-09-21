#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <future>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace http = boost::beast::http;
namespace net = boost::asio;

namespace {

// 同じ接続で keep-alive のまま n 本投げる。timeout は client 側の保険。
std::string pipeline_on_one_connection(std::uint16_t port, int n) {
    net::io_context ioc;
    boost::beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(10));
    stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    std::string last;
    for (int i = 0; i < n; ++i) {
        http::request<http::string_body> req{http::verb::get, "/echo", 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(i + 1 < n);
        http::write(stream, req);
        boost::beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        last = res.body();
    }
    return last;
}

} // namespace

// threads(4) で接続が並行に走る。1 接続の stream とそのタイマーが同じ strand に
// 載っていないと、読み書きと期限満了が別スレッドで重なる。
TEST(Concurrency, ManyClientsOnFourThreads) {
    TestServer srv([](hayate::App &app) {
        app.threads(4);
        app.get("/echo", [](hayate::Request &) { return hayate::Response::text("echo"); });
    });
    constexpr int kClients = 8;
    constexpr int kPerClient = 8;
    std::latch start(kClients + 1);
    std::vector<std::future<bool>> results;
    std::vector<std::thread> threads;
    results.reserve(kClients);
    threads.reserve(kClients);
    for (int c = 0; c < kClients; ++c) {
        std::promise<bool> p;
        results.emplace_back(p.get_future());
        threads.emplace_back([&srv, &start, c, p = std::move(p)]() mutable {
            start.arrive_and_wait();
            bool ok = true;
            if (c % 2 == 0) {
                ok = pipeline_on_one_connection(srv.port(), kPerClient) == "echo";
            } else {
                for (int i = 0; i < kPerClient; ++i) {
                    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/echo", {}, {},
                                       std::chrono::seconds(10));
                    ok = ok && r.status == 200 && r.body == "echo";
                }
            }
            p.set_value(ok);
        });
    }
    start.arrive_and_wait();
    for (auto &f : results) {
        EXPECT_TRUE(f.get());
    }
    for (auto &t : threads) {
        t.join();
    }
}

// idle 期限が短いと、同じ stream のタイマー満了と読み書きが重なる。strand が無いと
// tcp_stream の内部状態を別スレッドが同時に触る。
TEST(Concurrency, IdleTimerRunsWithReads) {
    TestServer srv([](hayate::App &app) {
        app.threads(4);
        auto l = app.limits();
        l.idle_timeout = std::chrono::milliseconds(10);
        app.limits(l);
        app.get("/echo", [](hayate::Request &) { return hayate::Response::text("echo"); });
    });
    constexpr int kClients = 8;
    std::latch start(kClients + 1);
    std::vector<std::future<bool>> first_ok;
    std::vector<std::thread> threads;
    first_ok.reserve(kClients);
    threads.reserve(kClients);
    for (int c = 0; c < kClients; ++c) {
        std::promise<bool> p;
        first_ok.emplace_back(p.get_future());
        threads.emplace_back([&srv, &start, p = std::move(p)]() mutable {
            start.arrive_and_wait();
            net::io_context ioc;
            boost::beast::tcp_stream stream(ioc);
            stream.expires_after(std::chrono::seconds(10));
            stream.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
            bool ok = false;
            // 期限切れで閉じられるまで keep-alive で投げ続ける。閉じられるのは正常。
            for (int i = 0; i < 16; ++i) {
                http::request<http::string_body> req{http::verb::get, "/echo", 11};
                req.set(http::field::host, "127.0.0.1");
                req.keep_alive(true);
                boost::system::error_code ec;
                http::write(stream, req, ec);
                if (ec) {
                    break;
                }
                boost::beast::flat_buffer buf;
                http::response<http::string_body> res;
                http::read(stream, buf, res, ec);
                if (ec) {
                    break;
                }
                if (i == 0) {
                    ok = res.result_int() == 200 && res.body() == "echo";
                }
            }
            p.set_value(ok);
        });
    }
    start.arrive_and_wait();
    for (auto &f : first_ok) {
        EXPECT_TRUE(f.get());
    }
    for (auto &t : threads) {
        t.join();
    }
}

// stop() を同時に何本呼んでも、acceptor の close と work の解放は 1 回分に直列化される。
TEST(Concurrency, ConcurrentStopsAreIdempotent) {
    TestServer srv([](hayate::App &app) {
        app.threads(4);
        app.get("/echo", [](hayate::Request &) { return hayate::Response::text("echo"); });
    });
    // 先に 1 本通して、accept ループが回っていることを確かめる。
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/echo").status, 200);

    constexpr int kStoppers = 8;
    std::latch start(kStoppers + 1);
    std::vector<std::thread> threads;
    threads.reserve(kStoppers);
    for (int i = 0; i < kStoppers; ++i) {
        threads.emplace_back([&srv, &start] {
            start.arrive_and_wait();
            srv.app().stop();
        });
    }
    start.arrive_and_wait();
    for (auto &t : threads) {
        t.join();
    }
    auto refused = http_call("127.0.0.1", srv.port(), http::verb::get, "/echo", {}, {},
                             std::chrono::milliseconds(300));
    EXPECT_TRUE(refused.error || refused.status == 0);
}

// 接続を張り続けている最中に停止しても、in-flight は完了し、新規は拒否される。
TEST(Concurrency, StopWhileAccepting) {
    std::atomic<bool> stop_clients{false};
    TestServer srv([](hayate::App &app) {
        app.threads(4);
        app.get("/echo", [](hayate::Request &) { return hayate::Response::text("echo"); });
    });
    std::promise<void> first_done;
    std::atomic<bool> signalled{false};
    std::vector<std::thread> clients;
    clients.reserve(4);
    for (int i = 0; i < 4; ++i) {
        clients.emplace_back([&] {
            while (!stop_clients.load()) {
                auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/echo", {}, {},
                                   std::chrono::seconds(5));
                if (r.status == 200 && !signalled.exchange(true)) {
                    first_done.set_value();
                }
            }
        });
    }
    first_done.get_future().wait();
    srv.app().stop();
    stop_clients = true;
    for (auto &t : clients) {
        t.join();
    }
    auto refused = http_call("127.0.0.1", srv.port(), http::verb::get, "/echo", {}, {},
                             std::chrono::milliseconds(300));
    EXPECT_TRUE(refused.error || refused.status == 0);
}

// SIGTERM と stop() が重なっても、停止は 1 回分だけ効き、serve() は返る（F03）。
TEST(Concurrency, SigtermDuringStop) {
    auto srv = std::make_unique<TestServer>([](hayate::App &app) {
        app.threads(2);
        app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    });
    // signal_set は accept ループより先に入る。1 本通れば SIGTERM で落ちない。
    ASSERT_EQ(http_call("127.0.0.1", srv->port(), http::verb::get, "/").status, 200);
    std::latch go(3);
    // raise は呼んだスレッドで handler を走らせてから返る。kill だと別スレッドに遅れて届き、
    // signal_set を壊した後に既定動作でプロセスごと落ちることがある。
    std::thread by_signal([&] {
        go.arrive_and_wait();
        ::raise(SIGTERM);
    });
    std::thread by_call([&] {
        go.arrive_and_wait();
        srv->app().stop();
    });
    go.arrive_and_wait();
    by_signal.join();
    by_call.join();
    srv.reset();
}
