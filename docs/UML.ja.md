# UML — Hayate（エージェント向け）

[English](UML.md) | 日本語

英語版 [UML.md](UML.md) の訳。正本は英語版で、食い違えば英語版に従う。

正本は `docs/SPEC.md`。関係の正本は `docs/ER.md`。
ここに無い型・インタフェース・基底クラスを足さない。
受け入れは Phase 1、Phase 2（CORS / 静的ファイル / レート制限）、Phase 3（TLS / JWT 検証 / OpenAPI 生成 /
最小 metrics / 静的ファイルのストリーミング送出）。multipart / WS / SSE / gzip は実装しない。

図は Mermaid。注釈の意味:

| 注釈 | 意味 |
|---|---|
| なし | 公開 API の型（`include/hayate/`） |
| `<<mw>>` | 公開 API の型で、`hayate::mw` 名前空間にある（`mw::Jwt` など） |
| `<<alias>>` | `std::function` などの別名。独自の型ではない |
| `<<concept>>` | C++20 の concept |
| `<<internal>>` | `src/` の中の型。公開ヘッダに出ない |
| `<<conceptual>>` | 型は無い。持ち方を図にしただけ |

## クラス図

```mermaid
classDiagram
    class App {
        +use(mw) App
        +get(path, handler) App
        +post(path, handler) App
        +group(prefix, fn) App
        +bind(host, port) App
        +port() uint16
        +threads(n) App
        +tls(Tls) App
        +limits(Limits) App
        +limits() Limits&
        +run() awaitable~void~
        +serve()
        +stop()
    }

    class Router {
        +use(mw) Router
        +get(path, handler) Router
        +post(path, handler) Router
        +group(prefix, fn) Router
        +add(method, path, handler)
        +dispatch(req) awaitable~Response~
    }

    class Route {
        <<internal>>
        +method : HttpMethod
        +pattern : string
        +segs : Segment list
        +handler : Handler
    }

    class Segment {
        <<internal>>
        +kind : lit | param | wild
        +s : literal text or name
    }

    class Handler {
        <<alias>>
        function awaitable~Response~ of Request&
    }

    class Next {
        <<alias>>
        Handler
    }

    class Middleware {
        <<alias>>
        function awaitable~Response~ of Request&, Next
    }

    class HandlerCallable {
        <<concept>>
        const-callable, returns Response or awaitable~Response~
    }

    class Limits {
        +max_header_bytes : uint64 = 8192
        +max_body_bytes : uint64 = 1048576
        +read_timeout : ms = 30s
        +write_timeout : ms = 30s
        +idle_timeout : ms = 60s
        +max_connections : uint32 = 1024
    }

    class Listener {
        <<conceptual>>
        acceptor + run() accept loop on admin strand
        +port : uint16
        +accepting : bool
    }

    class Connection~Stream~ {
        <<internal>>
        +peer : string
        +limits : Limits copy
        +router : Router&
        +counters : Counters&
        +shutting : atomic bool&
        +waiting : bool
        +run() awaitable~void~
        +cancel_if_waiting()
    }

    class ConnectionSet {
        <<internal>>
        +add(cancel) id
        +remove(id)
        +cancel_all()
    }

    class Counters {
        <<internal>>
        +accepted
        +rejected
        +requests
        +by_class 1xx..5xx
    }

    class Request {
        +method() HttpMethod
        +target() string_view
        +path() string_view
        +query(key) string_view
        +header(name) string_view
        +param(name) string_view
        +peer() string_view
        +body() span~const byte~
        +json() Result~Json~
        +json~T~() Result~T~
        +set~T~(args) T&
        +get~T~() T*
        -target, path, body : string
        -headers, query, params : string pairs
        -ext : type_index to shared_ptr void
    }

    class Response {
        +status() uint16
        +status(code) Response&
        +body() string_view
        +is_file() bool
        +file_source() FileSource*
        +header(name) string_view
        +set_header(name, value) Response&
        +text(s)$ Response
        +json(v)$ Response
        +no_content()$ Response
        +from_error(e)$ Response
        +file(src)$ Response
    }

    class FileSource {
        +path : filesystem path
        +size : uint64
        +pool : shared_ptr thread_pool
        +file : shared_ptr OpenFile
    }

    class OpenFile {
        <<internal>>
        verified open fd, read sequentially
    }

    class Tls {
        +cert_file : string
        +key_file : string
        +key_password : string
    }

    class Jwt {
        <<mw>>
        +secret : string
        +issuer : string
        +audience : string
        +leeway : seconds
    }

    class Claims {
        +json : Json
    }

    class OpenApiInfo {
        +title : string
        +version : string
    }

    class Cors {
        <<mw>>
        +origin : string = "*"
        +methods : string
        +headers : string
    }

    class RateLimit {
        <<mw>>
        +max : uint32 = 60
        +window : ms = 60s
    }

    class Result~T~ {
        +ok(value)$ Result
        +err(error)$ Result
        +ok() bool
        +value() T
        +error() Error
    }

    class Error {
        +code : string
        +message : string
        +http_status : uint16
    }

    App "1" *-- "1" Router : owns
    App "1" *-- "1" Limits : always
    App "1" *-- "0..1" Listener : bind
    App "1" *-- "1" ConnectionSet : stop cancels waiting
    App "1" *-- "1" Counters : metrics
    Router "1" *-- "*" Route : registers
    Router "1" *-- "*" Middleware : onion
    Route "1" *-- "*" Segment : pattern
    Route "1" *-- "1" Handler : by value
    Listener ..> Connection : accept creates, self-owned
    Connection --> Router : dispatch
    Connection --> ConnectionSet : registers
    Connection --> Counters : counts
    Connection ..> Request : builds one per request
    Connection ..> Response : writes
    Response "1" *-- "0..1" FileSource : bytes or file
    FileSource "1" *-- "1" OpenFile : sends
    Handler ..> Request : reads
    Handler ..> Response : returns
    Middleware ..> Next : may call
    HandlerCallable ..> Handler : wrap_handler adapts
    Result~T~ o-- Error
    Error ..> Response : from_error
    Tls ..> App : tls() builds ssl context
    Cors ..> Middleware : mw cors() builds
    Jwt ..> Middleware : mw jwt() builds
    RateLimit ..> Middleware : mw rate_limit() builds
    Claims ..> Request : set by mw jwt
    OpenApiInfo ..> App : openapi() reads routes
```

