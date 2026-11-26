cmake_minimum_required(VERSION 3.20)
project(lsmkv CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -O3 -g)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(LSMKV_SOURCES
    src/coding.cpp
    src/crc32.cpp
    src/format.cpp
    src/bloom.cpp
    src/wal.cpp
    src/memtable.cpp
    src/sstable_builder.cpp
    src/sstable_reader.cpp
    src/manifest.cpp
    src/compaction.cpp
    src/db.cpp
)

add_library(lsmkv STATIC ${LSMKV_SOURCES})

add_executable(lsmkv_tests
    tests/test_main.cpp
    tests/test_skiplist.cpp
    tests/test_wal.cpp
    tests/test_sstable.cpp
    tests/test_recovery.cpp
    tests/test_compaction.cpp
    tests/test_concurrency.cpp
)
target_link_libraries(lsmkv_tests PRIVATE lsmkv)

add_executable(lsmkv_bench
    benchmarks/bench_main.cpp
)
target_link_libraries(lsmkv_bench PRIVATE lsmkv)
