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

add_executable(sonny)

target_sources(sonny
    PRIVATE
        src/sonny.cpp
        src/common/datetime.cpp
        src/common/logger.cpp
        src/core/chartdata.cpp
        src/client/curlrequest.cpp
        src/client/simplecurlwrapper.cpp
        src/client/webullclient.cpp
        src/dukascopy/dukascopy.cpp
)

if(UNIX)
    # Linux-specific secrets manager
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED libsecret-1)
    target_include_directories(sonny 
        PRIVATE
            ${LIBSECRET_INCLUDE_DIRS}
    )
endif()

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

target_link_libraries(sonny
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

if(UNIX)
    target_link_libraries(sonny
        PRIVATE
            ${LIBSECRET_LIBRARIES}
    )
endif()