#pragma once

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace hayate::detail {

inline constexpr std::size_t kHmacSha256Size = 32;

// 生の 32 バイトを返す。計算に失敗したら空を返す（空署名として一致させないため）。
inline std::optional<std::string> hmac_sha256(std::string_view key, std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> buf{};
    unsigned int len = 0;
    const unsigned char *out =
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char *>(data.data()), data.size(), buf.data(), &len);
    if (out == nullptr || len != kHmacSha256Size) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char *>(buf.data()), len);
}

// 計算に失敗した mac はどの署名とも一致させない。空署名とも。
// 早期 return で長さを漏らさないよう、長さ一致を先に見てから定数時間比較する。
inline bool signature_matches(const std::optional<std::string> &mac, std::string_view sig) {
    return mac && mac->size() == sig.size() &&
           CRYPTO_memcmp(mac->data(), sig.data(), sig.size()) == 0;
}

} // namespace hayate::detail
