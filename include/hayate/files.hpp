#pragma once

#include <hayate/router.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>

namespace hayate {

// fs_timeout は正規化・stat・open とプール待ちの上限。超えたら 503。
Handler files(std::string_view root, std::uint64_t max_bytes = 0, std::uint32_t io_threads = 2,
              std::chrono::milliseconds fs_timeout = std::chrono::seconds(5));

} // namespace hayate
