/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Screen demo: runs the real menu code (src/) on the build host, drawing into
 * a 2240x1400 frame buffer like the stock lk console's, with the font read
 * from your stock lk payload. A scripted user opens the menu, moves down and
 * back up and switches to slot B. Every shown screen (at most one per 200 ms)
 * goes to stdout as "F <ms> <width> <height>\n" and RGB bytes;
 * tools/lk_demo.py turns them into PNG files.
 *
 *   demo_screen <stock lk payload>
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lk_stock.h"
#include "t70.h"

#define W 2240
#define H 1400
#define FONT 0xb1100	/* the console font in the G3D1 V1.08 lk payload */

static uint64_t now_us, last_frame_us;
static int dirty, frames;
static struct press { uint64_t from, to; unsigned keys; } script[16];
static int nscript;
static uint8_t misc[64 * 1024];
static jmp_buf halt_jump;
volatile uint8_t lk_boot_mode;
volatile uint16_t t70_test_kpd_mem1 = 0xffff;
volatile uint32_t t70_test_secao[0x44];
static struct lk_fbcon fbcon = { NULL, W * H * 4, 3, 4, W, H };
struct lk_fbcon *volatile lk_fbcon = &fbcon;
extern uint8_t *const sim_font;

static void press(uint64_t at_ms, uint64_t len_ms, unsigned keys)
{
	script[nscript++] = (struct press){ at_ms * 1000, (at_ms + len_ms) * 1000, keys };
}

static unsigned held(void)
{
	unsigned k = 0;
	for (int i = 0; i < nscript; i++)
		if (now_us >= script[i].from && now_us < script[i].to)
			k |= script[i].keys;
	return k;
}

static void sync_kpd(void)
{
	unsigned k = held();
	t70_test_kpd_mem1 = (uint16_t)(0xffff & ~((k & T70_KEY_UP) ? 1 : 0) & ~((k & T70_KEY_DOWN) ? 2 : 0));
}

/* Write the frame buffer (0x00RRGGBB per pixel) as RGB. */
static void emit(int force)
{
	static uint8_t rgb[W * H * 3];

	if (!dirty || (!force && frames && now_us - last_frame_us < 200000))
		return;
	const uint32_t *px = (const uint32_t *)fbcon.fb;
	for (int i = 0; i < W * H; i++) {
		rgb[3 * i] = (uint8_t)(px[i] >> 16);
		rgb[3 * i + 1] = (uint8_t)(px[i] >> 8);
		rgb[3 * i + 2] = (uint8_t)px[i];
	}
	printf("F %llu %d %d\n", (unsigned long long)(now_us / 1000), W, H);
	fwrite(rgb, 1, sizeof(rgb), stdout);
	dirty = 0;
	frames++;
	last_frame_us = now_us;
}

uint64_t lk_time_us(void) { return now_us; }
void lk_wdt_kick(void) {}
void t70_test_text(const char *s) { (void)s; }
void lk_delay_us(uint32_t us) { emit(0); now_us += us; sync_kpd(); }
int lk_detect_key(int key) { return key == LK_KEY_POWER && (held() & T70_KEY_POWER); }
void lk_disp_update(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; dirty = 1; }
int lk_dprintf(const char *fmt, ...) { (void)fmt; return 0; }
const char *lk_get_slot_suffix(void) { return "_a"; }
int lk_set_active_slot(const char *suffix) { (void)suffix; return 0; }
void lk_platform_halt(int action, int reason) { (void)action; (void)reason; emit(1); longjmp(halt_jump, 1); }
void lk_rgu_set_boot_mode(uint8_t mode) { (void)mode; }
void *lk_bio_open(const char *name) { return strcmp(name, "misc") ? NULL : misc; }
long lk_bio_read(void *dev, void *buf, long off, unsigned long len)
{
	memcpy(buf, (uint8_t *)dev + off, len);
	return (long)len;
}
long lk_bio_write(void *dev, const void *buf, long off, unsigned long len)
{
	memcpy((uint8_t *)dev + off, buf, len);
	return (long)len;
}
void lk_bio_close(void *dev) { (void)dev; }
void *lk_malloc(unsigned long size) { return malloc(size); }
void lk_free(void *p) { free(p); }

void t70_orange_wait(void);

int main(int argc, char **argv)
{
	FILE *f;

	if (argc != 2 || !(f = fopen(argv[1], "rb")) || fseek(f, FONT, SEEK_SET) ||
	    fread(sim_font, 1, 256 * 16, f) != 256 * 16) {
		fprintf(stderr, "usage: %s <stock lk payload>\n", argv[0]);
		return 2;
	}
	fclose(f);
	fbcon.fb = calloc(1, fbcon.size);
	press(1500, 150, T70_KEY_DOWN);		/* any key: open the menu */
	press(2700, 150, T70_KEY_DOWN);		/* "Switch to slot B" */
	press(3900, 150, T70_KEY_DOWN);		/* "Recovery" */
	press(5100, 150, T70_KEY_UP);		/* back to "Switch to slot B" */
	press(6300, 150, T70_KEY_POWER);	/* select */
	sync_kpd();
	if (!setjmp(halt_jump))
		t70_orange_wait();
	emit(1);
	return 0;
}
