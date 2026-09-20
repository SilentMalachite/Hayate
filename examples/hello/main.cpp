#include <hayate/hayate.hpp>

int main() {
    hayate::App app;
    app.get("/", [](hayate::Request &) { return hayate::Response::text("hello"); });
    app.bind("0.0.0.0", 8080).serve();
    return 0;
}
