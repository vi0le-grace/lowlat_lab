cmake_minimum_required(VERSION 3.20)
project(spsc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

find_package(Threads REQUIRED)

set(SPSC_SANITIZER "" CACHE STRING "Sanitizer to enable: thread, address, or empty")
set_property(CACHE SPSC_SANITIZER PROPERTY STRINGS "" thread address)

if(SPSC_SANITIZER AND NOT SPSC_SANITIZER MATCHES "^(thread|address)$")
  message(FATAL_ERROR "SPSC_SANITIZER must be 'thread' or 'address'")
endif()

if(SPSC_SANITIZER)
  add_compile_options(-fsanitize=${SPSC_SANITIZER} -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=${SPSC_SANITIZER})
  add_compile_definitions(SPSC_STRESS_N=200000)
endif()

add_library(spsc INTERFACE)
target_include_directories(spsc INTERFACE include)

enable_testing()

function(add_spsc_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE spsc Threads::Threads)
  target_compile_options(${name} PRIVATE -Wall -Wextra -UNDEBUG)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

add_spsc_test(test_queues tests/test_queues.cpp)
add_spsc_test(test_mutex_condvar_queue tests/test_mutex_condvar_queue.cpp)

find_package(benchmark REQUIRED)

add_executable(bench_spsc bench/bench_spsc.cpp)
target_link_libraries(bench_spsc PRIVATE spsc Threads::Threads benchmark::benchmark)
target_compile_options(bench_spsc PRIVATE -Wall -Wextra)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(WARNING "Benchmarks should be built with -DCMAKE_BUILD_TYPE=Release")
endif()