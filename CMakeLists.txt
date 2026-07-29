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
)
target_include_directories(mlsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

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
)
target_link_libraries(mlsm_test PRIVATE mlsm GTest::gtest_main)
gtest_discover_tests(mlsm_test)