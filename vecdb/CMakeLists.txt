cmake_minimum_required(VERSION 3.20)
project(vecdb VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(VECDB_WARN -Wall -Wextra -Wpedantic -Wno-unused-parameter)
set(VECDB_OPT  -O3 -march=native -funroll-loops)
set(VECDB_DBG  -O0 -g -fsanitize=address,undefined)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GRPC     REQUIRED grpc++_unsecure)
pkg_check_modules(PROTOBUF REQUIRED protobuf)

set(PROTO_SRCS
    src/proto/vecdb.pb.cc
    src/proto/vecdb.grpc.pb.cc)

add_library(vecdb_proto STATIC ${PROTO_SRCS})
target_include_directories(vecdb_proto PUBLIC
    ${CMAKE_SOURCE_DIR}/src/proto
    ${PROTOBUF_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS})
target_compile_options(vecdb_proto PRIVATE -Wno-unused-parameter)
target_link_libraries(vecdb_proto PUBLIC
    ${GRPC_LIBRARIES}
    ${PROTOBUF_LIBRARIES})

add_library(vecdb_simd STATIC src/simd/distance.cpp)
target_include_directories(vecdb_simd PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(vecdb_simd PRIVATE
    ${VECDB_WARN} -O3 -mavx2 -mfma -mavx512f -mavx512dq)

add_library(vecdb_wal STATIC src/wal/wal.cpp)
target_include_directories(vecdb_wal PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(vecdb_wal PRIVATE ${VECDB_WARN})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vecdb_wal PRIVATE ${VECDB_OPT})
endif()

add_library(vecdb_storage STATIC
    src/storage/mmap_store.cpp
    src/storage/snapshot.cpp)
target_include_directories(vecdb_storage PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(vecdb_storage PRIVATE ${VECDB_WARN})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vecdb_storage PRIVATE ${VECDB_OPT})
endif()

add_library(vecdb_core STATIC
    src/hnsw/index.cpp
    src/hnsw/neighbour_store.cpp)
target_include_directories(vecdb_core PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(vecdb_core PRIVATE ${VECDB_WARN})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vecdb_core PRIVATE ${VECDB_OPT})
endif()
target_link_libraries(vecdb_core PUBLIC vecdb_simd)

add_library(vecdb_shard STATIC
    src/shard/consistent_hash.cpp
    src/shard/shard_manager.cpp)
target_include_directories(vecdb_shard PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(vecdb_shard PRIVATE ${VECDB_WARN})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vecdb_shard PRIVATE ${VECDB_OPT})
endif()
target_link_libraries(vecdb_shard PUBLIC vecdb_core vecdb_wal vecdb_storage)

add_library(vecdb_server STATIC
    src/server/auth_middleware.cpp
    src/server/rate_limiter.cpp
    src/server/grpc_server.cpp
    src/server/http_api.cpp)
target_include_directories(vecdb_server PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${GRPC_INCLUDE_DIRS}
    ${PROTOBUF_INCLUDE_DIRS})
target_compile_options(vecdb_server PRIVATE ${VECDB_WARN})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vecdb_server PRIVATE ${VECDB_OPT})
endif()
target_link_libraries(vecdb_server PUBLIC
    vecdb_shard vecdb_proto
    ${GRPC_LIBRARIES} ${PROTOBUF_LIBRARIES}
    Threads::Threads)

add_library(vecdb_all INTERFACE)
target_link_libraries(vecdb_all INTERFACE
    vecdb_server vecdb_shard vecdb_core vecdb_wal vecdb_storage vecdb_proto Threads::Threads)

add_executable(vecdb_server_bin src/server/main.cpp)
set_target_properties(vecdb_server_bin PROPERTIES OUTPUT_NAME vecdb_server)
target_include_directories(vecdb_server_bin PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(vecdb_server_bin PRIVATE ${VECDB_WARN})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vecdb_server_bin PRIVATE ${VECDB_OPT})
endif()
target_link_libraries(vecdb_server_bin PRIVATE vecdb_all)

enable_testing()

function(add_vecdb_test name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests/unit
        ${GRPC_INCLUDE_DIRS}
        ${PROTOBUF_INCLUDE_DIRS})
    target_compile_options(${name} PRIVATE ${VECDB_WARN} -O2 -march=native)
    target_link_libraries(${name} PRIVATE vecdb_all)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

add_vecdb_test(test_hnsw           tests/unit/test_hnsw.cpp)
add_vecdb_test(test_distance       tests/unit/test_distance.cpp)
add_vecdb_test(test_wal            tests/unit/test_wal.cpp)
add_vecdb_test(test_storage        tests/unit/test_storage.cpp)
add_vecdb_test(test_shard          tests/unit/test_shard.cpp)
add_vecdb_test(test_server         tests/unit/test_server.cpp)
add_vecdb_test(test_crash_recovery tests/integration/test_crash_recovery.cpp)

add_executable(bench_search tests/bench/bench_search.cpp)
target_include_directories(bench_search PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(bench_search PRIVATE ${VECDB_WARN} -O3 -march=native)
target_link_libraries(bench_search PRIVATE vecdb_all)

message(STATUS "=================================================")
message(STATUS " vecdb Phase 7 — gRPC + HTTP API + React Dashboard")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ std    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "=================================================")
