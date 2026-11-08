cmake_minimum_required(VERSION 3.16)
project(cleanbasic VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Catch2 v3 integration (find_package with FetchContent fallback)
set(CATCH_CONFIG_DISABLE_EXCEPTIONS ON CACHE BOOL "Disable exceptions in Catch2" FORCE)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "Disable Catch2 docs" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "Disable Catch2 extras" FORCE)

find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

# Core header-only transpiler library
add_library(cleanbasic_core INTERFACE)
target_include_directories(cleanbasic_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(cleanbasic_core SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)
target_compile_definitions(cleanbasic_core INTERFACE
    CLEANBASIC_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
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

# CleanBasic test executor executable (Catch2)
add_executable(cleanbasic_tests
    tests/test_main.cpp
)
target_link_libraries(cleanbasic_tests PRIVATE
    cleanbasic_core
    Catch2::Catch2WithMain
)

# CTest integration
include(CTest)
include(Catch)
catch_discover_tests(cleanbasic_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(run_fix13 ALL COMMAND python3 ${CMAKE_SOURCE_DIR}/fix13.py)
add_dependencies(cleanbasic_tests run_fix13)
