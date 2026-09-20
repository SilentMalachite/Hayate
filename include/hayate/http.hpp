#pragma once

#include <nlohmann/json.hpp>

namespace hayate {

enum class HttpMethod { get, post, options, unknown };

using Json = nlohmann::json;

} // namespace hayate
