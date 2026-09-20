#include "connection.hpp"
#include "detail/asio.hpp"

#include <hayate/app.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace hayate {
namespace beast = detail::beast;
namespace net = detail::net;
using tcp = detail::tcp;

namespace {

// 0 を下回らせない。accept 側と Connection の終了側の両方から呼ばれる。
void release_one(std::atomic<std::uint32_t> &n) {
    auto cur = n.load();
    while (cur > 0 && !n.compare_exchange_weak(cur, cur - 1)) {
    }
}

} // namespace

struct App::Impl {
    net::io_context ioc;
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

App &App::limits(Limits l) {
    impl_->limits = l;
    return *this;
}

Limits &App::limits() noexcept { return impl_->limits; }

boost::asio::awaitable<void> App::run() {
    impl_->accepting = true;
    impl_->shutting = false;
    while (impl_->accepting.load()) {
        auto [ec, sock] = co_await impl_->acceptor->async_accept(net::as_tuple);
        if (ec) {
            break;
        }
        const auto n = impl_->connections.fetch_add(1) + 1;
        if (n > impl_->limits.max_connections) {
            release_one(impl_->connections);
            boost::system::error_code ignored;
            sock.close(ignored);
            continue;
        }
        beast::tcp_stream stream(std::move(sock));
        net::co_spawn(impl_->ioc,
                      serve_connection(std::move(stream), impl_->limits, impl_->router,
                                       impl_->shutting,
                                       [impl = impl_.get()] { release_one(impl->connections); }),
                      net::detached);
    }
    co_return;
}

void App::serve() {
    impl_->ioc.restart();
    impl_->work = std::make_unique<net::executor_work_guard<net::io_context::executor_type>>(
        net::make_work_guard(impl_->ioc));
    impl_->signals = std::make_unique<net::signal_set>(impl_->ioc, SIGINT, SIGTERM);
    impl_->signals->async_wait([this](const boost::system::error_code &ec, int) {
        if (!ec) {
            stop();
        }
    });
    net::co_spawn(impl_->ioc, run(), net::detached);
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
    net::post(impl_->ioc, [impl = impl_.get()] {
        impl->shutting = true;
        impl->accepting = false;
        boost::system::error_code ec;
        if (impl->acceptor) {
            impl->acceptor->close(ec);
        }
        if (impl->signals) {
            impl->signals->cancel(ec);
        }
        impl->work.reset();
    });
}

void App::add_route(HttpMethod method, std::string_view path, Handler handler) {
    impl_->router.add(method, path, std::move(handler));
}

void App::add_middleware(Middleware mw) { impl_->router.use(std::move(mw)); }

} // namespace hayate
