cmake_minimum_required(VERSION 3.16)
project(hw1_ota_protocol_host_tests C)

enable_testing()

add_executable(hw1_ota_protocol_tests
    test_hw1_ota_protocol.c
    ../../hw1_ota_protocol.c
)
target_include_directories(hw1_ota_protocol_tests PRIVATE ../../include)
target_compile_features(hw1_ota_protocol_tests PRIVATE c_std_11)
target_compile_options(hw1_ota_protocol_tests PRIVATE
    -Wall
    -Wextra
    -Werror
    -pedantic
)

add_test(NAME hw1_ota_protocol_tests COMMAND hw1_ota_protocol_tests)
