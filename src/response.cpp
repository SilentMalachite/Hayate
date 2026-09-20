#include <hayate/response.hpp>

namespace hayate {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

} // namespace

std::uint16_t Response::status() const noexcept { return status_; }

std::string_view Response::body() const noexcept { return body_; }

std::string_view Response::header(std::string_view name) const noexcept {
    for (const auto &[k, v] : headers_) {
        if (iequals(k, name)) {
            return v;
        }
    }
    return {};
}

Response &Response::status(std::uint16_t code) noexcept {
    status_ = code;
    return *this;
}

Response &Response::set_header(std::string_view name, std::string_view value) {
    for (auto &[k, v] : headers_) {
        if (iequals(k, name)) {
            v = std::string(value);
            return *this;
        }
    }
    headers_.emplace_back(std::string(name), std::string(value));
    return *this;
}

Response Response::text(std::string_view s) {
    Response r;
    r.status_ = 200;
    r.body_ = std::string(s);
    r.set_header("Content-Type", "text/plain; charset=utf-8");
    return r;
}

Response Response::json(const Json &v) {
    Response r;
    r.status_ = 200;
    r.body_ = v.dump();
    r.set_header("Content-Type", "application/json");
    return r;
}

Response Response::no_content() {
    Response r;
    r.status_ = 204;
    return r;
}

Response Response::from_error(const Error &e) {
    Response r;
    r.status_ = e.http_status;
    r.body_ = e.message;
    r.set_header("Content-Type", "text/plain; charset=utf-8");
    return r;
}

} // namespace hayate
