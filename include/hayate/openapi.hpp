#pragma once

#include <hayate/http.hpp>

#include <string>

namespace hayate {

class App;

struct OpenApiInfo {
    std::string title{"hayate"};
    std::string version{"0.1.0"};
};

// 登録済みルートから OpenAPI 3.1 の文書を作る。定義は src/app.cpp。
Json openapi(const App &app, OpenApiInfo info = {});

} // namespace hayate
