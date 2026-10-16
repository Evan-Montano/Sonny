add_executable(sonny_common_tests
    common/datetime_tests.cpp
    common/jsonutil_tests.cpp
    core/halfhourindex_tests.cpp
)

target_compile_definitions(sonny_common_tests
    PRIVATE
        SONNY_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(sonny_common_tests
    PRIVATE
        sonny_core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(sonny_common_tests)