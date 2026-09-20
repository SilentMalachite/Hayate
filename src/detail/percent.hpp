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

} // namespace hayate::detail
