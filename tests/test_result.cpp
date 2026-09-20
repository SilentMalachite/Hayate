#include <hayate/result.hpp>

#include <gtest/gtest.h>

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
