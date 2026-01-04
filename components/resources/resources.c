#include "resources.h"

// icons
extern const lv_image_dsc_t play_circle_80dp_FFFFFF_FILL0_wght200_GRAD0_opsz48;
extern const lv_image_dsc_t pause_circle_80dp_FFFFFF_FILL0_wght200_GRAD0_opsz48;
extern const lv_image_dsc_t volume_off_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24;
extern const lv_image_dsc_t volume_mute_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24;
extern const lv_image_dsc_t volume_down_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24;
extern const lv_image_dsc_t volume_up_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24;
extern const lv_image_dsc_t brightness_5_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24;
extern const lv_image_dsc_t brightness_6_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24;
extern const lv_image_dsc_t brightness_7_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24;

const struct Resources R = {
    .icon = {
        .play_circle = &play_circle_80dp_FFFFFF_FILL0_wght200_GRAD0_opsz48,
        .pause_circle = &pause_circle_80dp_FFFFFF_FILL0_wght200_GRAD0_opsz48,
        .volume_off = &volume_off_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24,
        .volume_mute = &volume_mute_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24,
        .volume_down = &volume_down_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24,
        .volume_up = &volume_up_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24,
        .brightness_low = &brightness_5_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24,
        .brightness_mid = &brightness_6_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24,
        .brightness_high = &brightness_7_30dp_FFFFFF_FILL0_wght300_GRAD0_opsz24,
    },
};
