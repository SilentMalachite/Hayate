#pragma once

#include <chrono>
#include <cstdint>

namespace hayate {

struct Limits {
    std::uint64_t max_header_bytes{8192};
    std::uint64_t max_body_bytes{1048576};
    std::chrono::milliseconds read_timeout{30000};
    std::chrono::milliseconds write_timeout{30000};
    std::chrono::milliseconds idle_timeout{60000};
    std::uint32_t max_connections{1024};
};

} // namespace hayate
