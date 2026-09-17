cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_SOURCE_DIR}/../esp-devkit/devkit.cmake)
devkit_idf_init(UI_FRAMEWORK COMPONENT_DIRS ../components/media_buffer ../components/avi_demux ../components/mkv_demux ../components/h264_dec ../app)
project(tab5-media-player)
