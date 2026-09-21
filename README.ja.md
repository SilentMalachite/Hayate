# Hayate

[English](README.md) | 日本語

英語版 [README.md](README.md) の訳。正本は英語版で、食い違えば英語版に従う。

C++20 の HTTP フレームワーク兼サーバー。namespace は `hayate`。

ハンドラはコルーチン。公開 API は Fluent。マクロでルートを登録しない。
土台は [Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html) と
[Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/) で、
自前のイベントループも自前の HTTP パーサも書かない。

```cpp
#include <hayate/hayate.hpp>

int main() {
    hayate::App app;
    app.get("/", [](hayate::Request &) { return hayate::Response::text("hello"); });
    app.bind("0.0.0.0", 8080).serve();
}
```

## 状態

| Phase | 中身 | |
|---|---|---|
| 0 | CMake Presets、公開ヘッダ、固定応答 | 済 |
| 1 | HTTP/1.1、ルーティング、JSON、Middleware、Limits、graceful shutdown | 済 |
| 2 | CORS / 静的ファイル / レート制限 | 済 |
| 3 | TLS / JWT 検証 / OpenAPI 生成 / 最小 metrics / 静的ファイルのストリーミング送出 | 済 |

テストは GoogleTest。debug preset は ASan + UBSan、`tsan` preset は TSan 込みで走る。

multipart / WebSocket / SSE / gzip は**実装しない**。

## 必要なもの

- C++20（macOS の Apple Clang、Linux の GCC 13+ / Clang 16+）。Windows は未対応。
  Linux は GitHub Actions（ubuntu-24.04、GCC 13 と Clang 16）で確かめている。
  GCC 12 はコルーチンの誤コンパイル（PR 101367）があるので使えない
- CMake 3.28+、Ninja
- Boost 1.83+（Asio / Beast、ヘッダのみ）
- OpenSSL 3（TLS と JWT の HMAC）
- nlohmann/json 3.11.3、GoogleTest 1.15.2 は FetchContent で取る

```bash
# macOS
brew install boost openssl@3 cmake ninja
# Ubuntu 24.04 以降（libboost-dev は古い版を指すことがあるので 1.83 を名指しする）
apt install libboost1.83-dev libssl-dev cmake ninja-build
```

## ビルドとテスト

```bash
cmake --preset debug
cmake --build --preset debug
cmake --build --preset test && ctest --preset test --output-on-failure
# データ競合を見る
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan --output-on-failure
```

`release` preset はサニタイザ無し。macOS の Apple Clang の ASan には LeakSanitizer が無いので、
debug preset で見えるのは UAF まで。リークは Homebrew LLVM の clang で見る:

```bash
cmake -S . -B build/lsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DHAYATE_ENABLE_SANITIZERS=ON -DHAYATE_BUILD_TESTS=ON
cmake --build build/lsan && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/lsan
```

## 取り込み

FetchContent か `add_subdirectory` で取り込み、`hayate::hayate` にリンクする。install / `find_package` は無い。

```cmake
include(FetchContent)
FetchContent_Declare(hayate
    GIT_REPOSITORY https://github.com/SilentMalachite/Hayate.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(hayate)
target_link_libraries(app PRIVATE hayate::hayate)
```

C++20 は `hayate::hayate` から伝わる。Boost と OpenSSL は利用側の環境から `find_package` で探す。

版は SemVer。0.x の間はマイナーで公開 API が変わり得る。パッチでは変えない。

## できること

### ルーティング

```cpp
app.get("/users/:id", [](hayate::Request &req) -> asio::awaitable<hayate::Response> {
    co_return hayate::Response::json({{"id", std::string(req.param("id"))}});
});
app.post("/echo", [](hayate::Request &req) {
    auto body = req.json();
    return body.ok() ? hayate::Response::json(body.value())
                     : hayate::Response::from_error(body.error());
});
app.group("/api/v1", [](hayate::Router &r) { r.get("/ping", ...); });
```

`:name` は空でない 1 セグメント、`*name` は残り全部（空でも可）で最後にだけ置ける。
優先はセグメントごとに static > param > wildcard で、`/a` は `/a/*rest` に勝つ。
同じメソッドで同じ形のルート（`/u/:id` と `/u/:name`）の二重登録、名前の無い `:` / `*`、途中の `*name`、
GET / POST 以外は、登録時に `std::invalid_argument` を投げる。
メソッド違いは 405（`Allow` 付き）、パス無しは 404。

ハンドラは値を返しても `awaitable<Response>` を返してもよい。ただし const で呼べること
（`mutable` ラムダはコンパイルエラー）。全接続で共有されるので、状態は App か Request の Extension に置く。

### Middleware

```cpp
app.use([](hayate::Request &req, hayate::Next next) -> asio::awaitable<hayate::Response> {
    auto res = co_await next(req);
    res.set_header("X-Trace", "1");
    co_return res;
});
```

