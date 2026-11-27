/*
 * Smooth scroll encoder for ZMK.
 * SPDX-License-Identifier: MIT
 *
 * A sensor behavior that turns encoder detents directly into wheel reports:
 * - no behavior queue (ZMK's &msc + sensor-rotate holds a key per detent and
 *   plays them one after another, which causes lag and stepping),
 * - each detent is spread over `smooth-ms` in small high-resolution steps
 *   (uses the host's resolution multiplier, up to 16 steps per notch),
 * - fast turning accelerates (`acceleration-window-ms`, `max-acceleration-percent`),
 * - reversing direction stops the remaining glide immediately.
 */

#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/virtual_key_position.h>

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#include <zmk/pointing/resolution_multipliers.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_behavior_smooth_scroll

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct smooth_scroll_config {
    int32_t notch_percent;
    int32_t smooth_ms;
    int32_t tick_ms;
    int32_t accel_window_ms;
    int32_t max_accel_percent;
    bool invert;
    bool horizontal;
};

struct smooth_scroll_data {
    const struct device *dev;
    struct k_work_delayable tick_work;
    struct k_spinlock lock;
    int32_t pending;  /* wheel units still to send (signed) */
    int32_t ticks_left;
    int32_t frac;     /* 1/100 unit carry */
    int64_t last_detent;
    int8_t last_dir;
    struct sensor_value remainder[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
    int triggers[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
};

static int units_per_notch(void) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
    return zmk_pointing_resolution_multipliers_get_current_profile().wheel + 1;
#else
    return 1;
#endif
}

static void send_scroll(const struct smooth_scroll_config *cfg, int32_t units) {
    if (units > INT16_MAX) {
        units = INT16_MAX;
    } else if (units < -INT16_MAX) {
        units = -INT16_MAX;
    }
    if (cfg->horizontal) {
        zmk_hid_mouse_scroll_set((int16_t)units, 0);
    } else {
        zmk_hid_mouse_scroll_set(0, (int16_t)units);
    }
    zmk_endpoint_send_mouse_report();
    zmk_hid_mouse_scroll_set(0, 0);
}

static void tick_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct smooth_scroll_data *data = CONTAINER_OF(dwork, struct smooth_scroll_data, tick_work);
    const struct smooth_scroll_config *cfg = data->dev->config;
    int32_t emit;
    bool more;

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    if (data->pending == 0) {
        k_spin_unlock(&data->lock, key);
        return;
    }
    int32_t ticks = MAX(data->ticks_left, 1);
    emit = data->pending / ticks;
    if (emit == 0) {
        emit = data->pending > 0 ? 1 : -1;
    }
    data->pending -= emit;
    if (data->ticks_left > 0) {
        data->ticks_left--;
    }
    more = data->pending != 0;
    k_spin_unlock(&data->lock, key);

    send_scroll(cfg, emit);

    if (more) {
        k_work_reschedule(&data->tick_work, K_MSEC(cfg->tick_ms));
    }
}

static void add_detents(const struct device *dev, int detents) {
    struct smooth_scroll_data *data = dev->data;
    const struct smooth_scroll_config *cfg = dev->config;

    int8_t dir = detents > 0 ? 1 : -1;
    int count = abs(detents);
    int64_t now = k_uptime_get();

    /* acceleration: the faster the detents come, the bigger each one */
    int32_t factor = 100;
    if (cfg->accel_window_ms > 0 && dir == data->last_dir) {
        int64_t dt = now - data->last_detent;
        if (dt < cfg->accel_window_ms) {
            factor += (cfg->max_accel_percent - 100) * (cfg->accel_window_ms - dt) /
                      cfg->accel_window_ms;
        }
    }

    /* hundredths of a wheel unit */
    int32_t add = cfg->notch_percent * units_per_notch() * factor / 100 * count;

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    if (dir != data->last_dir) {
        /* reversing: drop the rest of the old glide */
        data->pending = 0;
        data->frac = 0;
    }
    data->frac += add;
    int32_t units = data->frac / 100;
    data->frac %= 100;
    int32_t sign = cfg->invert ? dir : -dir; /* clockwise = scroll down */
    data->pending += sign * units;
    data->ticks_left = MAX(cfg->smooth_ms / MAX(cfg->tick_ms, 1), 1);
    data->last_dir = dir;
    data->last_detent = now;
    k_spin_unlock(&data->lock, key);

    /* send the first step right away for instant response */
    k_work_reschedule(&data->tick_work, K_NO_WAIT);
}

static int smooth_scroll_accept_data(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event,
                                     const struct zmk_sensor_config *sensor_config,
                                     size_t channel_data_size,
                                     const struct zmk_sensor_channel_data *channel_data) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct smooth_scroll_data *data = dev->data;
    const struct sensor_value value = channel_data[0].value;
    int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);
    int triggers;

    if (value.val1 == 0) {
        triggers = value.val2;
    } else {
        struct sensor_value rem = data->remainder[sensor_index][event.layer];
        rem.val1 += value.val1;
        rem.val2 += value.val2;
        if (abs(rem.val2) >= 1000000) {
            rem.val1 += rem.val2 / 1000000;
            rem.val2 %= 1000000;
        }
        int trigger_degrees = 360 / sensor_config->triggers_per_rotation;
        triggers = rem.val1 / trigger_degrees;
        rem.val1 %= trigger_degrees;
        data->remainder[sensor_index][event.layer] = rem;
    }

    data->triggers[sensor_index][event.layer] = triggers;
    return 0;
}

static int smooth_scroll_process(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event,
                                 enum behavior_sensor_binding_process_mode mode) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct smooth_scroll_data *data = dev->data;
    int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);
    int triggers = data->triggers[sensor_index][event.layer];

    data->triggers[sensor_index][event.layer] = 0;
    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER || triggers == 0) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    add_detents(dev, triggers);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int smooth_scroll_init(const struct device *dev) {
    struct smooth_scroll_data *data = dev->data;
    data->dev = dev;
    k_work_init_delayable(&data->tick_work, tick_handler);
    return 0;
}

static const struct behavior_driver_api smooth_scroll_api = {
    .sensor_binding_accept_data = smooth_scroll_accept_data,
    .sensor_binding_process = smooth_scroll_process,
};

#define SMOOTH_SCROLL_INST(n)                                                                      \
    static struct smooth_scroll_data smooth_scroll_data_##n;                                       \
    static const struct smooth_scroll_config smooth_scroll_config_##n = {                          \
        .notch_percent = DT_INST_PROP(n, notch_percent),                                           \
        .smooth_ms = DT_INST_PROP(n, smooth_ms),                                                   \
        .tick_ms = DT_INST_PROP(n, tick_ms),                                                       \
        .accel_window_ms = DT_INST_PROP(n, acceleration_window_ms),                                \
        .max_accel_percent = DT_INST_PROP(n, max_acceleration_percent),                            \
        .invert = DT_INST_PROP(n, invert),                                                         \
        .horizontal = DT_INST_PROP(n, horizontal),                                                 \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, smooth_scroll_init, NULL, &smooth_scroll_data_##n,                  \
                            &smooth_scroll_config_##n, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &smooth_scroll_api);

DT_INST_FOREACH_STATUS_OKAY(SMOOTH_SCROLL_INST)

#endif
