#pragma once

#include "asio.hpp"

#include <hayate/router.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace hayate::detail {

// files() の本体。プールを外から受けるので、テストはプールを塞いで fs_timeout を確かめられる。
// root は解決済みの絶対パス。
Handler files_on(std::filesystem::path root, std::uint64_t max_bytes,
                 std::chrono::milliseconds fs_timeout, std::shared_ptr<net::thread_pool> pool);

} // namespace hayate::detail
