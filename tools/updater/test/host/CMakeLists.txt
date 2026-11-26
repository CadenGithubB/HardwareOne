cmake_minimum_required(VERSION 3.16)
project(hw1_updater_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

# Compiles the REAL throttle source, not a host reimplementation of it - the
# whole point of keeping recovery_auth_throttle.c free of ESP-IDF dependencies.
add_executable(test_recovery_auth_throttle
    test_recovery_auth_throttle.c
    ../../main/recovery_auth_throttle.c)
target_include_directories(test_recovery_auth_throttle PRIVATE ../../main)
target_compile_options(test_recovery_auth_throttle PRIVATE -Wall -Wextra -Werror)

add_test(NAME recovery_auth_throttle COMMAND test_recovery_auth_throttle)
