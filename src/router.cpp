#include <hayate/router.hpp>

#include <algorithm>
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

std::vector<Seg> parse_pattern(std::string_view pattern) {
    auto parts = split_path(pattern);
    std::vector<Seg> segs;
    segs.reserve(parts.size());
    for (auto &p : parts) {
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
        segs.push_back(std::move(seg));
    }
    return segs;
}

std::string join_prefix(std::string_view prefix, std::string_view path) {
    if (prefix.empty()) {
        return std::string(path);
    }
    if (path.empty()) {
        return std::string(prefix);
    }
    std::string out;
    out.reserve(prefix.size() + path.size() + 1);
    out.append(prefix);
    if (out.back() == '/' && path.front() == '/') {
        out.append(path.substr(1));
    } else if (out.back() != '/' && path.front() != '/') {
        out.push_back('/');
        out.append(path);
    } else {
        out.append(path);
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
    return false;
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

Response not_allowed(std::string allow) {
    auto r = Response::from_error({"method_not_allowed", "Method Not Allowed", 405});
    r.set_header("Allow", std::move(allow));
    return r;
}

std::string method_name(HttpMethod m) { return m == HttpMethod::post ? "POST" : "GET"; }

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
Router::Router(Router &&) noexcept = default;
Router &Router::operator=(Router &&) noexcept = default;
Router::~Router() = default;

Router &Router::use(Middleware mw) {
    impl_->mws.push_back(std::move(mw));
    return *this;
}

void Router::add(HttpMethod method, std::string_view path, Handler handler) {
    Impl::Route r;
    r.method = method;
    r.pattern = std::string(path);
    r.segs = parse_pattern(r.pattern);
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

boost::asio::awaitable<Response> Router::dispatch(Request &req) const {
    const auto parts = split_path(req.path());
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
    Handler h = compose(impl_->mws, best->handler);
    try {
        co_return co_await h(req);
    } catch (...) {
        co_return Response::from_error({"internal", "Internal Server Error", 500});
    }
}

} // namespace hayate
