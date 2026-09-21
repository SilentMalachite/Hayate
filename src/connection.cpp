#include "connection.hpp"
#include "detail/offload.hpp"
#include "detail/open_file.hpp"
#include "detail/percent.hpp"

#include <hayate/error.hpp>
#include <hayate/request.hpp>
#include <hayate/response.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hayate {
namespace beast = detail::beast;
namespace http = detail::http;
namespace net = detail::net;
using tcp = detail::tcp;

namespace {

// 1 応答のメモリをファイルサイズから切り離す窓。
constexpr std::size_t chunk_bytes = 65536;

// タイマと socket は TCP 側にある。TLS では 1 枚下を見る。
beast::tcp_stream &lowest(beast::tcp_stream &s) { return s; }
beast::tcp_stream &lowest(detail::tls_stream &s) { return s.next_layer(); }

net::awaitable<bool> do_handshake(beast::tcp_stream &, std::chrono::milliseconds) {
    co_return true;
}

net::awaitable<bool> do_handshake(detail::tls_stream &s, std::chrono::milliseconds window) {
    s.next_layer().expires_after(window);
    auto [ec] = co_await s.async_handshake(detail::ssl::stream_base::server, net::as_tuple);
    co_return !ec;
}

net::awaitable<void> shutdown_stream(beast::tcp_stream &s, std::chrono::milliseconds) {
    boost::system::error_code ignored;
    s.socket().shutdown(tcp::socket::shutdown_send, ignored);
    co_return;
}

net::awaitable<void> shutdown_stream(detail::tls_stream &s, std::chrono::milliseconds window) {
    // close_notify を待つので窓が要る。切れていれば即エラーで戻る。
    s.next_layer().expires_after(window);
    co_await s.async_shutdown(net::as_tuple);
    boost::system::error_code ignored;
    s.next_layer().socket().shutdown(tcp::socket::shutdown_send, ignored);
    co_return;
}

http::response<http::string_body> to_beast(const Response &src, unsigned version, bool keep_alive) {
    http::response<http::string_body> out{http::status(src.status()), version};
    out.keep_alive(keep_alive);
    src.for_each_header([&](std::string_view k, std::string_view v) { out.set(k, v); });
    out.body() = std::string(src.body());
    out.prepare_payload();
    return out;
}

// 上限超過だけは応答を書ける。timeout や peer close は書き先が無い。
std::optional<Error> limit_error(const boost::system::error_code &ec) {
    if (ec == http::error::body_limit) {
        return Error{"payload_too_large", "Payload Too Large", 413};
    }
    if (ec == http::error::header_limit || ec == http::error::buffer_overflow) {
        return Error{"header_too_large", "Request Header Fields Too Large", 431};
    }
    return std::nullopt;
}

} // namespace

template <typename Stream>
class Connection : public std::enable_shared_from_this<Connection<Stream>> {
  public:
    Connection(Stream stream, Limits limits, Router &router, detail::Counters &counters,
               std::atomic<bool> &shutting, std::function<void()> on_done)
        : stream_(std::move(stream)), limits_(limits), router_(router), counters_(counters),
          shutting_(shutting), on_done_(std::move(on_done)) {
        // accept 直後なら必ず取れる。リクエストごとに引くと切断済みで空になる。
        boost::system::error_code pec;
        auto ep = lowest(stream_).socket().remote_endpoint(pec);
        peer_ = pec ? std::string{} : ep.address().to_string();
    }

    ~Connection() { finish(); }

    void load(Request &dst, const http::request<http::string_body> &src) {
        if (src.method() == http::verb::get) {
            dst.method_ = HttpMethod::get;
        } else if (src.method() == http::verb::post) {
            dst.method_ = HttpMethod::post;
        } else if (src.method() == http::verb::options) {
            dst.method_ = HttpMethod::options;
        } else {
            dst.method_ = HttpMethod::unknown;
        }
        dst.target_ = std::string(src.target());
        auto qpos = dst.target_.find('?');
        dst.path_ = qpos == std::string::npos ? dst.target_ : dst.target_.substr(0, qpos);
        dst.query_.clear();
        if (qpos != std::string::npos) {
            std::string_view qs(dst.target_);
            qs.remove_prefix(qpos + 1);
            while (!qs.empty()) {
                auto amp = qs.find('&');
                auto pair = qs.substr(0, amp);
                auto eq = pair.find('=');
                std::string k;
                std::string v;
                if (eq == std::string_view::npos) {
                    k = detail::percent_decode(pair, true);
                } else {
                    k = detail::percent_decode(pair.substr(0, eq), true);
                    v = detail::percent_decode(pair.substr(eq + 1), true);
                }
                dst.query_.emplace_back(std::move(k), std::move(v));
                if (amp == std::string_view::npos) {
                    break;
                }
                qs.remove_prefix(amp + 1);
            }
        }
        dst.headers_.clear();
        for (const auto &h : src) {
            dst.headers_.emplace_back(std::string(h.name_string()), std::string(h.value()));
        }
        dst.body_ = src.body();
        dst.params_.clear();
        dst.ext_.clear();
        dst.peer_ = peer_;
    }

