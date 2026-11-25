/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The stock font for the host programs: filled at run time (a test pattern,
 * or the font read from your stock lk by the screen demo). */
#include <stdint.h>

uint8_t lk_font_8x16[256 * 16];
uint8_t *const sim_font = lk_font_8x16;
