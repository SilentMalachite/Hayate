#pragma once

#include <hayate/error.hpp>
#include <utility>
#include <variant>

namespace hayate {

template <typename T> class Result {
  public:
    static Result ok(T value) {
        return Result(std::variant<T, Error>(std::in_place_index<0>, std::move(value)));
    }
    static Result err(Error error) {
        return Result(std::variant<T, Error>(std::in_place_index<1>, std::move(error)));
    }
    bool ok() const noexcept { return v_.index() == 0; }
    T &value() & { return std::get<0>(v_); }
    const T &value() const & { return std::get<0>(v_); }
    T value() && { return std::get<0>(std::move(v_)); }
    const Error &error() const & { return std::get<1>(v_); }

  private:
    explicit Result(std::variant<T, Error> v) : v_(std::move(v)) {}
    std::variant<T, Error> v_;
};

} // namespace hayate
