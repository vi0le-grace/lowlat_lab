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

add_library(spsc INTERFACE)
target_include_directories(spsc INTERFACE include)

add_executable(test_queues tests/test_queues.cpp)
target_link_libraries(test_queues PRIVATE spsc Threads::Threads)

target_compile_options(test_queues PRIVATE -Wall -Wextra -UNDEBUG)