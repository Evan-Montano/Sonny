add_executable(sonny_common_tests
    common/datetime_tests.cpp
    common/jsonutil_tests.cpp
)

target_link_libraries(sonny_common_tests
    PRIVATE
        sonny_common
        GTest::gtest_main
)

include(GoogleTest)

gtest_discover_tests(sonny_common_tests)