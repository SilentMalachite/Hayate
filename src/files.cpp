#include "detail/ascii.hpp"
#include "detail/offload.hpp"

#include <hayate/files.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <utility>

namespace hayate {
namespace fs = std::filesystem;
namespace net = detail::net;

namespace {

Response not_found() { return Response::from_error({"not_found", "Not Found", 404}); }

bool contained(const fs::path &root, const fs::path &cand) {
    const auto r = root.native();
    const auto c = cand.native();
    if (c == r) {
        return true;
    }
    const auto sep = static_cast<fs::path::value_type>(fs::path::preferred_separator);
    return c.size() > r.size() && c.compare(0, r.size(), r) == 0 && c[r.size()] == sep;
}

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
Response load_blocking(const fs::path &root, const std::string &raw, std::uint64_t max_bytes) {
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
    std::error_code file_ec;
    if (!fs::is_regular_file(target, file_ec)) {
        return not_found();
    }
    // 全文をメモリに読むので、読む前に上限で切る。
    std::error_code size_ec;
    const auto size = fs::file_size(target, size_ec);
    if (size_ec || size > max_bytes) {
        return not_found();
    }
    std::ifstream in(target, std::ios::binary);
    if (!in) {
        return not_found();
    }
    std::string body;
    body.resize(static_cast<std::size_t>(size));
    in.read(body.data(), static_cast<std::streamsize>(size));
    body.resize(static_cast<std::size_t>(in.gcount()));
    auto res = Response::text(body);
    res.set_header("Content-Type", mime_type(target));
    return res;
}

} // namespace

Handler files(std::string_view root, std::uint64_t max_bytes, std::uint32_t io_threads) {
    fs::path root_path = fs::weakly_canonical(fs::path{std::string(root)});
    // root が未作成だと weakly_canonical が末尾 separator を残し、contained() が常に偽になる。
    if (root_path.filename().empty() && root_path.parent_path() != root_path) {
        root_path = root_path.parent_path();
    }
    // FS 呼び出しは io スレッドから外す。Handler はコピー可能が要るので shared_ptr で持つ。
    auto pool = std::make_shared<net::thread_pool>(io_threads == 0 ? 1 : io_threads);
    return [root_path = std::move(root_path), max_bytes,
            pool = std::move(pool)](Request &req) -> net::awaitable<Response> {
        // view はスレッドをまたがせない。プールへ渡す前にコピーする。
        const std::string raw(req.param("path"));
        // NUL があると OS は手前で切る。contained() が見る名前と開く名前がずれる。
        if (raw.find('\0') != std::string::npos || fs::path{raw}.is_absolute()) {
            co_return not_found();
        }
        co_return co_await detail::offload(*pool, [root_path, raw, max_bytes] {
            return load_blocking(root_path, raw, max_bytes);
        });
    };
}

} // namespace hayate
