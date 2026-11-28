/*
 * &uc_ind <mode>: remembers the unicode input mode (0 = Windows, 1 = Linux)
 * and raises zmk_uc_mode_changed for the display. It is a GLOBAL behavior,
 * so the left (central) half also runs it on the right half. When the right
 * half (re)connects, the central re-sends the current mode.
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include "uc_indicator.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_uc_mode_changed);

static uint8_t uc_mode = UC_IND_MODE_WIN; /* zmk-unicode default-mode is WIN_COMPOSE */

uint8_t zmk_uc_indicator_get_mode(void) { return uc_mode; }

#define DT_DRV_COMPAT zmk_behavior_uc_indicator

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_uc_ind_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    uc_mode = binding->param1 ? UC_IND_MODE_LINUX : UC_IND_MODE_WIN;
    raise_zmk_uc_mode_changed((struct zmk_uc_mode_changed){.mode = uc_mode});
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_uc_ind_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api uc_ind_api = {
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
    .binding_pressed = on_uc_ind_pressed,
    .binding_released = on_uc_ind_released,
};

static int uc_ind_init(const struct device *dev) { return 0; }

BEHAVIOR_DT_INST_DEFINE(0, uc_ind_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &uc_ind_api);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
#include <zephyr/bluetooth/conn.h>
#include <zmk/events/position_state_changed.h>

/* re-send the current mode to the right half after it (re)connects */
static void resend_handler(struct k_work *work) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_INST_GET(0)->name,
        .param1 = uc_mode,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .layer = 0,
        .timestamp = k_uptime_get(),
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
    };
    zmk_behavior_invoke_binding(&binding, event, true);
    zmk_behavior_invoke_binding(&binding, event, false);
}

static K_WORK_DELAYABLE_DEFINE(resend_work, resend_handler);

static void uc_ind_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;
    if (err || bt_conn_get_info(conn, &info) != 0) {
        return;
    }
    if (info.role == BT_CONN_ROLE_CENTRAL) {
        /* split link: wait for service discovery to finish */
        k_work_reschedule(&resend_work, K_SECONDS(5));
    }
}

BT_CONN_CB_DEFINE(uc_ind_conn_callbacks) = {
    .connected = uc_ind_connected,
};
#endif

#endif
