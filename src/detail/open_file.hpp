#pragma once

#include <boost/beast/core/file.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#if !defined(BOOST_BEAST_USE_POSIX_FILE)
#error "hayate は POSIX のファイル API を前提にする（Windows 版は別リポジトリ）"
#endif

namespace hayate::detail {

namespace fs = std::filesystem;

// 検証済みの開いたファイル。Response が運び、Connection が読む。
struct OpenFile {
    boost::beast::file f;
};

struct OpenedFile {
    std::shared_ptr<OpenFile> file;
    std::uint64_t size{0};
};

// cand が root と同じか、その下にあるか。文字列前置ではなくパス要素で比べる。
// 「root の直後が区切りか」で見ると、root が `/` のとき配下が全部外れる。
inline bool contained(const fs::path &root, const fs::path &cand) {
    const auto [r, c] = std::mismatch(root.begin(), root.end(), cand.begin(), cand.end());
    return r == root.end();
}

// 開いた fd 自身の実パス。パス名から辿り直さないための確認に使う。
inline std::optional<fs::path> path_of(int fd) {
#if defined(__APPLE__)
    std::string buf(PATH_MAX, '\0');
    if (::fcntl(fd, F_GETPATH, buf.data()) == -1) {
        return std::nullopt;
    }
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    return fs::path(std::move(buf));
#elif defined(__linux__)
    const auto link = "/proc/self/fd/" + std::to_string(fd);
    std::error_code ec;
    auto target = fs::read_symlink(link, ec);
    if (ec) {
        return std::nullopt;
    }
    return target;
#else
#error "fd の実パスを取る方法がこのプラットフォームには無い"
#endif
}

// canonical を開き、開いた fd が root 内の通常ファイルであることを確かめる。
// 検証と送出の間に経路を辿り直さないので、symlink の差し替えが効かない。
// FIFO やデバイスで待たされないよう非ブロッキングで開き、通常ファイルと
// 分かってから外す。
inline std::optional<OpenedFile> open_verified(const fs::path &root, const fs::path &canonical) {
    const int fd = ::open(canonical.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd == -1) {
        return std::nullopt;
    }
    auto opened = std::make_shared<OpenFile>();
    opened->f.native_handle(fd);

    struct ::stat st{};
    if (::fstat(fd, &st) == -1 || !S_ISREG(st.st_mode)) {
        return std::nullopt;
    }
    const auto real = path_of(fd);
    if (!real || !contained(root, *real)) {
        return std::nullopt;
    }
    // 通常ファイルの read は待たない。EAGAIN を返させないために外す。
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags == -1 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        return std::nullopt;
    }
    return OpenedFile{std::move(opened), static_cast<std::uint64_t>(st.st_size)};
}

} // namespace hayate::detail
