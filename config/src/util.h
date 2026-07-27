/*
 * Canvas helpers for the portrait-mounted 128x32 OLED (viewer: 32x128).
 * Rotation trick adapted from the stock ZMK nice_view widgets.
 * Copyright (c) 2023 The ZMK Contributors
 * Copyright (c) 2026 ric
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/endpoints.h>
#endif

/* Everything is drawn on 32x32 canvases in viewer orientation, then rotated
 * 90° clockwise in place (the panel is mounted 90° CCW). */
#define CANVAS_SIZE 32

#define LVGL_BACKGROUND lv_color_black()
#define LVGL_FOREGROUND lv_color_white()

struct status_state {
    uint8_t battery;
    bool charging;
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    uint8_t layer_index;
    const char *layer_label;
    uint8_t work_frac; /* 0..255 progress toward the pause reminder */
#else
    bool connected;
#endif
};

struct battery_status_state {
    uint8_t level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    bool usb_present;
#endif
};

void rotate_canvas(lv_obj_t *canvas, lv_color_t cbuf[]);
void draw_battery(lv_obj_t *canvas, const struct status_state *state);
void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align);
void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color);
