#pragma once

#include <nlohmann/json.hpp>

namespace hayate {

enum class HttpMethod { get, post };

using Json = nlohmann::json;

} // namespace hayate
