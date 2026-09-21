#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>
#include <type_traits>

// keep-alive で同じ接続に何本も送るための client。1 操作ごとに期限を掛ける。
// Beast の期限は同期 I/O に効かないので、非同期で回して timer で cancel する。
// 期限切れでも閉じないので後で読み直せる。期限切れは beast::error::timeout で返す。
// Stream は平文の tcp_stream か、その上の ssl_stream。
template <typename Stream> class BasicConn {
    static constexpr bool plain = std::is_same_v<Stream, boost::beast::tcp_stream>;

  public:
    // 接続を後に回す印。fd を使い切るテストでは、fd だけ先に取って枯渇中に繋ぐ。
    struct Deferred {};
    static constexpr Deferred deferred{};

    // receive_buffer > 0 なら接続前に SO_RCVBUF を絞る。後からでは受信窓に効かない。
    explicit BasicConn(std::uint16_t port,
                       std::chrono::milliseconds limit = std::chrono::seconds(2),
                       int receive_buffer = 0)
        requires plain
        : ep_(boost::asio::ip::make_address("127.0.0.1"), port), stream_(ioc_) {
        if (receive_buffer > 0) {
            lowest().socket().open(ep_.protocol());
            lowest().socket().set_option(
                boost::asio::socket_base::receive_buffer_size(receive_buffer));
        }
        connect_error_ = connect(limit);
    }

    BasicConn(std::uint16_t port, Deferred)
        requires plain
        : ep_(boost::asio::ip::make_address("127.0.0.1"), port), stream_(ioc_) {
        lowest().socket().open(ep_.protocol());
    }

    // 繋いでからハンドシェイクまで済ませる。どちらの失敗も connect_error() に入る。
    BasicConn(std::uint16_t port, boost::asio::ssl::context &ctx,
              std::chrono::milliseconds limit = std::chrono::seconds(2))
        requires(!plain)
        : ep_(boost::asio::ip::make_address("127.0.0.1"), port), stream_(ioc_, ctx) {
        connect_error_ = connect(limit);
        if (!connect_error_) {
            connect_error_ = handshake(limit);
        }
    }

    boost::system::error_code connect(std::chrono::milliseconds limit = std::chrono::seconds(2)) {
        return run(limit, [&]() -> boost::asio::awaitable<boost::system::error_code> {
            auto [ec] = co_await lowest().async_connect(ep_, boost::asio::as_tuple);
            co_return ec;
        });
    }

    boost::system::error_code handshake(std::chrono::milliseconds limit = std::chrono::seconds(2))
        requires(!plain)
    {
        return run(limit, [&]() -> boost::asio::awaitable<boost::system::error_code> {
            auto [ec] = co_await stream_.async_handshake(boost::asio::ssl::stream_base::client,
                                                         boost::asio::as_tuple);
            co_return ec;
        });
    }

    boost::system::error_code connect_error() const { return connect_error_; }
    Stream &stream() { return stream_; }

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
                lowest().cancel();
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
    boost::beast::tcp_stream &lowest() { return boost::beast::get_lowest_layer(stream_); }

    boost::asio::ip::tcp::endpoint ep_;
    boost::asio::io_context ioc_;
    Stream stream_;
    boost::beast::flat_buffer buf_;
    boost::system::error_code connect_error_;
};

using Conn = BasicConn<boost::beast::tcp_stream>;
using TlsConn = BasicConn<boost::beast::ssl_stream<boost::beast::tcp_stream>>;
