cmake_minimum_required(VERSION 3.10)

project(FCBQPParallel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(FCBQP_parallel main.cpp)

find_package(OpenMP)
if(OpenMP_CXX_FOUND)
   target_link_libraries(FCBQP_parallel PRIVATE OpenMP::OpenMP_CXX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
   target_compile_options(FCBQP_parallel PRIVATE -fopenmp)
   target_link_libraries(FCBQP_parallel PRIVATE -fopenmp)
else()
   message(FATAL_ERROR "OpenMP was not found. Please install an OpenMP-capable C++ compiler.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
   target_link_libraries(FCBQP_parallel PRIVATE stdc++fs)
endif()

add_custom_target(run
   COMMAND $<TARGET_FILE:FCBQP_parallel>
   DEPENDS FCBQP_parallel
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
