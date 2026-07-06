cmake_minimum_required(VERSION 3.10)

project(FCBQP_parallel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenMP REQUIRED)

add_executable(main
    main.cpp
)

target_compile_options(main PRIVATE
    -O3
)

target_link_libraries(main PRIVATE
    OpenMP::OpenMP_CXX
)
