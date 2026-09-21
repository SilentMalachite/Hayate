#pragma once

#include "asio.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace hayate::detail {

// ブロッキング呼び出しを pool のスレッドで走らせ、呼び出し元の executor に戻る。
// net::post(pool, use_awaitable) では完了ハンドラの associated executor が
// 呼び出し元コルーチンのものになり、f がプールに乗らない。co_spawn が要る。
template <typename F>
net::awaitable<std::invoke_result_t<F &>> offload(net::thread_pool &pool, F f) {
    using R = std::invoke_result_t<F &>;
    co_return co_await net::co_spawn(
        pool.get_executor(), [f = std::move(f)]() mutable -> net::awaitable<R> { co_return f(); },
        net::use_awaitable);
}

// offload と同じだが limit で待つのをやめる。諦めたら空を返す。
// 仕事は止まらない。f と f が触る状態は shared_ptr でワーカー側に残るので、
// 待つのをやめた後に書かれても壊れない。f は投げない前提（投げたら空扱い）。
// 呼び出し側の実行コンテキストは pool より長生きすること（App::Impl は ioc を
// 先頭に宣言していて最後に壊れる）。
template <typename F>
net::awaitable<std::optional<std::invoke_result_t<F &>>>
offload_until(net::thread_pool &pool, std::chrono::milliseconds limit, F f) {
    using R = std::invoke_result_t<F &>;
    struct State {
        std::optional<R> value;
        net::steady_timer gate;
        State(const net::any_io_executor &ex, std::chrono::milliseconds d) : gate(ex, d) {}
    };
    auto st = std::make_shared<State>(co_await net::this_coro::executor, limit);
    net::post(pool, [st, f = std::move(f)]() mutable {
        std::optional<R> got;
        try {
            got.emplace(f());
        } catch (...) {
        }
        // 結果の受け渡しと gate の操作は呼び出し側の executor 上だけで行う。
        net::post(st->gate.get_executor(), [st, got = std::move(got)]() mutable {
            st->value = std::move(got);
            st->gate.cancel();
        });
    });
    auto [ec] = co_await st->gate.async_wait(net::as_tuple);
    (void)ec;
    co_return std::move(st->value);
}

} // namespace hayate::detail
