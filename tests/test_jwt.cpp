#include "detail/base64.hpp"
#include "detail/jwt_time.hpp"
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

// 範囲外の exp も nbf と同じく 401。
TEST(Jwt, ExpBeyondInt64Is401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok = make_token(
        hs256_header(),
        Json::object({{"sub", "alice"}, {"exp", std::numeric_limits<std::uint64_t>::max()}}),
        kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

// 負の exp は遠い過去。飽和加算の下限でも通さない。
TEST(Jwt, NegativeExpIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    for (const std::int64_t exp : {std::int64_t{-1}, std::numeric_limits<std::int64_t>::min()}) {
        const auto tok =
            make_token(hs256_header(), Json::object({{"sub", "alice"}, {"exp", exp}}), kSecret);
        EXPECT_EQ(call(srv.port(), tok).status, 401) << exp;
    }
}

// 負の nbf は遠い過去なので通る。
TEST(Jwt, NegativeNbfPasses) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    for (const std::int64_t nbf : {std::int64_t{-1}, std::numeric_limits<std::int64_t>::min()}) {
        const auto tok = make_token(
            hs256_header(), Json::object({{"sub", "alice"}, {"exp", now_s() + 300}, {"nbf", nbf}}),
            kSecret);
        EXPECT_EQ(call(srv.port(), tok).status, 200) << nbf;
    }
}

TEST(Jwt, NonNumericExpIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    for (const auto &exp : {Json(std::to_string(now_s() + 300)), Json(true), Json(nullptr)}) {
        const auto tok =
            make_token(hs256_header(), Json::object({{"sub", "alice"}, {"exp", exp}}), kSecret);
        EXPECT_EQ(call(srv.port(), tok).status, 401) << exp.dump();
    }
}

// nbf の型と範囲は exp と同じ。小数は受けない。
TEST(Jwt, FractionalNbfIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok = make_token(hs256_header(),
                                Json::object({{"sub", "alice"},
                                              {"exp", now_s() + 300},
                                              {"nbf", static_cast<double>(now_s() - 10) + 0.5}}),
                                kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

// now + leeway >= nbf なら通る。署名時の now より検査時の now が小さくなることはない。
TEST(Jwt, NbfWithinLeewayPasses) {
    TestServer srv([](hayate::App &app) {
        auto cfg = base_cfg();
        cfg.leeway = std::chrono::seconds(60);
        protect(app, cfg);
    });
    const auto tok = make_token(
        hs256_header(),
        Json::object({{"sub", "alice"}, {"exp", now_s() + 300}, {"nbf", now_s() + 30}}), kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 200);
}

// leeway が最大でも now + leeway は溢れない。
TEST(Jwt, MaxLeewayDoesNotOverflow) {
    TestServer srv([](hayate::App &app) {
        auto cfg = base_cfg();
        cfg.leeway = std::chrono::seconds::max();
        protect(app, cfg);
    });
    const auto tok = make_token(hs256_header(),
                                Json::object({{"sub", "alice"},
                                              {"exp", now_s() + 300},
                                              {"nbf", std::numeric_limits<std::int64_t>::max()}}),
                                kSecret);
    EXPECT_EQ(call(srv.port(), tok).status, 200);
}

// base64url はパディング無しの URL 用アルファベットだけ。改変後の文字列に署名し直すので、
// 緩い decoder なら 200 になる。
TEST(Jwt, NonCanonicalBase64urlIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto h64 = base64url_encode(hs256_header().dump());
    // 6 文字続けると、3 バイト境界に揃った "???" と "~~~" が必ず入り、`_` と `-` になる。
    const auto p64 = base64url_encode(
        Json::object({{"sub", "alice"}, {"exp", now_s() + 300}, {"x", "??????~~~~~~"}}).dump());
    ASSERT_NE(p64.find('-'), std::string::npos);
    ASSERT_NE(p64.find('_'), std::string::npos);
    ASSERT_EQ(call(srv.port(), sign_parts(h64, p64, kSecret)).status, 200);

    auto standard = p64;
    for (auto &c : standard) {
        c = c == '-' ? '+' : c == '_' ? '/' : c;
    }
    auto len_mod4_is_1 = h64;
    while (len_mod4_is_1.size() % 4 != 1) {
        len_mod4_is_1 += 'A';
    }
    struct Case {
        const char *what;
        std::string header;
        std::string payload;
    };
    for (const auto &c :
         {Case{"header padding", h64 + "=", p64}, Case{"payload padding", h64, p64 + "="},
          Case{"standard alphabet", h64, standard}, Case{"length mod 4 == 1", len_mod4_is_1, p64},
          Case{"outside alphabet", h64, p64 + "*"}}) {
        EXPECT_EQ(call(srv.port(), sign_parts(c.header, c.payload, kSecret)).status, 401) << c.what;
    }
    const auto good = sign_parts(h64, p64, kSecret);
    EXPECT_EQ(call(srv.port(), good + "=").status, 401) << "signature padding";
    EXPECT_EQ(call(srv.port(), good + "*").status, 401) << "signature outside alphabet";
}

TEST(Base64url, DecodesUrlAlphabetOnly) {
    using hayate::detail::base64url_decode;
    EXPECT_EQ(base64url_decode(""), std::string());
    EXPECT_EQ(base64url_decode("YQ"), "a");
    EXPECT_EQ(base64url_decode("YWI"), "ab");
    EXPECT_EQ(base64url_decode("YWJj"), "abc");
    EXPECT_EQ(base64url_decode("-_8"), "\xfb\xff");
    for (const auto *bad : {"YQ==", "YWI=", "Y", "YWJjZ", "+_8", "-/8", "YW j", "YW.j"}) {
        EXPECT_FALSE(base64url_decode(bad).has_value()) << bad;
    }
}

// 最後の文字の未使用ビットを捨てると、同じバイト列に綴りが何通りもできる。
TEST(Base64url, RejectsNonZeroTrailingBits) {
    using hayate::detail::base64url_decode;
    EXPECT_EQ(base64url_decode("QQ"), "A");
    EXPECT_FALSE(base64url_decode("QR").has_value());
    EXPECT_EQ(base64url_decode("YWI"), "ab");
    EXPECT_FALSE(base64url_decode("YWJ").has_value());
}

// 署名の最後の文字は下位 2 ビットが未使用。そこだけ変えても、緩い decoder では同じ署名に戻る。
TEST(Jwt, NonCanonicalSignatureIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto good = make_token(hs256_header(),
                                 Json::object({{"sub", "alice"}, {"exp", now_s() + 300}}), kSecret);
    ASSERT_EQ(call(srv.port(), good).status, 200);
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    auto tok = good;
    const auto v = alphabet.find(tok.back());
    ASSERT_NE(v, std::string_view::npos);
    tok.back() = alphabet[v ^ 1];
    EXPECT_EQ(call(srv.port(), tok).status, 401);
}

// 形の崩れた header / payload は、署名が正しくても 401。
TEST(Jwt, MalformedJoseIs401) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto claims = Json::object({{"sub", "alice"}, {"exp", now_s() + 300}});
    struct Case {
        const char *what;
        Json header;
        Json payload;
    };
    for (const auto &c : {Case{"no alg", Json::object({{"typ", "JWT"}}), claims},
                          Case{"header array", Json::array({"HS256"}), claims},
                          Case{"lowercase alg", Json::object({{"alg", "hs256"}}), claims},
                          Case{"payload array", hs256_header(), Json::array({claims})}}) {
        EXPECT_EQ(call(srv.port(), make_token(c.header, c.payload, kSecret)).status, 401) << c.what;
    }
}

// 製品と同じ HMAC で作ったトークンしか無いと、署名対象や符号化を両方同じに間違えても通る。
// RFC 7515 付録 A.1 のトークンを、外で作られたものとしてそのまま検証する。
TEST(Jwt, Rfc7515A1Token) {
    const auto key = hayate::detail::base64url_decode(
        "AyM1SysPpbyDfgZld3umj1qzKObwVMkoqQ-EstJQLr_T-1qS0gZH75aKtMN3Yj0iPS4hcgUuTwjAzZr1Z9CAow");
    ASSERT_TRUE(key.has_value());
    const std::string token =
        "eyJ0eXAiOiJKV1QiLA0KICJhbGciOiJIUzI1NiJ9"
        ".eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFtcGxlLmNvbS9pc19yb290Ijp0"
        "cnVlfQ"
        ".dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    auto serve = [&](std::chrono::seconds leeway) {
        return TestServer([&](hayate::App &app) {
            app.use(hayate::mw::jwt({.secret = *key, .issuer = "joe", .leeway = leeway}));
            app.get("/me", [](hayate::Request &req) {
                auto *c = req.get<hayate::Claims>();
                return hayate::Response::text(c == nullptr ? "" : c->json.value("iss", ""));
            });
        });
    };
    // exp は 2011 年。既定では期限切れ。
    {
        auto srv = serve(std::chrono::seconds(0));
        EXPECT_EQ(call(srv.port(), token).status, 401);
    }
    {
        auto srv = serve(std::chrono::seconds::max());
        auto r = call(srv.port(), token);
        EXPECT_EQ(r.status, 200);
        EXPECT_EQ(r.body, "joe");
    }
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

// 秒の境界は実時計では決定的に作れない。now を固定して確かめる。
TEST(JwtTime, ExpBoundary) {
    using hayate::detail::times_ok;
    const std::int64_t now = 1'000'000;
    EXPECT_TRUE(times_ok(Json::object({{"exp", now}}), now, 0)) << "exp == now";
    EXPECT_FALSE(times_ok(Json::object({{"exp", now - 1}}), now, 0));
    EXPECT_TRUE(times_ok(Json::object({{"exp", now - 60}}), now, 60)) << "now == exp + leeway";
    EXPECT_FALSE(times_ok(Json::object({{"exp", now - 61}}), now, 60));
    EXPECT_FALSE(times_ok(Json::object({{"sub", "a"}}), now, 60)) << "no exp";
}

TEST(JwtTime, NbfBoundary) {
    using hayate::detail::times_ok;
    const std::int64_t now = 1'000'000;
    const std::int64_t exp = now + 300;
    EXPECT_TRUE(times_ok(Json::object({{"exp", exp}, {"nbf", now}}), now, 0)) << "nbf == now";
    EXPECT_FALSE(times_ok(Json::object({{"exp", exp}, {"nbf", now + 1}}), now, 0));
    EXPECT_TRUE(times_ok(Json::object({{"exp", exp}, {"nbf", now + 60}}), now, 60))
        << "nbf == now + leeway";
    EXPECT_FALSE(times_ok(Json::object({{"exp", exp}, {"nbf", now + 61}}), now, 60));
}

// 認証スキームの名前は大小を区別しない（RFC 9110 §11.1）。
TEST(Jwt, BearerSchemeIsCaseInsensitive) {
    TestServer srv([](hayate::App &app) { protect(app, base_cfg()); });
    const auto tok = make_token(hs256_header(),
                                Json::object({{"sub", "alice"}, {"exp", now_s() + 300}}), kSecret);
    for (const char *scheme : {"bearer ", "BEARER ", "bEaReR "}) {
        auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/me", {}, {},
                           std::chrono::seconds(2), {{"Authorization", scheme + tok}});
        EXPECT_EQ(r.status, 200) << scheme;
        EXPECT_EQ(r.body, "alice") << scheme;
    }
}
