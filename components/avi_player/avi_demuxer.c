#include "avi_demuxer.h"
#include "avi_structure.h"
#include "buffered_reader.h"
#include <fcntl.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "avi_dmux";
#define LOG_ERROR(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)
#else
#define LOG_ERROR(fmt, ...) printf("\e[31mE: "fmt"\e[m\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) printf("I: "fmt"\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) printf("D: "fmt"\n", ##__VA_ARGS__)
#endif

static void *memory_allocate(size_t size) { return malloc(size); }
static void memory_free(void *ptr) { return free(ptr); }

typedef struct avi_dmux {
    buffered_reader_t *reader;
    avi_dmux_info_t *info;
    uint32_t video_frame_count;
} avi_dmux_t;

avi_dmux_t *avi_dmux_create(const char *file) {
    buffered_reader_t *reader;
    reader = br_open(file);
    if (!reader) {
        LOG_ERROR("Failed to open file: %s", file);
        return NULL;
    }
    avi_dmux_t *dmux = memory_allocate(sizeof(avi_dmux_t));
    dmux->reader = reader;
    dmux->info = NULL;
    dmux->video_frame_count = 0;
    return dmux;
}

void avi_dmux_delete(avi_dmux_t *dmux) {
    if (!dmux) {
        return;
    }
    br_close(dmux->reader);
    if (dmux->info) {
        memory_free(dmux->info);
    }
    memory_free(dmux);
}

