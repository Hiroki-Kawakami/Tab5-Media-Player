idf_component_register(
    SRCS "src/riff.c" "src/avi_demux.c" "src/wav_demux.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES media_buffer)
