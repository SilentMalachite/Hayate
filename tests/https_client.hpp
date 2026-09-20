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
        beast::get_lowest_layer(stream).expires_after(timeout);
        beast::get_lowest_layer(stream).connect(
            net::ip::tcp::endpoint(net::ip::make_address(host), port));
        stream.handshake(net::ssl::stream_base::client);
        http::request<http::string_body> req{method, target, 11};
        req.set(http::field::host, host);
        req.keep_alive(false);
        req.set(http::field::connection, "close");
        http::write(stream, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        out.status = res.result_int();
        out.body = res.body();
        out.content_type = std::string(res[http::field::content_type]);
        for (const auto &name : want) {
            out.extra[name] = std::string(res[name]);
        }
        boost::system::error_code ec;
        stream.shutdown(ec);
    } catch (const std::exception &ex) {
        out.error = std::make_error_code(std::errc::connection_refused);
        out.error_message = ex.what();
    }
    return out;
}
