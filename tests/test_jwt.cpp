#include "http_client.hpp"
#include "jwt_token.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace http = boost::beast::http;
using hayate::Json;

namespace {

const std::string kSecret = "s3cr3t-for-tests";

hayate::mw::Jwt base_cfg() { return {.secret = kSecret}; }

// sub を返すだけの保護ルート。
void protect(hayate::App &app, hayate::mw::Jwt cfg) {
    app.use(hayate::mw::jwt(std::move(cfg)));
    app.get("/me", [](hayate::Request &req) {
        auto *c = req.get<hayate::Claims>();
        return hayate::Response::text(c == nullptr ? "" : c->json.value("sub", ""));
    });
}

std::string to_hex(std::string_view raw) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out += digits[b >> 4];
        out += digits[b & 0x0f];
    }
    return out;
}

HttpCall call(std::uint16_t port, const std::string &token, std::vector<std::string> want = {}) {
    std::vector<std::pair<std::string, std::string>> headers;
    if (!token.empty()) {
        headers.emplace_back("Authorization", "Bearer " + token);
    }
    return http_call("127.0.0.1", port, http::verb::get, "/me", {}, {}, std::chrono::seconds(2),
                     headers, std::move(want));
}

} // namespace

TEST(Jwt, ValidTokenPasses) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok = make_token(hs256_header(),
                                Json::object({{"sub", "alice"}, {"exp", now_s() + 300}}), kSecret);
    auto r = call(srv.port(), tok);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "alice");
}

TEST(Jwt, MissingHeaderIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    auto r = call(srv.port(), "", {"WWW-Authenticate"});
    EXPECT_EQ(r.status, 401);
    EXPECT_NE(r.extra["WWW-Authenticate"].find("Bearer"), std::string::npos);
}

TEST(Jwt, MalformedTokenIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    for (const auto *bad : {"abc", "a.b", "a.b.c.d", "..", "a..c"}) {
        EXPECT_EQ(call(srv.port(), bad).status, 401) << bad;
    }
}

