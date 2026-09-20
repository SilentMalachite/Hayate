#pragma once

#include <hayate/router.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace hayate::mw {

struct RateLimit {
    std::uint32_t max{60};
    std::chrono::milliseconds window{60000};
};

inline Middleware rate_limit(RateLimit cfg = {}) {
    struct Window {
        std::chrono::steady_clock::time_point start{};
        std::uint32_t count{0};
    };
    struct State {
        std::mutex mu;
        std::unordered_map<std::string, Window> by_peer;
        std::chrono::steady_clock::time_point last_sweep{};
    };
    auto state = std::make_shared<State>();
    return [cfg, state](Request &req, Next next) -> boost::asio::awaitable<Response> {
        const auto now = std::chrono::steady_clock::now();
        const auto key = std::string(req.peer());
        std::uint32_t retry_s = 1;
        bool limited = false;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            // 窓の切れたキーを残すと peer ごとに map が増え続ける。
            // 毎回舐めると O(peers) になるので、掃除は窓 1 つにつき 1 回。
            if (now - state->last_sweep >= cfg.window) {
                state->last_sweep = now;
                for (auto it = state->by_peer.begin(); it != state->by_peer.end();) {
                    if (now - it->second.start >= cfg.window) {
                        it = state->by_peer.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            auto &w = state->by_peer[key];
            if (w.count == 0 || now - w.start >= cfg.window) {
                w.start = now;
                w.count = 0;
            }
            ++w.count;
            if (w.count > cfg.max) {
                limited = true;
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - w.start);
                auto rem = cfg.window - elapsed;
                if (rem.count() < 1000) {
                    retry_s = 1;
                } else {
                    retry_s = static_cast<std::uint32_t>((rem.count() + 999) / 1000);
                }
            }
        }
        if (limited) {
            auto res = Response::from_error({"rate_limited", "Too Many Requests", 429});
            res.set_header("Retry-After", std::to_string(retry_s));
            co_return res;
        }
        co_return co_await next(req);
    };
}

} // namespace hayate::mw
