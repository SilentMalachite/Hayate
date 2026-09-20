#include "http_client.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>

namespace http = boost::beast::http;

TEST(Router, GetStatic) {
    TestServer srv([](hayate::App &app) {
        app.get("/ping", [](hayate::Request &) { return hayate::Response::text("pong"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/ping");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "pong");
}

TEST(Router, MissingPathIs404) {
    TestServer srv([](hayate::App &app) {
        app.get("/ping", [](hayate::Request &) { return hayate::Response::text("pong"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/nope");
    EXPECT_EQ(r.status, 404);
}

TEST(Router, MethodMismatchIs405WithAllow) {
    TestServer srv([](hayate::App &app) {
        app.get("/only-get", [](hayate::Request &) { return hayate::Response::text("x"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::post, "/only-get");
    EXPECT_EQ(r.status, 405);
    EXPECT_NE(r.allow.find("GET"), std::string::npos);
}

TEST(Router, PutOnGetIs405) {
    TestServer srv([](hayate::App &app) {
        app.get("/only-get", [](hayate::Request &) { return hayate::Response::text("x"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::put, "/only-get");
    EXPECT_EQ(r.status, 405);
    EXPECT_NE(r.allow.find("GET"), std::string::npos);
}

TEST(Router, ParamCapture) {
    TestServer srv([](hayate::App &app) {
        app.get("/users/:id", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.param("id")));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/users/42");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "42");
}

TEST(Router, StaticBeatsParam) {
    TestServer srv([](hayate::App &app) {
        app.get("/users/:id", [](hayate::Request &) { return hayate::Response::text("param"); });
        app.get("/users/me", [](hayate::Request &) { return hayate::Response::text("me"); });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/users/me");
    EXPECT_EQ(r.body, "me");
}

TEST(Router, Wildcard) {
    TestServer srv([](hayate::App &app) {
        app.get("/files/*rest", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.param("rest")));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/files/a/b.txt");
    EXPECT_EQ(r.body, "a/b.txt");
}

TEST(Router, GroupPrefix) {
    TestServer srv([](hayate::App &app) {
        app.group("/api", [](hayate::Router &r) {
            r.get("/ping", [](hayate::Request &) { return hayate::Response::text("ok"); });
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/api/ping");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "ok");
}

TEST(Router, QueryValue) {
    TestServer srv([](hayate::App &app) {
        app.get("/q", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.query("id")));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/q?id=7");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "7");
}

TEST(Router, MissingQueryIsEmptyView) {
    TestServer srv([](hayate::App &app) {
        app.get("/q", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.query("nope")));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/q");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "");
}

TEST(Router, MissingParamIsEmptyView) {
    TestServer srv([](hayate::App &app) {
        app.get("/x", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.param("nope")));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/x");
    EXPECT_EQ(r.body, "");
}

TEST(Router, ParamIsPercentDecoded) {
    TestServer srv([](hayate::App &app) {
        app.get("/u/:id", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.param("id")));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/u/a%20b%21");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "a b!");
}

TEST(Router, EncodedSlashStaysInOneSegment) {
    TestServer srv([](hayate::App &app) {
        app.get("/u/:id", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.param("id")));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/u/a%2Fb");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "a/b");
}

TEST(Router, BrokenPercentStaysLiteral) {
    TestServer srv([](hayate::App &app) {
        app.get("/u/:id", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.param("id")));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/u/a%zz");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "a%zz");
}

TEST(Router, QueryIsPercentDecodedWithPlus) {
    TestServer srv([](hayate::App &app) {
        app.get("/q", [](hayate::Request &req) {
            return hayate::Response::text(std::string(req.query("k")));
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/q?k=a+b%21");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "a b!");
}

TEST(Router, HeaderValueCannotInject) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) {
            auto res = hayate::Response::text("ok");
            res.set_header("X-Evil", "a\r\nX-Injected: 1");
            return res;
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                       std::chrono::seconds(2), {}, {"X-Injected", "X-Evil"});
    EXPECT_EQ(r.status, 200) << r.error_message;
    EXPECT_EQ(r.extra["X-Injected"], "");
    EXPECT_EQ(r.extra["X-Evil"], "aX-Injected: 1");
}

TEST(Router, InvalidHeaderNameIsIgnored) {
    TestServer srv([](hayate::App &app) {
        app.get("/", [](hayate::Request &) {
            auto res = hayate::Response::text("ok");
            res.set_header("Bad Name", "x");
            return res;
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/", {}, {},
                       std::chrono::seconds(2), {}, {"Bad Name"});
    EXPECT_EQ(r.status, 200) << r.error_message;
    EXPECT_EQ(r.extra["Bad Name"], "");
}
