#pragma once

#include "detail/hmac.hpp"

#include <hayate/http.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

// テスト側でトークンを組む。署名は実装と同じ HMAC を使う。
inline std::string base64url_encode(std::string_view in) {
    static constexpr std::string_view tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const auto a = static_cast<unsigned char>(in[i]);
        const auto b = static_cast<unsigned char>(in[i + 1]);
        const auto c = static_cast<unsigned char>(in[i + 2]);
        out += tbl[a >> 2];
        out += tbl[((a & 0x03) << 4) | (b >> 4)];
        out += tbl[((b & 0x0f) << 2) | (c >> 6)];
        out += tbl[c & 0x3f];
        i += 3;
    }
    if (i + 1 == in.size()) {
        const auto a = static_cast<unsigned char>(in[i]);
        out += tbl[a >> 2];
        out += tbl[(a & 0x03) << 4];
    } else if (i + 2 == in.size()) {
        const auto a = static_cast<unsigned char>(in[i]);
        const auto b = static_cast<unsigned char>(in[i + 1]);
        out += tbl[a >> 2];
        out += tbl[((a & 0x03) << 4) | (b >> 4)];
        out += tbl[(b & 0x0f) << 2];
    }
    return out;
}

inline std::int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline hayate::Json hs256_header() {
    return hayate::Json::object({{"alg", "HS256"}, {"typ", "JWT"}});
}

// 改竄テストのために署名対象を組み立てられる形で置く。
inline std::string sign_parts(const std::string &header_b64, const std::string &payload_b64,
                              const std::string &secret) {
    const auto signing = header_b64 + "." + payload_b64;
    return signing + "." + base64url_encode(hayate::detail::hmac_sha256(secret, signing).value());
}

inline std::string make_token(const hayate::Json &header, const hayate::Json &payload,
                              const std::string &secret) {
    return sign_parts(base64url_encode(header.dump()), base64url_encode(payload.dump()), secret);
}
