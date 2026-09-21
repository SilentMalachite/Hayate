#pragma once

#include <hayate/http.hpp>

#include <cstdint>
#include <limits>
#include <optional>

namespace hayate::detail {

// exp / nbf は int64 秒の整数だけ受ける。小数・範囲外・非数値は不正なトークン扱い。
inline std::optional<std::int64_t> numeric_date(const Json &v) {
    if (v.is_number_unsigned()) {
        const auto u = v.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(u);
    }
    if (v.is_number_integer()) {
        return v.get<std::int64_t>();
    }
    return std::nullopt;
}

// leeway を足しても溢れない。
constexpr std::int64_t sat_add(std::int64_t a, std::int64_t b) noexcept {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    if (b > 0 && a > max - b) {
        return max;
    }
    if (b < 0 && a < min - b) {
        return min;
    }
    return a + b;
}

// exp と nbf の検査。now を引数で受けるので、境界を作り物の時刻で確かめられる。
inline bool times_ok(const Json &payload, std::int64_t now, std::int64_t leeway) {
    // exp 無しは永久トークンになる。RFC 上は任意だが締める。
    const auto exp = payload.find("exp");
    if (exp == payload.end()) {
        return false;
    }
    const auto exp_at = numeric_date(*exp);
    if (!exp_at || now > sat_add(*exp_at, leeway)) {
        return false;
    }
    const auto nbf = payload.find("nbf");
    if (nbf != payload.end()) {
        const auto nbf_at = numeric_date(*nbf);
        if (!nbf_at || sat_add(now, leeway) < *nbf_at) {
            return false;
        }
    }
    return true;
}

} // namespace hayate::detail
