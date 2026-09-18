idf_component_register(
    SRCS "src/kernels_c.c"
    INCLUDE_DIRS "include")

if(IDF_TARGET STREQUAL "esp32p4")
    target_sources(${COMPONENT_LIB} PRIVATE "src/kernels_p4.S")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VDEC_PIE)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
