/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "packed_yuv_scaler.hpp"
#include "esp_log.h"

static const char *TAG = "packed_yuv";

static ppa_srm_rotation_angle_t ppa_rotation(bsp_rotation_t rotation) {
    switch (rotation) {
    case BSP_ROTATION_90:  return PPA_SRM_ROTATION_ANGLE_90;
    case BSP_ROTATION_180: return PPA_SRM_ROTATION_ANGLE_180;
    case BSP_ROTATION_270: return PPA_SRM_ROTATION_ANGLE_270;
    default:               return PPA_SRM_ROTATION_ANGLE_0;
    }
}

bool PackedYuvScaler::open(bsp_pixel_format_t format, std::string *error) {
    close();
    switch (format) {
    case BSP_PIXEL_FORMAT_RGB565: color_mode_ = PPA_SRM_COLOR_MODE_RGB565; break;
    case BSP_PIXEL_FORMAT_RGB888: color_mode_ = PPA_SRM_COLOR_MODE_RGB888; break;
    default:
        *error = "unsupported panel pixel format for video";
        return false;
    }
    ppa_client_config_t client = {};
    client.oper_type = PPA_OPERATION_SRM;
    client.max_pending_trans_num = 1;
    const esp_err_t err = ppa_register_client(&client, &ppa_);
    if (err != ESP_OK) {
        ppa_ = nullptr;
        *error = std::string("video scaler unavailable: ") + esp_err_to_name(err);
        return false;
    }
    return true;
}

void PackedYuvScaler::close() {
    if (ppa_) {
        ppa_unregister_client(ppa_);
        ppa_ = nullptr;
    }
}

bool PackedYuvScaler::draw(const PackedYuvImage &image, const RenderTarget &target,
                           std::string *error) {
    ppa_srm_oper_config_t op = {};
    op.in.buffer = image.packed;
    op.in.pic_w = image.coded_width;
    op.in.pic_h = image.coded_height;
    op.in.block_w = image.width;
    op.in.block_h = image.height;
    op.in.block_offset_x = image.crop_left;
    op.in.block_offset_y = image.crop_top;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    op.in.yuv_range = image.full_range ? PPA_COLOR_RANGE_FULL : PPA_COLOR_RANGE_LIMIT;
    op.in.yuv_std = image.bt709 ? PPA_COLOR_CONV_STD_RGB_YUV_BT709
                                : PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.out.buffer = target.framebuffer;
    op.out.buffer_size = target.framebuffer_bytes;
    op.out.pic_w = (uint32_t)target.panel.width;
    op.out.pic_h = (uint32_t)target.panel.height;
    op.out.block_offset_x = (uint32_t)target.rect.origin.x;
    op.out.block_offset_y = (uint32_t)target.rect.origin.y;
    op.out.srm_cm = color_mode_;
    op.rotation_angle = ppa_rotation(target.rotation);
    op.scale_x = (float)target.scale_n / kScaleDenominator;
    op.scale_y = op.scale_x;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    const esp_err_t err = ppa_ ? ppa_do_scale_rotate_mirror(ppa_, &op) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa: %s", esp_err_to_name(err));
        *error = std::string("video scaling failed: ") + esp_err_to_name(err);
        return false;
    }
    return true;
}
