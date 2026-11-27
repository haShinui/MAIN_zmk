/*
 * Cyclotab for ZMK - port of Pascal Getreuer's QMK community module
 * https://getreuer.info/posts/keyboards/cyclotab/
 * SPDX-License-Identifier: MIT
 *
 * - A hotkey (e.g. &kp LA(TAB)) starts a cycle and keeps Alt held after release.
 * - Further hotkey taps cycle. Shift (held, sticky or hold-tap) reverses,
 *   as does a hotkey that contains Shift, e.g. LS(LA(TAB)).
 * - Pressing or releasing any other key ends the cycle; a pressed key is
 *   consumed (not sent). Modifiers and arrow keys pass through.
 * - ESC is sent, then the mods are released (cancels the switcher).
 * - Releasing a layer that was active when the cycle started ends it at once.
 * - Mods are released after timeout-ms (0 = no timeout).
 * - &key_repeat repeating a hotkey works like a hotkey (QMK QK_REP).
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/hid.h>
#include <zmk/keys.h>
#include <zmk/keymap.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_cyclotab

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static const uint32_t hotkeys[] = DT_INST_PROP(0, keys);
#define CYC_TIMEOUT_MS DT_INST_PROP(0, timeout_ms)

#define SHIFT_MASK (MOD_LSFT | MOD_RSFT)
#define MAX_CONSUMED 8

static bool active;
static uint8_t held_mods;
static zmk_keymap_layers_state_t start_layers;
static int armed_layer = -1;

static struct {
    uint16_t page;
    uint32_t id;
} consumed[MAX_CONSUMED];
static int consumed_count;

static struct k_work_delayable timeout_work;
static struct k_work_delayable arm_work;
static struct k_work end_work;

static void set_mods(uint8_t mods, bool pressed) {
    for (int i = 0; i < 8; i++) {
        if (mods & BIT(i)) {
            raise_zmk_keycode_state_changed_from_encoded(HID_USAGE_KEY_KEYBOARD_LEFTCONTROL + i,
                                                         pressed, k_uptime_get());
        }
    }
}

static void disarm(void) {
    k_work_cancel_delayable(&arm_work);
    if (armed_layer >= 0) {
        int layer = armed_layer;
        armed_layer = -1;
        zmk_keymap_layer_deactivate(layer, false);
    }
}

static void end_cycle(void) {
    if (active) {
        active = false;
        k_work_cancel_delayable(&timeout_work);
        uint8_t mods = held_mods;
        held_mods = 0;
        set_mods(mods, false);
        LOG_DBG("cyclotab: end");
    }
    disarm();
}

static void timeout_handler(struct k_work *work) { end_cycle(); }
static void end_handler(struct k_work *work) { end_cycle(); }
static void arm_handler(struct k_work *work) {
    if (!active) {
        disarm();
    }
}

static int match_hotkey(const struct zmk_keycode_state_changed *ev) {
    for (int i = 0; i < ARRAY_SIZE(hotkeys); i++) {
        uint16_t page = ZMK_HID_USAGE_PAGE(hotkeys[i]);
        if (page == 0) {
            page = HID_USAGE_KEY;
        }
        if (page == ev->usage_page && ZMK_HID_USAGE_ID(hotkeys[i]) == ev->keycode &&
            (SELECT_MODS(hotkeys[i]) & ~SHIFT_MASK) == (ev->implicit_modifiers & ~SHIFT_MASK)) {
            return i;
        }
    }
    return -1;
}

static bool is_arrow(const struct zmk_keycode_state_changed *ev) {
    return ev->usage_page == HID_USAGE_KEY && ev->keycode >= HID_USAGE_KEY_KEYBOARD_RIGHTARROW &&
           ev->keycode <= HID_USAGE_KEY_KEYBOARD_UPARROW;
}

static bool is_esc(const struct zmk_keycode_state_changed *ev) {
    return ev->usage_page == HID_USAGE_KEY && ev->keycode == HID_USAGE_KEY_KEYBOARD_ESCAPE;
}

static bool take_consumed(uint16_t page, uint32_t id) {
    for (int i = 0; i < consumed_count; i++) {
        if (consumed[i].page == page && consumed[i].id == id) {
            consumed[i] = consumed[--consumed_count];
            return true;
        }
    }
    return false;
}

static void add_consumed(uint16_t page, uint32_t id) {
    if (consumed_count < MAX_CONSUMED) {
        consumed[consumed_count].page = page;
        consumed[consumed_count].id = id;
        consumed_count++;
    }
}

static int on_keycode(struct zmk_keycode_state_changed *ev) {
    if (is_mod(ev->usage_page, ev->keycode)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int h = match_hotkey(ev);
    if (h >= 0) {
        if (ev->state) {
            uint8_t mods = SELECT_MODS(hotkeys[h]) & ~SHIFT_MASK;
            k_work_cancel_delayable(&arm_work);
            if (active && mods != held_mods) {
                /* switching e.g. from Alt+Tab to Gui+Tab */
                set_mods(held_mods, false);
                held_mods = 0;
                active = false;
            }
            if (!active) {
                active = true;
                held_mods = mods;
                start_layers = zmk_keymap_layer_state();
                set_mods(mods, true);
                LOG_DBG("cyclotab: start, mods 0x%02X", mods);
            }
            if (CYC_TIMEOUT_MS > 0) {
                k_work_reschedule(&timeout_work, K_MSEC(CYC_TIMEOUT_MS));
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!ev->state && take_consumed(ev->usage_page, ev->keycode)) {
        return ZMK_EV_EVENT_HANDLED;
    }

    if (!active) {
        /* armed switch layer but a normal key was pressed: drop the layer */
        if (ev->state && armed_layer >= 0) {
            disarm();
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (is_arrow(ev)) {
        if (CYC_TIMEOUT_MS > 0) {
            k_work_reschedule(&timeout_work, K_MSEC(CYC_TIMEOUT_MS));
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (is_esc(ev)) {
        if (ev->state) {
            /* ESC goes out with the mods still held; release them afterwards */
            k_work_submit(&end_work);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        add_consumed(ev->usage_page, ev->keycode);
        end_cycle();
        return ZMK_EV_EVENT_HANDLED;
    }

    end_cycle();
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_layer(struct zmk_layer_state_changed *ev) {
    if (!ev->state && active && (start_layers & BIT(ev->layer))) {
        end_cycle();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int cyclotab_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *kc = as_zmk_keycode_state_changed(eh);
    if (kc != NULL) {
        return on_keycode(kc);
    }
    struct zmk_layer_state_changed *ls = as_zmk_layer_state_changed(eh);
    if (ls != NULL) {
        return on_layer(ls);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(cyclotab, cyclotab_listener);
ZMK_SUBSCRIPTION(cyclotab, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(cyclotab, zmk_layer_state_changed);

static int cyclotab_init(void) {
    k_work_init_delayable(&timeout_work, timeout_handler);
    k_work_init_delayable(&arm_work, arm_handler);
    k_work_init(&end_work, end_handler);
    return 0;
}

SYS_INIT(cyclotab_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ------------------------------------------------- &cyc_layer (optional) */

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_cyclotab_layer

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct cyc_layer_config {
    int32_t arm_timeout_ms;
    uint32_t tap_key;
};

static int on_layer_pressed(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct cyc_layer_config *cfg = dev->config;
    int layer = binding->param1;

    if (active) {
        end_cycle();
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (armed_layer == layer) {
        /* second tap while armed: plain key (Tab) */
        disarm();
        if (cfg->tap_key) {
            raise_zmk_keycode_state_changed_from_encoded(cfg->tap_key, true, k_uptime_get());
            raise_zmk_keycode_state_changed_from_encoded(cfg->tap_key, false, k_uptime_get());
        }
        return ZMK_BEHAVIOR_OPAQUE;
    }

    armed_layer = layer;
    zmk_keymap_layer_activate(layer, false);
    if (cfg->arm_timeout_ms > 0) {
        k_work_reschedule(&arm_work, K_MSEC(cfg->arm_timeout_ms));
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_layer_released(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static int cyc_layer_init(const struct device *dev) { return 0; }

static const struct behavior_driver_api cyc_layer_api = {
    .binding_pressed = on_layer_pressed,
    .binding_released = on_layer_released,
};

#define CYC_LAYER_INST(n)                                                                          \
    static const struct cyc_layer_config cyc_layer_config_##n = {                                  \
        .arm_timeout_ms = DT_INST_PROP(n, arm_timeout_ms),                                         \
        .tap_key = DT_INST_PROP(n, tap_key),                                                       \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, cyc_layer_init, NULL, NULL, &cyc_layer_config_##n, POST_KERNEL,     \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &cyc_layer_api);

DT_INST_FOREACH_STATUS_OKAY(CYC_LAYER_INST)

#endif /* cyclotab-layer */

#endif /* zmk,cyclotab */
