#include "http_client.hpp"
#include "test_server.hpp"

#include <hayate/hayate.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace http = boost::beast::http;

namespace {

// "name 12" / "name{class=\"2xx\"} 3" の値を拾う。無ければ -1。
long long value_of(const std::string &body, const std::string &name) {
    std::size_t pos = 0;
    while ((pos = body.find(name, pos)) != std::string::npos) {
        const bool at_line_start = pos == 0 || body[pos - 1] == '\n';
        const auto eol = body.find('\n', pos);
        const auto line = body.substr(pos, eol - pos);
        if (at_line_start) {
            const auto sp = line.rfind(' ');
            if (sp != std::string::npos && line.compare(0, name.size(), name) == 0) {
                return std::stoll(line.substr(sp + 1));
            }
        }
        pos += name.size();
    }
    return -1;
}

void setup(hayate::App &app) {
    app.get("/", [](hayate::Request &) { return hayate::Response::text("ok"); });
    app.get("/metrics", hayate::metrics(app));
}

} // namespace

TEST(Metrics, CountsRequests) {
    TestServer srv(setup);
    http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(value_of(r.body, "hayate_requests_total"), 2);
}

TEST(Metrics, CountsStatusClasses) {
    TestServer srv(setup);
    http_call("127.0.0.1", srv.port(), http::verb::get, "/nope");
    auto first = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_EQ(value_of(first.body, "hayate_responses_total{class=\"4xx\"}"), 1);
    EXPECT_EQ(value_of(first.body, "hayate_responses_total{class=\"2xx\"}"), 0);
    EXPECT_EQ(value_of(first.body, "hayate_responses_total{class=\"5xx\"}"), 0);
    // 1 本目のスクレイプ（200）は自分の出力には入らず、次に現れる。
    auto second = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_EQ(value_of(second.body, "hayate_responses_total{class=\"2xx\"}"), 1);
}

TEST(Metrics, CountsAcceptedConnections) {
    TestServer srv(setup);
    http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    http_call("127.0.0.1", srv.port(), http::verb::get, "/");
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    // http_call は 1 回 1 接続。スクレイプ自身の接続も accept 済み。
    EXPECT_EQ(value_of(r.body, "hayate_connections_accepted_total"), 3);
}

TEST(Metrics, ContentType) {
    TestServer srv(setup);
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_NE(r.content_type.find("text/plain"), std::string::npos);
    EXPECT_NE(r.content_type.find("version=0.0.4"), std::string::npos);
}

TEST(Metrics, RejectedOverMaxConnections) {
    TestServer srv([](hayate::App &app) {
        app.limits({.max_connections = 1});
        setup(app);
    });
    namespace net = boost::asio;
    namespace beast = boost::beast;
    net::io_context ioc;
    // A を keep-alive で握ったままにする。これで open = 1。
    beast::tcp_stream a(ioc);
    a.expires_after(std::chrono::seconds(5));
    a.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    beast::flat_buffer abuf;
    auto send = [&](const std::string &target) {
        a.expires_after(std::chrono::seconds(5));
        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, "127.0.0.1");
        req.keep_alive(true);
        http::write(a, req);
        http::response<http::string_body> res;
        http::read(a, abuf, res);
        return res.body();
    };
    EXPECT_EQ(send("/"), "ok");

    // B は上限に当たる。EOF を見た時点で拒否カウンタは進んでいる。
    net::ip::tcp::socket b(ioc);
    b.connect(net::ip::tcp::endpoint(net::ip::make_address("127.0.0.1"), srv.port()));
    boost::system::error_code ec;
    std::array<char, 16> sink{};
    b.read_some(net::buffer(sink), ec);
    EXPECT_TRUE(static_cast<bool>(ec)) << "B should be closed by the server";
    b.close(ec);

    const auto body = send("/metrics");
    EXPECT_EQ(value_of(body, "hayate_connections_rejected_total"), 1);
    EXPECT_EQ(value_of(body, "hayate_connections_open"), 1);
}

// 送れないヘッダは 500 に差し替える。metrics もハンドラの 200 ではなく送った 500 で数える。
TEST(Metrics, OversizedHeaderCountsAs5xx) {
    TestServer srv([](hayate::App &app) {
        app.get("/big", [](hayate::Request &) {
            auto res = hayate::Response::text("ok");
            res.set_header("X-Big", std::string(70000, 'a'));
            return res;
        });
        app.get("/metrics", hayate::metrics(app));
    });
    http_call("127.0.0.1", srv.port(), http::verb::get, "/big");
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/metrics");
    EXPECT_EQ(value_of(r.body, "hayate_responses_total{class=\"5xx\"}"), 1);
    EXPECT_EQ(value_of(r.body, "hayate_responses_total{class=\"2xx\"}"), 0);
}
