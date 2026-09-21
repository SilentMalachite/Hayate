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

今の受け入れは Phase 1 と Phase 2（CORS / 静的ファイル / レート制限）と Phase 3（TLS / JWT 検証 / OpenAPI 生成 / 最小 metrics / 静的ファイルのストリーミング送出）。multipart / WS / SSE / gzip は実装しない。

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

Phase 3（OpenAPI 生成）

- 登録した全ルートが `paths` に出る
- `:id` が `parameters` になる（`in: path`、`required: true`、`type: string`）
- 同じパスの GET と POST が 1 つの path 項目にまとまる
- 登録していないメソッドのキーが出ない
- ワイルドカードの parameter に `x-hayate-wildcard` が付く
- `group()` の prefix が付いた形で出る

Phase 3（JWT 検証）

- 有効なトークンが 200 で、ハンドラが `Claims` から `sub` を読める
- ヘッダ欠落 / 形式不正 / 署名改竄 / payload 改竄 / `exp` 切れ / `exp` 無し / `nbf` 未来 /
  `alg: none` / `alg` 詐称（RS256 ヘッダを HMAC で署名）がすべて 401
- 401 に `WWW-Authenticate: Bearer` が付く
- `leeway` の内側で切れたトークンは通る
- `secret` が空だと `jwt()` が投げる
- group の外は無認証で通る

Phase 3（metrics）

- 2 本投げてから読むと `requests_total` が 2
- 404 を投げると `4xx` が 1 増え、`5xx` は増えない
- `max_connections` 超過の接続で `rejected_total` が 1 増える
- 1 接続につき `accepted_total` が 1 増える

## 技術判断

- 言語: C++20 厳守。`std::expected` は使わない
- ビルド: CMake 3.28+、Presets `debug` / `release` / `test`。ASan は debug の既定。
  `test` は `debug` と同じビルドディレクトリ（`--preset debug` の直後に `--build --preset test` が通る）
- 対象: macOS (Apple Clang) と Linux (GCC 12+ / Clang 16+)。Windows は後追い
- I/O: Boost.Asio 1.83+。`asio::awaitable` / `co_spawn`。公開ヘッダで `namespace asio = boost::asio;`
- ファイル I/O: ブロッキング FS 呼び出しは `asio::thread_pool` に逃がす。`asio::stream_file` は `BOOST_ASIO_HAS_FILE`（Windows ハンドル / Linux io_uring）依存で macOS に無いため使わない
- HTTP / WS: Boost.Beast（HTTP/1.1）
- TLS: OpenSSL 3 via `asio::ssl`。`find_package(OpenSSL 3 REQUIRED)`。最低 TLS 1.2
- JSON: nlohmann/json v3.11.3 1 本。`hayate::Json` は `nlohmann::json` の別名。現行 glaze は C++23 必須のため採用しない。混在禁止
- エラー: `hayate::Result<T>` は `std::variant<T, Error>` の薄い自前 1 本。Boost.Outcome は使わない
- テスト: GoogleTest。実装の前に失敗するテスト。ループバック + エフェメラルポート。スリープ同期しない。
  テスト用クライアントは非同期 I/O で期限を効かせ（Beast の期限は同期 I/O に効かない）、CTest にも TIMEOUT を置く
- 依存: Boost と OpenSSL は `find_package`。nlohmann/json と GoogleTest は FetchContent。vcpkg は使わない
- 所有: 入力は `string_view` / `span<const byte>`。寿命は Request。足りるならコピーしない
- スレッド: io_context あたり 1。`app.threads(n)` で複数。共有可変は strand か mutex を書いてから
- 並行の単位は接続。1 接続 1 strand で直列、異なる接続は並行に走る
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
`std::move(r).value()` の後の `r` は読まない（中身は move 済みで、`ok()` は true のまま）。
`error()` は `const&` だけで、move して取り出す版は持たない。

### Handler / Middleware

```cpp
using Handler = std::function<asio::awaitable<Response>(Request&)>;
using Next = Handler;
using Middleware = std::function<asio::awaitable<Response>(Request&, Next)>;
```