onion（入り A→B、戻り B→A）。`next` を呼ばなければそこで短絡する。
`Router::group` の中で `use` すれば、その配下だけに掛かる。

### CORS / レート制限

```cpp
app.use(hayate::mw::cors({.origin = "https://app.example"}));
app.use(hayate::mw::rate_limit({.max = 60, .window = std::chrono::seconds(60)}));
```

### 静的ファイル

```cpp
app.get("/assets/*path", hayate::files("public"));
// 配布上限 4 MiB、ワーカー 4 本、FS 待ちの上限 2 秒
app.get("/dl/*path", hayate::files("downloads", 4u * 1024 * 1024, 4, std::chrono::seconds(2)));
```

root の外・上限超過・無いファイル・通常ファイル以外は 404（存在を漏らさない）。
FS 待ちが `fs_timeout`（既定 5 秒）を超えたら 503。
ファイル I/O は `files()` が持つワーカープールで行い、io スレッドは filesystem を待たない。
本体はサイズに関係なく 64 KiB ずつ送るので、1 応答のメモリはファイルサイズに依らない。
`Content-Length` を立て、chunked encoding は使わない。

### TLS

```cpp
app.tls({.cert_file = "server.pem", .key_file = "server.key"})
   .bind("0.0.0.0", 8443)
   .serve();
```

`tls()` を呼んだ App は全接続が TLS。TLS 1.1 以下は無効。
読めない証明書・鍵と、証明書と対でない鍵は `tls()` の時点で投げる。

### JWT 検証（HS256）

```cpp
app.group("/api", [](hayate::Router &r) {
    r.use(hayate::mw::jwt({.secret = secret, .leeway = std::chrono::seconds(60)}));
    r.get("/me", [](hayate::Request &req) {
        auto *c = req.get<hayate::Claims>();
        return hayate::Response::text(c->json.value("sub", ""));
    });
});
```

`alg` は署名を見る前に検査するので `alg: none` とアルゴリズム混同を断つ。
署名比較は定数時間。`exp` 無しは拒否する。失敗は理由によらず 401 + `WWW-Authenticate: Bearer`。

### metrics

```cpp
app.get("/metrics", hayate::metrics(app));
```

Prometheus テキストで accept / 拒否 / 同時接続 / リクエスト数 / ステータスクラス別応答数。

### OpenAPI

```cpp
app.get("/openapi.json", [&app](hayate::Request &) {
    return hayate::Response::json(hayate::openapi(app, {.title = "my api"}));
});
```

登録済みルートから OpenAPI 3.1 を作る。Router は `pattern` と `method` しか持たないので、
リクエスト / レスポンスのスキーマは**推測せず出さない**。

### Limits

```cpp
app.limits({.max_body_bytes = 4u * 1024 * 1024, .max_connections = 4096});
```

| 項目 | 既定 |
|---|---|
| `max_header_bytes` | 8192 |
| `max_body_bytes` | 1 MiB |
| `read_timeout` / `write_timeout` | 30s |
| `idle_timeout` | 60s |
| `max_connections` | 1024 |

超過は header 431 / body 413。タイムアウト切れは接続を閉じる。
`stop()` は新規 accept を止め、要求を待っているだけの接続（keep-alive の次の要求待ちなど）はすぐ閉じ、
ヘッダが揃った要求は完了させてから io_context を止める。TLS は停止中、close_notify の返事を待たない。

## 設計の約束

- `std::expected` / C++ Modules / C++23 以降の必須機能は使わない
- JSON は nlohmann 1 本。`hayate::Json` はその別名
- `new` / `delete` / `malloc`、生配列を書かない
- `Request` は要求の文字列を所有し、アクセサは view を返す。寿命は `Request`。view を `App` や `Response` に保存しない
- 例外はハンドラ / Middleware の境界を出ない。Beast / Asio の失敗は、応答を書ける段階なら `Error` にして
  応答する（413 / 431 / 500）。書けない段階（timeout・相手の切断・handshake 失敗）なら閉じるだけ
- 共有可変グローバルを持たない。状態は `App` か `Request` の Extension、または Middleware / Handler の工場が持つ
- 既存フレームワーク（Drogon / Crow / Oat++ / Cinatra / userver）のコードをコピーしない

## やらないこと

HTTP/2、HTTP/3、gRPC、GraphQL、HTML テンプレート、ORM、マイグレーション、
multipart、WebSocket、SSE、gzip、JWT の RS256 / JWKS、mTLS、OpenAPI のスキーマ推論。

## ライセンス

[Apache License 2.0](LICENSE)。

## ドキュメント

英語が正本。各文書の日本語訳は隣の `*.ja.md`。食い違えば英語版に従う。
正本は [`docs/SPEC.md`](docs/SPEC.md)（訳は [`docs/SPEC.ja.md`](docs/SPEC.ja.md)）。図は [`docs/ER.ja.md`](docs/ER.ja.md) と
[`docs/UML.ja.md`](docs/UML.ja.md)。チャットでの合意はファイルに落とすまで存在しない。
