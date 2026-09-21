#include "detail/files.hpp"
#include "detail/ascii.hpp"
#include "detail/offload.hpp"
#include "detail/open_file.hpp"

#include <hayate/files.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

// 開いて確かめたファイル。Response は io 側で組む。
struct Found {
    fs::path path;
    std::uint64_t size{0};
    std::shared_ptr<detail::OpenFile> file;
};

// ここは io スレッドではなくワーカープールで走る。例外を投げない（境界を越えさせない）。
// バイトは読まない。どこを読めばよいかだけを返す。空は 404。
// プールは受け取らない。期限切れで置いていった仕事がプールを持つと、プールが App より長く生き、
// 完了を壊れた io_context に post する。
std::optional<Found> stat_blocking(const fs::path &root, const std::string &raw,
                                   std::uint64_t max_bytes) {
    const fs::path rel{raw};
    std::error_code ec;
    auto target = fs::weakly_canonical(root / rel, ec);
    if (ec || !contained(root, target)) {
        return std::nullopt;
    }
    std::error_code dir_ec;
    if (rel.empty() || fs::is_directory(target, dir_ec)) {
        target /= "index.html";
    }
    std::error_code canon_ec;
    const auto resolved = fs::weakly_canonical(target, canon_ec);
    if (canon_ec || !contained(root, resolved)) {
        return std::nullopt;
    }
    // ここで開き切る。判定した fd をそのまま送るので、送出までの間に
    // symlink を差し替えられても効かない。種別・サイズも fd から取る。
    auto opened = detail::open_verified(root, resolved);
    if (!opened) {
        return std::nullopt;
    }
    // max_bytes は配布上限。0 は無制限。メモリはサイズに依らず一定。
    if (max_bytes != 0 && opened->size > max_bytes) {
        return std::nullopt;
    }
    return Found{resolved, opened->size, std::move(opened->file)};
}

} // namespace

Handler detail::files_on(fs::path root, std::uint64_t max_bytes,
                         std::chrono::milliseconds fs_timeout,
                         std::shared_ptr<net::thread_pool> pool) {
    return [root_path = std::move(root), max_bytes, fs_timeout,
            pool = std::move(pool)](Request &req) -> net::awaitable<Response> {
        // view はスレッドをまたがせない。プールへ渡す前にコピーする。
        const std::string raw(req.param("path"));
        // NUL があると OS は手前で切る。contained() が見る名前と開く名前がずれる。
        if (raw.find('\0') != std::string::npos || fs::path{raw}.is_absolute()) {
            co_return not_found();
        }
        // FS が返らない・プールが詰まっているときに接続を抱えない。
        auto got = co_await detail::offload_until(*pool, fs_timeout, [root_path, raw, max_bytes] {
            return stat_blocking(root_path, raw, max_bytes);
        });
        if (!got) {
            co_return unavailable();
        }
        if (!*got) {
            co_return not_found();
        }
        auto &found = **got;
        auto res = Response::file({std::move(found.path), found.size, pool, std::move(found.file)});
        res.set_header("Content-Type", mime_type(res.file_source()->path));
        co_return res;
    };
}

Handler files(std::string_view root, std::uint64_t max_bytes, std::uint32_t io_threads,
              std::chrono::milliseconds fs_timeout) {
    // 先に絶対パスにする。libstdc++ の weakly_canonical は未作成の相対パスを相対のまま返し、
    // 後で作られると候補（絶対パス）が contained() で弾かれる。
    std::error_code abs_ec;
    auto abs_root = fs::absolute(fs::path{std::string(root)}, abs_ec);
    if (abs_ec) {
        abs_root = fs::path{std::string(root)};
    }
    fs::path root_path = fs::weakly_canonical(abs_root);
    // root が未作成だと weakly_canonical が末尾 separator を残し、contained() が常に偽になる。
    if (root_path.filename().empty() && root_path.parent_path() != root_path) {
        root_path = root_path.parent_path();
    }
    // FS 呼び出しは io スレッドから外す。Handler はコピー可能が要るので shared_ptr で持つ。
    return detail::files_on(std::move(root_path), max_bytes, fs_timeout,
                            std::make_shared<net::thread_pool>(io_threads == 0 ? 1 : io_threads));
}

} // namespace hayate
