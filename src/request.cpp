#include <hayate/request.hpp>

namespace hayate {

HttpMethod Request::method() const noexcept { return method_; }
std::string_view Request::target() const noexcept { return target_; }
std::string_view Request::path() const noexcept { return path_; }

std::string_view Request::query(std::string_view key) const noexcept {
    for (const auto &[k, v] : query_) {
        if (k == key) {
            return v;
        }
    }
    return {};
}

std::string_view Request::header(std::string_view name) const noexcept {
    for (const auto &[k, v] : headers_) {
        if (k.size() == name.size()) {
            bool eq = true;
            for (std::size_t i = 0; i < k.size(); ++i) {
                char a = k[i];
                char b = name[i];
                if (a >= 'A' && a <= 'Z') {
                    a = static_cast<char>(a - 'A' + 'a');
                }
                if (b >= 'A' && b <= 'Z') {
                    b = static_cast<char>(b - 'A' + 'a');
                }
                if (a != b) {
                    eq = false;
                    break;
                }
            }
            if (eq) {
                return v;
            }
        }
    }
    return {};
}

std::string_view Request::peer() const noexcept { return peer_; }

std::string_view Request::param(std::string_view name) const noexcept {
    for (const auto &[k, v] : params_) {
        if (k == name) {
            return v;
        }
    }
    return {};
}

std::span<const std::byte> Request::body() const noexcept {
    return std::as_bytes(std::span<const char>(body_.data(), body_.size()));
}

Result<Json> Request::json() const {
    try {
        auto parsed = Json::parse(body_, nullptr, false);
        if (parsed.is_discarded()) {
            return Result<Json>::err(Error{"bad_json", "broken json", 400});
        }
        return Result<Json>::ok(std::move(parsed));
    } catch (...) {
        return Result<Json>::err(Error{"bad_json", "broken json", 400});
    }
}

} // namespace hayate
