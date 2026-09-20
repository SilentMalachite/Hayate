#pragma once

#include <hayate/router.hpp>

#include <string>
#include <utility>

namespace hayate::mw {

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
                res.set_header("Vary", "Origin");
            }
            co_return res;
        }
        auto res = co_await next(req);
        if (has_origin) {
            res.set_header("Access-Control-Allow-Origin", cfg.origin);
            if (vary) {
                res.set_header("Vary", "Origin");
            }
        }
        co_return res;
    };
}

} // namespace hayate::mw
