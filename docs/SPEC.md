# SPEC

English (canonical) | [日本語](SPEC.ja.md)

## Purpose

Build **Hayate** (namespace `hayate`), a framework and HTTP server for C++20 web services.
Small, fast, strongly typed. Handlers are coroutines. The public API is fluent and uses concepts. Routes are never registered with macros.

What users write (Phase 1):

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

`include/hayate/hayate.hpp` introduces `namespace asio = boost::asio;`.

Phases:

- Phase 0: CMake Presets, skeleton public headers, Hello with a fixed response
- Phase 1: HTTP/1.1, routing, JSON, middleware, limits, graceful shutdown
- Phase 2: CORS / static files / multipart / WS / SSE / gzip / rate limiting
- Phase 3: TLS / JWT verification / OpenAPI generation / minimal metrics / streaming of static files

Currently accepted: Phase 1, Phase 2 (CORS / static files / rate limiting) and Phase 3 (TLS / JWT verification / OpenAPI generation / minimal metrics / streaming of static files). Multipart / WS / SSE / gzip are not implemented.

## Non-goals

- Copying code, internal structure or macros from existing C++ web frameworks (Drogon / Crow / Oat++ / Cinatra / userver)
- An event loop, HTTP parser, TLS or JSON of our own
- HTTP/2, HTTP/3, gRPC, GraphQL
- HTML templates, ORM, migrations
- Features that require C++23/26, C++ Modules, reflection macro DSLs
- Huge configuration file hierarchies, plugin loaders, hot reload
- Windows-only code (for now)
- Samples, abstractions or "for the future" layers nobody asked for
- Decisions that live only in chat (the canonical source is `docs/SPEC.md`)

## Acceptance criteria

Phase 0

- `cmake --preset debug` builds
- Hello opens a port and returns a fixed response
- Public headers live in `include/hayate/`

Phase 1

- `cmake --build --preset test && ctest --preset test` has zero failures
- `GET /` on `examples/hello` returns 200
- `:param` and JSON POST are shown by tests
- Middleware is tested as an onion (in A→B, out B→A, short-circuit without `next`)
- A wrong method is 405 (with Allow), no matching path is 404
- Oversized body / timeout / shutdown (in-flight requests complete, new ones are refused) are tested
- The repository has no HTTP/2 / ORM / templates (TLS was accepted in Phase 3)
- hello depends only on public headers
- No new leaks or UAF under debug + ASan. Apple Clang's ASan on macOS has no LeakSanitizer
  (`detect_leaks is not supported on this platform`), so the debug preset only shows UAF.
  Leaks are checked by building with ASan using Homebrew LLVM's clang and running every test with
  `ASAN_OPTIONS=detect_leaks=1` (2026-09-21, LLVM 23, 234 tests, 0 leaks)
- The diff contains no files nobody asked for

Phase 2 (CORS)

- A GET with `Origin` gets `Access-Control-Allow-Origin`: `*` by default, the configured value if set
- A request without `Origin` (including an empty `Origin:`) gets no `Access-Control-*`
- `OPTIONS` + `Origin` is 204 with `Allow-Methods` / `Allow-Headers`, and the handler is not called
- With a fixed origin, `Vary: Origin` is sent whether or not `Origin` is present and even on error responses. An existing `Vary` is kept
- A 404 also gets `Access-Control-Allow-Origin`

Phase 2 (static files)

- Files inside root are returned with the `Content-Type` for their extension. A directory returns `index.html`
- Missing files, paths outside root (`..`, absolute paths, symlinks leading outside root) and paths containing NUL are 404
- Files larger than `max_bytes` are 404
- A root that does not exist at registration is served once it is created

Phase 2 (rate limiting)

- Going over `max` within the window gives 429 with `Retry-After` (rounded up, at least 1)
- The key is the connection's peer. Changing `X-Forwarded-For` lands in the same window. Keep-alive requests land in the same window
- After the window passes, counting starts over. Expired keys are swept

Phase 3 (streaming static files)