`App::get/post` は次を受け付ける。

- `asio::awaitable<Response>(Request&)`
- `Response(Request&)`（内部で awaitable に包む）
- どちらも **const で呼べること**（`mutable` ラムダは不可、コンパイル時に弾く）。ハンドラは全接続で
  共有されるので、状態を持たせると `threads(n>1)` で競合する。状態は App か Request の Extension に置く。
  条件は公開 concept `hayate::HandlerCallable` で表す

onion: 入りは登録順 A→B、戻りは B→A。`next` を呼ばなければ短絡。
App の `use()` は 404/405 を含む dispatch 全体を包む（CORS preflight とエラー応答にヘッダが要る）。
`group` の MW はマッチしたルートにだけ付く。group のパスで出る 404 / 405 はどのルートにも
マッチしていないので、group の MW を通らない（App の MW だけ）。
入れ子は App → 外の group → 内の group → ハンドラ。`use()` は呼んだ位置に関係なく、その App / Router の
全ルートに付く。
ハンドラと MW が投げた例外は `dispatch` が 500 に変換する。接続は閉じない。

### ルーティング

- メソッドは `GET` と `POST` のみ
- `:name` は空でない 1 セグメント。`*name` は残り全部（空でも可）で、最後のセグメントにだけ置ける
- 名前の無い `:` / `*` と、最後以外の `*name` は登録時に `std::invalid_argument` を投げる
- 欠けた param / query / header は空 `string_view`
- 一致優先: 各セグメントで static > param > wildcard。そこまで同点で片方だけが空の `*name` で
  終わるなら、終わらない方（`/a` と `/a/*rest` への `GET /a` は `/a`）。登録順では決めない
- 同じメソッドで同じ形（各セグメントの種類と static の文字列が同じ。param / wildcard の名前は見ない）を
  2 回登録したら投げる。後の方に一致する要求は無い
- パス無し 404。パスはあるがメソッド違い 405 + `Allow`
- `group(prefix, fn)` は接頭辞連結。連結した全体で、連続する `/` を 1 つに畳む。末尾 `/` は消さない
- `/a` と `/a/` は別ルート
- パスはセグメントに分けてからパーセントデコードする。`%2F` は区切りにせずセグメント内の `/` になる
- query はパーセントデコードし、`+` は空白として読む
- 壊れた `%` 列は復号せずそのまま残す
- `target()` と `path()` は生のまま。復号後が見えるのは `param()` と `query()`

### App 寿命

- App が `io_context` と `Router` と `Limits` と acceptor を所有する
- `bind(host, port)` は socket bind + listen まで同期。`port()==0` ならエフェメラル
- `port()` は bind 後の実ポート
- `run()` は accept ループ。`asio::awaitable<void>`。`stop()` で終わる。
  acceptor は App の `io_context` に束縛されているので、`run()` もその `io_context` 上で spawn する。
  外部の executor では動かない
- accept が失敗しても accept ループは終えない（終えるのは `stop()` だけ）。失敗したら 100 ms 待って
  次の accept へ（fd 枯渇で空回りしない）。`stop()` はその待ちも取り消す
- accept した接続ごとに strand を 1 本作る。その接続の socket・stream・タイマー・coroutine は
  すべてその strand 上で動く（`threads(n)` で n>1 のとき Beast の stream が要求する条件）
- accept ループと `stop()` は管理用 strand 1 本で直列化する。`stop()` はどのスレッドから
  何度呼んでもよく、効果は 1 回分
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

Beast / Asio の失敗は、応答を書ける段階なら `Error` にして応答する（413 / 431 / 500）。書けない段階（timeout・相手の切断・handshake 失敗）なら閉じるだけで、受け取る側のない `Error` は作らない。

窓の切り分け: 1 本目のヘッダ読みは `read_timeout`。keep-alive で次の要求のヘッダを待つ間は `idle_timeout`。ヘッダが揃った後の本文読みは何本目でも `read_timeout`。

HTTP/1.1 の約束:

- HEAD はルーティングしない（GET 扱いにしない。404 / 405 のまま）。ただし HEAD への応答は
  ステータスによらず本文を送らない。`Content-Length` は本文を送った場合の値を付ける
