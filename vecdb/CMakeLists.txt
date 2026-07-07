cmake_minimum_required(VERSION 3.20)
project(vecdb VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(VECDB_WARN  -Wall -Wextra -Wpedantic -Wno-unused-parameter)
set(VECDB_OPT   -O3 -march=native -funroll-loops)
set(VECDB_DBG   -O0 -g -fsanitize=address,undefined)

# ── SIMD distance kernels ─────────────────────────────────────────────────────
add_library(vecdb_simd STATIC src/simd/distance.cpp)
target_include_directories(vecdb_simd PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(vecdb_simd PRIVATE
    ${VECDB_WARN} -O3 -mavx2 -mfma -mavx512f -mavx512dq)

# ── WAL (Phase 3) ─────────────────────────────────────────────────────────────
add_library(vecdb_wal STATIC src/wal/wal.cpp)
target_include_directories(vecdb_wal PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(vecdb_wal PRIVATE ${VECDB_WARN})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vecdb_wal PRIVATE ${VECDB_OPT})
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(vecdb_wal PRIVATE ${VECDB_DBG})
    target_link_options(vecdb_wal PUBLIC -fsanitize=address,undefined)
endif()

# ── Core library (HNSW + neighbour store) ─────────────────────────────────────
add_library(vecdb_core STATIC
    src/hnsw/index.cpp
    src/hnsw/neighbour_store.cpp)
target_include_directories(vecdb_core PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(vecdb_core PRIVATE ${VECDB_WARN})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vecdb_core PRIVATE ${VECDB_OPT})
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(vecdb_core PRIVATE ${VECDB_DBG})
    target_link_options(vecdb_core PUBLIC -fsanitize=address,undefined)
endif()
target_link_libraries(vecdb_core PUBLIC vecdb_simd)

# ── Full library (core + WAL) ─────────────────────────────────────────────────
add_library(vecdb INTERFACE)
target_link_libraries(vecdb INTERFACE vecdb_core vecdb_wal)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

function(add_vecdb_test name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests/unit)
    target_compile_options(${name} PRIVATE ${VECDB_WARN} -O2 -march=native)
    target_link_libraries(${name} PRIVATE vecdb)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(${name} PROPERTIES TIMEOUT 180)
endfunction()

add_vecdb_test(test_hnsw        tests/unit/test_hnsw.cpp)
add_vecdb_test(test_distance    tests/unit/test_distance.cpp)
add_vecdb_test(test_wal         tests/unit/test_wal.cpp)
add_vecdb_test(test_crash_recovery tests/integration/test_crash_recovery.cpp)

# ── Benchmark ─────────────────────────────────────────────────────────────────
add_executable(bench_search tests/bench/bench_search.cpp)
target_include_directories(bench_search PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(bench_search PRIVATE ${VECDB_WARN} -O3 -march=native)
target_link_libraries(bench_search PRIVATE vecdb)

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "=================================================")
message(STATUS " vecdb Phase 3 — WAL + Crash Recovery")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ std    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "=================================================")