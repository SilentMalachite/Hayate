#include "detail/offload.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
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
