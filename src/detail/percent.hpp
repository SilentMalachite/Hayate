#pragma once

#include <string>
#include <string_view>

namespace hayate::detail {

inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// 壊れた % 列はそのまま残す。400 にすると既存の生パス扱いと食い違う。
inline std::string percent_decode(std::string_view in, bool plus_as_space) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '+' && plus_as_space) {
            out.push_back(' ');
            continue;
        }
        if (c != '%' || i + 2 >= in.size()) {
            out.push_back(c);
            continue;
        }
        const int hi = hex_digit(in[i + 1]);
        const int lo = hex_digit(in[i + 2]);
        if (hi < 0 || lo < 0) {
            out.push_back(c);
            continue;
        }
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return out;
}

// RFC 3986 の pchar（unreserved / sub-delims / ":" / "@"）以外を %XX にする。
// `%` 自身も符号化するので、復号すると元に戻る。
inline std::string percent_encode_pchar(std::string_view in) {
    static constexpr std::string_view keep = "-._~!$&'()*+,;=:@";
    static constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        const auto b = static_cast<unsigned char>(c);
        const bool alnum =
            (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9');
        if (alnum || keep.find(c) != std::string_view::npos) {
            out.push_back(c);
            continue;
        }
        out.push_back('%');
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0f]);
    }
    return out;
}

} // namespace hayate::detail
