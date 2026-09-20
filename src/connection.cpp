#include "connection.hpp"
#include "detail/percent.hpp"

#include <hayate/request.hpp>
#include <hayate/response.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace hayate {
namespace beast = detail::beast;
namespace http = detail::http;
namespace net = detail::net;
using tcp = detail::tcp;

namespace {

http::response<http::string_body> to_beast(const Response &src, unsigned version, bool keep_alive) {
    http::response<http::string_body> out{http::status(src.status()), version};
    out.keep_alive(keep_alive);
    src.for_each_header([&](std::string_view k, std::string_view v) { out.set(k, v); });
    out.body() = std::string(src.body());
    out.prepare_payload();
    return out;
}

} // namespace

class Connection : public std::enable_shared_from_this<Connection> {
  public:
    Connection(beast::tcp_stream stream, Limits limits, Router &router, std::atomic<bool> &shutting,
               std::function<void()> on_done)
        : stream_(std::move(stream)), limits_(limits), router_(router), shutting_(shutting),
          on_done_(std::move(on_done)) {
        // accept 直後なら必ず取れる。リクエストごとに引くと切断済みで空になる。
        boost::system::error_code pec;
        auto ep = stream_.socket().remote_endpoint(pec);
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

    net::awaitable<void> run() {
        auto self = shared_from_this();
        try {
            bool first_req = true;
            for (;;) {
                if (shutting_.load()) {
                    break;
                }
                // 1 本目は接続直後なので read_timeout。2 本目以降は次の要求を待つ idle_timeout。
                stream_.expires_after(first_req ? limits_.read_timeout : limits_.idle_timeout);
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
                    stream_.expires_after(limits_.read_timeout);
                    auto [bec, bbytes] =
                        co_await http::async_read(stream_, buffer_, parser, net::as_tuple);
                    (void)bbytes;
                    ec = bec;
                }
                if (ec) {
                    if (ec == http::error::body_limit) {
                        auto res =
                            Response::from_error({"payload_too_large", "Payload Too Large", 413});
                        auto out = to_beast(res, 11, false);
                        stream_.expires_after(limits_.write_timeout);
                        co_await http::async_write(stream_, out, net::as_tuple);
                    } else if (ec == http::error::header_limit ||
                               ec == http::error::buffer_overflow) {
                        auto res = Response::from_error(
                            {"header_too_large", "Request Header Fields Too Large", 431});
                        auto out = to_beast(res, 11, false);
                        stream_.expires_after(limits_.write_timeout);
                        co_await http::async_write(stream_, out, net::as_tuple);
                    }
                    break;
                }
                Request req;
                load(req, parser.get());
                Response res = co_await router_.dispatch(req);
                bool keep = parser.get().keep_alive() && !shutting_.load();
                auto out = to_beast(res, parser.get().version(), keep);
                stream_.expires_after(limits_.write_timeout);
                auto [wec, wbytes] = co_await http::async_write(stream_, out, net::as_tuple);
                (void)wbytes;
                if (wec || !keep) {
                    break;
                }
            }
        } catch (...) {
        }
        boost::system::error_code ignored;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
        finish();
        co_return;
    }

  private:
    void finish() {
        if (finished_.exchange(true)) {
            return;
        }
        if (on_done_) {
            auto cb = std::move(on_done_);
            cb();
        }
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::string peer_;
    Limits limits_;
    Router &router_;
    std::atomic<bool> &shutting_;
    std::function<void()> on_done_;
    std::atomic<bool> finished_{false};
};

net::awaitable<void> serve_connection(beast::tcp_stream stream, const Limits &limits,
                                      Router &router, std::atomic<bool> &shutting,
                                      std::function<void()> on_done) {
    auto conn = std::make_shared<Connection>(std::move(stream), limits, router, shutting,
                                             std::move(on_done));
    co_await conn->run();
}

} // namespace hayate
