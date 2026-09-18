idf_component_register(
    SRCS
        "src/bits.c"
        "src/cabac.c"
        "src/cabac_tables.c"
        "src/deblock.c"
        "src/decoder.c"
        "src/inter.c"
        "src/intra.c"
        "src/kernels_c.c"
        "src/macroblock.c"
        "src/params.c"
        "src/rows.c"
        "src/slice.c"
        "src/tables.c"
        "src/transform.c"
    INCLUDE_DIRS "include"
    REQUIRES vdec_common)

if(IDF_TARGET STREQUAL "esp32p4")
    target_sources(${COMPONENT_LIB} PRIVATE "src/kernels_p4.S")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE H264_DEC_PIE)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

if(H264_DEC_PROFILE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC H264_DEC_PROFILE)
endif()
