#pragma once

#include <hayate/http.hpp>
#include <hayate/router.hpp>

#include <chrono>
#include <string>

namespace hayate {

// 検証を通った payload。Json の別名にしない（Extension のキーが typeid のため）。
struct Claims {
    Json json;
};

namespace mw {

struct Jwt {
    std::string secret;
    // 空なら iss を見ない。
    std::string issuer;
    // 空なら aud を見ない。
    std::string audience;
    // 負なら jwt() が投げる。
    std::chrono::seconds leeway{0};
};

// HS256 のみ。secret が空なら投げる。定義は src/jwt.cpp（OpenSSL を公開ヘッダに出さない）。
Middleware jwt(Jwt cfg);

} // namespace mw
} // namespace hayate
