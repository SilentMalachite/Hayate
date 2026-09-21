#include <hayate/result.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <utility>

TEST(Result, OkHoldsValue) {
    auto r = hayate::Result<int>::ok(7);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 7);
}

TEST(Result, ErrHoldsError) {
    auto r = hayate::Result<int>::err({"bad_json", "broken", 400});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, "bad_json");
    EXPECT_EQ(r.error().http_status, 400);
}

// move しかできない T も持てる。rvalue の value() で取り出せる。
TEST(Result, MoveOnlyValue) {
    using R = hayate::Result<std::unique_ptr<int>>;
    static_assert(!std::is_copy_constructible_v<R>);
    static_assert(std::is_move_constructible_v<R>);
    auto r = R::ok(std::make_unique<int>(5));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r.value(), 5);
    auto moved = std::move(r);
    ASSERT_TRUE(moved.ok());
    auto p = std::move(moved).value();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 5);

    auto e = R::err({"internal", "broken", 500});
    ASSERT_FALSE(e.ok());
    EXPECT_EQ(e.error().http_status, 500);
}
