#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hayate::detail {

// JWT の base64url。パディング無し。A-Za-z0-9-_ 以外が来たら受け取らない。
inline std::optional<std::string> base64url_decode(std::string_view in) {
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '-') {
            return 62;
        }
        if (c == '_') {
            return 63;
        }
        return -1;
    };
    // 余り 1 文字では 1 バイトも作れない。壊れた入力。
    if (in.size() % 4 == 1) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(in.size() / 4 * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        const int v = value(c);
        if (v < 0) {
            return std::nullopt;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    // 余りのビットを捨てると、同じバイト列に綴りが何通りもできる。正準な 0 だけ受け取る。
    if ((acc & ((1u << bits) - 1)) != 0) {
        return std::nullopt;
    }
    return out;
}

} // namespace hayate::detail