avi_dmux_info_t *avi_dmux_parse_info(avi_dmux_t *dmux) {
    avi_dmux_info_t *info = memory_allocate(sizeof(avi_dmux_info_t));
    if (!info) {
        LOG_ERROR("Failed to allocate memory for info");
        return NULL;
    }

    // Seek to start
    br_lseek(dmux->reader, 0, SEEK_SET);

    // Read RIFF header
    chunk_header_t riff_header;
    if (br_read(dmux->reader, &riff_header, sizeof(riff_header)) != sizeof(riff_header)) {
        LOG_ERROR("Failed to read RIFF header");
        memory_free(info);
        return NULL;
    }

    if (riff_header.fourcc != FOURCC_RIFF) {
        LOG_ERROR("Invalid RIFF signature");
        memory_free(info);
        return NULL;
    }

    // Read AVI signature
    fourcc_t avi_sig;
    if (br_read(dmux->reader, &avi_sig, sizeof(avi_sig)) != sizeof(avi_sig)) {
        LOG_ERROR("Failed to read AVI signature");
        memory_free(info);
        return NULL;
    }

    if (avi_sig != FOURCC_AVI) {
        LOG_ERROR("Invalid AVI signature");
        memory_free(info);
        return NULL;
    }

    // Parse chunks
    while (1) {
        chunk_header_t chunk;
        off_t chunk_pos = br_lseek(dmux->reader, 0, SEEK_CUR);
        if (br_read(dmux->reader, &chunk, sizeof(chunk)) != sizeof(chunk)) {
            break;
        }

        LOG_DEBUG("Parsing chunk at %lld: fourcc=0x%08x, size=%u", (long long)chunk_pos, chunk.fourcc, (unsigned int)chunk.size);

        if (chunk.fourcc == FOURCC_LIST) {
            fourcc_t list_type;
            off_t list_end = chunk_pos + 8 + chunk.size;
            br_read(dmux->reader, &list_type, sizeof(list_type));
            LOG_DEBUG("  LIST type: 0x%08x, list_end=%lld", list_type, (long long)list_end);

            if (list_type == FOURCC_movi) {
                // Found movie data (LIST movi), save location
                info->movi_location = br_lseek(dmux->reader, 0, SEEK_CUR);
                LOG_DEBUG("Found movi chunk at position %lld", (long long)info->movi_location);
                // Seek to movi data start for frame reading
                br_lseek(dmux->reader, info->movi_location, SEEK_SET);
                break;
            }

            // For hdrl and strl, we still need to parse the contents
            if (list_type == FOURCC_hdrl || list_type == FOURCC_strl) {
                // Parse sub-chunks within this LIST
                while (br_lseek(dmux->reader, 0, SEEK_CUR) < list_end) {
                    chunk_header_t sub_chunk;
                    off_t sub_pos = br_lseek(dmux->reader, 0, SEEK_CUR);
                    if (br_read(dmux->reader, &sub_chunk, sizeof(sub_chunk)) != sizeof(sub_chunk)) {
                        break;
                    }

                    LOG_DEBUG("  Sub-chunk at %lld: fourcc=0x%08x, size=%u", (long long)sub_pos, sub_chunk.fourcc, (unsigned int)sub_chunk.size);

                    if (sub_chunk.fourcc == FOURCC_avih) {
                        avi_main_header_t avih;
                        br_read(dmux->reader, &avih, sizeof(avih));
                        info->video.width = avih.width;
                        info->video.height = avih.height;
                        info->video.total_frames = avih.total_frames;
                        info->video.frame_rate = avih.micro_sec_per_frame;
                    } else if (sub_chunk.fourcc == FOURCC_LIST) {
                        // Nested LIST (e.g., LIST strl)
                        fourcc_t nested_list_type;
                        off_t nested_list_end = sub_pos + 8 + sub_chunk.size;
                        br_read(dmux->reader, &nested_list_type, sizeof(nested_list_type));
                        LOG_DEBUG("    Nested LIST type: 0x%08x", nested_list_type);

                        if (nested_list_type == FOURCC_strl) {
                            // Parse strl contents
                            while (br_lseek(dmux->reader, 0, SEEK_CUR) < nested_list_end) {
                                chunk_header_t strl_chunk;
                                if (br_read(dmux->reader, &strl_chunk, sizeof(strl_chunk)) != sizeof(strl_chunk)) {
                                    break;
                                }

                                LOG_DEBUG("      strl chunk: fourcc=0x%08x, size=%u", strl_chunk.fourcc, (unsigned int)strl_chunk.size);

                                if (strl_chunk.fourcc == FOURCC_strh) {
                                    avi_stream_header_t strh;
                                    br_read(dmux->reader, &strh, sizeof(strh));

                                    // Read the strf chunk that follows
                                    chunk_header_t strf_chunk;
                                    br_read(dmux->reader, &strf_chunk, sizeof(strf_chunk));

                                    if (strf_chunk.fourcc == FOURCC_strf) {
                                        if (strh.fourcc_type == FOURCC_vids) {
                                            bitmap_info_header_t bih;
                                            br_read(dmux->reader, &bih, sizeof(bih));
                                            info->video.codec = fourcc_to_video_codec(bih.compression);
                                            info->video.max_frame_size = strh.suggested_buffer_size;
                                            LOG_DEBUG("        Video: codec=0x%08x, max_size=%u", (unsigned int)bih.compression, (unsigned int)strh.suggested_buffer_size);
                                            // Skip remaining bytes if any
                                            if (strf_chunk.size > sizeof(bih)) {
                                                br_lseek(dmux->reader, strf_chunk.size - sizeof(bih), SEEK_CUR);
                                            }
                                        } else if (strh.fourcc_type == FOURCC_auds) {
                                            wave_format_ex_t wfx;
                                            br_read(dmux->reader, &wfx, sizeof(wfx));
                                            info->audio.codec = format_tag_to_audio_codec(wfx.format_tag);
                                            info->audio.channels = wfx.channels;
                                            info->audio.sampling_rate = wfx.samples_per_sec;
                                            info->audio.max_frame_size = strh.suggested_buffer_size;
                                            LOG_DEBUG("        Audio: format=0x%04x, channels=%u, rate=%u, max_size=%u",
                                                   wfx.format_tag, wfx.channels, (unsigned int)wfx.samples_per_sec, (unsigned int)strh.suggested_buffer_size);
                                            // Skip remaining bytes if any
                                            if (strf_chunk.size > sizeof(wfx)) {
                                                br_lseek(dmux->reader, strf_chunk.size - sizeof(wfx), SEEK_CUR);
                                            }
                                        } else {
                                            br_lseek(dmux->reader, strf_chunk.size, SEEK_CUR);
                                        }

                                        // Skip padding for strf
                                        if (strf_chunk.size & 1) {
                                            br_lseek(dmux->reader, 1, SEEK_CUR);
                                        }
                                    }

                                    // Skip padding for strh
                                    if (strl_chunk.size & 1) {
                                        br_lseek(dmux->reader, 1, SEEK_CUR);
                                    }
                                } else {
                                    // Skip unknown chunk in strl
                                    br_lseek(dmux->reader, strl_chunk.size, SEEK_CUR);
                                    if (strl_chunk.size & 1) {
                                        br_lseek(dmux->reader, 1, SEEK_CUR);
                                    }
                                }
                            }
                        }
                        // Seek to end of nested LIST
                        br_lseek(dmux->reader, nested_list_end, SEEK_SET);
                    } else {
                        // Skip unknown sub-chunk
                        br_lseek(dmux->reader, sub_chunk.size, SEEK_CUR);
                        // Skip padding if needed
                        if (sub_chunk.size & 1) {
                            br_lseek(dmux->reader, 1, SEEK_CUR);
                        }
                    }
                }
            }

            // Seek to the end of this LIST chunk
            br_lseek(dmux->reader, list_end, SEEK_SET);
        } else {
            // Skip unknown chunks
            br_lseek(dmux->reader, chunk.size, SEEK_CUR);
        }
    }

    dmux->info = info;

    // Print AVI information
    LOG_INFO("=== AVI File Information ===");
    LOG_INFO("[Video]");
    LOG_INFO("  Codec:       %s", video_codec_name(info->video.codec));
    LOG_INFO("  Resolution:  %ux%u", (unsigned int)info->video.width, (unsigned int)info->video.height);
    LOG_INFO("  Total Frames: %u", (unsigned int)info->video.total_frames);
    LOG_INFO("  Frame Rate:  %u us/frame (%.2f fps)", (unsigned int)info->video.frame_rate, 1000000.0 / info->video.frame_rate);
    LOG_INFO("  Max Frame Size: %u bytes", (unsigned int)info->video.max_frame_size);
    LOG_INFO("[Audio]");
    LOG_INFO("  Codec:       %s", audio_codec_name(info->audio.codec));
    LOG_INFO("  Channels:    %u", info->audio.channels);
    LOG_INFO("  Sample Rate: %u Hz", (unsigned int)info->audio.sampling_rate);
    LOG_INFO("  Bit Depth:   %u bits", info->audio.bits_per_sample);
    LOG_INFO("  Max Frame Size: %u bytes", (unsigned int)info->audio.max_frame_size);

    br_set_preload_enable(dmux->reader, true);
    return info;
}

