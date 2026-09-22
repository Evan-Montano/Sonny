cmake_minimum_required(VERSION 3.23)

project(sonny)

if(MSVC)
    add_compile_options(/std:c++latest)
else()
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

if (UNIX)
    set(CMAKE_BUILD_TYPE Debug)
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0") # disabled optimization to make debugging more predictable
endif()

include(FetchContent)
 
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v7.0.3  # Replace with a version, tag, or commit hash
)

FetchContent_MakeAvailable(ftxui)


add_executable(sonny)

target_sources(sonny
    PRIVATE
        src/sonny.cpp
        src/common/datetime.cpp
        src/common/logger.cpp
        src/common/storage.cpp
        src/core/chartdata.cpp
        src/client/curlrequest.cpp
        src/client/simplecurlwrapper.cpp
        src/client/webullclient.cpp
        src/dukascopy/dukascopy.cpp
        src/core/faiss.cpp
        src/core/setup.cpp
)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenMP REQUIRED)
find_package(faiss CONFIG REQUIRED)

target_link_libraries(sonny
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
        faiss
        ftxui::screen
        ftxui::dom
        ftxui::component
)