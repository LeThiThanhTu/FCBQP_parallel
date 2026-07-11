cmake_minimum_required(VERSION 3.10)

project(FCBQP_parallel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenMP QUIET)

# Apple Clang does not discover Homebrew's libomp automatically. Provide a
# portable fallback while keeping the standard FindOpenMP path for GCC/Clang.
if(APPLE AND NOT OpenMP_CXX_FOUND AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_path(LIBOMP_INCLUDE_DIR omp.h
        HINTS /opt/homebrew/opt/libomp/include /usr/local/opt/libomp/include)
    find_library(LIBOMP_LIBRARY omp
        HINTS /opt/homebrew/opt/libomp/lib /usr/local/opt/libomp/lib)
    if(LIBOMP_INCLUDE_DIR AND LIBOMP_LIBRARY)
        add_library(OpenMP::OpenMP_CXX INTERFACE IMPORTED)
        target_compile_options(OpenMP::OpenMP_CXX INTERFACE
            -Xpreprocessor -fopenmp)
        target_include_directories(OpenMP::OpenMP_CXX INTERFACE
            ${LIBOMP_INCLUDE_DIR})
        target_link_libraries(OpenMP::OpenMP_CXX INTERFACE
            ${LIBOMP_LIBRARY})
        set(OpenMP_CXX_FOUND TRUE)
    endif()
endif()

if(NOT OpenMP_CXX_FOUND)
    message(FATAL_ERROR "OpenMP C++ support was not found")
endif()

add_executable(main
    main.cpp
)

target_link_libraries(main PRIVATE
    OpenMP::OpenMP_CXX
)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME solver_smoke
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.py $<TARGET_FILE:main>)
    endif()
endif()
