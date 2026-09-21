#include "http_client.hpp"
#include "test_server.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace http = boost::beast::http;

// ハンドラは全接続で共有される。mutable は同期・非同期とも受けない。
namespace {
auto sync_const = [](hayate::Request &) { return hayate::Response::text("x"); };
auto async_const = [](hayate::Request &) -> boost::asio::awaitable<hayate::Response> {
    co_return hayate::Response::text("x");
};
auto sync_mutable = [n = 0](hayate::Request &) mutable {
    ++n;
    return hayate::Response::text("x");
};
auto async_mutable =
    [n = 0](hayate::Request &) mutable -> boost::asio::awaitable<hayate::Response> {
    ++n;
    co_return hayate::Response::text("x");
};
} // namespace
static_assert(hayate::HandlerCallable<decltype(sync_const)>);
static_assert(hayate::HandlerCallable<decltype(async_const)>);
static_assert(!hayate::HandlerCallable<decltype(sync_mutable)>);
static_assert(!hayate::HandlerCallable<decltype(async_mutable)>);

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

// 継ぎ目だけ見ると prefix 内の `//` が残り、`/api//v1/ping` で登録される。
TEST(Router, GroupCollapsesDoubleSlash) {
    TestServer srv([](hayate::App &app) {
        app.group("/api//v1", [](hayate::Router &r) {
            r.get("/ping", [](hayate::Request &) { return hayate::Response::text("ok"); });
        });
    });
    auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/api/v1/ping");
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

// 同じ形の後の方には一致する要求が無い。param / wildcard の名前は形に入らない。
TEST(Router, DuplicateShapeThrows) {
    auto h = [](hayate::Request &) { return hayate::Response::text("x"); };
    {
        hayate::App app;
        app.get("/u/:id", h);
        EXPECT_THROW(app.get("/u/:id", h), std::invalid_argument);
        EXPECT_THROW(app.get("/u/:name", h), std::invalid_argument);
        EXPECT_NO_THROW(app.post("/u/:name", h));
        EXPECT_NO_THROW(app.get("/u/me", h));
        EXPECT_NO_THROW(app.get("/u/*rest", h));
        EXPECT_THROW(app.get("/u/*all", h), std::invalid_argument);
    }
    {
        // group をまたいでも、group の中同士でも同じ。
        hayate::App app;
        app.group("/api", [&](hayate::Router &r) { r.get("/ping", h); });
        EXPECT_THROW(app.get("/api/ping", h), std::invalid_argument);
        EXPECT_THROW(app.group("/g",
                               [&](hayate::Router &r) {
                                   r.get("/x", h);
                                   r.get("/x", h);
                               }),
                     std::invalid_argument);
    }
}

// 登録できるのは GET と POST だけ。unknown のルートは HEAD や PUT に一致してしまう。
TEST(Router, AddRejectsOtherMethods) {
    auto h = [](hayate::Request &) -> boost::asio::awaitable<hayate::Response> {
        co_return hayate::Response::text("x");
    };
    hayate::Router r;
    EXPECT_THROW(r.add(hayate::HttpMethod::options, "/x", h), std::invalid_argument);
    EXPECT_THROW(r.add(hayate::HttpMethod::unknown, "/x", h), std::invalid_argument);
    EXPECT_NO_THROW(r.add(hayate::HttpMethod::get, "/x", h));
    hayate::App app;
    EXPECT_THROW(
        app.group("/g", [&](hayate::Router &g) { g.add(hayate::HttpMethod::unknown, "/x", h); }),
        std::invalid_argument);
}

// 名前の無い param / wildcard と途中の wildcard は、一致する要求が無いか名前で引けない。
TEST(Router, MalformedPatternThrows) {
    auto h = [](hayate::Request &) { return hayate::Response::text("x"); };
    hayate::App app;
    for (const char *p : {"/a/:", "/a/*", "/:", "/*", "/a/*rest/b"}) {
        EXPECT_THROW(app.get(p, h), std::invalid_argument) << p;
    }
    EXPECT_NO_THROW(app.get("/a/*rest", h));
}

// セグメントごとに param > wildcard。登録順で覆らない。
TEST(Router, ParamBeatsWildcard) {
    for (const bool wildcard_first : {false, true}) {
        TestServer srv([&](hayate::App &app) {
            auto param = [](hayate::Request &) { return hayate::Response::text("param"); };
            auto wild = [](hayate::Request &) { return hayate::Response::text("wild"); };
            if (wildcard_first) {
                app.get("/a/*rest", wild);
                app.get("/a/:x", param);
            } else {
                app.get("/a/:x", param);
                app.get("/a/*rest", wild);
            }
        });
        auto r = http_call("127.0.0.1", srv.port(), http::verb::get, "/a/b");
        EXPECT_EQ(r.body, "param") << "wildcard_first=" << wildcard_first;
    }
}

// 空の wildcard より完全一致。登録順で覆らない。
TEST(Router, ExactBeatsEmptyWildcard) {
    for (const bool wildcard_first : {false, true}) {
        TestServer srv([&](hayate::App &app) {
            auto exact = [](hayate::Request &) { return hayate::Response::text("exact"); };
            auto wild = [](hayate::Request &req) {
                return hayate::Response::text("wild:" + std::string(req.param("rest")));
            };
            if (wildcard_first) {
                app.get("/a/*rest", wild);
                app.get("/a", exact);
            } else {
                app.get("/a", exact);
                app.get("/a/*rest", wild);
            }
        });
        const auto ctx = "wildcard_first=" + std::to_string(wildcard_first);
        EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/a").body, "exact") << ctx;
        EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/a/b").body, "wild:b")
            << ctx;
    }
}

TEST(Router, RootBeatsEmptyWildcard) {
    TestServer srv([](hayate::App &app) {
        app.get("/*rest", [](hayate::Request &) { return hayate::Response::text("wild"); });
        app.get("/", [](hayate::Request &) { return hayate::Response::text("root"); });
    });
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/").body, "root");
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/x").body, "wild");
}

// `:name` は空のセグメントに一致しない。空を受けたいなら wildcard。
TEST(Router, ParamSkipsEmptySegment) {
    TestServer srv([](hayate::App &app) {
        app.get("/a/:x", [](hayate::Request &) { return hayate::Response::text("a"); });
        app.get("/b/:x/c", [](hayate::Request &) { return hayate::Response::text("b"); });
        app.get("/d/:x", [](hayate::Request &) { return hayate::Response::text("param"); });
        app.get("/d/*rest", [](hayate::Request &req) {
            return hayate::Response::text("wild:" + std::string(req.param("rest")));
        });
    });
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/a/").status, 404);
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/b//c").status, 404);
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/d/").body, "wild:");
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/d/e").body, "param");
}

// 末尾の `/` は消さないので、`/a` と `/a/` は別ルート。group の "/" も `/api/` になる。
TEST(Router, TrailingSlashIsDistinct) {
    TestServer srv([](hayate::App &app) {
        app.get("/a", [](hayate::Request &) { return hayate::Response::text("a"); });
        app.group("/api", [](hayate::Router &r) {
            r.get("/", [](hayate::Request &) { return hayate::Response::text("api"); });
        });
    });
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/a").status, 200);
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/a/").status, 404);
    auto api = http_call("127.0.0.1", srv.port(), http::verb::get, "/api/");
    EXPECT_EQ(api.status, 200);
    EXPECT_EQ(api.body, "api");
    EXPECT_EQ(http_call("127.0.0.1", srv.port(), http::verb::get, "/api").status, 404);
}
