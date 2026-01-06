#pragma once
#include "avi_demuxer.h"

// FourCC Definitions
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
typedef enum {
    FOURCC_RIFF = FOURCC('R', 'I', 'F', 'F'),
    FOURCC_AVI  = FOURCC('A', 'V', 'I', ' '),
    FOURCC_LIST = FOURCC('L', 'I', 'S', 'T'),
    FOURCC_avih = FOURCC('a', 'v', 'i', 'h'),
    FOURCC_strh = FOURCC('s', 't', 'r', 'h'),
    FOURCC_strf = FOURCC('s', 't', 'r', 'f'),
    FOURCC_hdrl = FOURCC('h', 'd', 'r', 'l'),
    FOURCC_strl = FOURCC('s', 't', 'r', 'l'),
    FOURCC_movi = FOURCC('m', 'o', 'v', 'i'),
    FOURCC_idx1 = FOURCC('i', 'd', 'x', '1'),
    FOURCC_vids = FOURCC('v', 'i', 'd', 's'),
    FOURCC_auds = FOURCC('a', 'u', 'd', 's'),
    FOURCC_00db = FOURCC('0', '0', 'd', 'b'),
    FOURCC_00dc = FOURCC('0', '0', 'd', 'c'),
    FOURCC_01wb = FOURCC('0', '1', 'w', 'b'),
    FOURCC_JUNK = FOURCC('J', 'U', 'N', 'K'),
    FOURCC_MJPG = FOURCC('M', 'J', 'P', 'G'),
    FOURCC_mjpg = FOURCC('m', 'j', 'p', 'g'),
} fourcc_t;

// AVI file structures
typedef struct {
    fourcc_t fourcc;
    uint32_t size;
} chunk_header_t;

typedef struct {
    uint32_t micro_sec_per_frame;
    uint32_t max_bytes_per_sec;
    uint32_t padding_granularity;
    uint32_t flags;
    uint32_t total_frames;
    uint32_t initial_frames;
    uint32_t streams;
    uint32_t suggested_buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t reserved[4];
} __attribute__((packed)) avi_main_header_t;

typedef struct {
    fourcc_t fourcc_type;
    fourcc_t fourcc_handler;
    uint32_t flags;
    uint16_t priority;
    uint16_t language;
    uint32_t initial_frames;
    uint32_t scale;
    uint32_t rate;
    uint32_t start;
    uint32_t length;
    uint32_t suggested_buffer_size;
    uint32_t quality;
    uint32_t sample_size;
    struct {
        int16_t left;
        int16_t top;
        int16_t right;
        int16_t bottom;
    } frame;
} __attribute__((packed)) avi_stream_header_t;

typedef struct {
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t compression;
    uint32_t size_image;
    uint32_t x_pels_per_meter;
    uint32_t y_pels_per_meter;
    uint32_t clr_used;
    uint32_t clr_important;
} __attribute__((packed)) bitmap_info_header_t;

typedef struct {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t samples_per_sec;
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t size;
} __attribute__((packed)) wave_format_ex_t;

// idx1 index entry structure
typedef struct {
    fourcc_t chunk_id;    // '00dc', '00db', '01wb' etc
    uint32_t flags;       // Flags (keyframe, etc)
    uint32_t offset;      // Offset from movi start
    uint32_t size;        // Chunk size
} __attribute__((packed)) avi_index_entry_t;

// Maximum number of index entries to keep in memory
#ifndef AVI_DMUX_MAX_INDEX_ENTRIES
#define AVI_DMUX_MAX_INDEX_ENTRIES 36000
#endif

inline static avi_dmux_video_codec_t fourcc_to_video_codec(fourcc_t fourcc) {
    switch (fourcc) {
        case FOURCC_MJPG:
        case FOURCC_mjpg:
            return AVI_DMUX_VIDEO_CODEC_MJPEG;
        default:
            return AVI_DMUX_VIDEO_CODEC_UNKNOWN;
    }
}

inline static avi_dmux_audio_codec_t format_tag_to_audio_codec(uint16_t format_tag) {
    switch (format_tag) {
        case 0x0001: // PCM
            return AVI_DMUX_AUDIO_CODEC_PCM;
        case 0x0055: // MP3
            return AVI_DMUX_AUDIO_CODEC_MP3;
        default:
            return AVI_DMUX_AUDIO_CODEC_UNKNOWN;
    }
}

inline static const char* video_codec_name(avi_dmux_video_codec_t codec) {
    switch (codec) {
        case AVI_DMUX_VIDEO_CODEC_MJPEG: return "MJPEG";
        case AVI_DMUX_VIDEO_CODEC_UNKNOWN: return "Unknown";
        default: return "Invalid";
    }
}

inline static const char* audio_codec_name(avi_dmux_audio_codec_t codec) {
    switch (codec) {
        case AVI_DMUX_AUDIO_CODEC_PCM: return "PCM";
        case AVI_DMUX_AUDIO_CODEC_MP3: return "MP3";
        case AVI_DMUX_AUDIO_CODEC_UNKNOWN: return "Unknown";
        default: return "Invalid";
    }
}
