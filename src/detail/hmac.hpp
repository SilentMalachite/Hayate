#pragma once

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <string>
#include <string_view>

namespace hayate::detail {

// 生の 32 バイトを返す。
inline std::string hmac_sha256(std::string_view key, std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> buf{};
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(data.data()), data.size(), buf.data(), &len);
    return std::string(reinterpret_cast<const char *>(buf.data()), len);
}

} // namespace hayate::detail
