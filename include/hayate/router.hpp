#pragma once

#include <hayate/http.hpp>
#include <hayate/request.hpp>
#include <hayate/response.hpp>

#include <boost/asio.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace hayate {

using Handler = std::function<boost::asio::awaitable<Response>(Request &)>;
using Next = Handler;
using Middleware = std::function<boost::asio::awaitable<Response>(Request &, Next)>;

// ハンドラは全接続で共有される。const で呼べるものだけ受ける（mutable は threads(n>1)
// で競合する）。
template <typename H>
concept HandlerCallable =
    std::is_invocable_r_v<boost::asio::awaitable<Response>, const std::decay_t<H> &, Request &> ||
    std::is_invocable_r_v<Response, const std::decay_t<H> &, Request &>;

template <typename H> Handler wrap_handler(H &&h) {
    static_assert(HandlerCallable<H>, "hayate: handler は const で呼べること。mutable にせず、"
                                      "状態は App か Request の Extension に置く");
    // 弾いた後は下の分岐を実体化させない。副次エラーで static_assert の文言が埋もれる。
    if constexpr (!HandlerCallable<H>) {
        return {};
    } else if constexpr (std::is_invocable_r_v<boost::asio::awaitable<Response>, H, Request &>) {
        return Handler(std::forward<H>(h));
    } else {
        return [fn = std::forward<H>(h)](Request &req) -> boost::asio::awaitable<Response> {
            co_return fn(req);
        };
    }
}

class Router {
  public:
    Router();
    Router(const Router &) = delete;
    Router &operator=(const Router &) = delete;
    Router(Router &&) noexcept;
    Router &operator=(Router &&) noexcept;
    ~Router();

    Router &use(Middleware mw);

    template <typename H> Router &get(std::string_view path, H &&h) {
        add(HttpMethod::get, path, wrap_handler(std::forward<H>(h)));
        return *this;
    }

    template <typename H> Router &post(std::string_view path, H &&h) {
        add(HttpMethod::post, path, wrap_handler(std::forward<H>(h)));
        return *this;
    }

    Router &group(std::string_view prefix, std::function<void(Router &)> fn);

    void add(HttpMethod method, std::string_view path, Handler handler);
    boost::asio::awaitable<Response> dispatch(Request &req) const;

  private:
    boost::asio::awaitable<Response> dispatch_route(Request &req) const;
    std::vector<std::pair<HttpMethod, std::string>> route_table() const;
    friend class App;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hayate
