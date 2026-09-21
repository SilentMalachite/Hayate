# ER — Hayate (for agents)

English (canonical) | [日本語](ER.ja.md)

The canonical source is `docs/SPEC.md`. This diagram shows **types and their ownership**; it is not an RDB schema.
Do not create entities that are not in the tables. Accepted: Phase 1, Phase 2 (CORS / static files / rate limiting), Phase 3 (TLS /
JWT verification / OpenAPI generation / minimal metrics / streaming of static files). Multipart / WS / SSE / gzip are not implemented.

Lifetime abbreviations: `App` = process, `Conn` = TCP connection, `Req` = one request.
Views do not own. Never keep one longer than its owner.

## Kinds of entity

| Kind | Entities |
|---|---|
| Public types (`include/hayate/`) | APP, ROUTER, LIMITS, TLS, REQUEST, RESPONSE, FILE_SOURCE, RESULT, ERROR, HTTP_METHOD, JWT, CLAIMS, CORS, RATE_LIMIT, OPEN_API_INFO |
| Public aliases (`std::function`) | HANDLER, MIDDLEWARE |
| Internal types (`src/`) | ROUTE, SEGMENT, CONNECTION, CONNECTION_SET, COUNTERS, OPEN_FILE, RATE_TABLE (`mw::detail`, owned by the middleware) |
| Asio / OpenSSL types | IO_CONTEXT, SSL_CONTEXT, WORKER_POOL (`asio::thread_pool`) |
| No type (only how things are held) | LISTENER (acceptor and accept loop), HEADER, QUERY_PAIR, PATH_PARAM, BODY, EXTENSION, STATUS |

## Diagram

```mermaid
erDiagram
    APP ||--|| ROUTER : owns
    APP ||--|| LIMITS : has
    APP ||--|| IO_CONTEXT : runs
    APP ||--o| LISTENER : bind
    APP ||--o| SSL_CONTEXT : "tls() builds"
    APP ||--|| COUNTERS : counts
    APP ||--|| CONNECTION_SET : "stop cancels"
    TLS ||--o| SSL_CONTEXT : "read once by tls()"

    ROUTER ||--o{ ROUTE : registers
    ROUTER ||--o{ MIDDLEWARE : uses

    ROUTE ||--|| HANDLER : owns
    ROUTE }o--|| HTTP_METHOD : method
    ROUTE ||--o{ SEGMENT : pattern

    LISTENER ||--o{ CONNECTION : accepts
    CONNECTION }o--|| CONNECTION_SET : registers
    CONNECTION ||--o{ REQUEST : reads
    CONNECTION ||--o{ RESPONSE : writes

    REQUEST ||--|| HTTP_METHOD : method
    REQUEST ||--o{ HEADER : owns
    REQUEST ||--o{ QUERY_PAIR : owns
    REQUEST ||--o{ PATH_PARAM : owns
    REQUEST ||--o| BODY : owns
    REQUEST ||--o{ EXTENSION : slot
    EXTENSION ||--o| CLAIMS : "mw::jwt stores"

    HANDLER ||--|| RESPONSE : returns
    MIDDLEWARE ||--o| RESPONSE : short_circuit
    MIDDLEWARE }o--|| REQUEST : sees
    MIDDLEWARE ||--o| RATE_TABLE : "mw::rate_limit owns"
    HANDLER }o--o| WORKER_POOL : "files() owns"

    RESPONSE ||--|| STATUS : status
    RESPONSE ||--o{ HEADER : headers
    RESPONSE ||--o| FILE_SOURCE : "bytes or file"
    FILE_SOURCE ||--|| OPEN_FILE : sends
    FILE_SOURCE }o--|| WORKER_POOL : reads_on

    RESULT ||--o| ERROR : err
    HANDLER }o--|| RESULT : may_use
    ERROR ||--|| STATUS : http_status

    CORS ||--|| MIDDLEWARE : "mw::cors configures"
    RATE_LIMIT ||--|| MIDDLEWARE : "mw::rate_limit configures"
    JWT ||--|| MIDDLEWARE : "mw::jwt configures"
    OPEN_API_INFO }o--|| APP : "openapi reads"

    APP {
        uint32 threads
        uint16 port
        bool accepting
        bool shutting
        uint32 connections
    }
    LISTENER {
        acceptor socket "null until bind"
    }
    TLS {
        string cert_file
        string key_file
        string key_password
    }
    LIMITS {
        uint64 max_header_bytes
        uint64 max_body_bytes
        ms read_timeout
        ms write_timeout
        ms idle_timeout
        uint32 max_connections
    }
    IO_CONTEXT {
        int count "exactly one per App"
    }
    ROUTE {
        HttpMethod method
        string pattern
    }
    SEGMENT {
        enum kind "lit|param|wild"
        string s "literal text or name"
    }
    HTTP_METHOD {
        enum value "get|post|options|unknown"
    }
    MIDDLEWARE {
        function fn "Request&, Next"
    }
    HANDLER {
        function fn "const-callable, sync ones wrapped"
    }
    CONNECTION {
        string peer "remote IP at accept"
        Limits limits "copy"
        bool waiting "strand only"
    }
    CONNECTION_SET {
        map cancels "id to post-cancel"
    }
    COUNTERS {
        uint64 accepted
        uint64 rejected
        uint64 requests
        uint64 by_class "1xx..5xx"
    }
    REQUEST {
        string target
        string path
        string peer
        string body
    }
    RESPONSE {
        uint16 status
        variant body "string or FileSource"
    }
    HEADER {
        string name "case_insensitive"
        string value
    }
    QUERY_PAIR {
        string key "decoded"
        string value "decoded"
    }
    PATH_PARAM {
        string name
        string value "decoded"
    }
    BODY {
        string bytes "span view out"
    }
    FILE_SOURCE {
        path path "Content-Type only"
        uint64 size "sent exactly"
    }
    OPEN_FILE {
        fd file "verified inside root"
    }
    EXTENSION {
        type_index key "Claims etc"
        shared_ptr value
    }
    STATUS {
        uint16 code
    }
    RESULT {
        variant value "T or Error"
    }
    ERROR {
        string code
        string message
        uint16 http_status
    }
    JWT {
        string secret
        string issuer
        string audience
        seconds leeway
    }
    CLAIMS {
        Json json
    }
    CORS {
        string origin
        string methods
        string headers
    }
    RATE_LIMIT {
        uint32 max
        ms window
    }
    RATE_TABLE {
        map by_peer
        time_point last_sweep
    }
    OPEN_API_INFO {
        string title
        string version
    }
```

