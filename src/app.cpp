#include "connection.hpp"
#include "detail/asio.hpp"
#include "detail/metrics.hpp"
#include "detail/openapi.hpp"

#include <hayate/app.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hayate {
namespace beast = detail::beast;
namespace net = detail::net;
namespace ssl = detail::ssl;
using tcp = detail::tcp;

namespace {

constexpr std::chrono::milliseconds accept_retry_delay{100};

// 0 を下回らせない。accept 側と Connection の終了側の両方から呼ばれる。
void release_one(std::atomic<std::uint32_t> &n) {
    auto cur = n.load();
    while (cur > 0 && !n.compare_exchange_weak(cur, cur - 1)) {
    }
}

} // namespace

struct App::Impl {
    net::io_context ioc;
    // accept ループと stop() をここで直列化する。多重 stop と SIGTERM が重ならない。
    net::strand<net::io_context::executor_type> admin{net::make_strand(ioc)};
    // accept の失敗後の待ち。stop() が取り消せるようにここに置く。
    net::steady_timer accept_retry{admin};
    // acceptor より後に壊れ、ioc より先に壊れる位置に置く。
    std::unique_ptr<ssl::context> ssl_ctx;
    std::unique_ptr<tcp::acceptor> acceptor;
    std::unique_ptr<net::executor_work_guard<net::io_context::executor_type>> work;
    std::unique_ptr<net::signal_set> signals;
    Router router;
    Limits limits;
    std::uint32_t threads{1};
    std::uint16_t port{0};
    std::atomic<bool> accepting{false};
    std::atomic<bool> shutting{false};
    std::atomic<std::uint32_t> connections{0};
    detail::Counters counters;
    // stop() が、要求の到着を待っている接続を取り消すのに使う。
    detail::ConnectionSet conns;
};

App::App() : impl_(std::make_unique<Impl>()) {}
App::~App() { stop(); }

App &App::group(std::string_view prefix, std::function<void(Router &)> fn) {
    impl_->router.group(prefix, std::move(fn));
    return *this;
}

App &App::bind(std::string_view host, std::uint16_t port) {
    auto addr = net::ip::make_address(std::string(host));
    tcp::endpoint ep(addr, port);
    impl_->acceptor = std::make_unique<tcp::acceptor>(impl_->ioc);
    impl_->acceptor->open(ep.protocol());
    impl_->acceptor->set_option(tcp::acceptor::reuse_address(true));
    impl_->acceptor->bind(ep);
    impl_->acceptor->listen();
    impl_->port = impl_->acceptor->local_endpoint().port();
    return *this;
}

std::uint16_t App::port() const noexcept { return impl_->port; }

App &App::threads(std::uint32_t n) {
    impl_->threads = n == 0 ? 1 : n;
    return *this;
}

App &App::tls(Tls cfg) {
    auto ctx = std::make_unique<ssl::context>(ssl::context::tls_server);
    ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                     ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1 |
                     ssl::context::single_dh_use);
    // 空でも設定する。無いと OpenSSL の既定が端末でパスフレーズを尋ね、tls() が止まる。
    // 空を返せば、暗号化された鍵は読めずに投げる。
    ctx->set_password_callback(
        [pw = cfg.key_password](std::size_t, ssl::context::password_purpose) { return pw; });
    // 読めない証明書・鍵はここで投げる。bind() と同じく設定時に落とす。
    ctx->use_certificate_chain_file(cfg.cert_file);
    ctx->use_private_key_file(cfg.key_file, ssl::context::pem);
    // 鍵は種類ごとのスロットに入るので、種類が違うと読み込みでは照合されない。
    if (SSL_CTX_check_private_key(ctx->native_handle()) != 1) {
        throw std::invalid_argument("hayate::App::tls: key does not match certificate");
    }
    impl_->ssl_ctx = std::move(ctx);
    return *this;
}

App &App::limits(Limits l) {
    impl_->limits = l;
    return *this;
}

Limits &App::limits() noexcept { return impl_->limits; }

