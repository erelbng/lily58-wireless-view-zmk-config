/*
 * Fireplace status screen, central half — portrait 128x32 OLED (viewer sees
 * 32 wide x 128 tall):
 *
 *   viewer y   0..31   battery + output/profile      (square A, canvas)
 *   viewer y  32..63   layer name + work-timer bar   (square B, canvas)
 *   viewer y  64..127  animated campfire             (pre-rotated images)
 *
 * Pause reminder: after CONFIG_FIREPLACE_WORK_MINUTES of active typing
 * (idle breaks of CONFIG_FIREPLACE_RESET_IDLE_SECONDS reset it), the fire is
 * replaced by a steaming mug and square B shows a countdown for
 * CONFIG_FIREPLACE_BREAK_SECONDS.
 *
 * Event-listener plumbing adapted from the stock ZMK nice_view widgets
 * (Copyright (c) 2025 The ZMK Contributors, MIT).
 *
 * Copyright (c) 2026 ric
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"
#include "fire_art.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

#define WORK_SECONDS (CONFIG_FIREPLACE_WORK_MINUTES * 60)
#define BREAK_SECONDS CONFIG_FIREPLACE_BREAK_SECONDS
#define RESET_IDLE_SECONDS CONFIG_FIREPLACE_RESET_IDLE_SECONDS

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

/* pause-reminder bookkeeping, ticked once a second from an LVGL timer */
static uint32_t work_seconds;
static uint32_t idle_run_seconds;
static uint32_t reminder_left; /* >0 while the reminder is showing */
static uint8_t fire_frame;

/* square A: battery + output endpoint */
static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    /* symbols exist only in montserrat >= 14, so use 16 here */
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    draw_battery(canvas, state);

    char output_text[12] = {};
    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        strcat(output_text, LV_SYMBOL_USB);
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            strcat(output_text, state->active_profile_connected ? LV_SYMBOL_WIFI
                                                                : LV_SYMBOL_CLOSE);
        } else {
            strcat(output_text, LV_SYMBOL_SETTINGS);
        }
        snprintf(output_text + strlen(output_text), 3, " %d", state->active_profile_index + 1);
        break;
    }
    lv_canvas_draw_text(canvas, 0, 17, CANVAS_SIZE, &label_dsc, output_text);

    rotate_canvas(canvas, cbuf);
}

/* square B: layer + work bar, or the countdown while the reminder shows */
static void draw_middle(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

#if IS_ENABLED(CONFIG_FIREPLACE_PAUSE_REMINDER)
    if (reminder_left > 0) {
        lv_draw_label_dsc_t count_dsc;
        init_label_dsc(&count_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
        char count_text[8];
        snprintf(count_text, sizeof(count_text), "%u:%02u", reminder_left / 60,
                 reminder_left % 60);
        lv_canvas_draw_text(canvas, 0, 8, CANVAS_SIZE, &count_dsc, count_text);
        rotate_canvas(canvas, cbuf);
        return;
    }
#endif

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_12, LV_TEXT_ALIGN_CENTER);

    if (state->layer_label == NULL || strlen(state->layer_label) == 0) {
        char text[10] = {};
        sprintf(text, "L%i", state->layer_index);
        lv_canvas_draw_text(canvas, 0, 4, CANVAS_SIZE, &label_dsc, text);
    } else {
        lv_canvas_draw_text(canvas, 0, 4, CANVAS_SIZE, &label_dsc, state->layer_label);
    }

#if IS_ENABLED(CONFIG_FIREPLACE_PAUSE_REMINDER)
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_canvas_draw_rect(canvas, 1, 24, 30, 4, &rect_white_dsc);
    lv_canvas_draw_rect(canvas, 2, 25, 28, 2, &rect_black_dsc);
    int fill = (state->work_frac * 28) / 255;
    if (fill > 0) {
        lv_canvas_draw_rect(canvas, 2, 25, fill, 2, &rect_white_dsc);
    }
#endif

    rotate_canvas(canvas, cbuf);
}

