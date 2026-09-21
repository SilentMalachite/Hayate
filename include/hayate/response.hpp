#pragma once

#include <hayate/error.hpp>
#include <hayate/http.hpp>

#include <boost/asio/thread_pool.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace hayate {

namespace detail {
// 検証済みの開いたファイル。定義は src/detail/open_file.hpp（OS の型を公開ヘッダに出さない）。
struct OpenFile;
} // namespace detail

class Response {
  public:
    // 全文をメモリに積まずに送るファイル。読みは pool で走らせる。
    struct FileSource {
        // Content-Type の判定に使う。送出時にこのパスを辿り直さない。
        std::filesystem::path path;
        std::uint64_t size{0};
        std::shared_ptr<boost::asio::thread_pool> pool;
        // root 内であることを確かめた上で開いたファイル。送るのはこれ。
        std::shared_ptr<detail::OpenFile> file;
    };

    std::uint16_t status() const noexcept;
    // FileSource のときは空。バイトはまだ読んでいない。
    std::string_view body() const noexcept;
    bool is_file() const noexcept;
    const FileSource *file_source() const noexcept;
    std::string_view header(std::string_view name) const noexcept;
    Response &status(std::uint16_t code) noexcept;
    Response &set_header(std::string_view name, std::string_view value);

    static Response text(std::string_view s);
    static Response json(const Json &v);
    static Response no_content();
    static Response from_error(const Error &e);
    static Response file(FileSource src);

    template <typename F> void for_each_header(F &&fn) const {
        for (const auto &[k, v] : headers_) {
            fn(k, v);
        }
    }

  private:
    std::uint16_t status_{200};
    std::vector<std::pair<std::string, std::string>> headers_;
    std::variant<std::string, FileSource> body_;
};

} // namespace hayate
