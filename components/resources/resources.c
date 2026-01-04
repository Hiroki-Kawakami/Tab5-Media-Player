#include "resources.h"

// icons
extern const lv_image_dsc_t play_circle_80dp_FFFFFF_FILL0_wght200_GRAD0_opsz48;
extern const lv_image_dsc_t pause_circle_80dp_FFFFFF_FILL0_wght200_GRAD0_opsz48;

const struct Resources R = {
    .icon = {
        .play_circle = &play_circle_80dp_FFFFFF_FILL0_wght200_GRAD0_opsz48,
        .pause_circle = &pause_circle_80dp_FFFFFF_FILL0_wght200_GRAD0_opsz48,
    },
};
