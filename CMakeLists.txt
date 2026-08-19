cmake_minimum_required(VERSION 3.16)

project(FCBQP_parallel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(main
    main.cpp
)

option(FCBQP_ENABLE_LTO "Enable interprocedural/link-time optimization" OFF)
option(FCBQP_NATIVE_ARCH "Tune the executable for the build machine" OFF)
option(FCBQP_PGO_GENERATE "Build an instrumented profile-generation executable" OFF)
option(FCBQP_PGO_USE "Build using a previously collected profile" OFF)
option(FCBQP_ENABLE_PHASE_PROFILING "Collect detailed search phase timings" OFF)
set(FCBQP_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH
    "GCC profile directory or directory containing the Clang merged.profdata file")

if(FCBQP_PGO_GENERATE AND FCBQP_PGO_USE)
    message(FATAL_ERROR "FCBQP_PGO_GENERATE and FCBQP_PGO_USE are mutually exclusive")
endif()

if(FCBQP_ENABLE_PHASE_PROFILING)
    target_compile_definitions(main PRIVATE FCBQP_ENABLE_PHASE_PROFILING=1)
endif()

if(FCBQP_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT FCBQP_IPO_SUPPORTED OUTPUT FCBQP_IPO_ERROR)
    if(FCBQP_IPO_SUPPORTED)
        set_property(TARGET main PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(FATAL_ERROR "LTO was requested but is unavailable: ${FCBQP_IPO_ERROR}")
    endif()
endif()

if(FCBQP_NATIVE_ARCH)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(main PRIVATE -march=native)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(APPLE)
            target_compile_options(main PRIVATE -mcpu=native)
        else()
            target_compile_options(main PRIVATE -march=native)
        endif()
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(FCBQP_PGO_GENERATE)
        target_compile_options(main PRIVATE "-fprofile-generate=${FCBQP_PGO_DIR}")
        target_link_options(main PRIVATE "-fprofile-generate=${FCBQP_PGO_DIR}")
    elseif(FCBQP_PGO_USE)
        target_compile_options(main PRIVATE
            "-fprofile-use=${FCBQP_PGO_DIR}" -fprofile-correction -Wno-missing-profile)
        target_link_options(main PRIVATE "-fprofile-use=${FCBQP_PGO_DIR}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(FCBQP_PGO_GENERATE)
        target_compile_options(main PRIVATE -fprofile-instr-generate -Wno-pass-failed)
        target_link_options(main PRIVATE -fprofile-instr-generate -Wno-pass-failed)
    elseif(FCBQP_PGO_USE)
        target_compile_options(main PRIVATE
            "-fprofile-instr-use=${FCBQP_PGO_DIR}/merged.profdata")
        target_link_options(main PRIVATE
            "-fprofile-instr-use=${FCBQP_PGO_DIR}/merged.profdata")
    endif()
endif()


if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(main PRIVATE -ffp-contract=off)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-fexperimental-library" FCBQP_HAS_EXPERIMENTAL_LIBRARY)
    if(FCBQP_HAS_EXPERIMENTAL_LIBRARY)
        target_compile_options(main PRIVATE -fexperimental-library)
        target_link_options(main PRIVATE -fexperimental-library)
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(main PRIVATE Threads::Threads)


if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    find_package(TBB CONFIG REQUIRED)
    target_link_libraries(main PRIVATE TBB::tbb)
    message(STATUS "Parallel STL backend: TBB")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME solver_smoke
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.py $<TARGET_FILE:main>)
    endif()
endif()