- 合成 `*--` は所有。Request は要求の文字列（target・ヘッダ・本文・param）を所有し、アクセサは view を返す。
  view を Response や App に保存しない
- `group()` は一時的な子の Router を作り、そのルートを子の MW で包んで親に足してから捨てる。
  Router は Router を持たない。接頭辞は連結して `//` を畳む
- Connection は `shared_ptr` と detached `co_spawn` で自分を持つ。App が持つのは接続の数と、`stop()` が
  取り消しに使う `ConnectionSet` だけ。接続ごとに strand を 1 本作り、その接続の I/O・タイマー・
  coroutine を載せる。accept ループと `stop()` は admin strand 1 本に載せる
- Handler と Middleware は利用者の関数オブジェクト。const で呼べること（`HandlerCallable`。`mutable` ラムダは
  コンパイル時に弾く）。同期のハンドラは `wrap_handler` が awaitable に包む。仮想基底を切らない
- 設定値の struct: `Limits` / `Tls` / `mw::Jwt` / `mw::Cors` / `mw::RateLimit` / `OpenApiInfo`。
  工場: `mw::cors()` / `mw::jwt()` / `mw::rate_limit()` が Middleware、`files()` / `metrics()` が Handler、
  `openapi()` が `Json` を返す。`ssl::context` は App の中にしか出てこない
- `Claims` を `Json` の別名にしないのは、Extension のキーが `typeid` で、別名だと他の `Json` と衝突するため
- `files(root, max_bytes, io_threads, fs_timeout)` のワーカープールは、返したクロージャと、返した
  `FileSource` が共有する。`FileSource` を作るのは `files()` だけ（`OpenFile` は公開しない）。
  プールを取り出して App より長く持たない
- `StaticFile` / `Service` / `TlsContext` / `Metrics` のようなクラスは作らない

## パッケージ

```mermaid
classDiagram
    class include_hayate {
        App, Router, Request, Response
        Result, Error, Limits, Tls
        HttpMethod, Json
        Handler, Middleware, Next, HandlerCallable
        files, metrics, openapi, OpenApiInfo
        mw cors, Cors, mw jwt, Jwt, Claims
        mw rate_limit, RateLimit
    }
    class src {
        App impl, accept, stop
        Connection
        Router impl, Route, Segment
        Request, Response impl
        files impl, jwt impl
    }
    class src_detail {
        asio : Asio, Beast, SSL names
        ascii, percent, base64
        hmac : OpenSSL
        jwt_time, openapi, metrics
        offload : thread_pool
        open_file, files
        connection_set
    }
    class examples_hello
    class tests

    include_hayate <.. examples_hello : public headers only
    include_hayate <.. tests
    src_detail <.. tests : unit tests of detail
    src ..> include_hayate : implements
    src ..> src_detail : uses
    examples_hello ..> src_detail : forbidden
```

