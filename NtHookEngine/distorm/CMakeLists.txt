cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(distorm)

add_library(distorm STATIC
    include/distorm.h
    include/mnemonics.h
    src/config.h
    src/decoder.c
    src/decoder.h
    src/distorm.c
    src/instructions.c
    src/instructions.h
    src/insts.c
    src/insts.h
    src/mnemonics.c
    src/operands.c
    src/operands.h
    src/prefix.c
    src/prefix.h
    src/textdefs.c
    src/textdefs.h
    src/wstring.h
    src/x86defs.h)

target_include_directories(distorm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
