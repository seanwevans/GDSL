cmake_minimum_required(VERSION 3.16)
project(gdsl C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(gdsl STATIC
    src/gdsl/verify.c
    src/gdsl/diff.c)

target_include_directories(gdsl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()

add_executable(gdsl_verify_tests tests/test_verify.c)
target_link_libraries(gdsl_verify_tests PRIVATE gdsl)
add_test(NAME gdsl_verify_tests COMMAND gdsl_verify_tests)

add_executable(gdsl_diff_tests tests/test_diff.c)
target_link_libraries(gdsl_diff_tests PRIVATE gdsl)
add_test(NAME gdsl_diff_tests COMMAND gdsl_diff_tests)
