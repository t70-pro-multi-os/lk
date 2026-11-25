/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Drawing straight into the stock lk console frame buffer. Colours are 0xRRGGBB. */
#pragma once
#include <stdint.h>

int gfx_init(void);		/* 0 if the frame buffer is usable */
int gfx_w(void);
int gfx_h(void);
void gfx_fill(int x, int y, int w, int h, uint32_t rgb);
void gfx_round(int x, int y, int w, int h, int r, uint32_t rgb);
/* Stock 8x16 font enlarged 1-4x (2x, 3x and 4x smoothed); returns the width. */
int gfx_text(int x, int y, int scale, uint32_t rgb, const char *s);
int gfx_text_w(int scale, const char *s);
void gfx_text_center(int y, int scale, uint32_t rgb, const char *s);
void gfx_dim(void);		/* darken the whole screen (behind a message box) */
void gfx_flush(void);		/* show what was drawn */
