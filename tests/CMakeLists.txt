add_executable(sonny_datetime_tests
    common/datetime_tests.cpp
)

target_link_libraries(sonny_datetime_tests
    PRIVATE
        sonny_common
        GTest::gtest_main
)

include(GoogleTest)

gtest_discover_tests(sonny_datetime_tests)