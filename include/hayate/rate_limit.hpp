#pragma once

#include <hayate/router.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace hayate::mw {

struct RateLimit {
    std::uint32_t max{60};
    std::chrono::milliseconds window{60000};
};

namespace detail {

struct RateWindow {
    std::chrono::steady_clock::time_point start{};
    std::uint32_t count{0};
};

// 1 要求を窓に数える。通すなら true。
// 拒否中は数えない。数え続けると 2^32 で 0 に戻り、制限が外れる。
inline bool admit(RateWindow &w, std::uint32_t max) {
    if (w.count >= max) {
        return false;
    }
    ++w.count;
    return true;
}

// peer ごとの窓。mutex は呼ぶ側が持つ。
struct RateTable {
    std::unordered_map<std::string, RateWindow> by_peer;
    std::chrono::steady_clock::time_point last_sweep{};
};

// 1 要求を数える。通すなら nullopt、断るなら Retry-After の秒。
// 時刻を引数で受けるのは、窓の境界を時計を進めずに試すため。
inline std::optional<std::uint32_t> hit(RateTable &t, std::string_view key,
                                        std::chrono::steady_clock::time_point now,
                                        const RateLimit &cfg) {
    // 窓の切れたキーを残すと peer ごとに map が増え続ける。
    // 毎回舐めると O(peers) になるので、掃除は窓 1 つにつき 1 回。
    if (now - t.last_sweep >= cfg.window) {
        t.last_sweep = now;
        for (auto it = t.by_peer.begin(); it != t.by_peer.end();) {
            if (now - it->second.start >= cfg.window) {
                it = t.by_peer.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 新規かどうかは count で見ない。max が 0 だと count が 0 のままで、
    // 窓が毎回始め直しになる。
    auto [it, fresh] = t.by_peer.try_emplace(std::string(key));
    auto &w = it->second;
    if (fresh || now - w.start >= cfg.window) {
        w.start = now;
        w.count = 0;
    }
    if (admit(w, cfg.max)) {
        return std::nullopt;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - w.start);
    const auto rem = cfg.window - elapsed;
    if (rem.count() < 1000) {
        return 1;
    }
    return static_cast<std::uint32_t>((rem.count() + 999) / 1000);
}

} // namespace detail

inline Middleware rate_limit(RateLimit cfg = {}) {
    struct State {
        std::mutex mu;
        detail::RateTable table;
    };
    auto state = std::make_shared<State>();
    return [cfg, state](Request &req, Next next) -> boost::asio::awaitable<Response> {
        const auto now = std::chrono::steady_clock::now();
        std::optional<std::uint32_t> retry_s;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            retry_s = detail::hit(state->table, req.peer(), now, cfg);
        }
        if (retry_s) {
            auto res = Response::from_error({"rate_limited", "Too Many Requests", 429});
            res.set_header("Retry-After", std::to_string(*retry_s));
            co_return res;
        }
        co_return co_await next(req);
    };
}

} // namespace hayate::mw
