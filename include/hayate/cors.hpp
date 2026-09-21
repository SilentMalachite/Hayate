#pragma once

#include <hayate/router.hpp>

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace hayate::mw {

namespace detail {

inline bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// 下流が付けた Vary を上書きしない。既に token か * を含めば何もしない。
inline void add_vary(Response &res, std::string_view token) {
    const auto cur = res.header("Vary");
    if (cur.empty()) {
        res.set_header("Vary", token);
        return;
    }
    std::string_view rest = cur;
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        auto item = rest.substr(0, comma);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
            item.remove_prefix(1);
        }
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
            item.remove_suffix(1);
        }
        if (item == "*" || iequals_ascii(item, token)) {
            return;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    // set_header で cur が無効になる前に組み立てる。
    std::string merged(cur);
    merged += ", ";
    merged += token;
    res.set_header("Vary", merged);
}

} // namespace detail

struct Cors {
    std::string origin{"*"};
    std::string methods{"GET, POST, OPTIONS"};
    std::string headers{"Content-Type, Authorization"};
};

inline Middleware cors(Cors cfg = {}) {
    return [cfg = std::move(cfg)](Request &req, Next next) -> boost::asio::awaitable<Response> {
        // Allow-Origin が固定値のとき、共有キャッシュが別 Origin に配らないよう Vary が要る。
        const bool vary = cfg.origin != "*";
        const bool has_origin = !req.header("origin").empty();
        if (has_origin && req.method() == HttpMethod::options) {
            auto res = Response::no_content();
            res.set_header("Access-Control-Allow-Origin", cfg.origin);
            res.set_header("Access-Control-Allow-Methods", cfg.methods);
            res.set_header("Access-Control-Allow-Headers", cfg.headers);
            if (vary) {
                detail::add_vary(res, "Origin");
            }
            co_return res;
        }
        auto res = co_await next(req);
        if (has_origin) {
            res.set_header("Access-Control-Allow-Origin", cfg.origin);
        }
        // Origin の有無で応答が変わるので、Origin が無い応答にも付ける。
        if (vary) {
            detail::add_vary(res, "Origin");
        }
        co_return res;
    };
}

} // namespace hayate::mw
