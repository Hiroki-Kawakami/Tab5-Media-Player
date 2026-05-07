#include "mediaplayer.hpp"
#include "platform_port.hpp"
#include "lvgl.hpp"

static void lvgl_setup() {
    auto fb = pf_port::display_get_frame_buffer(0);
    auto disp = lv_display_create(720, 1280);
    lv_display_set_buffers(disp, fb, NULL, 720 * 1280 * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, [](lv_display_t *disp, const lv_area_t *area, uint8_t *px_map){
        pf_port::display_flush(0);
        lv_display_flush_ready(disp);
    });

    auto indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev, NULL);
    lv_indev_set_read_cb(indev, [](lv_indev_t *indev, lv_indev_data_t *data){
        auto touch = pf_port::touch_get_point();
        if (touch.has_value()) {
            data->state = LV_INDEV_STATE_PRESSED;
            data->point.x = std::get<0>(touch.value());
            data->point.y = std::get<1>(touch.value());
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    });
}

static void hello_world(void *args) {
    auto btn = lv_button_create(lv_screen_active());
    lv_obj_set_pos(btn, 10, 10);
    lv_obj_set_size(btn, 120, 50);
    lv_obj_add_event_cb(btn, [](lv_event_t *e){
        static uint8_t cnt = 0;
        lv_obj_t * label = lv_obj_get_child(lv_event_get_target_obj(e), 0);
        lv_label_set_text_fmt(label, "Button: %d", ++cnt);
    }, LV_EVENT_CLICKED, NULL);
    auto label = lv_label_create(btn);
    lv_label_set_text(label, "Button");
    lv_obj_center(label);
}

void mediaplayer_app() {
    pf_port::init(2, pf_port::PixelFormat::RGB565);
    lvgl_setup();
    lv_async_call(hello_world, NULL);
    pf_port::display_set_brightness(80);
}
