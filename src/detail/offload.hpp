#pragma once

#include "asio.hpp"

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

} // namespace hayate::detail
