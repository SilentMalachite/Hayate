#include "detail/offload.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace net = hayate::detail::net;

TEST(Offload, RunsOffTheIoThreadAndResumesOnIt) {
    net::io_context ioc;
    net::thread_pool pool(1);
    std::thread::id io_id;
    std::thread::id work_id;
    std::thread::id resume_id;
    int got = 0;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void> {
            io_id = std::this_thread::get_id();
            got = co_await hayate::detail::offload(pool, [&] {
                work_id = std::this_thread::get_id();
                return 42;
            });
            resume_id = std::this_thread::get_id();
        },
        net::detached);
    ioc.run();
    EXPECT_EQ(got, 42);
    EXPECT_NE(work_id, io_id);
    EXPECT_EQ(resume_id, io_id);
}

TEST(Offload, DeadlineAbandonsSlowWork) {
    net::io_context ioc;
    net::thread_pool pool(1);
    std::promise<void> release;
    auto released = std::make_shared<std::future<void>>(release.get_future());
    std::optional<int> got{42};
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void> {
            got = co_await hayate::detail::offload_until(pool, std::chrono::milliseconds(50),
                                                         [released] {
                                                             released->wait();
                                                             return 7;
                                                         });
        },
        net::detached);
    const auto began = std::chrono::steady_clock::now();
    ioc.run();
    const auto took = std::chrono::steady_clock::now() - began;
    EXPECT_FALSE(got.has_value());
    EXPECT_LT(took, std::chrono::seconds(2));
    // 諦めた後に仕事が終わる。参照しているものが生きていれば壊れない。
    release.set_value();
    pool.join();
}

TEST(Offload, LateResultDoesNotDangle) {
    net::io_context ioc;
    net::thread_pool pool(1);
    std::promise<void> release;
    auto released = std::make_shared<std::future<void>>(release.get_future());
    auto scratch = std::make_shared<std::string>("untouched");
    std::optional<std::size_t> got{1};
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void> {
            got = co_await hayate::detail::offload_until(pool, std::chrono::milliseconds(50),
                                                         [released, scratch] {
                                                             released->wait();
                                                             *scratch = "written late";
                                                             return scratch->size();
                                                         });
        },
        net::detached);
    ioc.run();
    EXPECT_FALSE(got.has_value());
    // 待つのをやめた後にワーカーが書く。共有状態は生きている。
    release.set_value();
    pool.join();
    EXPECT_EQ(*scratch, "written late");
}

TEST(Offload, ExceptionCrossesThePoolBoundary) {
    net::io_context ioc;
    net::thread_pool pool(1);
    bool caught = false;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void> {
            try {
                co_await hayate::detail::offload(pool, [] { throw std::runtime_error("boom"); });
            } catch (const std::runtime_error &) {
                caught = true;
            }
        },
        net::detached);
    ioc.run();
    EXPECT_TRUE(caught);
}
