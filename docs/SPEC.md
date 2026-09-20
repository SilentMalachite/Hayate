# SPEC

## 目的

C++20 の Web サービス用フレームワーク兼 HTTP サーバー **Hayate**（namespace `hayate`）を作る。
小さく、速く、型が立つ。ハンドラはコルーチン。公開 API は Fluent + concepts。マクロでルートを登録しない。

利用者が書く形（Phase 1。`request_id` は Phase 2）:

```cpp
#include <hayate/hayate.hpp>

int main() {
    hayate::App app;
    app.get("/", [](hayate::Request&) {
        return hayate::Response::text("hello");
    });
    app.get("/users/:id", [](hayate::Request& req) -> asio::awaitable<hayate::Response> {
        auto id = req.param("id");
        co_return hayate::Response::json({{"id", std::string(id)}});
    });
    app.post("/echo", [](hayate::Request& req) {
        auto body = req.json();
        if (!body.ok()) {
            return hayate::Response::from_error(body.error());
        }
        return hayate::Response::json(body.value());
    });
    app.bind("0.0.0.0", 8080).serve();
}
```

`include/hayate/hayate.hpp` が `namespace asio = boost::asio;` を導入する。

段階:

- Phase 0: CMake Presets、公開ヘッダの骨格、固定応答の Hello
- Phase 1: HTTP/1.1、ルーティング、JSON、MW、制限、graceful shutdown
- Phase 2: CORS / 静的ファイル / multipart / WS / SSE / gzip / レート制限
- Phase 3: TLS / JWT 検証 / OpenAPI 生成 / 最小 metrics / 静的ファイルのストリーミング送出

今の受け入れは Phase 1 と CORS と静的ファイルとレート制限と静的ファイルのストリーミング送出。multipart / WS / SSE / gzip は実装しない。

## やらないこと

- 既存 C++ Web フレームワーク（Drogon / Crow / Oat++ / Cinatra / userver）のコード・内部構造・マクロのコピー
- 自前イベントループ、自前 HTTP パーサ、自前 TLS、自前 JSON
- HTTP/2, HTTP/3, gRPC, GraphQL
- HTML テンプレート、ORM、マイグレーション
- C++23/26 必須機能、C++ Modules、反射マクロ DSL
- 設定ファイルの巨大階層、プラグインローダ、ホットリロード
- Windows 専用コード（初期）
- 頼んでいないサンプル・抽象・「将来のため」の層
- チャットだけに残る決定（正本は `docs/SPEC.md`）

## 受け入れ基準

Phase 0

- `cmake --preset debug` でビルドできる
- Hello がポートを開き固定応答を返す
- 公開ヘッダの置き場が `include/hayate/` になっている

Phase 1

- `cmake --build --preset test && ctest --preset test` が失敗ゼロ
- `examples/hello` の `GET /` が 200
- `:param` と JSON POST がテストで示されている
- MW が onion（入り A→B、戻り B→A、`next` なし短絡）でテストされている
- メソッド違いは 405（Allow 付き）、パス無しは 404
- 過大 body / timeout / shutdown（in-flight 完了・新規拒否）がテストされている
- TLS / HTTP/2 / ORM / テンプレートがリポジトリに無い
- hello が公開ヘッダだけに依存する
- debug + ASan で新規リーク・UAF が無い
- 頼んでいないファイルが diff に無い

Phase 3（静的ファイルのストリーミング送出）

- ファイルサイズに関わらず 1 応答のメモリが 64 KiB で頭打ちになる
- `Content-Length` が実際の送出バイト数と一致し、`Transfer-Encoding: chunked` が付かない
- ストリーミング応答の後で keep-alive の次の要求が通る
- 0 バイトのファイルが 200 / `Content-Length: 0` で返る
- 既定引数で `max_bytes` の上限が掛からない

Phase 3（TLS）

- TLS で `GET /` が 200 を返し、同じ接続で 2 本目も通る
- `tls()` した App に平文クライアントが繋いでも応答を得られない
- 読めない証明書 / 鍵で `tls()` が投げる
- 静的ファイルのストリーミングが TLS 上でも `Content-Length` ちょうどで届く

## 技術判断

