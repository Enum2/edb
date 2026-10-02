cmake_minimum_required(VERSION 3.16)
project(AHSE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# --- Locate dependencies (Homebrew on macOS, system paths on Linux) ---
# Allow override via -DDEP_PREFIX=/path. Defaults cover Apple Silicon Homebrew.
if(NOT DEFINED DEP_PREFIX)
  if(EXISTS "/opt/homebrew")
    set(DEP_PREFIX "/opt/homebrew")
  elseif(EXISTS "/usr/local")
    set(DEP_PREFIX "/usr/local")
  else()
    set(DEP_PREFIX "/usr")
  endif()
endif()
message(STATUS "AHSE dependency prefix: ${DEP_PREFIX}")

find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h HINTS "${DEP_PREFIX}/include")
find_library(ROCKSDB_LIB rocksdb HINTS "${DEP_PREFIX}/lib")
find_path(LMDB_INCLUDE_DIR lmdb.h HINTS "${DEP_PREFIX}/include")
find_library(LMDB_LIB lmdb HINTS "${DEP_PREFIX}/lib")
find_path(ABSL_INCLUDE_DIR absl/container/btree_map.h HINTS "${DEP_PREFIX}/include")

if(NOT ROCKSDB_INCLUDE_DIR OR NOT ROCKSDB_LIB)
  message(FATAL_ERROR "RocksDB not found under ${DEP_PREFIX}. Install it or pass -DDEP_PREFIX=...")
endif()
if(NOT LMDB_INCLUDE_DIR OR NOT LMDB_LIB)
  message(FATAL_ERROR "LMDB not found under ${DEP_PREFIX}. Install it or pass -DDEP_PREFIX=...")
endif()
if(NOT ABSL_INCLUDE_DIR)
  message(FATAL_ERROR "Abseil headers not found under ${DEP_PREFIX}.")
endif()

find_package(Threads REQUIRED)

function(ahse_target name src)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${ROCKSDB_INCLUDE_DIR}
    ${LMDB_INCLUDE_DIR}
    ${ABSL_INCLUDE_DIR})
  target_link_libraries(${name} PRIVATE
    ${ROCKSDB_LIB} ${LMDB_LIB} Threads::Threads)
endfunction()

ahse_target(ahse_bench       src/bench/main.cpp)
ahse_target(exp1_workload_a  src/bench/exp1_workload_a.cpp)
ahse_target(htap             src/bench/htap.cpp)
ahse_target(exp4_concurrency src/bench/exp4_concurrency.cpp)

message(STATUS "AHSE: configured targets (RocksDB=${ROCKSDB_LIB}, LMDB=${LMDB_LIB})")
