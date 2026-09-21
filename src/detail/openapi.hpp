#pragma once

#include "percent.hpp"

#include <hayate/http.hpp>
#include <hayate/openapi.hpp>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hayate::detail {

struct PathDoc {
    std::string path;
    // name, wildcard
    std::vector<std::pair<std::string, bool>> params;
    // パラメータ名を消した形。"/users/{}"、"/assets/{*}"。名前だけ違うパスを同一視するのに使う。
    std::string shape;
};

// "/users/:id" -> "/users/{id}"、"/assets/*path" -> "/assets/{path}"。
// 規則の正本は SPEC の『ルーティング』節。
inline PathDoc split_pattern(std::string_view pattern) {
    PathDoc out;
    if (pattern.empty() || pattern == "/") {
        out.path = "/";
        out.shape = "/";
        return out;
    }
    if (pattern.front() == '/') {
        pattern.remove_prefix(1);
    }
    for (;;) {
        const auto slash = pattern.find('/');
        const auto part = pattern.substr(0, slash);
        out.path.push_back('/');
        out.shape.push_back('/');
        if (part.size() > 1 && (part.front() == ':' || part.front() == '*')) {
            const auto name = std::string(part.substr(1));
            out.path += "{" + name + "}";
            out.shape += part.front() == '*' ? "{*}" : "{}";
            out.params.emplace_back(name, part.front() == '*');
        } else {
            // 静的な `{id}` をそのまま出すとテンプレート変数に化け、形も param と同じになる。
            const auto lit = percent_encode_pchar(part);
            out.path += lit;
            out.shape += lit;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        pattern.remove_prefix(slash + 1);
    }
    return out;
}

// 登録できるのは GET と POST だけ。OPTIONS は preflight 用にフレームワークが扱う。
inline const char *method_key(HttpMethod m) {
    switch (m) {
    case HttpMethod::get:
        return "get";
    case HttpMethod::post:
        return "post";
    default:
        return nullptr;
    }
}

inline Json build_openapi(const std::vector<std::pair<HttpMethod, std::string>> &routes,
                          const OpenApiInfo &info) {
    Json paths = Json::object();
    // /users/{id} と /users/{name} は OpenAPI では同じテンプレート。
    // 別キーにすると不正な文書になる。
    std::map<std::string, PathDoc> first_by_shape;
    for (const auto &[method, pattern] : routes) {
        const auto *key = method_key(method);
        if (key == nullptr) {
            continue;
        }
        auto doc = split_pattern(pattern);
        // 形が同じなら、最初に登録したルートのパスとパラメータ名に揃える。形に種別
        // （{} と {*}）まで入っているので、位置で対応させてよい。
        const auto [first, inserted] = first_by_shape.try_emplace(doc.shape, doc);
        if (!inserted) {
            doc.path = first->second.path;
            for (std::size_t i = 0; i < doc.params.size() && i < first->second.params.size(); ++i) {
                doc.params[i].first = first->second.params[i].first;
            }
        }
        Json op = Json::object();
        if (!doc.params.empty()) {
            Json params = Json::array();
            for (const auto &[name, wildcard] : doc.params) {
                Json p = Json::object({{"name", name},
                                       {"in", "path"},
                                       {"required", true},
                                       {"schema", Json::object({{"type", "string"}})}});
                // OpenAPI の {} は本来 `/` を含まない。違いを機械可読な形で残す。
                if (wildcard) {
                    p["x-hayate-wildcard"] = true;
                }
                params.push_back(std::move(p));
            }
            op["parameters"] = std::move(params);
        }
        // ステータスを知らないので創作しない。
        op["responses"] = Json::object({{"default", Json::object({{"description", "Response"}})}});
        paths[doc.path][key] = std::move(op);
    }
    return Json::object({{"openapi", "3.1.0"},
                         {"info", Json::object({{"title", info.title}, {"version", info.version}})},
                         {"paths", std::move(paths)}});
}

} // namespace hayate::detail