- Memory per response caps at 64 KiB whatever the file size
- `Content-Length` equals the bytes actually sent, and there is no `Transfer-Encoding: chunked`
- The next keep-alive request succeeds after a streamed response
- A 0-byte file returns 200 / `Content-Length: 0`
- With the default arguments, `max_bytes` imposes no limit

Phase 3 (TLS)

- `GET /` over TLS returns 200, and a second request on the same connection succeeds too
- A plaintext client connecting to an App with `tls()` gets no response
- `tls()` throws on an unreadable certificate / key
- Static-file streaming arrives with exactly `Content-Length` bytes over TLS too

Phase 3 (OpenAPI generation)

- Every registered route appears in `paths`
- `:id` becomes a `parameters` entry (`in: path`, `required: true`, `type: string`)
- GET and POST on the same path are merged into one path item
- No keys appear for methods that were not registered
- Wildcard parameters carry `x-hayate-wildcard`
- Paths appear with the `group()` prefix applied

Phase 3 (JWT verification)

- A valid token gives 200, and the handler can read `sub` from `Claims`
- Missing header / malformed / tampered signature / tampered payload / expired `exp` / no `exp` / future `nbf` /
  `alg: none` / spoofed `alg` (an RS256 header signed with HMAC) are all 401
- The 401 carries `WWW-Authenticate: Bearer`
- A token that expired within `leeway` passes
- `jwt()` throws when `secret` is empty
- Routes outside the group pass without authentication

Phase 3 (metrics)

- After two requests, `requests_total` reads 2
- A 404 raises `4xx` by 1 and leaves `5xx` alone
- A connection over `max_connections` raises `rejected_total` by 1
- Each connection raises `accepted_total` by 1

## Technical decisions

- Language: strict C++20. No `std::expected`
- Build: CMake 3.28+, presets `debug` / `release` / `test` / `tsan`. debug has ASan + UBSan, `tsan` has TSan.
  `test` shares the build directory with `debug` (`--build --preset test` works right after `--preset debug`)
- Targets: macOS (Apple Clang) and Linux (GCC 13+ / Clang 16+). Windows comes later.
  Linux is checked in CI (GitHub Actions, ubuntu-24.04, GCC 13 and Clang 16, `test` and `tsan`).
  GCC 12 is not a target. For a temporary capturing lambda placed inside a co_await expression, it runs the
  destructors of the captures twice (GCC PR 101367, fixed in 13, not backported to 12). Perfectly ordinary
  handler code hits it
