cmake_minimum_required(VERSION 3.23)

project(sonny)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

add_executable(sonny)

target_sources(sonny
    PRIVATE
        src/sonny.cpp
        src/client/curlrequest.cpp
        src/client/simplecurlwrapper.cpp
        src/client/webullclient.cpp
)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
target_link_libraries(sonny
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)