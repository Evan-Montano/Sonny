cmake_minimum_required(VERSION 3.23)

project(sonny)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0") # disabled optimization to make debugging more predictable

include(FetchContent)

# Bundling both Paho libraries keeps their TLS support and versions aligned across hosts.
set(PAHO_WITH_MQTT_C ON CACHE BOOL "Build Paho MQTT C with C++ client")
set(PAHO_WITH_SSL ON CACHE BOOL "Build Paho MQTT with TLS")
set(PAHO_BUILD_STATIC ON CACHE BOOL "Build static Paho libraries")
set(PAHO_BUILD_SHARED OFF CACHE BOOL "Do not build shared Paho libraries")
set(PAHO_BUILD_EXAMPLES OFF CACHE BOOL "Do not build Paho examples")
set(PAHO_BUILD_SAMPLES OFF CACHE BOOL "Do not build Paho samples")
set(PAHO_BUILD_TESTS OFF CACHE BOOL "Do not build Paho tests")
set(PAHO_BUILD_DOCUMENTATION OFF CACHE BOOL "Do not build Paho documentation")
set(PAHO_ENABLE_TESTING OFF CACHE BOOL "Do not build Paho C tests")

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v7.0.3
)

FetchContent_MakeAvailable(ftxui)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
)

FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
    paho_mqtt_cpp
    GIT_REPOSITORY https://github.com/eclipse-paho/paho.mqtt.cpp.git
    GIT_TAG v1.6.0
    GIT_SUBMODULES_RECURSE TRUE
)
# Keep Sonny's warnings-as-errors policy on its own targets. The vendored Paho
# C source triggers a Clang const-qualification warning under -Werror.
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
FetchContent_MakeAvailable(paho_mqtt_cpp)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenMP REQUIRED)
find_package(faiss CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

add_library(sonny_common STATIC
    src/common/datetime.cpp
    src/common/logger.cpp
    src/common/storage.cpp
    src/common/secretstore.cpp
)

target_include_directories(sonny_common
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(sonny_common
    PRIVATE
        PkgConfig::LIBSECRET
        ftxui::screen
        ftxui::dom
        ftxui::component
)

add_library(sonny_core STATIC
    src/core/engine.cpp
    src/core/transformer.cpp
)

target_include_directories(sonny_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(sonny_core
    PUBLIC
        sonny_common
        faiss
)

add_executable(sonny
    src/sonny.cpp
    src/client/curlrequest.cpp
    src/client/simplecurlwrapper.cpp
    src/dukascopy/dukascopy.cpp
    src/core/setup.cpp
)

add_library(sonny_webull STATIC src/client/webullstream.cpp)
target_include_directories(sonny_webull PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(sonny_webull
    PUBLIC sonny_common nlohmann_json::nlohmann_json
    PRIVATE CURL::libcurl OpenSSL::Crypto PahoMqttCpp::paho-mqttpp3-static
)

add_library(sonny_runtime STATIC
    src/runtime/runtime.cpp
)

target_include_directories(sonny_runtime
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(sonny_runtime
    PUBLIC
        sonny_core
        sonny_common
        sonny_webull
    PRIVATE
        ftxui::screen
        ftxui::dom
        ftxui::component
)

target_compile_definitions(sonny
    PRIVATE
        SONNY_BROWSER_CLIENT_PATH="${PROJECT_SOURCE_DIR}/src/client/browserclient.js"
)

target_link_libraries(sonny
    PRIVATE
        sonny_runtime
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenMP::OpenMP_CXX
        ftxui::screen
        ftxui::dom
        ftxui::component
)

enable_testing()
add_subdirectory(tests)
