#pragma once

#include <hayate/http.hpp>
#include <hayate/result.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hayate {

template <typename Stream> class Connection;

class Request {
  public:
    HttpMethod method() const noexcept;
    std::string_view target() const noexcept;
    std::string_view path() const noexcept;
    std::string_view query(std::string_view key) const noexcept;
    std::string_view header(std::string_view name) const noexcept;
    std::string_view param(std::string_view name) const noexcept;
    std::string_view peer() const noexcept;
    std::span<const std::byte> body() const noexcept;
    Result<Json> json() const;

    template <typename T> Result<T> json() const;

    template <typename T, typename... Args> T &set(Args &&...args);

    template <typename T> T *get() noexcept;

  private:
    template <typename Stream> friend class Connection;
    friend class Router;

    HttpMethod method_{HttpMethod::get};
    std::string peer_;
    std::string target_;
    std::string path_;
    std::string body_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::vector<std::pair<std::string, std::string>> query_;
    std::vector<std::pair<std::string, std::string>> params_;
    std::unordered_map<std::type_index, std::shared_ptr<void>> ext_;
};

template <typename T> Result<T> Request::json() const {
    auto parsed = json();
    if (!parsed.ok()) {
        return Result<T>::err(parsed.error());
    }
    try {
        return Result<T>::ok(parsed.value().template get<T>());
    } catch (...) {
        return Result<T>::err(Error{"bad_json", "type mismatch", 400});
    }
}

template <typename T, typename... Args> T &Request::set(Args &&...args) {
    auto ptr = std::make_shared<T>(std::forward<Args>(args)...);
    T &ref = *ptr;
    ext_[std::type_index(typeid(T))] = std::move(ptr);
    return ref;
}

template <typename T> T *Request::get() noexcept {
    auto it = ext_.find(std::type_index(typeid(T)));
    if (it == ext_.end()) {
        return nullptr;
    }
    return static_cast<T *>(it->second.get());
}

} // namespace hayate
