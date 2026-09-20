#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>

struct HttpCall {
    unsigned status{};
    std::string body;
    std::string allow;
    std::string content_type;
    std::error_code error;
};

inline HttpCall http_call(std::string host, std::uint16_t port, boost::beast::http::verb method,
                          std::string target, std::string body = {}, std::string content_type = {},
                          std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    namespace net = boost::asio;
    namespace http = boost::beast::http;
    HttpCall out;
    try {
        net::io_context ioc;
        boost::beast::tcp_stream stream(ioc);
        stream.expires_after(timeout);
        stream.connect(net::ip::tcp::endpoint(net::ip::make_address(host), port));
        http::request<http::string_body> req{method, target, 11};
        req.set(http::field::host, host);
        if (!body.empty()) {
            req.body() = std::move(body);
            req.prepare_payload();
            if (!content_type.empty()) {
                req.set(http::field::content_type, content_type);
            }
        }
        http::write(stream, req);
        boost::beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        out.status = res.result_int();
        out.body = res.body();
        out.allow = std::string(res[http::field::allow]);
        out.content_type = std::string(res[http::field::content_type]);
        boost::system::error_code ec;
        stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);
    } catch (const std::exception &) {
        out.error = std::make_error_code(std::errc::connection_refused);
    }
    return out;
}