## Relations (what agents must keep)

| Left | Multiplicity | Right | Ownership / lifetime | Notes |
|---|---|---|---|---|
| App | 1—1 | Router | owned by App | Do not add god objects. The root is App |
| App | 1—0..1 | Listener | owned by App | Made by `bind`. Not a type: the App's acceptor and the accept loop on the admin strand |
| App | 1—1 | IoContext | owned by App | Exactly one. `threads(n)` runs the same io_context on n threads |
| App | 1—1 | Limits | owned by App | Always present. Defaults are in the SPEC |
| App | 1—0..1 | SslContext | owned by App | `tls()` reads `Tls` and builds it. `Tls` itself is not kept |
| App | 1—1 | Counters / ConnectionSet | owned by App | For metrics, and the registry `stop()` uses to cancel connections waiting for a request |
| Router | 1—* | Route | owned by Router | GET / POST only. Duplicate shapes and malformed patterns throw at registration |
| Router | 1—* | Middleware | owned by Router | Onion: in registration order on the way in, reverse on the way out. App `use` also wraps 404/405. `group` ones apply only to matched Routes |
| Route | 1—* | Segment | owned by Route | `group()` keeps no child Router; it adds routes to the parent with the joined pattern (`//` collapsed) |
| Route | 1—1 | Handler | owned by value by Route | `awaitable<Response>(Request&)`. Must be const-callable. Sync ones are wrapped internally |
| Listener | 1—* | Connection | created by Listener, Conn owns itself | Over the limit: accepted and closed at once. One strand per connection |
| Connection | 1—* | Request | created by Conn, Req lifetime | Consecutive with keep-alive. `peer` is a copy of the remote IP taken once at accept |
| Request | 1—* | Header / Query / PathParam / Body | owned by Request | Accessors return views. Missing ones are empty views. `param` / `query` are decoded, `path` is raw |
| Request | 0—* | Extension | typed slot with Req lifetime | Replaces global state. `set` of the same type overwrites and invalidates earlier references |
| Handler | 1—1 | Response | returned by value | Factories are `text` / `json` / `no_content` / `from_error` / `file`. `set_header` token-checks the name and strips CTLs from the value |
| Response | 0—1 | FileSource | owned by Response | The body is either bytes or a FileSource. Only `files()` creates a FileSource |
| FileSource | *—1 | WorkerPool | shared | Shared by the `files()` closure and FileSources. Never kept longer than the App |
| Middleware | 0—1 | Response | short-circuits when next is not called | |
| Result | 0—1 | Error | value | A single `std::variant` implementation. `std::expected` is banned |
| Error | 1—1 | Status | value | Exceptions never leak across the handler boundary |

