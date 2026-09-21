#pragma once

#include "http_client.hpp"

#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>

// http_call の TLS 版。テストなので証明書は検証しない。
inline boost::asio::ssl::context test_client_ctx() {
    namespace ssl = boost::asio::ssl;
    ssl::context ctx{ssl::context::tls_client};
    ctx.set_verify_mode(ssl::verify_none);
    return ctx;
}

inline HttpCall https_call(std::string host, std::uint16_t port, boost::beast::http::verb method,
                           std::string target,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5),
                           std::vector<std::string> want = {}) {
    namespace net = boost::asio;
    namespace http = boost::beast::http;
    namespace beast = boost::beast;
    HttpCall out;
    try {
        net::io_context ioc;
        auto ctx = test_client_ctx();
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};
        const net::ip::tcp::endpoint ep(net::ip::make_address(host), port);
        http::request<http::string_body> req{method, target, 11};
        req.set(http::field::host, host);
        req.keep_alive(false);
        req.set(http::field::connection, "close");
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        boost::system::error_code failed;
        // http_call と同じく非同期で回し、timeout を全体に効かせる。
        net::co_spawn(
            ioc,
            [&]() -> net::awaitable<void> {
                beast::get_lowest_layer(stream).expires_after(timeout);
                auto [cec] = co_await beast::get_lowest_layer(stream).async_connect(
                    ep, net::as_tuple(net::use_awaitable));
                if (cec) {
                    failed = cec;
                    co_return;
                }
                auto [hec] = co_await stream.async_handshake(net::ssl::stream_base::client,
                                                             net::as_tuple(net::use_awaitable));
                if (hec) {
                    failed = hec;
                    co_return;
                }
                auto [wec, wn] =
                    co_await http::async_write(stream, req, net::as_tuple(net::use_awaitable));
                (void)wn;
                if (wec) {
                    failed = wec;
                    co_return;
                }
                auto [rec, rn] =
                    co_await http::async_read(stream, buf, res, net::as_tuple(net::use_awaitable));
                (void)rn;
                if (rec) {
                    failed = rec;
                    co_return;
                }
                // close_notify を送る。サーバーが相手の close_notify を待ち続けないように。
                auto [sec] = co_await stream.async_shutdown(net::as_tuple(net::use_awaitable));
                (void)sec;
            },
            net::detached);
        ioc.run();
        if (failed) {
            out.error = std::make_error_code(std::errc::connection_refused);
            out.error_message = failed.message();
            return out;
        }
        out.status = res.result_int();
        out.body = res.body();
        out.content_type = std::string(res[http::field::content_type]);
        for (const auto &name : want) {
            out.extra[name] = std::string(res[name]);
        }
    } catch (const std::exception &ex) {
        out.error = std::make_error_code(std::errc::connection_refused);
        out.error_message = ex.what();
    }
    return out;
}
