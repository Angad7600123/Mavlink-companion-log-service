cmake_minimum_required(VERSION 3.16)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(mcls_tests
    test_config.cpp
    test_received_ranges.cpp
    test_sha256.cpp
    test_database.cpp
    test_mavlink_log_protocol.cpp
    test_fc_sample_offsets.cpp
    test_dataflash_validator.cpp
    test_chunk_coverage.cpp
    test_service_snapshot.cpp
    test_companion_protocol.cpp
)

target_link_libraries(mcls_tests PRIVATE mcls_lib GTest::gtest_main)

include(GoogleTest)
add_test(NAME mcls_tests COMMAND mcls_tests)
