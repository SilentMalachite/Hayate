#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

namespace hayate::detail {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;
using tls_stream = beast::ssl_stream<beast::tcp_stream>;
} // namespace hayate::detail
