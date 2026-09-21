#pragma once

#include <stdlib.h>

#include <cerrno>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

// ctest は並行にプロセスを走らせる。名前をアドレスや固定値から作ると別プロセスと衝突し、
// 一方の remove_all が他方のファイルを消す。mkdtemp は作成まで原子的に一意にする。
struct TempDir {
    std::filesystem::path dir;

    TempDir() {
        auto tmpl = (std::filesystem::temp_directory_path() / "hayate_XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        dir = std::move(tmpl);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};
