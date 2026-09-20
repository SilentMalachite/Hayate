#pragma once

#include <cstdint>
#include <string>

namespace hayate {

struct Error {
    std::string code;
    std::string message;
    std::uint16_t http_status{500};
};

} // namespace hayate
