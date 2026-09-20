#pragma once

#include "detail/asio.hpp"
#include "detail/metrics.hpp"

#include <hayate/limits.hpp>
#include <hayate/router.hpp>

#include <atomic>
#include <functional>

namespace hayate {

detail::net::awaitable<void> serve_connection(detail::beast::tcp_stream stream,
                                              const Limits &limits, Router &router,
                                              detail::Counters &counters,
                                              std::atomic<bool> &shutting,
                                              std::function<void()> on_done);

detail::net::awaitable<void> serve_connection(detail::tls_stream stream, const Limits &limits,
                                              Router &router, detail::Counters &counters,
                                              std::atomic<bool> &shutting,
                                              std::function<void()> on_done);

} // namespace hayate
