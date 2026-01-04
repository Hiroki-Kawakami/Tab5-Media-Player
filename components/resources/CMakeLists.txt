file(GLOB R_SRCS "*.c" "converted/*.c")
idf_component_register(SRCS ${R_SRCS}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES lvgl)
