/*
 * Cyclotab for ZMK: a Cyclotab-style (Pascal Getreuer, QMK) window switcher.
 * SPDX-License-Identifier: MIT
 *
 * &cyc_enter L  - activate switch layer L (auto-drops after arm-timeout-ms)
 * &cyc_alt ...  - (zmk,behavior-cyclotab) holds modifier + taps Tab, repeatable
 * &cyc_end      - (zmk,behavior-cyclotab-end) swallows key, releases modifier,
 *                 drops the layer; optional send-key (e.g. ESC) first.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---------------------------------------------------------------- cyclotab */

#define DT_DRV_COMPAT zmk_behavior_cyclotab

struct cyc_config {
    uint32_t modifier;
    uint32_t key;
    int32_t layer;
    int32_t timeout_ms;
};

struct cyc_data {
    bool active;
    struct k_work_delayable timer;
    const struct device *dev;
};

#define GET_CYC_DEV(inst) DEVICE_DT_INST_GET(inst),
static const struct device *cyc_devs[] = {DT_INST_FOREACH_STATUS_OKAY(GET_CYC_DEV)};

static struct k_work_delayable arm_work;
static int32_t arm_layer = -1;

static void send_key(uint32_t encoded, bool pressed) {
    raise_zmk_keycode_state_changed_from_encoded(encoded, pressed, k_uptime_get());
}

static void cyc_stop(const struct device *dev, bool drop_layer) {
    struct cyc_data *data = dev->data;
    const struct cyc_config *cfg = dev->config;

    if (!data->active) {
        return;
    }
    data->active = false;
    k_work_cancel_delayable(&data->timer);
    send_key(cfg->modifier, false);
    if (drop_layer && cfg->layer >= 0) {
        zmk_keymap_layer_deactivate(cfg->layer, false);
    }
}

/* Ends every active cycle and drops `layer`. */
static void cyc_finish_all(int32_t layer) {
    k_work_cancel_delayable(&arm_work);
    for (int i = 0; i < ARRAY_SIZE(cyc_devs); i++) {
        cyc_stop(cyc_devs[i], false);
    }
    if (layer >= 0) {
        zmk_keymap_layer_deactivate(layer, false);
    }
}

/* Releasing the layer a cyclotab key lives on releases the modifier immediately. */
static int cyc_layer_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL || ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < ARRAY_SIZE(cyc_devs); i++) {
        const struct cyc_config *cfg = cyc_devs[i]->config;
        if (cfg->layer == ev->layer) {
            cyc_stop(cyc_devs[i], false);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_cyclotab, cyc_layer_listener);
ZMK_SUBSCRIPTION(behavior_cyclotab, zmk_layer_state_changed);

static void cyc_timer_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct cyc_data *data = CONTAINER_OF(dwork, struct cyc_data, timer);
    cyc_stop(data->dev, true);
}

static int on_cyc_pressed(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct cyc_data *data = dev->data;
    const struct cyc_config *cfg = dev->config;

    k_work_cancel_delayable(&arm_work);

    /* switching between S/T/C: release the other modifier first */
    for (int i = 0; i < ARRAY_SIZE(cyc_devs); i++) {
        if (cyc_devs[i] != dev) {
            cyc_stop(cyc_devs[i], false);
        }
    }

    if (!data->active) {
        data->active = true;
        send_key(cfg->modifier, true);
    }
    send_key(cfg->key, true);

    if (cfg->timeout_ms > 0) {
        k_work_reschedule(&data->timer, K_MSEC(cfg->timeout_ms));
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_cyc_released(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct cyc_config *cfg = dev->config;

    send_key(cfg->key, false);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int cyc_init(const struct device *dev) {
    struct cyc_data *data = dev->data;
    data->dev = dev;
    data->active = false;
    k_work_init_delayable(&data->timer, cyc_timer_handler);
    return 0;
}

static const struct behavior_driver_api cyc_api = {
    .binding_pressed = on_cyc_pressed,
    .binding_released = on_cyc_released,
};

#define CYC_INST(n)                                                                                \
    static struct cyc_data cyc_data_##n;                                                           \
    static const struct cyc_config cyc_config_##n = {                                              \
        .modifier = DT_INST_PROP(n, modifier),                                                     \
        .key = DT_INST_PROP(n, key),                                                               \
        .layer = DT_INST_PROP(n, layer),                                                           \
        .timeout_ms = DT_INST_PROP(n, timeout_ms),                                                 \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, cyc_init, NULL, &cyc_data_##n, &cyc_config_##n, POST_KERNEL,        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cyc_api);

DT_INST_FOREACH_STATUS_OKAY(CYC_INST)

#undef DT_DRV_COMPAT

/* ------------------------------------------------------------ cyclotab-end */

#define DT_DRV_COMPAT zmk_behavior_cyclotab_end

struct cyc_end_config {
    int32_t layer;
    uint32_t send_key;
};

static int on_end_pressed(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct cyc_end_config *cfg = dev->config;

    if (cfg->send_key) {
        send_key(cfg->send_key, true);
        send_key(cfg->send_key, false);
    }
    cyc_finish_all(cfg->layer);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_end_released(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static int cyc_end_init(const struct device *dev) { return 0; }

static const struct behavior_driver_api cyc_end_api = {
    .binding_pressed = on_end_pressed,
    .binding_released = on_end_released,
};

#define CYC_END_INST(n)                                                                            \
    static const struct cyc_end_config cyc_end_config_##n = {                                      \
        .layer = DT_INST_PROP(n, layer),                                                           \
        .send_key = DT_INST_PROP(n, send_key),                                                     \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, cyc_end_init, NULL, NULL, &cyc_end_config_##n, POST_KERNEL,         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cyc_end_api);

DT_INST_FOREACH_STATUS_OKAY(CYC_END_INST)

#undef DT_DRV_COMPAT

/* ---------------------------------------------------------- cyclotab-enter */

#define DT_DRV_COMPAT zmk_behavior_cyclotab_enter

struct cyc_enter_config {
    int32_t arm_timeout_ms;
};

static void arm_timeout_handler(struct k_work *work) {
    if (arm_layer >= 0) {
        zmk_keymap_layer_deactivate(arm_layer, false);
    }
}

static int on_enter_pressed(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct cyc_enter_config *cfg = dev->config;
    int32_t layer = binding->param1;

    if (zmk_keymap_layer_active(layer)) {
        cyc_finish_all(layer);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    arm_layer = layer;
    zmk_keymap_layer_activate(layer, false);
    if (cfg->arm_timeout_ms > 0) {
        k_work_reschedule(&arm_work, K_MSEC(cfg->arm_timeout_ms));
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_enter_released(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static int cyc_enter_init(const struct device *dev) {
    k_work_init_delayable(&arm_work, arm_timeout_handler);
    return 0;
}

static const struct behavior_driver_api cyc_enter_api = {
    .binding_pressed = on_enter_pressed,
    .binding_released = on_enter_released,
};

#define CYC_ENTER_INST(n)                                                                          \
    static const struct cyc_enter_config cyc_enter_config_##n = {                                  \
        .arm_timeout_ms = DT_INST_PROP(n, arm_timeout_ms),                                         \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, cyc_enter_init, NULL, NULL, &cyc_enter_config_##n, POST_KERNEL,     \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cyc_enter_api);

DT_INST_FOREACH_STATUS_OKAY(CYC_ENTER_INST)
