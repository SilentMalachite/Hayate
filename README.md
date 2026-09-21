# Hayate

English | [日本語](README.ja.md)

An HTTP framework and server for C++20. The namespace is `hayate`.

Handlers are coroutines. The public API is fluent. Routes are never registered with macros.
It is built on [Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html) and
[Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/);
it has no event loop or HTTP parser of its own.

```cpp
#include <hayate/hayate.hpp>

int main() {
    hayate::App app;
    app.get("/", [](hayate::Request &) { return hayate::Response::text("hello"); });
    app.bind("0.0.0.0", 8080).serve();
}
```

## Status

| Phase | Contents | |
|---|---|---|
| 0 | CMake Presets, public headers, fixed response | Done |
| 1 | HTTP/1.1, routing, JSON, middleware, limits, graceful shutdown | Done |
| 2 | CORS / static files / rate limiting | Done |
| 3 | TLS / JWT verification / OpenAPI generation / minimal metrics / streaming of static files | Done |

Tests use GoogleTest. The debug preset runs them with ASan + UBSan, the `tsan` preset with TSan.

Multipart / WebSocket / SSE / gzip are **not implemented**.

## Requirements

- C++20 (Apple Clang on macOS, GCC 13+ / Clang 16+ on Linux). Windows is not supported.
  Linux is checked on GitHub Actions (ubuntu-24.04, GCC 13 and Clang 16).
  GCC 12 cannot be used: it miscompiles coroutines (PR 101367)
- CMake 3.28+, Ninja
- Boost 1.83+ (Asio / Beast, header-only)
- OpenSSL 3 (TLS and the HMAC for JWT)
- nlohmann/json 3.11.3 and GoogleTest 1.15.2 are fetched with FetchContent

```bash
# macOS
brew install boost openssl@3 cmake ninja
# Ubuntu 24.04 or later (name 1.83 explicitly; libboost-dev may point at an older version)
apt install libboost1.83-dev libssl-dev cmake ninja-build
```

## Build and test

```bash
cmake --preset debug
cmake --build --preset debug
cmake --build --preset test && ctest --preset test --output-on-failure
# look for data races
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan --output-on-failure
```

The `release` preset has no sanitizers. Apple Clang's ASan on macOS has no LeakSanitizer, so the
debug preset only catches UAF. Check for leaks with Homebrew LLVM's clang:

```bash
cmake -S . -B build/lsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DHAYATE_ENABLE_SANITIZERS=ON -DHAYATE_BUILD_TESTS=ON
cmake --build build/lsan && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/lsan
```

## Using Hayate

Pull it in with FetchContent or `add_subdirectory` and link `hayate::hayate`. There is no install / `find_package`.

```cmake
include(FetchContent)
FetchContent_Declare(hayate
    GIT_REPOSITORY https://github.com/SilentMalachite/Hayate.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(hayate)
target_link_libraries(app PRIVATE hayate::hayate)
```

C++20 propagates from `hayate::hayate`. Boost and OpenSSL are found with `find_package` in the consumer's environment.

Versioning is SemVer. While 0.x, a minor release may change the public API. A patch release does not.

## Features

### Routing

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

`:name` is one non-empty segment; `*name` is the whole rest (may be empty) and may only come last.
Priority per segment is static > param > wildcard, and `/a` beats `/a/*rest`.
Registering the same shape twice for the same method (`/u/:id` and `/u/:name`), a `:` / `*` without a name,
a `*name` in the middle, or any method other than GET / POST throws `std::invalid_argument` at registration.
A wrong method is 405 (with `Allow`); no matching path is 404.

A handler may return a value or an `awaitable<Response>`, but it must be callable as const
(a `mutable` lambda is a compile error). Handlers are shared by all connections, so keep state in the App or a Request Extension.

### Middleware

```cpp
app.use([](hayate::Request &req, hayate::Next next) -> asio::awaitable<hayate::Response> {
    auto res = co_await next(req);
    res.set_header("X-Trace", "1");
    co_return res;
});
```

Onion order (in A→B, out B→A). Not calling `next` short-circuits there.
`use` inside `Router::group` applies only to that group.

### CORS / rate limiting

```cpp
app.use(hayate::mw::cors({.origin = "https://app.example"}));
app.use(hayate::mw::rate_limit({.max = 60, .window = std::chrono::seconds(60)}));
```

