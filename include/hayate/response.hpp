#pragma once

#include <hayate/error.hpp>
#include <hayate/http.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hayate {

class Response {
  public:
    std::uint16_t status() const noexcept;
    std::string_view body() const noexcept;
    std::string_view header(std::string_view name) const noexcept;
    Response &status(std::uint16_t code) noexcept;
    Response &set_header(std::string_view name, std::string_view value);

    static Response text(std::string_view s);
    static Response json(const Json &v);
    static Response no_content();
    static Response from_error(const Error &e);

    template <typename F> void for_each_header(F &&fn) const {
        for (const auto &[k, v] : headers_) {
            fn(k, v);
        }
    }

  private:
    std::uint16_t status_{200};
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
};

} // namespace hayate
