#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>

// keep-alive で同じ接続に何本も送るための client。1 操作ごとに期限を掛ける。
// Beast の期限は同期 I/O に効かないので、非同期で回して timer で cancel する。
// 期限切れでも閉じないので後で読み直せる。期限切れは beast::error::timeout で返す。
class Conn {
  public:
    // 接続を後に回す印。fd を使い切るテストでは、fd だけ先に取って枯渇中に繋ぐ。
    struct Deferred {};
    static constexpr Deferred deferred{};

    // receive_buffer > 0 なら接続前に SO_RCVBUF を絞る。後からでは受信窓に効かない。
    explicit Conn(std::uint16_t port, std::chrono::milliseconds limit = std::chrono::seconds(2),
                  int receive_buffer = 0)
        : ep_(boost::asio::ip::make_address("127.0.0.1"), port) {
        if (receive_buffer > 0) {
            stream_.socket().open(ep_.protocol());
            stream_.socket().set_option(
                boost::asio::socket_base::receive_buffer_size(receive_buffer));
        }
        connect_error_ = connect(limit);
    }

    Conn(std::uint16_t port, Deferred) : ep_(boost::asio::ip::make_address("127.0.0.1"), port) {
        stream_.socket().open(ep_.protocol());
    }

    boost::system::error_code connect(std::chrono::milliseconds limit = std::chrono::seconds(2)) {
        return run(limit, [&]() -> boost::asio::awaitable<boost::system::error_code> {
            auto [ec] = co_await stream_.async_connect(ep_, boost::asio::as_tuple);
            co_return ec;
        });
    }

    boost::system::error_code connect_error() const { return connect_error_; }
    boost::beast::tcp_stream &stream() { return stream_; }

    // 生のバイトを書く。要求を途中まで送るときに使う。
    boost::system::error_code write_raw(std::string_view bytes,
                                        std::chrono::milliseconds limit = std::chrono::seconds(2)) {
        return run(limit, [&]() -> boost::asio::awaitable<boost::system::error_code> {
            auto [ec, n] = co_await boost::asio::async_write(
                stream_, boost::asio::buffer(bytes.data(), bytes.size()), boost::asio::as_tuple);
            (void)n;
            co_return ec;
        });
    }

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
    boost::asio::ip::tcp::endpoint ep_;
    boost::asio::io_context ioc_;
    boost::beast::tcp_stream stream_{ioc_};
    boost::beast::flat_buffer buf_;
    boost::system::error_code connect_error_;
};
