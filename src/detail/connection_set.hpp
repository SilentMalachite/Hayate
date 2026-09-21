#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace hayate::detail {

// stop() が、要求の到着を待っている接続を取り消すための登録簿。
// 登録と解除は各接続の strand、cancel_all は admin strand から来るので mutex で守る。
class ConnectionSet {
  public:
    // cancel は post するだけの関数。lock を持ったまま呼ぶので、中で待たない。
    std::uint64_t add(std::function<void()> cancel) {
        const std::lock_guard lock(mutex_);
        const auto id = next_++;
        cancels_.emplace(id, std::move(cancel));
        return id;
    }

    void remove(std::uint64_t id) {
        const std::lock_guard lock(mutex_);
        cancels_.erase(id);
    }

    void cancel_all() {
        const std::lock_guard lock(mutex_);
        for (auto &[id, cancel] : cancels_) {
            cancel();
        }
    }

  private:
    std::mutex mutex_;
    std::uint64_t next_{0};
    std::unordered_map<std::uint64_t, std::function<void()>> cancels_;
};

} // namespace hayate::detail