boost::asio::awaitable<void> App::run() {
    impl_->accepting = true;
    impl_->shutting = false;
    while (impl_->accepting.load()) {
        // 接続ごとに strand を 1 本。socket をそれに束縛して accept するので、
        // stream の読み書きも内部タイマーも同じ strand 上で直列に走る。
        net::any_io_executor conn_ex(net::make_strand(impl_->ioc));
        auto [ec, sock] = co_await impl_->acceptor->async_accept(conn_ex, net::as_tuple);
        if (ec) {
            if (!impl_->accepting.load() || ec == net::error::operation_aborted ||
                !impl_->acceptor->is_open()) {
                break;
            }
            // fd 枯渇などで終えると、負荷が引いた後も誰も繋げない。すぐ再試行すると空回りする。
            impl_->accept_retry.expires_after(accept_retry_delay);
            co_await impl_->accept_retry.async_wait(net::as_tuple);
            continue;
        }
        const auto n = impl_->connections.fetch_add(1) + 1;
        if (n > impl_->limits.max_connections) {
            release_one(impl_->connections);
            // close より先に数える。相手が EOF を見た時点で値が確定している。
            impl_->counters.rejected.fetch_add(1, std::memory_order_relaxed);
            boost::system::error_code ignored;
            sock.close(ignored);
            continue;
        }
        impl_->counters.accepted.fetch_add(1, std::memory_order_relaxed);
        beast::tcp_stream stream(std::move(sock));
        auto done = [impl = impl_.get()] { release_one(impl->connections); };
        if (impl_->ssl_ctx) {
            net::co_spawn(conn_ex,
                          serve_connection(detail::tls_stream{std::move(stream), *impl_->ssl_ctx},
                                           impl_->limits, impl_->router, impl_->counters,
                                           impl_->shutting, impl_->conns, std::move(done)),
                          net::detached);
        } else {
            net::co_spawn(conn_ex,
                          serve_connection(std::move(stream), impl_->limits, impl_->router,
                                           impl_->counters, impl_->shutting, impl_->conns,
                                           std::move(done)),
                          net::detached);
        }
    }
    co_return;
}

void App::serve() {
    impl_->ioc.restart();
    impl_->work = std::make_unique<net::executor_work_guard<net::io_context::executor_type>>(
        net::make_work_guard(impl_->ioc));
    // signal ハンドラと accept ループを admin strand に載せ、stop() と直列化する。
    impl_->signals = std::make_unique<net::signal_set>(impl_->admin, SIGINT, SIGTERM);
    impl_->signals->async_wait([this](const boost::system::error_code &ec, int) {
        if (!ec) {
            stop();
        }
    });
    net::co_spawn(impl_->admin, run(), net::detached);
    std::vector<std::thread> extras;
    extras.reserve(impl_->threads > 0 ? impl_->threads - 1 : 0);
    for (std::uint32_t i = 1; i < impl_->threads; ++i) {
        extras.emplace_back([this] { impl_->ioc.run(); });
    }
    impl_->ioc.run();
    for (auto &t : extras) {
        t.join();
    }
}

void App::stop() {
    if (!impl_) {
        return;
    }
    // admin strand なので、多重呼び出しも accept ループも直列になる。
    net::post(impl_->admin, [impl = impl_.get()] {
        impl->shutting = true;
        // 要求を待っているだけの接続は閉じる。始まった要求は Connection が完了させる。
        impl->conns.cancel_all();
        impl->accepting = false;
        boost::system::error_code ec;
        if (impl->acceptor) {
            impl->acceptor->close(ec);
        }
        impl->accept_retry.cancel();
        if (impl->signals) {
            impl->signals->cancel(ec);
        }
        impl->work.reset();
    });
}

// App::Impl が見えるのはこの翻訳単位だけなので、ここで定義する。
Handler metrics(App &app) {
    return [impl = app.impl_.get()](Request &) -> net::awaitable<Response> {
        auto res = Response::text(detail::render(impl->counters, impl->connections.load()));
        res.set_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
        co_return res;
    };
}

std::vector<std::pair<HttpMethod, std::string>> App::route_table() const {
    return impl_->router.route_table();
}

Json openapi(const App &app, OpenApiInfo info) {
    return detail::build_openapi(app.route_table(), info);
}

void App::add_route(HttpMethod method, std::string_view path, Handler handler) {
    impl_->router.add(method, path, std::move(handler));
}

void App::add_middleware(Middleware mw) { impl_->router.use(std::move(mw)); }

} // namespace hayate
