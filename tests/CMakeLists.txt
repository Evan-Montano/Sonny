add_executable(sonny_common_tests
    common/datetime_tests.cpp
    common/jsonutil_tests.cpp
    core/halfhourindex_tests.cpp
    core/transformer_tests.cpp
    client/webullstream_tests.cpp
    client/webull_proto_tests.cpp
)

target_include_directories(sonny_common_tests
    PRIVATE
        ${WEBULL_PROTO_BINARY_DIR}
)

target_compile_definitions(sonny_common_tests
    PRIVATE
        SONNY_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(sonny_common_tests
    PRIVATE
        sonny_core
        sonny_webull
        protobuf::libprotobuf
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(sonny_common_tests)
