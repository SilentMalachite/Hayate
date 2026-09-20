#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace hayate::detail {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
} // namespace hayate::detail