- `Expect: 100-continue` の要求には、本文を読む前に `100 Continue` を返す。
  `Content-Length` が `max_body_bytes` を超えるなら 100 を出さずに 413。
  HTTP/1.0 の要求の 100-continue は無視する（1xx を知らない。RFC 9110 §10.1.1）
- 接続を続けるかは「サーバーの判断」かつ「応答の `Connection`」。ハンドラが `Connection: close` を
  付ければ閉じる。サーバーが閉じると決めたら（要求が close、停止中など）、ハンドラの keep-alive は
  無視して `Connection: close` を送る。送ったヘッダと実際の挙動を食い違わせない
- 1xx / 204 / 304 の応答は本文を持たない。ハンドラの本文は捨て、`Content-Length` /
  `Transfer-Encoding` は付けない（RFC 9110 §8.6、§6.4.1）
- ヘッダの名前か値が 65533 バイト（Beast の上限）を超える応答は送れない。500 に差し替え、metrics も 500 で数える

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
同じ `T` を `set` し直すと前の値は壊れ、前に返した参照とポインタは無効になる。

### TLS（Phase 3）

```cpp
app.tls({.cert_file = "server.pem", .key_file = "server.key"})
   .bind("0.0.0.0", 8443)
   .serve();
```

- 新しい公開型は設定値 struct `Tls` 1 つだけ。`TlsContext` のようなクラスは作らない
- `ssl::context` は App が内部で組む。公開ヘッダに `asio::ssl` は出さない
- `tls()` を呼んだ App は全接続が TLS。平文との同時待ち受けはしない
- 証明書 / 鍵が読めない、または鍵が証明書と対でなければ `tls()` が投げる（`bind()` と同じく設定時に落とす）
- `key_password` が空なら鍵にパスフレーズ無しとして扱う
- ハンドシェイクの窓は `read_timeout`。失敗した接続は応答を書かずに閉じる
- 終了は TLS shutdown → socket shutdown の順。相手の close_notify を待つのは `write_timeout` まで
- sslv2 / sslv3 / tlsv1 / tlsv1.1 を無効化する。最低 TLS 1.2
- `Request::peer()` は TLS でも accept 時の remote IP

### OpenAPI 生成（Phase 3）

```cpp
app.get("/openapi.json", [&app](hayate::Request &) {
    return hayate::Response::json(hayate::openapi(app, {.title = "my api"}));
});
```

- `Json hayate::openapi(const App&, OpenApiInfo = {})`。Handler も Middleware も返さない
- 新しい公開型は設定値 struct `OpenApiInfo{title, version}` だけ
- 出るのは `openapi` / `info` / `paths` の 3 つ。`"openapi": "3.1.0"`
- 情報源は登録済みルートの `pattern` と `method` だけ。
  **body / response のスキーマは出さない**（Router が型情報を持っていない）
- `:name` → `{name}`、`*name` → `{name}` + `x-hayate-wildcard: true`
  （OpenAPI の `{}` は本来 `/` を含まないので、違いを機械可読な形で残す）
- 静的セグメントは RFC 3986 の pchar 以外（`{` `}` `%` を含む）を percent-encode する。
  静的な `{id}` がテンプレート変数に化けず、`/a/{}` と `/a/:x` が同じ形にまとまらない
- path パラメータは `in: path` / `required: true` / `schema: {type: string}`
- 同じパスの GET と POST は 1 つの path 項目にまとまる
- 形が同じでパラメータ名だけ違うパス（`/users/:id` と `/users/:name`）も 1 つにまとめる。
  OpenAPI では同じテンプレートとして扱われるため。キーとパラメータ名は最初に登録したルートのもの
- 各 operation の `responses` は `default` 1 つだけ。ステータスを知らないので創作しない
- `group()` の prefix は畳み込まれた形（`/api/users`）で出る
- 文書の配り方は決めない。ルートに載せるのは利用者の仕事
- スキーマ推論 / `summary` / `tags` / `servers` / 認証定義 / YAML 出力はしない

### JWT 検証（Phase 3）