`include/hayate/rate_limit.hpp` の `mw::detail`（`RateTable` / `hit`）は、ヘッダだけの MW を作るために
公開ヘッダにあるが、API ではない。

## 接続の状態

```mermaid
stateDiagram-v2
    [*] --> handshake : accept, TLS
    [*] --> waiting : accept, plain
    handshake --> waiting : ok
    handshake --> shutdown : fail, read_timeout, stop
    waiting --> in_request : header complete
    waiting --> shutdown : timeout, peer close, stop, 431 or 413 written
    in_request --> waiting : response written, keep-alive
    in_request --> shutdown : Connection close, write error, write_timeout, file read fail
    shutdown --> closed : TLS close_notify, then socket shutdown
    closed --> [*]
    note right of waiting : stop() cancels only here and in handshake
    note right of shutdown : while stopping, TLS sends close_notify without waiting for the reply
```

- `waiting` の窓は、1 本目のヘッダ待ちが `read_timeout`、keep-alive の次のヘッダ待ちが `idle_timeout`。
  ヘッダが揃った後の本文読みは `read_timeout`
- timeout は閉じる。`shutting` を立てるのは `stop()` だけで、立っていれば次の要求を待たずに閉じ、
  書く応答には `Connection: close` を付ける
- `max_connections` を超えた接続は、Connection を作る前に accept してすぐ閉じる（応答は書かない）

## シーケンス（1 リクエスト）

```mermaid
sequenceDiagram
    actor Client
    participant A as App admin strand
    participant C as Connection strand
    participant R as Router
    participant M as App MW
    participant G as group MW
    participant H as Handler
    participant P as files() pool

    Client->>A: TCP connect
    A->>A: accept, close if over max_connections
    A->>C: co_spawn on a new strand
    opt TLS
        Client->>C: handshake within read_timeout
    end
    Client->>C: request header
    alt header over max_header_bytes, or Content-Length over max_body_bytes
        C-->>Client: 431 or 413, no middleware
    else ok
        opt Expect 100-continue on HTTP/1.1
            C-->>Client: 100 Continue
        end
        Client->>C: body within read_timeout
        C->>R: dispatch(Request)
        R->>M: enter A then B
        alt short circuit
            M-->>R: Response
        else next
            M->>R: match
            alt no path, method mismatch, or HEAD
                R-->>M: 404 or 405 Allow
            else match
                R->>G: group MW composed into the route
                G->>H: Request
                opt files()
                    H->>P: stat and open, 503 after fs_timeout
                    P-->>H: FileSource
                end
                H-->>G: Response
                G-->>R: Response
                R-->>M: Response
            end
            M->>M: leave B then A
        end
        R-->>C: Response
        alt FileSource
            loop every 64 KiB
                C->>P: read within read_timeout
                C-->>Client: chunk within write_timeout
            end
        else bytes
            C-->>Client: response
        end
    end
```

- HEAD は本文を送らないが、`Content-Length` は本文を送った場合の値を付ける
- 1xx / 204 / 304 は本文も `Content-Length` も送らない
- ヘッダが 65533 バイトを超える応答は送れないので、Connection が 500 に差し替える（MW は通らない）

## エージェント向け制約（図から外さないこと）

- マクロで Route を登録しない
- `Router::add` は GET / POST だけ。同じメソッドの同じ形の二重登録、名前の無い `:` / `*`、途中の `*name` は
  登録時に `std::invalid_argument`。`:name` は空のセグメントに一致しない。完全一致は空の `*name` に勝つ
- パスはセグメントに分けてから復号する。`param()` / `query()` は復号後、`path()` は生
- `set_header` は token でない名前を捨て、値から CTL を落とす（ヘッダ注入を断つ）
- 例外は Handler / Middleware の境界を出ない。ハンドラの分は `dispatch_route`、MW の分は `dispatch` が
  受けて 500 にする
- Asio/Beast の失敗は、応答を書ける段階なら Error にして応答する（413 / 431 / 500）。書けない段階
  （timeout・相手の切断・handshake 失敗）なら閉じるだけで、受け取る側のない Error は作らない
- `std::expected` 禁止。`Result<T>` は `std::variant` の 1 実装
- Request の view を Response や App に保存しない
- 共有可変グローバルを置かない。状態は App か Request の Extension、または MW / Handler の工場が持つ
  もの（`rate_limit` の mutex 付き map、`files()` のプール）
- `stop()` が取り消すのは要求を待っている接続だけ。ヘッダが揃った要求は完了させる
- この図に無い基底（Service / Context / ApplicationBuilder）を足さない
- App の `use` は 404/405 を含む dispatch 全体を包む。`group` の MW はマッチした Route だけ