#if IS_ENABLED(CONFIG_FIREPLACE_PAUSE_REMINDER)
/* mug square, drawn over the upper half of the fire area while pausing */
static void draw_mug(struct zmk_widget_status *widget) {
    lv_obj_t *canvas = widget->mug;

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    lv_canvas_draw_img(canvas, (CANVAS_SIZE - mug_art.header.w) / 2, 1, &mug_art, &img_dsc);

    rotate_canvas(canvas, widget->cbuf3);
}

static void reminder_show(struct zmk_widget_status *widget, bool show) {
    if (show) {
        lv_obj_add_flag(widget->fire, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(widget->mug, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(widget->fire, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(widget->mug, LV_OBJ_FLAG_HIDDEN);
    }
}

static void second_tick_cb(lv_timer_t *timer) {
    struct zmk_widget_status *widget = timer->user_data;

    if (reminder_left > 0) {
        reminder_left--;
        if (reminder_left == 0) {
            work_seconds = 0;
            idle_run_seconds = 0;
            widget->state.work_frac = 0;
            reminder_show(widget, false);
        }
        draw_middle(widget->obj, widget->cbuf2, &widget->state);
        return;
    }

    if (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE) {
        idle_run_seconds = 0;
        work_seconds++;
    } else {
        idle_run_seconds++;
        if (idle_run_seconds >= RESET_IDLE_SECONDS && work_seconds > 0) {
            work_seconds = 0;
        }
    }

    uint8_t frac = (uint8_t)((MIN(work_seconds, WORK_SECONDS) * 255) / WORK_SECONDS);
    if (frac != widget->state.work_frac) {
        widget->state.work_frac = frac;
        draw_middle(widget->obj, widget->cbuf2, &widget->state);
    }

    if (work_seconds >= WORK_SECONDS) {
        reminder_left = BREAK_SECONDS;
        reminder_show(widget, true);
        draw_mug(widget);
        draw_middle(widget->obj, widget->cbuf2, &widget->state);
    }
}
#endif /* CONFIG_FIREPLACE_PAUSE_REMINDER */

static void fire_anim_cb(lv_timer_t *timer) {
    struct zmk_widget_status *widget = timer->user_data;

    if (reminder_left > 0) {
        return;
    }
#if IS_ENABLED(CONFIG_FIREPLACE_IDLE_FREEZE)
    if (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE) {
        return;
    }
#endif
    fire_frame = (fire_frame + 1) % FIRE_FRAME_COUNT;
    lv_img_set_src(widget->fire, fire_frames[fire_frame]);
}

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;

    draw_middle(widget->obj, widget->cbuf2, &widget->state);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index, .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index))};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 128, 32);
    /* keep any uncovered area dark */
    lv_obj_set_style_bg_color(widget->obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, LV_PART_MAIN);

    /* child 0, square A: viewer top = framebuffer right */
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    /* child 1, square B */
    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, 64, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    /* fire: viewer bottom = framebuffer left, frames are pre-rotated */
    widget->fire = lv_img_create(widget->obj);
    lv_img_set_src(widget->fire, fire_frames[0]);
    lv_obj_align(widget->fire, LV_ALIGN_TOP_LEFT, 0, 0);

    /* mug square over the upper fire area, hidden until pause time */
    widget->mug = lv_canvas_create(widget->obj);
    lv_obj_align(widget->mug, LV_ALIGN_TOP_LEFT, 32, 0);
    lv_canvas_set_buffer(widget->mug, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(widget->mug, LV_OBJ_FLAG_HIDDEN);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();

    lv_timer_create(fire_anim_cb, CONFIG_FIREPLACE_FRAME_MS, widget);
#if IS_ENABLED(CONFIG_FIREPLACE_PAUSE_REMINDER)
    lv_timer_create(second_tick_cb, 1000, widget);
#endif

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