```cpp
app.use(hayate::mw::jwt({.secret = "...", .issuer = "", .audience = "",
                         .leeway = std::chrono::seconds(60)}));
```

- Middleware。公開型は設定 struct `Jwt` と `Claims` の 2 つだけ
- **HS256 のみ**。`alg` がそれ以外なら 401（`none` とアルゴリズム混同を断つ）
- 検証の順
  1. `Authorization` が `Bearer ` で始まる（スキームは大小無視）
  2. `.` で 3 つちょうどに割れる
  3. header と payload が base64url（パディング無し）で復号できる。末尾の未使用ビットが 0 でない
     符号は正準でないので復号できないとみなす（署名も同じ。1 つの署名に綴りが何通りもできない）
  4. header の `alg` が `HS256`（文字列でなければ 401）
  5. `HMAC-SHA256(secret, header_b64 + "." + payload_b64)` と署名が一致。比較は定数時間。
     HMAC の計算に失敗した場合と、計算結果が 32 バイトでない場合は 401（空署名として通さない）
  6. payload に `exp` があり、`now > exp + leeway` でない。`exp` 無しは 401。
     `exp` は int64 秒の整数のみ。小数・範囲外・非数値は 401。加算は飽和させ、溢れない
  7. `nbf` があれば `now + leeway >= nbf`。`nbf` の型と範囲は `exp` と同じ
  8. `issuer` 設定時は `iss` が一致（文字列でなければ 401）
  9. `audience` 設定時は `aud` が一致（文字列、または配列に含む）
- 失敗はすべて 401 `{"unauthorized"}` + `WWW-Authenticate: Bearer`。どの検査で落ちたかは返さない
- 通ったら `Claims` を Request Extension に入れる。寿命は Request
- `secret` が空、または `leeway` が負なら `jwt()` が投げる（`tls()` と同じく設定時に落とす）
- 適用範囲は `Router::group` で絞る。除外パスの設定項目は持たない
- RS256 / ES256 / JWKS / 鍵回転 / トークン発行 / 認可判定はしない
- トークンは `Authorization` からだけ取る。Cookie や query からは取らない

### metrics（Phase 3）

```cpp
app.get("/metrics", hayate::metrics(app));
```

- Handler 工場。`Metrics` / `Service` のようなクラスは作らない
- カウンタは App が 1 つ持つ。App をまたいで共有しない。グローバルにしない
- `Content-Type` は `text/plain; version=0.0.4; charset=utf-8`
- 出す系列

```
hayate_connections_accepted_total   counter
hayate_connections_rejected_total   counter  max_connections 超過で拒否した数
hayate_connections_open             gauge    いま開いている接続
hayate_requests_total               counter  書こうとした応答の数
hayate_responses_total{class="Nxx"} counter  1xx..5xx の 5 本
```

- `requests_total` と `responses_total` は応答を書く直前に数える。書き込みが途中で失敗しても数える。
  上限超過の 413 / 431 も数える（Router に届かなくても応答は返る）
- 系列はそれぞれ独立に進む。1 回の出力の中で系列同士（`requests_total` と `responses_total` の和など）が
  一致するとは限らない
- `responses_total` のクラスは `status / 100`。範囲外は数えない
- `/metrics` 自身は自分の出力に入らない。応答を書く直前に数えるので次のスクレイプに出る
- ヒストグラム / per-route ラベル / OpenTelemetry は出さない

### CORS（Phase 2）

```cpp
app.use(hayate::mw::cors());
app.use(hayate::mw::cors({.origin = "https://app.example"}));
```

- `hayate::mw::cors()` は Middleware。新しい公開型（Service 等）は足さない
- ルート登録はこれまで通り GET / POST のみ。OPTIONS は preflight 用にフレームワークが扱う
- `Origin` が無ければ `Access-Control-*` ヘッダを付けない
- `Origin` がある GET/POST（および 404/405）: `Access-Control-Allow-Origin`（既定 `*`、設定があればその値）
- `origin` が `*` 以外のときは `Vary: Origin` も付ける（共有キャッシュの取り違え防止）。
  応答が `Origin` の有無で変わるので、`Origin` が無い要求への応答にも付ける
