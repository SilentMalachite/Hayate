cmake_minimum_required(VERSION 3.28)
project(hayate VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HAYATE_ENABLE_SANITIZERS "ASan+UBSan" OFF)
option(HAYATE_BUILD_TESTS "Build tests" OFF)
option(HAYATE_BUILD_EXAMPLES "Build examples" OFF)

if(APPLE AND EXISTS /opt/homebrew)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
endif()

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.83 REQUIRED CONFIG)
find_package(Threads REQUIRED)

if(NOT Boost_FOUND)
    message(FATAL_ERROR "Boost 1.83+ required (Asio/Beast). brew install boost / apt install libboost-dev")
endif()

include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(hayate
    src/error.cpp
    src/request.cpp
    src/response.cpp
    src/router.cpp
    src/app.cpp
    src/listener.cpp
    src/connection.cpp
    src/files.cpp
)
target_include_directories(hayate PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
if(TARGET Boost::headers)
    target_link_libraries(hayate PUBLIC Boost::headers)
else()
    target_include_directories(hayate PUBLIC ${Boost_INCLUDE_DIRS})
endif()
target_link_libraries(hayate PUBLIC
    nlohmann_json::nlohmann_json
    Threads::Threads
)
target_compile_features(hayate PUBLIC cxx_std_20)

if(HAYATE_ENABLE_SANITIZERS)
    target_compile_options(hayate PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(hayate PUBLIC -fsanitize=address,undefined)
endif()

if(HAYATE_BUILD_EXAMPLES)
    add_executable(hello examples/hello/main.cpp)
    target_link_libraries(hello PRIVATE hayate)
endif()

if(HAYATE_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_executable(hayate_tests
        tests/test_result.cpp
        tests/test_hello.cpp
        tests/test_router.cpp
        tests/test_json.cpp
        tests/test_middleware.cpp
        tests/test_limits.cpp
        tests/test_shutdown.cpp
        tests/test_cors.cpp
        tests/test_static.cpp
        tests/test_rate_limit.cpp
    )
    target_link_libraries(hayate_tests PRIVATE hayate GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(hayate_tests)
endif()