The matching rules (priority, empty segments, duplicates) live in one place: the "Routing" section of the SPEC.
No matching path is 404. A wrong method is 405 + Allow.

## Request path (same entities)

The detailed order (TLS, 100-continue, HEAD, file sending) is in the sequence in `docs/UML.md`.

```mermaid
sequenceDiagram
    participant L as Listener
    participant C as Connection
    participant R as Router
    participant M as Middleware
    participant H as Handler
    L->>C: accept onto a new strand
    C->>C: read Request, 413 or 431 over Limits
    C->>R: dispatch
    R->>M: enter A then B
    alt short circuit
        M-->>C: Response
    else next
        M->>R: match Route
        alt no match
            R-->>M: 404 or 405 Allow
        else match
            R->>H: Request, through group MW
            H-->>R: Response
            R-->>M: Response
        end
        M->>M: leave B then A
        M-->>C: Response
    end
    C->>C: write Response
    alt stop
        L-->>L: close acceptor
        C-->>C: close if waiting for a request, else finish and close
    end
```

## Features accepted in Phase 2 / 3 (their entities are in the diagram above)

| Phase | Feature | Shape | Hangs off |
|---|---|---|---|
| 2 | CORS | `mw::cors(Cors)` | Middleware. A preflight short-circuits with 204. `Vary: Origin` with a fixed origin. Without `mw::cors`, OPTIONS is 404 / 405 |
| 2 | Static files | `files(root, max_bytes = 0, io_threads = 2, fs_timeout = 5s)` | Handler. Outside root, over the limit, missing, or not a regular file is 404. Filesystem waits over `fs_timeout` are 503. Filesystem calls run on the `files()` worker pool |
| 2 | Rate limiting | `mw::rate_limit(RateLimit)` | Middleware. Fixed window keyed by `Request::peer`. The middleware holds `RateTable` together with a mutex |
| 3 | TLS | `Tls{cert_file, key_file, key_password}` | The App builds the `ssl::context`. `tls()` throws when a file is unreadable or the key does not match the certificate. TLS 1.2 minimum. Once set, every connection is TLS |
| 3 | JWT verification | `mw::jwt(Jwt)` | Middleware. HS256 only. Throws on an empty `secret` or a negative `leeway`. Non-canonical base64url is 401. On success puts `Claims` in an Extension |
| 3 | metrics | `metrics(App&)` | Handler. The App owns one `Counters` |
| 3 | OpenAPI | `openapi(const App&, OpenApiInfo = {})` | Function. Builds `Json` from pattern and method only. No schemas |
| 3 | Streaming | `Response::file(FileSource)` | The Connection sends 64 KiB at a time. `Content-Length` is the stat'ed size |

No classes like `StaticFile` / `TlsContext` / `JwtAuth` / `OpenApi` / `Metrics`.

## Not implemented (outside the SPEC)

| Phase | Entity | Reason |
|---|---|---|
| 2 | Multipart / UrlEncoded | The SPEC decided not to implement it |
| 2 | WebSocket / Sse | Same |
| 2 | Gzip | Same |

User / Session / ORM / Template are outside the SPEC. They do not appear in the diagrams.
