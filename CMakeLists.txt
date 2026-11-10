cmake_minimum_required(VERSION 3.16)
project(cleanbasic VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Catch2 v3 integration (find_package with FetchContent fallback)
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
    -fno-rtti
    -Wall
    -Wextra
    -Werror
)

set(CLEANBASIC_PCH_HEADERS
    <algorithm>
    <chrono>
    <cstdint>
    <cstdlib>
    <filesystem>
    <fstream>
    <functional>
    <iomanip>
    <iostream>
    <memory>
    <mutex>
    <optional>
    <sstream>
    <string>
    <string_view>
    <tuple>
    <type_traits>
    <unordered_map>
    <unordered_set>
    <utility>
    <variant>
    <vector>
)

# CleanBasic transpiler CLI executable
add_executable(cleanbasic
    src/cleanbasic/cli_main.cpp
)
target_link_libraries(cleanbasic PRIVATE cleanbasic_core)
target_precompile_headers(cleanbasic PRIVATE ${CLEANBASIC_PCH_HEADERS})
option(CLEANBASIC_STATIC_RUNTIME "Link C++ runtime (libstdc++/libgcc) statically into cleanbasic" ON)
if(CLEANBASIC_STATIC_RUNTIME AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(MINGW)
        target_link_options(cleanbasic PRIVATE
            -static-libstdc++
            -static-libgcc
            -Wl,-Bstatic
            -lwinpthread
            -Wl,-Bdynamic
        )
    else()
        target_link_options(cleanbasic PRIVATE
            -static-libstdc++
            -static-libgcc
        )
    endif()
endif()

# CleanBasic test executor executable (Catch2)
add_executable(cleanbasic_tests
    tests/test_main.cpp
)
target_link_libraries(cleanbasic_tests PRIVATE
    cleanbasic_core
    Catch2::Catch2WithMain
)
target_precompile_headers(cleanbasic_tests PRIVATE ${CLEANBASIC_PCH_HEADERS})

# CTest integration
include(CTest)
include(Catch)
catch_discover_tests(cleanbasic_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

find_program(NPM_EXECUTABLE npm)
if(NPM_EXECUTABLE)
    set(VSCODE_EXT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/editors/vscode")
    set(VSCODE_EXT_STAMP "${CMAKE_CURRENT_BINARY_DIR}/vscode_ext_npm_install.stamp")
    add_custom_command(
        OUTPUT "${VSCODE_EXT_STAMP}"
        COMMAND "${NPM_EXECUTABLE}" install
        COMMAND "${CMAKE_COMMAND}" -E touch "${VSCODE_EXT_STAMP}"
        WORKING_DIRECTORY "${VSCODE_EXT_DIR}"
        DEPENDS "${VSCODE_EXT_DIR}/package.json" "${VSCODE_EXT_DIR}/package-lock.json"
        COMMENT "Installing VS Code extension dependencies..."
    )
    add_custom_target(vscode_extension ALL DEPENDS "${VSCODE_EXT_STAMP}")
endif()