bool avi_dmux_read_frame(avi_dmux_t *dmux, avi_dmux_frame_t *frame,
                           uint8_t *video_buffer, uint32_t video_buffer_size,
                           uint8_t *audio_buffer, uint32_t audio_buffer_size) {
    if (!dmux || !dmux->info || !frame) {
        LOG_ERROR("Invalid parameters");
        return false;
    }

    chunk_header_t chunk;

    // Read chunks until we find a video or audio frame
    while (true) {
        off_t pos = br_lseek(dmux->reader, 0, SEEK_CUR);
        ssize_t bytes_read = br_read(dmux->reader, &chunk, sizeof(chunk));
        if (bytes_read != sizeof(chunk)) {
            LOG_INFO("End of file or read error at position %lld, bytes_read=%zd", (long long)pos, bytes_read);
            return false;  // End of file or read error
        }

        // Check if this is a video frame (00db or 00dc)
        if (chunk.fourcc == FOURCC_00db || chunk.fourcc == FOURCC_00dc) {
            if (!video_buffer) {
                LOG_ERROR("Video buffer is NULL");
                return false;
            }

            if (chunk.size > video_buffer_size) {
                LOG_ERROR("Buffer too small for video frame: %u > %u", (unsigned int)chunk.size, (unsigned int)video_buffer_size);
                br_lseek(dmux->reader, chunk.size, SEEK_CUR);
                if (chunk.size & 1) br_lseek(dmux->reader, 1, SEEK_CUR);
                continue;
            }

            frame->type = AVI_DMUX_FRAME_TYPE_VIDEO;
            frame->size = chunk.size;
            frame->frame_index = dmux->video_frame_count++;

            if (br_read(dmux->reader, video_buffer, chunk.size) != chunk.size) {
                return false;
            }

            // Skip padding byte if chunk size is odd
            if (chunk.size & 1) {
                br_lseek(dmux->reader, 1, SEEK_CUR);
            }

            return true;
        }
        // Check if this is an audio frame (01wb)
        else if (chunk.fourcc == FOURCC_01wb) {
            if (!audio_buffer) {
                LOG_ERROR("Audio buffer is NULL");
                return false;
            }

            if (chunk.size > audio_buffer_size) {
                LOG_ERROR("Buffer too small for audio frame: %u > %u", (unsigned int)chunk.size, (unsigned int)audio_buffer_size);
                br_lseek(dmux->reader, chunk.size, SEEK_CUR);
                if (chunk.size & 1) br_lseek(dmux->reader, 1, SEEK_CUR);
                continue;
            }

            frame->type = AVI_DMUX_FRAME_TYPE_AUDIO;
            frame->size = chunk.size;
            frame->frame_index = 0;  // Not used for audio

            if (br_read(dmux->reader, audio_buffer, chunk.size) != chunk.size) {
                return false;
            }

            // Skip padding byte if chunk size is odd
            if (chunk.size & 1) {
                br_lseek(dmux->reader, 1, SEEK_CUR);
            }

            return true;
        }
        // Skip unknown chunks
        else {
            br_lseek(dmux->reader, chunk.size, SEEK_CUR);
            // Skip padding byte if chunk size is odd
            if (chunk.size & 1) {
                br_lseek(dmux->reader, 1, SEEK_CUR);
            }
        }
    }
}