- `Vary` は上書きしない。既存の値を残して `Origin` を足す。既に `Origin` か `*` を含むならそのまま
- `OPTIONS` + `Origin`: 204。`Allow-Origin` / `Allow-Methods` / `Allow-Headers`。`next` を呼ばない
- 既定 methods: `GET, POST, OPTIONS`。既定 headers: `Content-Type, Authorization`

### 静的ファイル（Phase 2 / 送出は Phase 3）

```cpp
app.get("/assets/*path", hayate::files("public"));
app.get("/assets/*path", hayate::files("public", 4u * 1024 * 1024));
app.get("/assets/*path", hayate::files("public", 4u * 1024 * 1024, 4));
```

- Handler 工場。`StaticFile` クラスは足さない（ER: Handler にぶら下がる）
- 署名は `Handler files(std::string_view root, std::uint64_t max_bytes = 0, std::uint32_t io_threads = 2,
  std::chrono::milliseconds fs_timeout = std::chrono::seconds(5))`
- `max_bytes` は配布上限。既定 `0` は無制限。`0` 以外でそれを超える実ファイルは 404（存在を漏らさない）
- root の末尾 `/` は無視する。登録時に root が無くても同じ扱い
- 相対パスの root は登録時のカレントディレクトリで絶対パスに解決する（未作成でも）
- wildcard 名は `path`。空なら `index.html`
- root の外（`..` / 絶対パス）は 404（存在を漏らさない）
- root 内かはパス要素単位で判定する。root が `/` でも配下を配る
- 復号後のパスに NUL が入っていたら 404
- 無いファイル・ディレクトリで `index.html` も無いときは 404
- ディレクトリで `index.html` があればそれを返す
- Content-Type は拡張子（`.html` `text/html`、`.css` `text/css`、`.js` `application/javascript`、`.json` `application/json`、`.txt` `text/plain`、その他 `application/octet-stream`）

I/O モデル:

- 正規化・stat・open・read は `files()` が所有するワーカープールで行う。io スレッドは filesystem を待たない
- プールは `files()` 1 回につき 1 つ、`io_threads` 本。既定 2。`0` は 1 に切り上げる
- 同時に走る読みは `io_threads` 本まで。溢れた分はプールのキューで待つ
- ハンドラはバイトを読まない。`Response` に `FileSource`（path / size / プール / 開いたファイル）を載せ、
  Connection が送出する。`path` は Content-Type の判定にだけ使う
- `FileSource` を作るのは `files()` だけ（開いたファイルの型は公開しない）。`pool` を取り出して
  App より長く持たない。ワーカーの完了は App の `io_context` に戻るので、App が先に壊れると
  壊れた strand を触る
- **open はハンドラ側（ワーカー）で済ませる。**root 内かの判定は開いた fd の実パスに対して行い、
  検証した対象と送る対象を同じにする。Connection は送出時にパスを辿り直さない（symlink 差し替えを防ぐ）
- 通常ファイル以外（FIFO・デバイス・ディレクトリ）は 404。open で待たされないよう
  非ブロッキングで開いてから種別を見て、通常ファイルと分かった時点で非ブロッキングを外す
- FS 待ち（正規化・stat・open とプールの順番待ち）が `fs_timeout` を超えたら 503。
  既定 5 秒。`files()` の第 4 引数で変える
- 期限を過ぎて待つのをやめた場合も、ワーカーが触る状態は処理が終わるまで生かす
- 本体はサイズに関係なく常に 64 KiB ずつ送る。1 応答のメモリはファイルサイズに依らず 64 KiB
- `Content-Length` を立てる。chunked encoding は使わない
- 送るのは stat した `size` まで。stat 後に伸びても増やさない
- ヘッダ送出後に読みが失敗したら（縮んだ・消えた）その場で接続を閉じる。status はもう直せない
- `write_timeout` はチャンクごとに張り直す。大きいファイルの総送出時間は縛らない
- 送出中のチャンク読みが `read_timeout` を超えたら接続を閉じる（応答はもう直せない）

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
