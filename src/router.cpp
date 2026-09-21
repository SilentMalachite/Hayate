#include "detail/percent.hpp"

#include <hayate/router.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hayate {
namespace {

enum class SegKind { lit, param, wild };

struct Seg {
    SegKind kind{SegKind::lit};
    std::string s;
};

int seg_score(SegKind k) {
    switch (k) {
    case SegKind::lit:
        return 2;
    case SegKind::param:
        return 1;
    case SegKind::wild:
        return 0;
    }
    return 0;
}

std::vector<std::string> split_path(std::string_view path) {
    if (path.empty() || path == "/") {
        return {};
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        auto pos = path.find('/', start);
        if (pos == std::string_view::npos) {
            out.emplace_back(path.substr(start));
            break;
        }
        out.emplace_back(path.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

// 名前の無い param / wildcard は名前で引けず、途中の wildcard には一致する要求が無い。
std::vector<Seg> parse_pattern(std::string_view pattern) {
    auto parts = split_path(pattern);
    std::vector<Seg> segs;
    segs.reserve(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) {
        auto &p = parts[i];
        Seg seg;
        if (!p.empty() && p.front() == ':') {
            seg.kind = SegKind::param;
            seg.s = p.substr(1);
        } else if (!p.empty() && p.front() == '*') {
            seg.kind = SegKind::wild;
            seg.s = p.substr(1);
        } else {
            seg.kind = SegKind::lit;
            seg.s = std::move(p);
        }
        if (seg.kind != SegKind::lit && seg.s.empty()) {
            throw std::invalid_argument("hayate::Router: unnamed segment in " +
                                        std::string(pattern));
        }
        if (seg.kind == SegKind::wild && i + 1 != parts.size()) {
            throw std::invalid_argument("hayate::Router: wildcard is not last in " +
                                        std::string(pattern));
        }
        segs.push_back(std::move(seg));
    }
    return segs;
}

bool same_shape(const std::vector<Seg> &a, const std::vector<Seg> &b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const Seg &x, const Seg &y) {
        return x.kind == y.kind && (x.kind != SegKind::lit || x.s == y.s);
    });
}

std::string join_prefix(std::string_view prefix, std::string_view path) {
    std::string joined(prefix);
    if (!prefix.empty() && !path.empty() && prefix.back() != '/' && path.front() != '/') {
        joined.push_back('/');
    }
    joined.append(path);
    // 継ぎ目だけ見ると prefix 内の `//` が残る。全体で畳む。
    std::string out;
    out.reserve(joined.size());
    for (const char c : joined) {
        if (c == '/' && !out.empty() && out.back() == '/') {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

struct Match {
    bool path_ok{false};
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<int> score;
};

Match match_path(const std::vector<Seg> &segs, const std::vector<std::string> &parts) {
    Match m;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < segs.size()) {
        const auto &seg = segs[i];
        if (seg.kind == SegKind::wild) {
            std::string rest;
            for (std::size_t k = j; k < parts.size(); ++k) {
                if (k != j) {
                    rest.push_back('/');
                }
                rest += parts[k];
            }
            m.params.emplace_back(seg.s, std::move(rest));
            m.score.push_back(seg_score(seg.kind));
            j = parts.size();
            ++i;
            break;
        }
        if (j >= parts.size()) {
            return {};
        }
        if (seg.kind == SegKind::lit) {
            if (seg.s != parts[j]) {
                return {};
            }
        } else {
            // 空を受けるのは wildcard だけ。
            if (parts[j].empty()) {
                return {};
            }
            m.params.emplace_back(seg.s, parts[j]);
        }
        m.score.push_back(seg_score(seg.kind));
        ++i;
        ++j;
    }
    if (i != segs.size() || j != parts.size()) {
        return {};
    }
    m.path_ok = true;
    return m;
}

bool better_score(const std::vector<int> &a, const std::vector<int> &b) {
    const auto n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    // 同じ形は登録できないので、長さが違うのは片方が空の wildcard で終わるときだけ。
    return a.size() < b.size();
}

Handler compose(const std::vector<Middleware> &mws, Handler h) {
    for (auto it = mws.rbegin(); it != mws.rend(); ++it) {
        Middleware mw = *it;
        Handler next = std::move(h);
        h = [mw, next](Request &req) -> boost::asio::awaitable<Response> {
            co_return co_await mw(req, next);
        };
    }
    return h;
}

Response not_found() { return Response::from_error({"not_found", "Not Found", 404}); }

Response internal_error() {
    return Response::from_error({"internal", "Internal Server Error", 500});
}

Response not_allowed(std::string allow) {
    auto r = Response::from_error({"method_not_allowed", "Method Not Allowed", 405});
    r.set_header("Allow", std::move(allow));
    return r;
}

std::string method_name(HttpMethod m) {
    switch (m) {
    case HttpMethod::post:
        return "POST";
    case HttpMethod::options:
        return "OPTIONS";
    case HttpMethod::get:
        return "GET";
    case HttpMethod::unknown:
        return "";
    }
    return "";
}

} // namespace

struct Router::Impl {
    struct Route {
        HttpMethod method{HttpMethod::get};
        std::string pattern;
        std::vector<Seg> segs;
        Handler handler;
    };
    std::vector<Middleware> mws;
    std::vector<Route> routes;
};

Router::Router() : impl_(std::make_unique<Impl>()) {}
Router::~Router() = default;

Router &Router::use(Middleware mw) {
    impl_->mws.push_back(std::move(mw));
    return *this;
}

void Router::add(HttpMethod method, std::string_view path, Handler handler) {
    // unknown は HEAD や PUT に一致し、OPTIONS は CORS の preflight と重なる。
    if (method != HttpMethod::get && method != HttpMethod::post) {
        throw std::invalid_argument("hayate::Router: only GET and POST routes, got " +
                                    std::string(path));
    }
    Impl::Route r;
    r.method = method;
    r.pattern = std::string(path);
    r.segs = parse_pattern(r.pattern);
    // 同じ形の後の方には一致する要求が無い。黙って死なせず登録時に落とす。
    for (const auto &other : impl_->routes) {
        if (other.method == method && same_shape(other.segs, r.segs)) {
            throw std::invalid_argument("hayate::Router: duplicate route " + method_name(method) +
                                        " " + r.pattern);
        }
    }
    r.handler = std::move(handler);
    impl_->routes.push_back(std::move(r));
}

Router &Router::group(std::string_view prefix, std::function<void(Router &)> fn) {
    Router child;
    fn(child);
    for (auto &r : child.impl_->routes) {
        auto joined = join_prefix(prefix, r.pattern);
        add(r.method, joined, compose(child.impl_->mws, std::move(r.handler)));
    }
    return *this;
}

std::vector<std::pair<HttpMethod, std::string>> Router::route_table() const {
    std::vector<std::pair<HttpMethod, std::string>> out;
    out.reserve(impl_->routes.size());
    for (const auto &r : impl_->routes) {
        out.emplace_back(r.method, r.pattern);
    }
    return out;
}

boost::asio::awaitable<Response> Router::dispatch(Request &req) const {
    // 中の catch はハンドラだけを守る。MW 自身が投げた分はここで受けないと接続が落ちる。
    // 合成も MW をコピーするので投げうる。try の中に置く。
    try {
        Handler inner = [this](Request &r) -> boost::asio::awaitable<Response> {
            co_return co_await dispatch_route(r);
        };
        Handler h = compose(impl_->mws, std::move(inner));
        co_return co_await h(req);
    } catch (...) {
        co_return internal_error();
    }
}

boost::asio::awaitable<Response> Router::dispatch_route(Request &req) const {
    // 分けてから復号する。先に復号すると %2F が区切りになって別のパスに化ける。
    auto parts = split_path(req.path());
    for (auto &p : parts) {
        p = detail::percent_decode(p, false);
    }
    const Impl::Route *best = nullptr;
    Match best_match;
    bool path_ok = false;
    std::vector<HttpMethod> allow;

    for (const auto &r : impl_->routes) {
        auto m = match_path(r.segs, parts);
        if (!m.path_ok) {
            continue;
        }
        path_ok = true;
        if (std::find(allow.begin(), allow.end(), r.method) == allow.end()) {
            allow.push_back(r.method);
        }
        if (r.method != req.method()) {
            continue;
        }
        if (best == nullptr || better_score(m.score, best_match.score)) {
            best = &r;
            best_match = std::move(m);
        }
    }

    if (!path_ok) {
        co_return not_found();
    }
    if (best == nullptr) {
        std::string allow_s;
        for (std::size_t i = 0; i < allow.size(); ++i) {
            if (i != 0) {
                allow_s += ", ";
            }
            allow_s += method_name(allow[i]);
        }
        co_return not_allowed(std::move(allow_s));
    }

    req.params_ = std::move(best_match.params);
    try {
        co_return co_await best->handler(req);
    } catch (...) {
        co_return internal_error();
    }
}

} // namespace hayate
