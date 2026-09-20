#include "detail/ascii.hpp"

#include <hayate/files.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <utility>

namespace hayate {
namespace fs = std::filesystem;

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

} // namespace

Handler files(std::string_view root, std::uint64_t max_bytes) {
    fs::path root_path = fs::weakly_canonical(fs::path{std::string(root)});
    // root が未作成だと weakly_canonical が末尾 separator を残し、contained() が常に偽になる。
    if (root_path.filename().empty() && root_path.parent_path() != root_path) {
        root_path = root_path.parent_path();
    }
    return [root_path = std::move(root_path),
            max_bytes](Request &req) -> boost::asio::awaitable<Response> {
        const std::string raw(req.param("path"));
        // NUL があると OS は手前で切る。contained() が見る名前と開く名前がずれる。
        if (raw.find('\0') != std::string::npos) {
            co_return not_found();
        }
        const fs::path rel{raw};
        if (rel.is_absolute()) {
            co_return not_found();
        }
        auto target = fs::weakly_canonical(root_path / rel);
        if (!contained(root_path, target)) {
            co_return not_found();
        }
        std::error_code dir_ec;
        if (rel.empty() || fs::is_directory(target, dir_ec)) {
            target /= "index.html";
        }
        if (!contained(root_path, fs::weakly_canonical(target))) {
            co_return not_found();
        }
        std::error_code file_ec;
        if (!fs::is_regular_file(target, file_ec)) {
            co_return not_found();
        }
        // 全文をメモリに読むので、読む前に上限で切る。
        std::error_code size_ec;
        const auto size = fs::file_size(target, size_ec);
        if (size_ec || size > max_bytes) {
            co_return not_found();
        }
        std::ifstream in(target, std::ios::binary);
        if (!in) {
            co_return not_found();
        }
        std::string body;
        body.resize(static_cast<std::size_t>(size));
        in.read(body.data(), static_cast<std::streamsize>(size));
        body.resize(static_cast<std::size_t>(in.gcount()));
        auto res = Response::text(body);
        res.set_header("Content-Type", mime_type(target));
        co_return res;
    };
}

} // namespace hayate
