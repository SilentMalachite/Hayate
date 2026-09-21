#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>

#include <chrono>

namespace http = boost::beast::http;
namespace net = boost::asio;

// listen だけして accept しない。接続は backlog で成立し、応答は永遠に来ない。
// Beast の期限は同期 I/O に効かないので、クライアントが非同期でないと固まる。
TEST(HttpClient, TimesOutOnSilentServer) {
    net::io_context ioc;
    net::ip::tcp::acceptor silent(ioc, {net::ip::make_address("127.0.0.1"), 0});
    const auto port = silent.local_endpoint().port();
    const auto began = std::chrono::steady_clock::now();
    auto r =
        http_call("127.0.0.1", port, http::verb::get, "/", {}, {}, std::chrono::milliseconds(200));
    EXPECT_TRUE(r.error);
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(2));
}
