#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

struct HttpCall {
    unsigned status{};
    std::string body;
    std::string allow;
    std::string content_type;
    std::string allow_origin;
    std::string allow_methods;
    std::string allow_headers;
    std::string retry_after;
    std::string vary;
    std::map<std::string, std::string> extra;
    std::error_code error;
    std::string error_message;
    // 期限切れ。拒否や EOF と分けないと、固まった実装でも「拒否された」と読める。
    bool timed_out{false};
};

// 呼び出し全体に timeout を掛ける。Beast の期限は非同期 I/O にしか効かないので、
// 同期 API ではなく co_spawn + ioc.run() で回す。
inline HttpCall http_call(std::string host, std::uint16_t port, boost::beast::http::verb method,
                          std::string target, std::string body = {}, std::string content_type = {},
                          std::chrono::milliseconds timeout = std::chrono::seconds(2),
                          std::vector<std::pair<std::string, std::string>> headers = {},
                          std::vector<std::string> want = {}) {
    namespace net = boost::asio;
    namespace http = boost::beast::http;
    HttpCall out;
    try {
        net::io_context ioc;
        boost::beast::tcp_stream stream(ioc);
        const net::ip::tcp::endpoint ep(net::ip::make_address(host), port);
        http::request<http::string_body> req{method, target, 11};
        req.set(http::field::host, host);
        req.keep_alive(false);
        req.set(http::field::connection, "close");
        if (!body.empty()) {
            req.body() = std::move(body);
            req.prepare_payload();
            if (!content_type.empty()) {
                req.set(http::field::content_type, content_type);
            }
        }
        for (const auto &[k, v] : headers) {
            req.set(k, v);
        }
        boost::beast::flat_buffer buf;
        http::response<http::string_body> res;
        boost::system::error_code failed;
        net::co_spawn(
            ioc,
            [&]() -> net::awaitable<void> {
                stream.expires_after(timeout);
                auto [cec] = co_await stream.async_connect(ep, net::as_tuple);
                if (cec) {
                    failed = cec;
                    co_return;
                }
                auto [wec, wn] = co_await http::async_write(stream, req, net::as_tuple);
                (void)wn;
                if (wec) {
                    failed = wec;
                    co_return;
                }
                auto [rec, rn] = co_await http::async_read(stream, buf, res, net::as_tuple);
                (void)rn;
                failed = rec;
            },
            net::detached);
        ioc.run();
        if (failed) {
            out.error = std::make_error_code(std::errc::connection_refused);
            out.error_message = failed.message();
            out.timed_out = failed == boost::beast::error::timeout;
            return out;
        }
        out.status = res.result_int();
        out.body = res.body();
        out.allow = std::string(res[http::field::allow]);
        out.content_type = std::string(res[http::field::content_type]);
        out.allow_origin = std::string(res[http::field::access_control_allow_origin]);
        out.allow_methods = std::string(res[http::field::access_control_allow_methods]);
        out.allow_headers = std::string(res[http::field::access_control_allow_headers]);
        out.retry_after = std::string(res[http::field::retry_after]);
        out.vary = std::string(res[http::field::vary]);
        for (const auto &name : want) {
            out.extra[name] = std::string(res[name]);
        }
        boost::system::error_code ec;
        stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);
    } catch (const std::exception &ex) {
        out.error = std::make_error_code(std::errc::connection_refused);
        out.error_message = ex.what();
    }
    return out;
}
