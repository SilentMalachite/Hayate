#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <cstdint>
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
    std::error_code error;
    std::string error_message;
};

inline HttpCall http_call(std::string host, std::uint16_t port, boost::beast::http::verb method,
                          std::string target, std::string body = {}, std::string content_type = {},
                          std::chrono::milliseconds timeout = std::chrono::seconds(2),
                          std::vector<std::pair<std::string, std::string>> headers = {}) {
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
        http::write(stream, req);
        boost::beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(stream, buf, res);
        out.status = res.result_int();
        out.body = res.body();
        out.allow = std::string(res[http::field::allow]);
        out.content_type = std::string(res[http::field::content_type]);
        out.allow_origin = std::string(res[http::field::access_control_allow_origin]);
        out.allow_methods = std::string(res[http::field::access_control_allow_methods]);
        out.allow_headers = std::string(res[http::field::access_control_allow_headers]);
        out.retry_after = std::string(res[http::field::retry_after]);
        out.vary = std::string(res[http::field::vary]);
        boost::system::error_code ec;
        stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);
    } catch (const std::exception &ex) {
        out.error = std::make_error_code(std::errc::connection_refused);
        out.error_message = ex.what();
    }
    return out;
}
