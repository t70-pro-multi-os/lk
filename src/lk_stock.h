/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stock lk (G3D1 V1.08) functions and data used by our code; see names.txt. */
#pragma once
#include <stdint.h>

uint64_t lk_time_us(void);
void lk_delay_us(uint32_t us);
int lk_detect_key(int key);
void lk_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void lk_console_goto(int row, int col);
int lk_console_rows(void);
void lk_display_update(void);
extern volatile uint8_t lk_boot_mode;

enum lk_key { LK_KEY_RECOVERY = 0, LK_KEY_VOLUP = 1, LK_KEY_POWER = 2 };
