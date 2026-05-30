cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(distorm)

project(NtHookEngine)

add_library(NtHookEngine STATIC
    NtHookEngine.cpp
    NtHookEngine.h
    stdafx.h)

target_include_directories(NtHookEngine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(NtHookEngine PRIVATE
    distorm)
