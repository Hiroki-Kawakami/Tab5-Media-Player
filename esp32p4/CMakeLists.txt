cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_SOURCE_DIR}/../esp-devkit/devkit.cmake)
devkit_idf_init(UI_FRAMEWORK COMPONENT_DIRS ../components/media_buffer ../components/riff_demux ../components/es_audio_demux ../components/mkv_demux ../components/mp4_demux ../components/vdec_common ../components/h264_dec ../components/mpeg2_dec ../components/usb_msc ../app)
project(tab5-media-player)
