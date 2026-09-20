#pragma once

#include "detail/asio.hpp"

#include <hayate/limits.hpp>
#include <hayate/router.hpp>

#include <atomic>
#include <functional>

namespace hayate {

detail::net::awaitable<void> serve_connection(detail::beast::tcp_stream stream,
                                              const Limits &limits, Router &router,
                                              std::atomic<bool> &shutting,
                                              std::function<void()> on_done);

} // namespace hayate
