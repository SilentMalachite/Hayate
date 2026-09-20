#pragma once

#include <hayate/router.hpp>

#include <cstdint>
#include <string_view>

namespace hayate {

Handler files(std::string_view root, std::uint64_t max_bytes = 1048576,
              std::uint32_t io_threads = 2);

} // namespace hayate
