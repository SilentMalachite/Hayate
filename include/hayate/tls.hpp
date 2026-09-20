#pragma once

#include <string>

namespace hayate {

struct Tls {
    std::string cert_file;
    std::string key_file;
    // 空なら鍵にパスフレーズ無しとして扱う。
    std::string key_password;
};

} // namespace hayate
