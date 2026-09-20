# ESP-IDF's requirement scan includes this file in script mode, where CONFIGURE_DEPENDS is an error.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(APP_GLOB_FLAGS CONFIGURE_DEPENDS)
endif()

file(GLOB_RECURSE APP_SRCS ${APP_GLOB_FLAGS}
     "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

set(APP_REQUIRES bsp ui_framework media_buffer avi_demux mkv_demux mp4_demux vdec_common h264_dec mpeg2_dec usb_msc jpeg_decode_enhanced esp_timer)
if(ESP_PLATFORM)
    list(APPEND APP_REQUIRES espressif__esp_audio_codec esp_mm esp_driver_ppa esp_driver_dma hal nvs_flash)
endif()

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${APP_REQUIRES})

file(GLOB APP_RESOURCES ${APP_GLOB_FLAGS} "${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.json")
if(APP_RESOURCES AND NOT CMAKE_SCRIPT_MODE_FILE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../tools/resgen/resgen.cmake")
    resgen_add_resources(TARGET ${COMPONENT_LIB} DEFINITION ${APP_RESOURCES})
endif()

if(ESP_PLATFORM AND H264_BENCH_CLIP)
    configure_file("${H264_BENCH_CLIP}" "${CMAKE_CURRENT_BINARY_DIR}/bench.h264" COPYONLY)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/bench.h264" BINARY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE H264_BENCH)
    idf_component_get_property(harness_lib harness COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${harness_lib})
endif()

if(ESP_PLATFORM AND MPEG2_BENCH_CLIP)
    configure_file("${MPEG2_BENCH_CLIP}" "${CMAKE_CURRENT_BINARY_DIR}/bench.m2v" COPYONLY)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/bench.m2v" BINARY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MPEG2_BENCH)
    idf_component_get_property(harness_lib harness COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${harness_lib})
endif()
