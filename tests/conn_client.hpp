#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <cstdint>

// keep-alive で同じ接続に何本も送るための client。1 操作ごとに期限を掛ける。
// Beast の期限は同期 I/O に効かないので、非同期で回して timer で cancel する。
// 期限切れでも閉じないので後で読み直せる。期限切れは beast::error::timeout で返す。
class Conn {
  public:
    explicit Conn(std::uint16_t port, std::chrono::milliseconds limit = std::chrono::seconds(2)) {
        const boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"), port);
        connect_error_ = run(limit, [&]() -> boost::asio::awaitable<boost::system::error_code> {
            auto [ec] = co_await stream_.async_connect(ep, boost::asio::as_tuple);
            co_return ec;
        });
    }

    boost::system::error_code connect_error() const { return connect_error_; }
    boost::beast::tcp_stream &stream() { return stream_; }

    // 要求か serializer を書く。serializer なら write_header の続きを書ける。
    template <typename Msg>
    boost::system::error_code write(Msg &&m,
                                    std::chrono::milliseconds limit = std::chrono::seconds(2)) {
        return run(limit, [&]() -> boost::asio::awaitable<boost::system::error_code> {
            auto [ec, n] =
                co_await boost::beast::http::async_write(stream_, m, boost::asio::as_tuple);
            (void)n;
            co_return ec;
        });
    }

    template <typename Serializer>
    boost::system::error_code
    write_header(Serializer &sr, std::chrono::milliseconds limit = std::chrono::seconds(2)) {
        return run(limit, [&]() -> boost::asio::awaitable<boost::system::error_code> {
            auto [ec, n] =
                co_await boost::beast::http::async_write_header(stream_, sr, boost::asio::as_tuple);
            (void)n;
            co_return ec;
        });
    }

    // 応答か parser を読む。
    template <typename Msg>
    boost::system::error_code read(Msg &m,
                                   std::chrono::milliseconds limit = std::chrono::seconds(2)) {
        return run(limit, [&]() -> boost::asio::awaitable<boost::system::error_code> {
            auto [ec, n] =
                co_await boost::beast::http::async_read(stream_, buf_, m, boost::asio::as_tuple);
            (void)n;
            co_return ec;
        });
    }

    // 任意の操作を期限つきで回す。op は awaitable<error_code> を返す。
    template <typename Op> boost::system::error_code run(std::chrono::milliseconds limit, Op op) {
        namespace net = boost::asio;
        boost::system::error_code out;
        bool expired = false;
        net::steady_timer timer(ioc_, limit);
        timer.async_wait([&](boost::system::error_code ec) {
            if (!ec) {
                expired = true;
                stream_.cancel();
            }
        });
        net::co_spawn(
            ioc_,
            [&]() -> net::awaitable<void> {
                out = co_await op();
                timer.cancel();
            },
            net::detached);
        ioc_.restart();
        ioc_.run();
        // 完了と期限が同時なら、成功した方を採る。
        if (expired && out) {
            return boost::beast::error::timeout;
        }
        return out;
    }

  private:
    boost::asio::io_context ioc_;
    boost::beast::tcp_stream stream_{ioc_};
    boost::beast::flat_buffer buf_;
    boost::system::error_code connect_error_;
};
