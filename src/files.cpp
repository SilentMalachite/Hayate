#include "detail/ascii.hpp"
#include "detail/offload.hpp"
#include "detail/open_file.hpp"

#include <hayate/files.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace hayate {
namespace fs = std::filesystem;
namespace net = detail::net;

namespace {

Response not_found() { return Response::from_error({"not_found", "Not Found", 404}); }

// FS が期限内に返らなかった。存在の有無ではなくサーバー側の事情なので 503。
Response unavailable() { return Response::from_error({"unavailable", "Service Unavailable", 503}); }

using detail::contained;

std::string mime_type(const fs::path &p) {
    const auto ext = detail::lower_copy(p.extension().string());
    if (ext == ".html" || ext == ".htm") {
        return "text/html";
    }
    if (ext == ".css") {
        return "text/css";
    }
    if (ext == ".js") {
        return "application/javascript";
    }
    if (ext == ".json") {
        return "application/json";
    }
    if (ext == ".txt") {
        return "text/plain";
    }
    return "application/octet-stream";
}

// ここは io スレッドではなくワーカープールで走る。例外を投げない（境界を越えさせない）。
// バイトは読まない。どこを読めばよいかだけを Response に載せて Connection に渡す。
Response stat_blocking(const fs::path &root, const std::string &raw, std::uint64_t max_bytes,
                       std::shared_ptr<net::thread_pool> pool) {
    const fs::path rel{raw};
    std::error_code ec;
    auto target = fs::weakly_canonical(root / rel, ec);
    if (ec || !contained(root, target)) {
        return not_found();
    }
    std::error_code dir_ec;
    if (rel.empty() || fs::is_directory(target, dir_ec)) {
        target /= "index.html";
    }
    std::error_code canon_ec;
    const auto resolved = fs::weakly_canonical(target, canon_ec);
    if (canon_ec || !contained(root, resolved)) {
        return not_found();
    }
    // ここで開き切る。判定した fd をそのまま送るので、送出までの間に
    // symlink を差し替えられても効かない。種別・サイズも fd から取る。
    auto opened = detail::open_verified(root, resolved);
    if (!opened) {
        return not_found();
    }
    // max_bytes は配布上限。0 は無制限。メモリはサイズに依らず一定。
    if (max_bytes != 0 && opened->size > max_bytes) {
        return not_found();
    }
    auto res = Response::file(
        {std::move(resolved), opened->size, std::move(pool), std::move(opened->file)});
    res.set_header("Content-Type", mime_type(res.file_source()->path));
    return res;
}

} // namespace

Handler files(std::string_view root, std::uint64_t max_bytes, std::uint32_t io_threads,
              std::chrono::milliseconds fs_timeout) {
    fs::path root_path = fs::weakly_canonical(fs::path{std::string(root)});
    // root が未作成だと weakly_canonical が末尾 separator を残し、contained() が常に偽になる。
    if (root_path.filename().empty() && root_path.parent_path() != root_path) {
        root_path = root_path.parent_path();
    }
    // FS 呼び出しは io スレッドから外す。Handler はコピー可能が要るので shared_ptr で持つ。
    auto pool = std::make_shared<net::thread_pool>(io_threads == 0 ? 1 : io_threads);
    return [root_path = std::move(root_path), max_bytes, fs_timeout,
            pool = std::move(pool)](Request &req) -> net::awaitable<Response> {
        // view はスレッドをまたがせない。プールへ渡す前にコピーする。
        const std::string raw(req.param("path"));
        // NUL があると OS は手前で切る。contained() が見る名前と開く名前がずれる。
        if (raw.find('\0') != std::string::npos || fs::path{raw}.is_absolute()) {
            co_return not_found();
        }
        // FS が返らない・プールが詰まっているときに接続を抱えない。
        auto res =
            co_await detail::offload_until(*pool, fs_timeout, [root_path, raw, max_bytes, pool] {
                return stat_blocking(root_path, raw, max_bytes, pool);
            });
        if (!res) {
            co_return unavailable();
        }
        co_return std::move(*res);
    };
}

} // namespace hayate
