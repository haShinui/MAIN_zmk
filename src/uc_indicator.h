/*
 * Unicode input mode indicator (Windows / Linux) shared by both halves.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

#define UC_IND_MODE_WIN 0
#define UC_IND_MODE_LINUX 1

struct zmk_uc_mode_changed {
    uint8_t mode;
};

ZMK_EVENT_DECLARE(zmk_uc_mode_changed);

uint8_t zmk_uc_indicator_get_mode(void);
