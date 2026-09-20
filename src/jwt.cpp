#include "detail/ascii.hpp"
#include "detail/base64.hpp"
#include "detail/hmac.hpp"

#include <hayate/jwt.hpp>
#include <hayate/request.hpp>
#include <hayate/response.hpp>

#include <openssl/crypto.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace hayate::mw {
namespace {

// どの検査で落ちたかは返さない。
Response unauthorized() {
    auto res = Response::from_error({"unauthorized", "Unauthorized", 401});
    res.set_header("WWW-Authenticate", "Bearer");
    return res;
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// "Bearer x" から x を取る。スキームは大小無視。取れなければ空。
std::string_view bearer(std::string_view value) {
    constexpr std::string_view scheme = "Bearer";
    if (value.size() <= scheme.size() || !detail::iequals(value.substr(0, scheme.size()), scheme)) {
        return {};
    }
    if (value[scheme.size()] != ' ') {
        return {};
    }
    auto token = value.substr(scheme.size() + 1);
    while (!token.empty() && token.front() == ' ') {
        token.remove_prefix(1);
    }
    return token;
}

// 早期 return で長さを漏らさないよう、長さ一致を先に見てから定数時間比較する。
bool same_signature(std::string_view a, std::string_view b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool audience_ok(const Json &payload, const std::string &want) {
    if (want.empty()) {
        return true;
    }
    const auto it = payload.find("aud");
    if (it == payload.end()) {
        return false;
    }
    if (it->is_string()) {
        return it->get<std::string>() == want;
    }
    if (it->is_array()) {
        for (const auto &v : *it) {
            if (v.is_string() && v.get<std::string>() == want) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

Middleware jwt(Jwt cfg) {
    // 空の secret は「全部通る」と同じ。設定時に落とす。
    if (cfg.secret.empty()) {
        throw std::invalid_argument("hayate::mw::jwt: secret must not be empty");
    }
    return [cfg = std::move(cfg)](Request &req, Next next) -> boost::asio::awaitable<Response> {
        const auto token = bearer(req.header("authorization"));
        if (token.empty()) {
            co_return unauthorized();
        }
        const auto first = token.find('.');
        if (first == std::string_view::npos) {
            co_return unauthorized();
        }
        const auto second = token.find('.', first + 1);
        if (second == std::string_view::npos ||
            token.find('.', second + 1) != std::string_view::npos) {
            co_return unauthorized();
        }
        const auto header_b64 = token.substr(0, first);
        const auto payload_b64 = token.substr(first + 1, second - first - 1);

        const auto header_raw = detail::base64url_decode(header_b64);
        const auto payload_raw = detail::base64url_decode(payload_b64);
        const auto signature = detail::base64url_decode(token.substr(second + 1));
        if (!header_raw || !payload_raw || !signature) {
            co_return unauthorized();
        }

        // alg はここで断つ。none もアルゴリズム混同も署名を見る前に落ちる。
        const auto header = Json::parse(*header_raw, nullptr, false);
        if (header.is_discarded() || !header.is_object() ||
            header.value("alg", std::string{}) != "HS256") {
            co_return unauthorized();
        }

        std::string signing;
        signing.reserve(header_b64.size() + 1 + payload_b64.size());
        signing.append(header_b64);
        signing.push_back('.');
        signing.append(payload_b64);
        if (!same_signature(detail::hmac_sha256(cfg.secret, signing), *signature)) {
            co_return unauthorized();
        }

        auto payload = Json::parse(*payload_raw, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) {
            co_return unauthorized();
        }
        const auto now = now_seconds();
        const auto leeway = static_cast<std::int64_t>(cfg.leeway.count());
        // exp 無しは永久トークンになる。RFC 上は任意だが締める。
        const auto exp = payload.find("exp");
        if (exp == payload.end() || !exp->is_number() || now > exp->get<std::int64_t>() + leeway) {
            co_return unauthorized();
        }
        const auto nbf = payload.find("nbf");
        if (nbf != payload.end() &&
            (!nbf->is_number() || now + leeway < nbf->get<std::int64_t>())) {
            co_return unauthorized();
        }
        if (!cfg.issuer.empty() && payload.value("iss", std::string{}) != cfg.issuer) {
            co_return unauthorized();
        }
        if (!audience_ok(payload, cfg.audience)) {
            co_return unauthorized();
        }

        req.set<Claims>(Claims{std::move(payload)});
        co_return co_await next(req);
    };
}

} // namespace hayate::mw
