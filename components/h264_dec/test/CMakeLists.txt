cmake_minimum_required(VERSION 3.16)
project(h264_dec_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB DECODER_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../src/*.c)
add_executable(h264_dec_test h264_dec_test.c ${DECODER_SRCS})
target_include_directories(h264_dec_test PRIVATE ../include ../src)
target_compile_options(h264_dec_test PRIVATE -Wall -Wextra -O2 -g)

option(H264_SANITIZE "Build with UBSan" OFF)
if(H264_SANITIZE)
    target_compile_options(h264_dec_test PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(h264_dec_test PRIVATE -fsanitize=undefined)
endif()
