#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hayate::detail {

// App が 1 つ持つ。グローバルにしない。
struct Counters {
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> requests{0};
    // 1xx..5xx
    std::array<std::atomic<std::uint64_t>, 5> by_class{};
};

// 応答 1 本ぶん。requests と class は必ず一緒に進める。
inline void count_response(Counters &c, std::uint16_t status) {
    c.requests.fetch_add(1, std::memory_order_relaxed);
    const auto k = static_cast<std::size_t>(status / 100);
    if (k >= 1 && k <= 5) {
        c.by_class[k - 1].fetch_add(1, std::memory_order_relaxed);
    }
}

inline std::string render(const Counters &c, std::uint32_t open) {
    const auto n = [](std::uint64_t v) { return std::to_string(v); };
    std::string out;
    out += "# HELP hayate_connections_accepted_total Connections accepted.\n";
    out += "# TYPE hayate_connections_accepted_total counter\n";
    out +=
        "hayate_connections_accepted_total " + n(c.accepted.load(std::memory_order_relaxed)) + "\n";
    out += "# HELP hayate_connections_rejected_total Connections refused over max_connections.\n";
    out += "# TYPE hayate_connections_rejected_total counter\n";
    out +=
        "hayate_connections_rejected_total " + n(c.rejected.load(std::memory_order_relaxed)) + "\n";
    out += "# HELP hayate_connections_open Connections currently open.\n";
    out += "# TYPE hayate_connections_open gauge\n";
    out += "hayate_connections_open " + n(open) + "\n";
    out += "# HELP hayate_requests_total Requests that produced a response.\n";
    out += "# TYPE hayate_requests_total counter\n";
    out += "hayate_requests_total " + n(c.requests.load(std::memory_order_relaxed)) + "\n";
    out += "# HELP hayate_responses_total Responses by status class.\n";
    out += "# TYPE hayate_responses_total counter\n";
    for (std::size_t i = 0; i < c.by_class.size(); ++i) {
        out += "hayate_responses_total{class=\"" + std::to_string(i + 1) + "xx\"} " +
               n(c.by_class[i].load(std::memory_order_relaxed)) + "\n";
    }
    return out;
}

} // namespace hayate::detail
