cmake_minimum_required(VERSION 3.20)
project(kaixa VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

# The build system for Kaixa itself is intentionally shallow. The architecture lives in directories and namespaces;
# Kaixa.toml will describe projects processed by the executable.
add_library(kaixa_core STATIC
    src/kaixa.cpp
    src/foundation/diagnostic.cpp
    src/foundation/filesystem.cpp
    src/foundation/process.cpp
    src/config/value.cpp
    src/config/parser.cpp
    src/config/table_reader.cpp
    src/config/build_configuration.cpp
    src/model/manifest.cpp
    src/model/graph.cpp
    src/workspace/loader.cpp
    src/build/executor.cpp
    src/services/build.cpp
)
target_include_directories(kaixa_core PUBLIC include)
target_include_directories(kaixa_core SYSTEM PRIVATE third_party)
target_compile_definitions(kaixa_core PRIVATE TOML_EXCEPTIONS=0)

add_library(kaixa_plugins STATIC
    plugins/bundle/src/plugins.cpp
    plugins/cmake/src/configuration.cpp
    plugins/cmake/src/resolver.cpp
)
target_include_directories(kaixa_plugins PUBLIC
    plugins/bundle/include
    plugins/cmake/include
)
target_link_libraries(kaixa_plugins PUBLIC kaixa_core)

add_executable(kaixa apps/kaixa/main.cpp)
target_link_libraries(kaixa PRIVATE kaixa_core kaixa_plugins)

enable_testing()

add_library(kaixa_test_support STATIC
    tests/support/test_support.cpp
)
target_include_directories(kaixa_test_support PUBLIC tests/support)

add_executable(kaixa_tests
    tests/support/test_main.cpp
    tests/smoke.cpp
    tests/mvp.cpp
    tests/configurations.cpp
    tests/workspace_fixtures.cpp
    tests/workspace_errors.cpp
)
target_compile_definitions(kaixa_tests PRIVATE KAIXA_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(kaixa_tests PRIVATE
    kaixa_core
    kaixa_plugins
    kaixa_test_support
)
add_test(NAME kaixa COMMAND kaixa_tests)
