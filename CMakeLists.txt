# ══════════════════════════════════════════════════════════════════════════════
# FCE — Flat Code Engine
#
# Standalone build:
#   cmake -B build && cmake --build build --config Release
#
# As a subdirectory:
#   add_subdirectory(fce)
#   target_link_libraries(my_app PRIVATE flat-code-engine)
# ══════════════════════════════════════════════════════════════════════════════

# ─── Standalone project setup (skipped when used as subdirectory) ─────────────
# CMAKE_SOURCE_DIR == CMAKE_CURRENT_SOURCE_DIR → top-level (standalone) build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(FlatCodeEngine VERSION 0.1.0 LANGUAGES C CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        add_compile_options(/utf-8)
    endif()

    set(BUILD_SHARED_LIBS OFF)
endif()

# Required for Linux: static libs linked into shared .so need -fPIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif()
set(CMAKE_VERBOSE_MAKEFILE ON)

include(FetchContent)

# ─── Dependency: tree-sitter ──────────────────────────────────────────────────
# Skip if parent has already Populated
if(NOT ts-core_POPULATED)
    FetchContent_Declare(ts-core
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
        GIT_TAG        v0.26.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-core)
    if(NOT ts-core_POPULATED)
        FetchContent_Populate(ts-core)
    endif()
endif()

if(NOT ts-cpp_POPULATED)
    FetchContent_Declare(ts-cpp
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
        GIT_TAG        v0.23.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-cpp)
    if(NOT ts-cpp_POPULATED)
        FetchContent_Populate(ts-cpp)
    endif()
endif()

if(NOT ts-c_POPULATED)
    FetchContent_Declare(ts-c
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
        GIT_TAG        v0.23.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-c)
    if(NOT ts-c_POPULATED)
        FetchContent_Populate(ts-c)
    endif()
endif()

if(NOT ts-python_POPULATED)
    FetchContent_Declare(ts-python
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
        GIT_TAG        v0.25.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-python)
    if(NOT ts-python_POPULATED)
        FetchContent_Populate(ts-python)
    endif()
endif()

if(NOT ts-java_POPULATED)
    FetchContent_Declare(ts-java
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-java.git
        GIT_TAG        v0.23.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-java)
    if(NOT ts-java_POPULATED)
        FetchContent_Populate(ts-java)
    endif()
endif()

if(NOT ts-javascript_POPULATED)
    FetchContent_Declare(ts-javascript
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
        GIT_TAG        v0.23.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-javascript)
    if(NOT ts-javascript_POPULATED)
        FetchContent_Populate(ts-javascript)
    endif()
endif()

if(NOT ts-typescript_POPULATED)
    FetchContent_Declare(ts-typescript
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
        GIT_TAG        v0.23.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-typescript)
    if(NOT ts-typescript_POPULATED)
        FetchContent_Populate(ts-typescript)
    endif()
endif()

if(NOT ts-rust_POPULATED)
    FetchContent_Declare(ts-rust
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-rust.git
        GIT_TAG        v0.24.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-rust)
    if(NOT ts-rust_POPULATED)
        FetchContent_Populate(ts-rust)
    endif()
endif()

if(NOT ts-go_POPULATED)
    FetchContent_Declare(ts-go
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-go.git
        GIT_TAG        v0.23.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-go)
    if(NOT ts-go_POPULATED)
        FetchContent_Populate(ts-go)
    endif()
endif()

