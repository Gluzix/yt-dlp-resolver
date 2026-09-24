cmake_minimum_required(VERSION 3.14)

project(ytres LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# The same flags for every target: the library, its harness and its tests.
function(ytres_compile_options target)
    if(MSVC)
        # /we4715: a function that can fall off its end without returning is
        # a bug, not a warning. /permissive-: MSVC refuses what Clang and GCC
        # would, since the core has to build on them unchanged later.
        target_compile_options(${target} PRIVATE /W4 /we4715 /utf-8 /permissive-)
    endif()
endfunction()

# Portable C++17 with no platform code. Static, so the consumer has no DLL
# of ours to keep in step beside its exe.
add_library(ytres STATIC
    include/ytres/ytres.h
    include/ytres/http.h
    src/format_pick.cpp
)
target_include_directories(ytres
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
ytres_compile_options(ytres)
