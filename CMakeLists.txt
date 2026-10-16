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

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenMP REQUIRED)
find_package(faiss CONFIG REQUIRED)

add_library(sonny_common STATIC
    src/common/datetime.cpp
    src/common/logger.cpp
    src/common/storage.cpp
)

target_include_directories(sonny_common
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(sonny_common
    PRIVATE
        ftxui::screen
        ftxui::dom
        ftxui::component
)

add_library(sonny_core STATIC
    src/core/engine.cpp
    src/core/transformer.cpp
    src/core/chartdata.cpp
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
    src/client/webullclient.cpp
    src/dukascopy/dukascopy.cpp
    src/core/setup.cpp
)

target_compile_definitions(sonny
    PRIVATE
        SONNY_BROWSER_CLIENT_PATH="${PROJECT_SOURCE_DIR}/src/client/browserclient.js"
)

target_link_libraries(sonny
    PRIVATE
        sonny_core
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenMP::OpenMP_CXX
        ftxui::screen
        ftxui::dom
        ftxui::component
)

enable_testing()
add_subdirectory(tests)