if(NOT ts-csharp_POPULATED)
    FetchContent_Declare(ts-csharp
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c-sharp.git
        GIT_TAG        v0.23.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(ts-csharp)
    if(NOT ts-csharp_POPULATED)
        FetchContent_Populate(ts-csharp)
    endif()
endif()

# ─── Dependency: nlohmann/json ────────────────────────────────────────────────
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ─── Dependency: Threads ─────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ─── tree-sitter core ─────────────────────────────────────────────────────────
if(NOT TARGET tree-sitter)
    add_library(tree-sitter STATIC ${ts-core_SOURCE_DIR}/lib/src/lib.c)
    target_include_directories(tree-sitter PUBLIC ${ts-core_SOURCE_DIR}/lib/include)
    set_target_properties(tree-sitter PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(tree-sitter PRIVATE /W0)
    else()
        target_compile_options(tree-sitter PRIVATE -fPIC)
    endif()
endif()

# ─── ts-grammars (language grammar bundle) ───────────────────────────────────
if(NOT TARGET ts-grammars)
    add_library(ts-grammars STATIC
        # C / C++ / Python (original)
        ${ts-cpp_SOURCE_DIR}/src/parser.c
        ${ts-cpp_SOURCE_DIR}/src/scanner.c
        ${ts-c_SOURCE_DIR}/src/parser.c
        ${ts-python_SOURCE_DIR}/src/parser.c
        ${ts-python_SOURCE_DIR}/src/scanner.c
        # Java
        ${ts-java_SOURCE_DIR}/src/parser.c
        # JavaScript
        ${ts-javascript_SOURCE_DIR}/src/parser.c
        ${ts-javascript_SOURCE_DIR}/src/scanner.c
        # TypeScript (TS + TSX — separate parsers)
        ${ts-typescript_SOURCE_DIR}/typescript/src/parser.c
        ${ts-typescript_SOURCE_DIR}/typescript/src/scanner.c
        ${ts-typescript_SOURCE_DIR}/tsx/src/parser.c
        ${ts-typescript_SOURCE_DIR}/tsx/src/scanner.c
        # Rust
        ${ts-rust_SOURCE_DIR}/src/parser.c
        ${ts-rust_SOURCE_DIR}/src/scanner.c
        # Go
        ${ts-go_SOURCE_DIR}/src/parser.c
        # C#
        ${ts-csharp_SOURCE_DIR}/src/parser.c
        ${ts-csharp_SOURCE_DIR}/src/scanner.c
    )
    target_include_directories(ts-grammars PUBLIC
        ${ts-core_SOURCE_DIR}/lib/include
        ${ts-cpp_SOURCE_DIR}/src
        ${ts-c_SOURCE_DIR}/src
        ${ts-python_SOURCE_DIR}/src
        ${ts-java_SOURCE_DIR}/src
        ${ts-javascript_SOURCE_DIR}/src
        ${ts-typescript_SOURCE_DIR}/typescript/src
        ${ts-typescript_SOURCE_DIR}/tsx/src
        ${ts-rust_SOURCE_DIR}/src
        ${ts-go_SOURCE_DIR}/src
        ${ts-csharp_SOURCE_DIR}/src
    )
    set_target_properties(ts-grammars PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(ts-grammars PRIVATE /W0)
    else()
        target_compile_options(ts-grammars PRIVATE -fPIC)
    endif()
endif()

# ─── FCE static library ──────────────────────────────────────────────────────
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    "Baked/*.cpp"
    "Core/*.cpp"
    "Symbol/*.cpp"
)

file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
    "Baked/*.hpp"
    "Core/*.hpp"
    "Symbol/*.hpp"
)

add_library(flat-code-engine STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS})
set_target_properties(flat-code-engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(flat-code-engine PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(flat-code-engine PRIVATE -Wall -Wextra -Wpedantic -fPIC)
endif()
target_include_directories(flat-code-engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ts-core_SOURCE_DIR}/lib/include
)
target_link_libraries(flat-code-engine
    PRIVATE tree-sitter ts-grammars
            nlohmann_json::nlohmann_json
            Threads::Threads
)

# VS Solution Explorer folder structure display
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_SOURCES} ${ENGINE_HEADERS})

# ─── pybind11 bindings (.pyd / .so) ──────────────────────────────────────────
option(FCE_BUILD_BINDINGS "Build pybind11 Python bindings" OFF)
if(FCE_BUILD_BINDINGS)
    FetchContent_Declare(pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.13.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(pybind11)

    pybind11_add_module(fce NO_EXTRAS bindings/fce_module.cpp)
    target_link_libraries(fce PRIVATE flat-code-engine)
    set_target_properties(fce PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bindings"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bindings"
        INTERPROCEDURAL_OPTIMIZATION OFF
    )
    install(TARGETS fce LIBRARY DESTINATION .)
endif()

# ─── Unit tests ──────────────────────────────────────────────────────────────
option(FCE_BUILD_TESTS "Build unit tests" OFF)
if(FCE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
