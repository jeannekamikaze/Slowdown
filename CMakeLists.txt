cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Multi-threaded statically-linked runtime library (-MT)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(NtHookEngine)

project(slowdown)

add_executable(slowdown
    slowdown_common.h
    slowdown.cc)

add_library(slowdown_hook SHARED
    slowdown_common.h
    slowdown_hook.cc)

target_link_libraries(slowdown_hook PRIVATE
    Ddraw
    NtHookEngine)

# Demo

add_executable(hello WIN32
    demo/hello.cc)

add_executable(game WIN32
    demo/game.cc)