TEST(Jwt, TamperedSignatureIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto header_b64 = base64url_encode(hs256_header().dump());
    const auto payload_b64 =
        base64url_encode(Json::object({{"sub", "alice"}, {"exp", now_s() + 300}}).dump());
    // base64url の末尾文字は未使用ビットを含む。復号後のバイトを変える。
    auto sig = hayate::detail::hmac_sha256(kSecret, header_b64 + "." + payload_b64).value();
    sig[0] = static_cast<char>(sig[0] ^ 0x01);
    const auto tok = header_b64 + "." + payload_b64 + "." + base64url_encode(sig);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

TEST(Jwt, TamperedPayloadIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto header_b64 = base64url_encode(hs256_header().dump());
    const auto good = Json::object({{"sub", "alice"}, {"exp", now_s() + 300}});
    const auto tok = sign_parts(header_b64, base64url_encode(good.dump()), kSecret);
    const auto sig = tok.substr(tok.rfind('.') + 1);
    const auto evil = Json::object({{"sub", "root"}, {"exp", now_s() + 300}});
    EXPECT_EQ(call(srv.port(), header_b64 + "." + base64url_encode(evil.dump()) + "." + sig).status,
              401);
}

TEST(Jwt, MissingExpIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok = make_token(hs256_header(), Json::object({{"sub", "alice"}}), kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

TEST(Jwt, ExpiredIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok = make_token(hs256_header(),
                                Json::object({{"sub", "alice"}, {"exp", now_s() - 300}}), kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

TEST(Jwt, LeewayAllowsRecentlyExpired) {
    TestServer srv([](hayate::App &app) {
        auto cfg = base_cfg();
        cfg.leeway = std::chrono::seconds(60);
        protect(app, cfg);
    });
    const auto tok =
        make_token(hs256_header(), Json::object({{"sub", "alice"}, {"exp", now_s() - 5}}), kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 200);
}

TEST(Jwt, FutureNbfIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok = make_token(
        hs256_header(),
        Json::object({{"sub", "alice"}, {"exp", now_s() + 300}, {"nbf", now_s() + 300}}), kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

// nbf が int64 を超えると負値に化けて「過去」になり、未来の nbf が通っていた。
TEST(Jwt, NbfBeyondInt64Is401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok = make_token(hs256_header(),
                                Json::object({{"sub", "alice"},
                                              {"exp", now_s() + 300},
                                              {"nbf", std::numeric_limits<std::uint64_t>::max()}}),
                                kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

// exp + leeway が溢れると、期限切れでないトークンを 401 にしていた。
TEST(Jwt, ExpAtInt64MaxIsNotExpired) {
    TestServer srv([](hayate::App &app) {
        auto cfg = base_cfg();
        cfg.leeway = std::chrono::seconds(60);
        protect(app, cfg);
    });
    const auto tok = make_token(
        hs256_header(),
        Json::object({{"sub", "alice"}, {"exp", std::numeric_limits<std::int64_t>::max()}}),
        kSecret);
    auto r = call(srv.port(), tok);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "alice");
}

TEST(Jwt, FractionalExpIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok = make_token(
        hs256_header(),
        Json::object({{"sub", "alice"}, {"exp", static_cast<double>(now_s() + 300) + 0.5}}),
        kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

TEST(Jwt, HugeFloatExpIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok =
        make_token(hs256_header(), Json::object({{"sub", "alice"}, {"exp", 1e100}}), kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

TEST(Jwt, NegativeLeewayThrows) {
    EXPECT_THROW(hayate::mw::jwt({.secret = kSecret, .leeway = std::chrono::seconds(-1)}),
                 std::exception);
}

// HMAC が失敗して空を返したとき、空署名と一致してはいけない。
TEST(Jwt, EmptySignatureIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto payload = Json::object({{"sub", "root"}, {"exp", now_s() + 300}});
    const auto tok =
        base64url_encode(hs256_header().dump()) + "." + base64url_encode(payload.dump()) + ".";
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

// テスト側の署名も同じ関数を使う。既知ベクタが無いと両方同時に壊れても気づけない。
TEST(Hmac, Sha256MatchesRfc4231Vector) {
    const auto mac = hayate::detail::hmac_sha256("Jefe", "what do ya want for nothing?");
    ASSERT_TRUE(mac.has_value());
    EXPECT_EQ(mac->size(), 32u);
    EXPECT_EQ(to_hex(*mac), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// 型違いの値は例外で 500 に化けていた。どの検査で落ちても 401。
TEST(Jwt, NonStringAlgIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto header = Json::object({{"alg", 123}, {"typ", "JWT"}});
    const auto tok =
        make_token(header, Json::object({{"sub", "a"}, {"exp", now_s() + 300}}), kSecret);
    auto r = call(srv.port(), tok, {"WWW-Authenticate"});
    EXPECT_EQ(r.status, 401);
    EXPECT_NE(r.extra["WWW-Authenticate"].find("Bearer"), std::string::npos);
}

TEST(Jwt, NonStringIssIs401) {
    TestServer srv([](hayate::App &app) {
        auto cfg = base_cfg();
        cfg.issuer = "https://issuer.example";
        protect(app, cfg);
    });
    const auto tok = make_token(
        hs256_header(),
        Json::object({{"sub", "a"}, {"exp", now_s() + 300}, {"iss", Json::array()}}), kSecret);
    auto r = call(srv.port(), tok, {"WWW-Authenticate"});
    EXPECT_EQ(r.status, 401);
    EXPECT_NE(r.extra["WWW-Authenticate"].find("Bearer"), std::string::npos);
}

TEST(Jwt, AlgNoneIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto header = Json::object({{"alg", "none"}, {"typ", "JWT"}});
    const auto payload = Json::object({{"sub", "root"}, {"exp", now_s() + 300}});
    const auto tok = base64url_encode(header.dump()) + "." + base64url_encode(payload.dump()) + ".";
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

TEST(Jwt, AlgConfusionIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    // RS256 と名乗りながら HMAC で署名したトークン。alg を見た時点で落とす。
    const auto header = Json::object({{"alg", "RS256"}, {"typ", "JWT"}});
    const auto tok =
        make_token(header, Json::object({{"sub", "root"}, {"exp", now_s() + 300}}), kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

TEST(Jwt, IssuerMismatchIs401) {
    TestServer srv([](hayate::App &app) {
        auto cfg = base_cfg();
        cfg.issuer = "https://issuer.example";
        protect(app, cfg);
    });
    const auto ok = make_token(
        hs256_header(),
        Json::object({{"sub", "a"}, {"exp", now_s() + 300}, {"iss", "https://issuer.example"}}),
        kSecret);
    EXPECT_EQ(call(srv.port(), ok).status, 200);
    const auto bad = make_token(
        hs256_header(),
        Json::object({{"sub", "a"}, {"exp", now_s() + 300}, {"iss", "https://evil.example"}}),
        kSecret);
    EXPECT_EQ(call(srv.port(), bad).status, 401);
}

TEST(Jwt, AudienceMismatchIs401) {
    TestServer srv([](hayate::App &app) {
        auto cfg = base_cfg();
        cfg.audience = "api";
        protect(app, cfg);
    });
    const auto bad =
        make_token(hs256_header(),
                   Json::object({{"sub", "a"}, {"exp", now_s() + 300}, {"aud", "other"}}), kSecret);
    EXPECT_EQ(call(srv.port(), bad).status, 401);
    const auto missing =
        make_token(hs256_header(), Json::object({{"sub", "a"}, {"exp", now_s() + 300}}), kSecret);
    EXPECT_EQ(call(srv.port(), missing).status, 401);
}

TEST(Jwt, AudienceArrayMatches) {
    TestServer srv([](hayate::App &app) {
        auto cfg = base_cfg();
        cfg.audience = "api";
        protect(app, cfg);
    });
    const auto tok = make_token(
        hs256_header(),
        Json::object({{"sub", "a"}, {"exp", now_s() + 300}, {"aud", Json::array({"web", "api"})}}),
        kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 200);
}

TEST(Jwt, EmptySecretThrows) { EXPECT_THROW(hayate::mw::jwt({}), std::exception); }

TEST(Jwt, ScopedToGroup) {
    TestServer srv([](hayate::App &app) {
        app.get("/health", [](hayate::Request &) { return hayate::Response::text("up"); });
        app.group("/api", [](hayate::Router &r) {
            r.use(hayate::mw::jwt({.secret = kSecret}));
            r.get("/me", [](hayate::Request &) { return hayate::Response::text("ok"); });
        });
    });
    auto open = http_call("127.0.0.1", srv.port(), http::verb::get, "/health");
    EXPECT_EQ(open.status, 200);
    EXPECT_EQ(open.body, "up");
    auto guarded = http_call("127.0.0.1", srv.port(), http::verb::get, "/api/me");
    EXPECT_EQ(guarded.status, 401);
}

// HMAC が失敗したら、どんな署名とも一致しない。空署名とも。
TEST(Hmac, MissingMacNeverMatches) {
    using hayate::detail::signature_matches;
    const auto mac = hayate::detail::hmac_sha256("key", "data");
    ASSERT_TRUE(mac.has_value());
    EXPECT_TRUE(signature_matches(mac, *mac));
    auto flipped = *mac;
    flipped[0] = static_cast<char>(flipped[0] ^ 0x01);
    EXPECT_FALSE(signature_matches(mac, flipped));
    EXPECT_FALSE(signature_matches(mac, ""));
    EXPECT_FALSE(signature_matches(std::nullopt, ""));
    EXPECT_FALSE(signature_matches(std::nullopt, *mac));
}
