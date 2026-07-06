cmake_minimum_required(VERSION 3.10)

project(FCBQP_parallel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenMP REQUIRED)

add_executable(FCBQP_parallel
    main.cpp
)

target_compile_options(FCBQP_parallel PRIVATE
    -O3
)

target_link_libraries(FCBQP_parallel PRIVATE
    OpenMP::OpenMP_CXX
)