    // 送出中も io スレッドを塞がない。読みは FileSource のプールで回す。
    // 返り値 false は「接続をもう使えない」。ヘッダを送った後は status を直せない。
    net::awaitable<bool> write_file(const Response &res, unsigned version, bool keep) {
        const auto *src = res.file_source();
        if (!src->file) {
            co_return false;
        }
        http::response<http::buffer_body> out{http::status(res.status()), version};
        out.keep_alive(keep);
        res.for_each_header([&](std::string_view k, std::string_view v) { out.set(k, v); });
        out.content_length(src->size);
        http::response_serializer<http::buffer_body> sr{out};
        // 期限切れで待つのをやめてもワーカーが書き続ける。バッファとファイルは
        // 共有で持ち、ラムダに値で渡す。
        auto file = src->file;
        auto buf = std::make_shared<std::vector<char>>(chunk_bytes);
        std::uint64_t left = src->size;
        for (;;) {
            std::size_t n = 0;
            if (left > 0) {
                const auto want =
                    static_cast<std::size_t>(std::min<std::uint64_t>(buf->size(), left));
                const auto got = co_await detail::offload_until(
                    *src->pool, limits_.read_timeout, [file, buf, want] {
                        boost::system::error_code rec;
                        return file->f.read(buf->data(), want, rec);
                    });
                // 期限切れ、stat より縮んだ、読めない。Content-Length を満たせないので打ち切る。
                if (!got || *got == 0) {
                    co_return false;
                }
                n = *got;
            }
            out.body().data = buf->data();
            out.body().size = n;
            left -= n;
            out.body().more = left > 0;
            lowest(stream_).expires_after(limits_.write_timeout);
            auto [wec, wbytes] = co_await http::async_write(stream_, sr, net::as_tuple);
            (void)wbytes;
            if (wec && wec != http::error::need_buffer) {
                co_return false;
            }
            if (left == 0) {
                break;
            }
        }
        co_return true;
    }

    net::awaitable<void> run() {
        auto self = this->shared_from_this();
        try {
            // TLS ならここで握手。平文は素通り。失敗したら応答を書かずに閉じる。
            if (co_await do_handshake(stream_, limits_.read_timeout)) {
                co_await serve_requests();
            }
        } catch (...) {
        }
        co_await shutdown_stream(stream_, limits_.write_timeout);
        finish();
        co_return;
    }

  private:
    net::awaitable<void> serve_requests() {
        {
            bool first_req = true;
            for (;;) {
                if (shutting_.load()) {
                    break;
                }
                // 1 本目は接続直後なので read_timeout。2 本目以降は次の要求を待つ idle_timeout。
                lowest(stream_).expires_after(first_req ? limits_.read_timeout
                                                        : limits_.idle_timeout);
                first_req = false;
                http::request_parser<http::string_body> parser;
                parser.header_limit(static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(limits_.max_header_bytes, 0xffffffffu)));
                parser.body_limit(limits_.max_body_bytes);
                auto [ec, bytes] =
                    co_await http::async_read_header(stream_, buffer_, parser, net::as_tuple);
                (void)bytes;
                if (!ec && !parser.is_done()) {
                    // ヘッダが来た後は本文の到着待ち。窓は idle ではなく read_timeout。
                    lowest(stream_).expires_after(limits_.read_timeout);
                    auto [bec, bbytes] =
                        co_await http::async_read(stream_, buffer_, parser, net::as_tuple);
                    (void)bbytes;
                    ec = bec;
                }
                if (ec) {
                    if (const auto over = limit_error(ec)) {
                        detail::count_response(counters_, over->http_status);
                        auto out = to_beast(Response::from_error(*over), 11, false);
                        lowest(stream_).expires_after(limits_.write_timeout);
                        co_await http::async_write(stream_, out, net::as_tuple);
                    }
                    break;
                }
                Request req;
                load(req, parser.get());
                Response res = co_await router_.dispatch(req);
                detail::count_response(counters_, res.status());
                bool keep = parser.get().keep_alive() && !shutting_.load();
                if (res.is_file()) {
                    const bool ok = co_await write_file(res, parser.get().version(), keep);
                    if (!ok || !keep) {
                        break;
                    }
                    continue;
                }
                auto out = to_beast(res, parser.get().version(), keep);
                lowest(stream_).expires_after(limits_.write_timeout);
                auto [wec, wbytes] = co_await http::async_write(stream_, out, net::as_tuple);
                (void)wbytes;
                if (wec || !keep) {
                    break;
                }
            }
        }
        co_return;
    }

    void finish() {
        if (finished_.exchange(true)) {
            return;
        }
        if (on_done_) {
            auto cb = std::move(on_done_);
            cb();
        }
    }

    Stream stream_;
    beast::flat_buffer buffer_;
    std::string peer_;
    Limits limits_;
    Router &router_;
    detail::Counters &counters_;
    std::atomic<bool> &shutting_;
    std::function<void()> on_done_;
    std::atomic<bool> finished_{false};
};

net::awaitable<void> serve_connection(beast::tcp_stream stream, const Limits &limits,
                                      Router &router, detail::Counters &counters,
                                      std::atomic<bool> &shutting, std::function<void()> on_done) {
    auto conn = std::make_shared<Connection<beast::tcp_stream>>(
        std::move(stream), limits, router, counters, shutting, std::move(on_done));
    co_await conn->run();
}

net::awaitable<void> serve_connection(detail::tls_stream stream, const Limits &limits,
                                      Router &router, detail::Counters &counters,
                                      std::atomic<bool> &shutting, std::function<void()> on_done) {
    auto conn = std::make_shared<Connection<detail::tls_stream>>(
        std::move(stream), limits, router, counters, shutting, std::move(on_done));
    co_await conn->run();
}

} // namespace hayate
