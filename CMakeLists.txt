cmake_minimum_required(VERSION 3.20)
project(factory_sim VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(FACTORY_BUILD_TESTS "Build the test suite" ON)

# ---- dependencies (downloaded at configure time, no git required) -----------------
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# cpp-httplib is a single header; SOURCE_SUBDIR points at a non-existent folder so its own
# CMakeLists (which probes for OpenSSL, zlib, ...) is never run.
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
  SOURCE_SUBDIR _no_cmake_
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(httplib)

if(FACTORY_BUILD_TESTS)
  # header-only use: skip doctest's own CMakeLists (it predates CMake 4)
  FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    SOURCE_SUBDIR _no_cmake_
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(doctest)
  add_library(doctest INTERFACE)
  target_include_directories(doctest SYSTEM INTERFACE ${doctest_SOURCE_DIR})
endif()

# ---- warnings ---------------------------------------------------------------------
function(factory_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
endfunction()

# ---- core library (engine, nodes, loader, analytics) ------------------------------
add_library(factory_core STATIC
  src/distribution.cpp
  src/event_engine.cpp
  src/node.cpp
  src/plant.cpp
  src/nodes.cpp
  src/params.cpp
  src/registry.cpp
  src/custom_nodes.cpp
  src/config_loader.cpp
  src/analytics.cpp
  src/pdf.cpp
  src/audit.cpp
  src/audit_pdf.cpp)
target_include_directories(factory_core PUBLIC include)
target_link_libraries(factory_core PUBLIC nlohmann_json::nlohmann_json)
factory_warnings(factory_core)
find_package(Threads REQUIRED)
target_link_libraries(factory_core PUBLIC Threads::Threads)

# ---- executable -------------------------------------------------------------------
add_executable(factory_sim src/main.cpp src/server.cpp)
target_include_directories(factory_sim SYSTEM PRIVATE ${httplib_SOURCE_DIR})
if(WIN32)
  target_link_libraries(factory_sim PRIVATE ws2_32)
endif()
target_link_libraries(factory_sim PRIVATE factory_core)
factory_warnings(factory_sim)

# Studio assets and examples travel next to the binary (refreshed on every build).
add_custom_target(factory_assets ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/web $<TARGET_FILE_DIR:factory_sim>/web
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/examples $<TARGET_FILE_DIR:factory_sim>/examples)
add_dependencies(factory_assets factory_sim)

# ---- tests ------------------------------------------------------------------------
if(FACTORY_BUILD_TESTS)
  enable_testing()
  add_executable(factory_tests
    tests/test_main.cpp
    tests/test_engine.cpp
    tests/test_blocking_logic.cpp
    tests/test_failures.cpp
    tests/test_nodes.cpp
    tests/test_config.cpp
    tests/test_analytics.cpp
    tests/test_audit.cpp)
  target_link_libraries(factory_tests PRIVATE factory_core doctest)
  target_compile_definitions(factory_tests PRIVATE FACTORY_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
  factory_warnings(factory_tests)
  add_test(NAME factory_tests COMMAND factory_tests)
endif()
