#pragma once

#include <hayate/app.hpp>

#include <cstdint>
#include <functional>
#include <thread>

class TestServer {
  public:
    explicit TestServer(std::function<void(hayate::App &)> setup) {
        setup(app_);
        app_.bind("127.0.0.1", 0);
        port_ = app_.port();
        th_ = std::thread([this] { app_.serve(); });
    }

    std::uint16_t port() const { return port_; }
    hayate::App &app() { return app_; }

    ~TestServer() {
        app_.stop();
        if (th_.joinable()) {
            th_.join();
        }
    }

  private:
    hayate::App app_;
    std::uint16_t port_{};
    std::thread th_;
};