- I/O: Boost.Asio 1.83+. `asio::awaitable` / `co_spawn`. Public headers declare `namespace asio = boost::asio;`
- File I/O: blocking filesystem calls are moved to an `asio::thread_pool`. `asio::stream_file` is not used: it depends on `BOOST_ASIO_HAS_FILE` (Windows handles / Linux io_uring) and does not exist on macOS
- HTTP: Boost.Beast (HTTP/1.1)
- TLS: OpenSSL 3 via `asio::ssl`. `find_package(OpenSSL 3 REQUIRED)`. TLS 1.2 minimum
- JSON: nlohmann/json v3.11.3 only. `hayate::Json` is an alias of `nlohmann::json`. Current glaze requires C++23, so it is not used. No mixing
- Errors: `hayate::Result<T>` is a single thin in-house `std::variant<T, Error>`. No Boost.Outcome
- Tests: GoogleTest. A failing test before the implementation. Loopback + ephemeral ports. No sleep-based synchronization.
  Test clients use async I/O so their deadlines take effect (Beast's deadlines do not apply to sync I/O), and CTest has a TIMEOUT too
- Dependencies: Boost and OpenSSL via `find_package`. nlohmann/json and GoogleTest via FetchContent. No vcpkg
- Distribution: source at annotated tags only. Consumers use FetchContent / `add_subdirectory`, and the target is `hayate::hayate`.
  There is no install / export (nlohmann/json comes from FetchContent, so that would also need a path that resolves it with `find_dependency`)
- Versioning: SemVer. While 0.x, a minor release may break the public API. A patch release does not
- Documents: English is canonical. The Japanese translation sits next to each file as `*.ja.md`.
  Change the English first and the translation in the same commit. When they disagree, the English wins
- Ownership: Request owns the request strings (target, headers, body), and its accessors return `string_view` / `span<const byte>`. Their lifetime is the Request
- Threads: one per io_context. Several with `app.threads(n)`. Shared mutable state needs a strand or a mutex, written down first
- The unit of concurrency is the connection. One strand per connection serializes it; different connections run in parallel
- Banned: `new`/`delete`/`malloc`, raw arrays, exceptions crossing the handler boundary, shared mutable globals
- Work order: this SPEC → public headers → failing tests → minimal implementation → all tests → stop
- The canonical source is `docs/SPEC.md`. The ARCHITECTURE that the ER refers to is the "Routing" section of this SPEC.

## Phase 1 public API

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

`value()` when `ok()==false`, and `error()` when `ok()==true`, are contract violations (assert). Results are never returned through exceptions.
Do not read `r` after `std::move(r).value()` (its contents have been moved from, and `ok()` stays true).
`error()` exists only as `const&`; there is no version that moves the error out.

### Handler / Middleware

```cpp
using Handler = std::function<asio::awaitable<Response>(Request&)>;
using Next = Handler;
using Middleware = std::function<asio::awaitable<Response>(Request&, Next)>;
```

`App::get/post` accept:

- `asio::awaitable<Response>(Request&)`
- `Response(Request&)` (wrapped into an awaitable internally)
- Either way it must be **callable as const** (`mutable` lambdas are rejected at compile time). Handlers are shared
  by all connections, so state kept in them races under `threads(n>1)`. Put state in the App or a Request Extension.
  The requirement is expressed by the public concept `hayate::HandlerCallable`

Onion: in registration order A→B on the way in, B→A on the way out. Not calling `next` short-circuits.
The App's `use()` wraps the whole dispatch, 404/405 included (CORS preflight and error responses need headers).
413 / 431, and the 500 that replaces an unsendable header, are made by the Connection before or after the Router, so they
skip App middleware too (and get no CORS headers).
`group` middleware attaches only to matched routes. A 404 / 405 under a group's path matched no route,
so it skips the group's middleware (App middleware only).
Nesting is App → outer group → inner group → handler. `use()` applies to every route of that App / Router,
wherever it is called.
Exceptions thrown by handlers and middleware are turned into 500 by `dispatch`. The connection stays open.

### Routing

- Methods are `GET` and `POST` only. Passing any other method to `Router::add` throws `std::invalid_argument`
- `:name` is one non-empty segment. `*name` is the whole rest (may be empty) and may only be the last segment
- A `:` / `*` without a name, and a `*name` that is not last, throw `std::invalid_argument` at registration
- A missing param / query / header is an empty `string_view`
- Match priority: static > param > wildcard at each segment. If still tied and exactly one of them ends with an empty `*name`,
  the other one wins (`GET /a` against `/a` and `/a/*rest` picks `/a`). Registration order never decides
- Registering the same shape (same kind for every segment and same static text; param / wildcard names are ignored)
  twice for the same method throws. No request could ever reach the later one
- No matching path is 404. A matching path with the wrong method is 405 + `Allow`
- The leading `/` of a pattern may be omitted (`get("ping")` is `/ping`). OpenAPI shows it with the `/` too
- `group(prefix, fn)` concatenates prefixes and inserts a `/` at the joint if it is missing (`group("/api")` + `get("ping")` is
  `/api/ping`). Runs of `/` in the joined whole collapse to one. A trailing `/` is kept
- `/a` and `/a/` are different routes
- A path is split into segments before percent-decoding. `%2F` does not split; it becomes a `/` inside the segment
- Queries are percent-decoded, and `+` reads as a space
- Malformed `%` sequences are left as they are, undecoded
- `target()` and `path()` are raw. Decoded values are visible only through `param()` and `query()`

### App lifetime

- App and `Router` can be neither copied nor moved. The only `Router`s are the one the App owns and the temporary one
  `group()` passes to `fn`
- The App owns the `io_context`, the `Router`, the `Limits` and the acceptor
- `bind(host, port)` is synchronous up to socket bind + listen. Port 0 means ephemeral
- `port()` is the actual port after bind
- `run()` is the accept loop, an `asio::awaitable<void>`. It ends on `stop()`.
  The acceptor is bound to the App's `io_context`, so `run()` must be spawned on that `io_context` too.
  It does not work on an external executor
- An accept failure does not end the accept loop (only `stop()` does). After a failure it waits 100 ms before the
  next accept (no busy loop on fd exhaustion). `stop()` cancels that wait as well
- Each accepted connection gets its own strand. That connection's socket, stream, timers and coroutine all
  run on it (Beast streams require this when `threads(n)` has n>1)
- The accept loop and `stop()` are serialized on one admin strand. `stop()` may be called from any thread
  any number of times; it takes effect once
- `serve()` runs the io_context blocking on `threads(n)` threads (default 1). SIGINT/SIGTERM call `stop()`
- `stop()` stops new accepts, lets in-flight reads and writes finish, then stops the io_context
- Connections waiting for a request to arrive (TLS handshake, waiting for the first header, waiting for the next keep-alive header)
  are closed by `stop()` at once, even if part of a header has arrived (the request is not complete yet). A request whose
  header is complete (reading the body, the handler, writing) is finished and closed with `Connection: close`

### Limits defaults

| Setting | Default |
|---|---|
| max_header_bytes | 8192 |
| max_body_bytes | 1048576 |
| read_timeout | 30s |
| write_timeout | 30s |
| idle_timeout | 60s |
| max_connections | 1024 |

Exceeded: header 431, body 413. A read/write/idle timeout closes the connection (no response if one cannot be written). A new connection over `max_connections` is accepted and closed at once (no response).

Beast / Asio failures become an `Error` and a response (413 / 431 / 500) while a response can still be written. When it cannot (timeout, peer disconnect, handshake failure), the connection is just closed, and no `Error` is created that nobody would receive.

Which window applies: reading the first header uses `read_timeout`. Waiting for the next request's header on keep-alive uses `idle_timeout`. Reading the body once the header is complete uses `read_timeout` for every request.

HTTP/1.1 promises:

- HEAD is not routed (it is not treated as GET, so it stays 404 / 405). But a response to HEAD never carries a body,
  whatever the status. `Content-Length` is the value it would have if the body were sent
- For a request with `Expect: 100-continue`, `100 Continue` is sent before the body is read.
  If `Content-Length` exceeds `max_body_bytes`, 413 is sent without the 100.
  100-continue on HTTP/1.0 requests is ignored (they do not know 1xx; RFC 9110 §10.1.1)
- The connection continues only if both "the server decides so" and "the response's `Connection` allows it". If the handler sets
  `Connection: close`, it closes. Once the server decides to close (the request asked for close, stopping, …), the handler's keep-alive
  is ignored and `Connection: close` is sent. The header sent and the actual behavior never disagree
- 1xx / 204 / 304 responses have no body. The handler's body is dropped, and neither `Content-Length` nor
  `Transfer-Encoding` is sent (RFC 9110 §8.6, §6.4.1)
- A response with a header name or value over 65533 bytes (Beast's limit) cannot be sent. It is replaced by 500, and metrics count it as 500

### JSON

- `Response::json(Json)` is 200 / `application/json`
- `Request::json()` reads the body as `Json`. Malformed input is `Error{code:"bad_json", http_status:400}`
- `Request::json<T>()` returns the same 400 on a type mismatch
- There is no Content-Type check (only the body bytes are looked at)

### Response headers

- `set_header(name, value)` replaces a header of the same name (case-insensitive)
- It does nothing if `name` is not an HTTP token
- It strips CTLs (including `\r` `\n`) from `value` and trims surrounding whitespace. This blocks header injection

### Extension

```cpp
template<typename T, typename... Args>
T& Request::set(Args&&... args);  // the same T overwrites
template<typename T>
T* Request::get() noexcept;       // nullptr if absent
```

The lifetime is the Request. Do not store the pointer in a Response / App.
`set` of the same `T` again destroys the previous value and invalidates the references and pointers returned before.

### TLS (Phase 3)

```cpp
app.tls({.cert_file = "server.pem", .key_file = "server.key"})
   .bind("0.0.0.0", 8443)
   .serve();
```

- The only new public type is the settings struct `Tls`. No class like `TlsContext`
- The App builds the `ssl::context` internally. `asio::ssl` does not appear in public headers
- After `tls()`, every connection of the App is TLS. There is no plaintext listener alongside it
- `tls()` throws if the certificate / key cannot be read or the key does not pair with the certificate (it fails at configuration time, like `bind()`)
- An empty `key_password` means the key has no passphrase. For an encrypted key `tls()` throws (it never prompts on the terminal)
- The handshake window is `read_timeout`. A failed connection is closed without writing a response
- Closing is TLS shutdown, then socket shutdown. Waiting for the peer's close_notify is bounded by `write_timeout`.
  While stopping, close_notify is sent without waiting for the reply (RFC 8446 §6.1; waiting would keep `serve()` from returning)
- sslv2 / sslv3 / tlsv1 / tlsv1.1 are disabled. TLS 1.2 minimum
- `Request::peer()` is the remote IP at accept, over TLS too

### OpenAPI generation (Phase 3)

```cpp
app.get("/openapi.json", [&app](hayate::Request &) {
    return hayate::Response::json(hayate::openapi(app, {.title = "my api"}));
});
```

- `Json hayate::openapi(const App&, OpenApiInfo = {})`. It returns neither a Handler nor a Middleware
- The only new public type is the settings struct `OpenApiInfo{title, version}`
- Only `openapi` / `info` / `paths` are emitted. `"openapi": "3.1.0"`
- The only sources are the `pattern` and `method` of the registered routes.
  **No body / response schemas** (the Router has no type information)
- `:name` → `{name}`, `*name` → `{name}` + `x-hayate-wildcard: true`
  (an OpenAPI `{}` normally does not contain `/`, so the difference is kept in machine-readable form)
- Static segments percent-encode everything outside RFC 3986 pchar (including `{` `}` `%`).
  A static `{id}` never turns into a template variable, and `/a/{}` and `/a/:x` do not merge into one shape
- Path parameters are `in: path` / `required: true` / `schema: {type: string}`
- GET and POST on the same path are merged into one path item
- Paths of the same shape that differ only in parameter names (GET `/users/:id` and POST `/users/:name`) are merged too,
  because OpenAPI treats them as the same template. The key and parameter names come from the route registered first.
  The same method with the same shape throws at registration, so merging only happens between GET and POST
- Each operation's `responses` has only `default`. The statuses are unknown, so none are invented
- `group()` prefixes appear folded in (`/api/users`)
- How the document is served is not decided. Putting it on a route is the user's job
- No schema inference / `summary` / `tags` / `servers` / security definitions / YAML output

### JWT verification (Phase 3)

```cpp
app.use(hayate::mw::jwt({.secret = "...", .issuer = "", .audience = "",
                         .leeway = std::chrono::seconds(60)}));
```

- Middleware. The only public types are the settings struct `Jwt` and `Claims`
- **HS256 only**. Any other `alg` is 401 (this blocks `none` and algorithm confusion)
- Verification order
  1. `Authorization` starts with `Bearer` and one or more spaces (the scheme is case-insensitive; RFC 6750 `1*SP`;
     tabs do not count as spaces)
  2. It splits on `.` into exactly 3 parts
  3. The header and payload decode as base64url (no padding). An encoding whose unused trailing bits are not 0
     is not canonical and counts as undecodable (the signature too, so one signature cannot have several spellings)
  4. The header's `alg` is `HS256` (401 if it is not a string)
  5. `HMAC-SHA256(secret, header_b64 + "." + payload_b64)` matches the signature, compared in constant time.
     If the HMAC computation fails or its result is not 32 bytes, 401 (it never passes as an empty signature)
  6. The payload is a JSON object. `exp` is present and `now > exp + leeway` does not hold. No `exp` is 401.
     `exp` must be an integer number of seconds in int64; fractional, out-of-range or non-numeric values are 401. The addition saturates and never overflows
  7. If `nbf` is present, `now + leeway >= nbf`. `nbf` has the same type and range rules as `exp`
  8. If `issuer` is set, `iss` matches (401 if it is not a string)
  9. If `audience` is set, `aud` matches (a string, or contained in an array)
- Every failure is 401 + `WWW-Authenticate: Bearer`. The body is the text/plain `Unauthorized` built from
  `Error{code: "unauthorized"}`. Which check failed is not revealed
- On success, `Claims` is put into a Request Extension. Its lifetime is the Request
- `jwt()` throws if `secret` is empty or `leeway` is negative (it fails at configuration time, like `tls()`)
- The scope is narrowed with `Router::group`. There is no setting for excluded paths
- No RS256 / ES256 / JWKS / key rotation / token issuance / authorization decisions
- Tokens are taken only from `Authorization`, never from cookies or queries

### metrics (Phase 3)

```cpp
app.get("/metrics", hayate::metrics(app));
```

- A Handler factory. No class like `Metrics` / `Service`
- The App owns one set of counters. It is never shared across Apps and never global
- `Content-Type` is `text/plain; version=0.0.4; charset=utf-8`
- Series emitted

```
hayate_connections_accepted_total   counter
hayate_connections_rejected_total   counter  refused for exceeding max_connections
hayate_connections_open             gauge    connections open now
hayate_requests_total               counter  responses about to be written
hayate_responses_total{class="Nxx"} counter  five series, 1xx..5xx
```

- `requests_total` and `responses_total` are counted just before a response is written. They count even if the write fails midway.
  The 413 / 431 from limits count too (a response goes out even though the Router was never reached)
- Each series advances on its own. Within one output, series are not guaranteed to agree with each other
  (for example `requests_total` and the sum of `responses_total`)
- The `responses_total` class is `status / 100`. Statuses out of range are not counted
- `/metrics` does not appear in its own output. It is counted just before its response is written, so it shows up in the next scrape
- No histograms / per-route labels / OpenTelemetry

### CORS (Phase 2)

```cpp
app.use(hayate::mw::cors());
app.use(hayate::mw::cors({.origin = "https://app.example"}));
```

- `hayate::mw::cors()` is Middleware. No new public types (Service etc.)
- Route registration stays GET / POST only. OPTIONS is treated as a preflight only when `mw::cors` is installed
  (otherwise it is 404 / 405)
- Without `Origin`, no `Access-Control-*` headers are added. An empty `Origin:` counts as absent
- GET/POST (and 404/405) with `Origin`: `Access-Control-Allow-Origin` (`*` by default, the configured value if set)
- When `origin` is not `*`, `Vary: Origin` is added too (so shared caches do not mix responses up).
  The response depends on whether `Origin` is present, so responses to requests without `Origin` get it as well
- `Vary` is not overwritten. Existing values are kept and `Origin` is added. If it already contains `Origin` or `*`, it is left as is
- `OPTIONS` + `Origin`: 204 with `Allow-Origin` / `Allow-Methods` / `Allow-Headers`. `next` is not called
- Default methods: `GET, POST, OPTIONS`. Default headers: `Content-Type, Authorization`

### Static files (Phase 2 / sending is Phase 3)

```cpp
app.get("/assets/*path", hayate::files("public"));
app.get("/assets/*path", hayate::files("public", 4u * 1024 * 1024));
app.get("/assets/*path", hayate::files("public", 4u * 1024 * 1024, 4));
```

- A Handler factory. No `StaticFile` class (ER: it hangs off Handler)
- The signature is `Handler files(std::string_view root, std::uint64_t max_bytes = 0, std::uint32_t io_threads = 2,
  std::chrono::milliseconds fs_timeout = std::chrono::seconds(5))`
- `max_bytes` is the serving limit. The default `0` means unlimited. When non-zero, a real file larger than it is 404 (existence is not leaked)
- A trailing `/` on root is ignored. The same holds when root does not exist at registration
- A relative root is resolved to an absolute path against the current directory at registration (even if it does not exist yet)
- The wildcard name is `path`. Empty means `index.html`
- Outside root (`..` / absolute paths) is 404 (existence is not leaked)
- Whether a path is inside root is judged per path element. Files under root are served even when root is `/`
- A decoded path containing NUL is 404
- A missing file, or a directory without `index.html`, is 404
- A directory with `index.html` returns that file
- Content-Type follows the extension (`.html` / `.htm` `text/html`, `.css` `text/css`, `.js` `application/javascript`, `.json` `application/json`, `.txt` `text/plain`, anything else `application/octet-stream`). Extension case is ignored

I/O model:

- Normalization, stat, open and read run on the worker pool owned by `files()`. io threads never wait on the filesystem
- One pool per `files()` call, with `io_threads` threads. Default 2. `0` rounds up to 1
- At most `io_threads` reads run at once. The rest wait in the pool's queue
- The handler reads no bytes. It puts a `FileSource` (path / size / pool / open file) in the `Response`,
  and the Connection sends it. `path` is used only to pick the Content-Type
- A file response's `body()` is empty (the bytes have not been read yet). Middleware tells them apart with `is_file()` / `file_source()`
- Only `files()` creates a `FileSource` (the open-file type is not public). Do not take the `pool` out and
  keep it longer than the App. Worker completions return to the App's `io_context`, so if the App is destroyed first
  they touch a destroyed strand
- **Opening happens on the handler side (the worker).** Whether the file is inside root is judged on the real path of the opened fd,
  so what was verified and what is sent are the same file. The Connection does not walk the path again when sending (this prevents symlink swaps)
- Anything but a regular file (FIFO, device, directory) is 404. To avoid blocking in open, the file is opened
  non-blocking, its type is checked, and non-blocking is cleared once it is known to be a regular file
- If filesystem waits (normalization, stat, open, and queuing in the pool) exceed `fs_timeout`, 503.
  Default 5 seconds. Change it with the 4th argument of `files()`
- Even after waiting stops at the deadline, the state the worker touches is kept alive until the work finishes. But abandoned work
  does not own the pool. Only the `files()` handler and `FileSource`s being sent decide the pool's lifetime, and it never outlives
  the App (work completions return to the App's `io_context`, so if the App is destroyed first they touch a destroyed strand)
- The body is always sent 64 KiB at a time, whatever the size. Memory per response is 64 KiB regardless of file size
- `Content-Length` is set. No chunked encoding
- Only up to the stat'ed `size` is sent. If the file grows after stat, nothing more is sent
- If a read fails after the header was sent (the file shrank or disappeared), the connection is closed on the spot. The status can no longer change
- `write_timeout` is re-armed for each chunk. The total send time of a large file is not bounded
- If reading a chunk while sending exceeds `read_timeout`, the connection is closed (the response can no longer change)

### Rate limiting (Phase 2)

```cpp
app.use(hayate::mw::rate_limit({.max = 60, .window = std::chrono::seconds(60)}));
```

- Middleware. Fixed window. The key is `Request::peer()` (the connection's remote IP; Connection.peer)
- `peer` is taken once right after accept and is the same for every request on that connection. If it cannot be taken, it is empty and all such connections share one window
- Going over `max` within the window gives 429. `Retry-After` is the seconds left in the window (rounded up, at least 1)
- Default `max` 60, `window` 60s
- The counters are a map with a mutex, owned by the middleware. No globals
- Keys whose window has passed are swept (the map does not grow without bound). Not on every request: the sweep runs on the first
  request after one window has passed since the previous sweep, so expired keys stay until then