### Static files

```cpp
app.get("/assets/*path", hayate::files("public"));
// serve up to 4 MiB, 4 workers, wait at most 2 s on the filesystem
app.get("/dl/*path", hayate::files("downloads", 4u * 1024 * 1024, 4, std::chrono::seconds(2)));
```

Outside root, over the limit, missing, or not a regular file is 404 (existence is not leaked).
Filesystem waits longer than `fs_timeout` (default 5 s) are 503.
File I/O runs on the worker pool owned by `files()`; io threads never wait on the filesystem.
The body is sent 64 KiB at a time whatever the size, so memory per response does not depend on file size.
`Content-Length` is set; chunked encoding is not used.

### TLS

```cpp
app.tls({.cert_file = "server.pem", .key_file = "server.key"})
   .bind("0.0.0.0", 8443)
   .serve();
```

After `tls()`, every connection of the App is TLS. TLS 1.1 and below are disabled.
An unreadable certificate or key, or a key that does not pair with the certificate, throws at `tls()`.

### JWT verification (HS256)

```cpp
app.group("/api", [](hayate::Router &r) {
    r.use(hayate::mw::jwt({.secret = secret, .leeway = std::chrono::seconds(60)}));
    r.get("/me", [](hayate::Request &req) {
        auto *c = req.get<hayate::Claims>();
        return hayate::Response::text(c->json.value("sub", ""));
    });
});
```

`alg` is checked before the signature, which blocks `alg: none` and algorithm confusion.
Signatures are compared in constant time. Tokens without `exp` are rejected. Every failure is 401 + `WWW-Authenticate: Bearer`, whatever the reason.

### metrics

```cpp
app.get("/metrics", hayate::metrics(app));
```

Prometheus text: accepted / rejected / open connections, request count, and responses by status class.

### OpenAPI

```cpp
app.get("/openapi.json", [&app](hayate::Request &) {
    return hayate::Response::json(hayate::openapi(app, {.title = "my api"}));
});
```

Builds OpenAPI 3.1 from the registered routes. The Router only knows `pattern` and `method`,
so request / response schemas are **neither guessed nor emitted**.

### Limits

```cpp
app.limits({.max_body_bytes = 4u * 1024 * 1024, .max_connections = 4096});
```

| Setting | Default |
|---|---|
| `max_header_bytes` | 8192 |
| `max_body_bytes` | 1 MiB |
| `read_timeout` / `write_timeout` | 30s |
| `idle_timeout` | 60s |
| `max_connections` | 1024 |

An oversized header is 431, an oversized body 413. A timeout closes the connection.
`stop()` stops new accepts, closes connections that are only waiting for a request (such as waiting for the next keep-alive request) at once,
completes requests whose header has arrived, then stops the io_context. While stopping, TLS does not wait for the close_notify reply.

## Design promises

- No `std::expected` / C++ Modules / features that require C++23 or later
- One JSON library: nlohmann. `hayate::Json` is an alias of it
- No `new` / `delete` / `malloc`, no raw arrays
- `Request` owns the request strings and its accessors return views. Their lifetime is the `Request`. Never store a view in `App` or `Response`
- Exceptions never leave the handler / middleware boundary. Beast / Asio failures become an `Error` and a response
  (413 / 431 / 500) while a response can still be written. Otherwise (timeout, peer disconnect, handshake failure) the connection is just closed
- No shared mutable globals. State belongs to the `App`, a `Request` Extension, or a middleware / handler factory
- No code is copied from existing frameworks (Drogon / Crow / Oat++ / Cinatra / userver)

## Non-goals

HTTP/2, HTTP/3, gRPC, GraphQL, HTML templates, ORM, migrations,
multipart, WebSocket, SSE, gzip, JWT RS256 / JWKS, mTLS, OpenAPI schema inference.

## License

[Apache License 2.0](LICENSE).

## Documentation

English is canonical. Each document has a Japanese translation next to it as `*.ja.md`; when they disagree, the English wins.
The source of truth is [`docs/SPEC.md`](docs/SPEC.md). Diagrams are in [`docs/ER.md`](docs/ER.md) and
[`docs/UML.md`](docs/UML.md). An agreement made in chat does not exist until it is written to a file.
