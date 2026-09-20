#pragma once

#include <hayate/http.hpp>
#include <hayate/limits.hpp>
#include <hayate/router.hpp>
#include <hayate/tls.hpp>

#include <boost/asio.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace hayate {

class App;

// カウンタを Prometheus テキストで返す Handler を作る。App が生きている間だけ有効。
Handler metrics(App &app);

class App {
  public:
    App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;
    ~App();

    template <typename H> App &get(std::string_view path, H &&h) {
        add_route(HttpMethod::get, path, wrap_handler(std::forward<H>(h)));
        return *this;
    }

    template <typename H> App &post(std::string_view path, H &&h) {
        add_route(HttpMethod::post, path, wrap_handler(std::forward<H>(h)));
        return *this;
    }

    template <typename M> App &use(M &&mw) {
        add_middleware(Middleware(std::forward<M>(mw)));
        return *this;
    }

    App &group(std::string_view prefix, std::function<void(Router &)> fn);

    App &bind(std::string_view host, std::uint16_t port);
    std::uint16_t port() const noexcept;
    App &threads(std::uint32_t n);
    App &tls(Tls cfg);
    App &limits(Limits l);
    Limits &limits() noexcept;
    boost::asio::awaitable<void> run();
    void serve();
    void stop();

  private:
    friend Handler metrics(App &);

    void add_route(HttpMethod method, std::string_view path, Handler handler);
    void add_middleware(Middleware mw);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hayate