- 言語: C++20 厳守。`std::expected` は使わない
- ビルド: CMake 3.28+、Presets `debug` / `release` / `test`。ASan は debug の既定
- 対象: macOS (Apple Clang) と Linux (GCC 12+ / Clang 16+)。Windows は後追い
- I/O: Boost.Asio 1.83+。`asio::awaitable` / `co_spawn`。公開ヘッダで `namespace asio = boost::asio;`
- ファイル I/O: ブロッキング FS 呼び出しは `asio::thread_pool` に逃がす。`asio::stream_file` は `BOOST_ASIO_HAS_FILE`（Windows ハンドル / Linux io_uring）依存で macOS に無いため使わない
- HTTP / WS: Boost.Beast（HTTP/1.1）
- TLS: OpenSSL 3 via `asio::ssl`。`find_package(OpenSSL 3 REQUIRED)`。最低 TLS 1.2
- JSON: nlohmann/json v3.11.3 1 本。`hayate::Json` は `nlohmann::json` の別名。現行 glaze は C++23 必須のため採用しない。混在禁止
- エラー: `hayate::Result<T>` は `std::variant<T, Error>` の薄い自前 1 本。Boost.Outcome は使わない
- テスト: GoogleTest。実装の前に失敗するテスト。ループバック + エフェメラルポート。スリープ同期しない
- 依存: Boost と OpenSSL は `find_package`。nlohmann/json と GoogleTest は FetchContent。vcpkg は使わない
- 所有: 入力は `string_view` / `span<const byte>`。寿命は Request。足りるならコピーしない
- スレッド: io_context あたり 1。`app.threads(n)` で複数。共有可変は strand か mutex を書いてから
- 禁止: `new`/`delete`/`malloc`、生配列、ハンドラ境界をまたぐ例外、共有可変グローバル
- 作業順: この SPEC → 公開ヘッダ → 失敗するテスト → 最小実装 → 全テスト → 停止
- 正本は `docs/SPEC.md`。ER が参照する ARCHITECTURE は本 SPEC の『ルーティング』節。

## Phase 1 公開 API

### Result / Error

```cpp
namespace hayate {
struct Error {
    std::string code;
    std::string message;
    std::uint16_t http_status{500};
};

template<typename T>
class Result {
public:
    static Result ok(T value);
    static Result err(Error error);
    bool ok() const noexcept;
    T& value() &;
    const T& value() const&;
    T value() &&;
    const Error& error() const&;
};
}
```

`ok()==false` で `value()`、`ok()==true` で `error()` は契約違反（assert）。例外で結果を返さない。

### Handler / Middleware

```cpp
using Handler = std::function<asio::awaitable<Response>(Request&)>;
using Next = Handler;
using Middleware = std::function<asio::awaitable<Response>(Request&, Next)>;
```

`App::get/post` は次を受け付ける。

- `asio::awaitable<Response>(Request&)`
- `Response(Request&)`（内部で awaitable に包む）

onion: 入りは登録順 A→B、戻りは B→A。`next` を呼ばなければ短絡。
App の `use()` は 404/405 を含む dispatch 全体を包む（CORS preflight とエラー応答にヘッダが要る）。
`group` の MW はマッチしたルートにだけ付く。
ハンドラと MW が投げた例外は `dispatch` が 500 に変換する。接続は閉じない。

### ルーティング

- メソッドは `GET` と `POST` のみ
- `:name` は 1 セグメント。`*name` は残り全部（空でも可）
- 欠けた param / query / header は空 `string_view`
- 一致優先: 各セグメントで static > param > wildcard。全セグメント同点なら先に登録した方
- パス無し 404。パスはあるがメソッド違い 405 + `Allow`
- `group(prefix, fn)` は接頭辞連結。`//` を `/` に正規化する。末尾 `/` は消さない
- `/a` と `/a/` は別ルート
- パスはセグメントに分けてからパーセントデコードする。`%2F` は区切りにせずセグメント内の `/` になる
- query はパーセントデコードし、`+` は空白として読む
- 壊れた `%` 列は復号せずそのまま残す
- `target()` と `path()` は生のまま。復号後が見えるのは `param()` と `query()`

### App 寿命

- App が `io_context` と `Router` と `Limits` と acceptor を所有する
- `bind(host, port)` は socket bind + listen まで同期。`port()==0` ならエフェメラル
- `port()` は bind 後の実ポート
- `run()` は accept ループ。`asio::awaitable<void>`。`stop()` で終わる
- `serve()` は `threads(n)`（既定 1）で io_context を blocking 実行する。SIGINT/SIGTERM で `stop()`
- `stop()` は新規 accept を止め、in-flight の読書きを完了させてから io_context を止める

### Limits 既定

| 項目 | 既定 |
|---|---|
| max_header_bytes | 8192 |
| max_body_bytes | 1048576 |
| read_timeout | 30s |
| write_timeout | 30s |
| idle_timeout | 60s |
| max_connections | 1024 |

超過: header 431、body 413。read/write/idle 切れは接続を閉じる（応答を書けなければ書かない）。`max_connections` 超過の新規は accept せず切る。

窓の切り分け: 1 本目のヘッダ読みは `read_timeout`。keep-alive で次の要求のヘッダを待つ間は `idle_timeout`。ヘッダが揃った後の本文読みは何本目でも `read_timeout`。

### JSON

- `Response::json(Json)` は 200 / `application/json`
- `Request::json()` は body を `Json` として読む。破損は `Error{code:"bad_json", http_status:400}`
- `Request::json<T>()` は型不一致も同じ 400
- Content-Type 検査は Phase 1 ではしない（body バイトだけ見る）

### Response ヘッダ

- `set_header(name, value)` は同名を置き換える（大文字小文字は無視）
- `name` が HTTP token でなければ何もしない
- `value` から CTL（`\r` `\n` を含む）を落とし、前後の空白を削る。ヘッダ注入を断つ

### Extension

```cpp
template<typename T, typename... Args>
T& Request::set(Args&&... args);  // 同じ T は上書き
template<typename T>
T* Request::get() noexcept;       // 無ければ nullptr
```

