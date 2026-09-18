idf_component_register(
    SRCS
        "src/decoder.c"
        "src/headers.c"
        "src/idct.c"
        "src/mc.c"
        "src/slice.c"
        "src/tables.c"
        "src/vlc.c"
    INCLUDE_DIRS "include"
    REQUIRES vdec_common)

if(IDF_TARGET STREQUAL "esp32p4")
    target_sources(${COMPONENT_LIB} PRIVATE "src/idct_p4.S")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MPEG2_DEC_PIE)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

if(MPEG2_DEC_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC MPEG2_DEC_PROFILE)
endif()
