cmake_minimum_required(VERSION 3.16)
project(mpeg2_dec_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB DECODER_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../src/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/../../vdec_common/src/*.c)
add_executable(mpeg2_dec_test mpeg2_dec_test.c ${DECODER_SRCS})
target_include_directories(mpeg2_dec_test PRIVATE ../include ../src ../../vdec_common/include)
target_compile_options(mpeg2_dec_test PRIVATE -Wall -Wextra -O2 -g)

option(MPEG2_SANITIZE "Build with UBSan" OFF)
if(MPEG2_SANITIZE)
    target_compile_options(mpeg2_dec_test PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(mpeg2_dec_test PRIVATE -fsanitize=undefined)
endif()
