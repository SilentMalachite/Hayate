#include <hayate/response.hpp>

#include <cstddef>
#include <string>

namespace hayate {
namespace {

// RFC 7230 tchar。名前が token でなければヘッダを作らない。
bool is_tchar(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

bool valid_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (char c : name) {
        if (!is_tchar(c)) {
            return false;
        }
    }
    return true;
}

// CR/LF を残すと値からヘッダを生やせる。CTL を落として前後の空白を削る。
std::string sanitize_value(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) {
            continue;
        }
        out.push_back(c);
    }
    std::size_t b = 0;
    while (b < out.size() && out[b] == ' ') {
        ++b;
    }
    std::size_t e = out.size();
    while (e > b && out[e - 1] == ' ') {
        --e;
    }
    return out.substr(b, e - b);
}

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
    if (!valid_name(name)) {
        return *this;
    }
    auto clean = sanitize_value(value);
    for (auto &[k, v] : headers_) {
        if (iequals(k, name)) {
            v = std::move(clean);
            return *this;
        }
    }
    headers_.emplace_back(std::string(name), std::move(clean));
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
