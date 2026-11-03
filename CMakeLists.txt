cmake_minimum_required(VERSION 3.16)
project(cleanbasic VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core header-only transpiler library
add_library(cleanbasic_core INTERFACE)
target_include_directories(cleanbasic_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(cleanbasic_core INTERFACE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Werror
)

# CleanBasic transpiler CLI executable
add_executable(cleanbasic
    src/cleanbasic/cli_main.cpp
)
target_link_libraries(cleanbasic PRIVATE cleanbasic_core)

# CleanBasic test executor executable
add_executable(cleanbasic_tests
    tests/test_main.cpp
)
target_link_libraries(cleanbasic_tests PRIVATE cleanbasic_core)

# CTest integration
enable_testing()
add_test(
    NAME cleanbasic_tests
    COMMAND cleanbasic_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