寿命は Request。ポインタを Response / App に保存しない。

### TLS（Phase 3）

```cpp
app.tls({.cert_file = "server.pem", .key_file = "server.key"})
   .bind("0.0.0.0", 8443)
   .serve();
```

- 新しい公開型は設定値 struct `Tls` 1 つだけ。`TlsContext` のようなクラスは作らない
- `ssl::context` は App が内部で組む。公開ヘッダに `asio::ssl` は出さない
- `tls()` を呼んだ App は全接続が TLS。平文との同時待ち受けはしない
- 証明書 / 鍵が読めなければ `tls()` が投げる（`bind()` と同じく設定時に落とす）
- `key_password` が空なら鍵にパスフレーズ無しとして扱う
- ハンドシェイクの窓は `read_timeout`。失敗した接続は応答を書かずに閉じる
- 終了は TLS shutdown → socket shutdown の順
- sslv2 / sslv3 / tlsv1 / tlsv1.1 を無効化する。最低 TLS 1.2
- `Request::peer()` は TLS でも accept 時の remote IP

### CORS（Phase 2）

```cpp
app.use(hayate::mw::cors());
app.use(hayate::mw::cors({.origin = "https://app.example"}));
```

- `hayate::mw::cors()` は Middleware。新しい公開型（Service 等）は足さない
- ルート登録はこれまで通り GET / POST のみ。OPTIONS は preflight 用にフレームワークが扱う
- `Origin` が無ければ CORS ヘッダを付けない
- `Origin` がある GET/POST（および 404/405）: `Access-Control-Allow-Origin`（既定 `*`、設定があればその値）
- `origin` が `*` 以外のときは `Vary: Origin` も付ける（共有キャッシュの取り違え防止）
- `OPTIONS` + `Origin`: 204。`Allow-Origin` / `Allow-Methods` / `Allow-Headers`。`next` を呼ばない
- 既定 methods: `GET, POST, OPTIONS`。既定 headers: `Content-Type, Authorization`

### 静的ファイル（Phase 2 / 送出は Phase 3）

```cpp
app.get("/assets/*path", hayate::files("public"));
app.get("/assets/*path", hayate::files("public", 4u * 1024 * 1024));
app.get("/assets/*path", hayate::files("public", 4u * 1024 * 1024, 4));
```

- Handler 工場。`StaticFile` クラスは足さない（ER: Handler にぶら下がる）
- 署名は `Handler files(std::string_view root, std::uint64_t max_bytes = 0, std::uint32_t io_threads = 2)`
- `max_bytes` は配布上限。既定 `0` は無制限。`0` 以外でそれを超える実ファイルは 404（存在を漏らさない）
- root の末尾 `/` は無視する。登録時に root が無くても同じ扱い
- wildcard 名は `path`。空なら `index.html`
- root の外（`..` / 絶対パス）は 404（存在を漏らさない）
- 復号後のパスに NUL が入っていたら 404
- 無いファイル・ディレクトリで `index.html` も無いときは 404
- ディレクトリで `index.html` があればそれを返す
- Content-Type は拡張子（`.html` `text/html`、`.css` `text/css`、`.js` `application/javascript`、`.json` `application/json`、`.txt` `text/plain`、その他 `application/octet-stream`）

I/O モデル:

- 正規化・stat・open・read は `files()` が所有するワーカープールで行う。io スレッドは filesystem を待たない
- プールは `files()` 1 回につき 1 つ、`io_threads` 本。既定 2。`0` は 1 に切り上げる
- 同時に走る読みは `io_threads` 本まで。溢れた分はプールのキューで待つ
- ハンドラはバイトを読まない。`Response` に `FileSource`（path / size / プール）を載せ、Connection が送出する
- 本体はサイズに関係なく常に 64 KiB ずつ送る。1 応答のメモリはファイルサイズに依らず 64 KiB
- `Content-Length` を立てる。chunked encoding は使わない
- 送るのは stat した `size` まで。stat 後に伸びても増やさない
- ヘッダ送出後に読みが失敗したら（縮んだ・消えた）その場で接続を閉じる。status はもう直せない
- `write_timeout` はチャンクごとに張り直す。大きいファイルの総送出時間は縛らない
- 読みが `read_timeout` を超えた接続は既存のタイムアウト窓どおり閉じる（応答は書かない）

### レート制限（Phase 2）

```cpp
app.use(hayate::mw::rate_limit({.max = 60, .window = std::chrono::seconds(60)}));
```

- Middleware。固定窓。キーは `Request::peer()`（接続の remote IP。Connection.peer）
- `peer` は accept 直後に 1 回だけ取り、その接続の全リクエストで同じ。取れなければ空で 1 つの窓に入る
- 窓内で `max` を超えたら 429。`Retry-After` は窓の残り秒（切り上げ、最小 1）
- 既定 `max` 60、`window` 60s
- カウンタは MW が所有する mutex 付き map。グローバル禁止
- 窓を過ぎたキーは次のリクエスト時に掃除する（map を無制限に太らせない）
