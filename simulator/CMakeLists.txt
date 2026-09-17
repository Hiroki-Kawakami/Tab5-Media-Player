cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_SOURCE_DIR}/../esp-devkit/devkit.cmake)
devkit_simulator_init()
project(simulator C CXX)
devkit_simulator(COMPONENT_DIRS ../components/media_buffer ../components/avi_demux ../components/mkv_demux ../components/mp4_demux ../components/h264_dec ../app)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
target_link_libraries(simulator PkgConfig::FFMPEG)
