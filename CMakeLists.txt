cmake_minimum_required(VERSION 3.16)
project(mini_lsm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ---- core library ----
# Sources are added here as you implement them (W1: crc32c, wal_writer, wal_reader ...).
add_library(mlsm
  src/crc32c.cc
  src/wal_writer.cc
  src/wal_reader.cc
  src/env_posix.cc
  src/db.cc
)
target_include_directories(mlsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- raft library ----
# Deterministic Raft core. Depends only on header-only Status/Slice, not on the
# storage engine, so it is its own library to keep the dependency direction
# one-way (a future integration layer will depend on both).
add_library(mlsm_raft
  src/raft/raft.cc
)
target_include_directories(mlsm_raft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- raft persistence (integration layer) ----
# The bridge between the I/O-free Raft core and the storage engine's WAL/Env, so
# this is the one raft component that links both mlsm and mlsm_raft.
add_library(mlsm_raft_store
  src/raft/storage.cc
)
target_include_directories(mlsm_raft_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mlsm_raft_store PUBLIC mlsm mlsm_raft)

# ---- googletest via FetchContent ----
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
# Windows-only setting, harmless elsewhere
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---- tests ----
enable_testing()
include(GoogleTest)

add_executable(mlsm_test
  test/sanity_test.cc
  test/crc32c_test.cc
  test/wal_writer_test.cc
  test/wal_reader_test.cc
  test/db_test.cc
  test/raft/raft_test.cc
  test/raft/storage_test.cc
)
target_link_libraries(mlsm_test PRIVATE mlsm mlsm_raft mlsm_raft_store GTest::gtest_main)
gtest_discover_tests(mlsm_test)

# ---- benchmark (manual perf tool, not part of ctest) ----
add_executable(db_bench bench/db_bench.cc)
target_link_libraries(db_bench PRIVATE mlsm)