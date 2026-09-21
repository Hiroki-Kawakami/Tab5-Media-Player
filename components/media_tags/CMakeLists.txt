idf_component_register(
    SRCS "src/media_tags.c" "src/id3.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES media_buffer)
