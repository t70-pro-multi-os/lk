/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Called in place of the stock orange-state warning screen (hooks.txt): the
 * stock lk would clear the screen, print the unlocked-device warning and wait
 * 5 s. We wait the configured time (default 5 s, the stock wait) with the
 * boot logo still on the screen and only a 1 px line along the bottom edge
 * running down; any key during the wait, or a volume key held when it
 * starts, opens the boot menu. With the wait set to 0 and no key held the
 * screen is left as it is. A boot whose mode is already chosen (recovery,
 * factory, META, fastboot, download) goes on at once. Afterwards the console
 * page is transparent again, so the logo stays while the kernel starts, as
 * after the stock warning.
 */
#include "gfx.h"
#include "lk_stock.h"
#include "t70.h"

#define LINE_H		1		/* the time left: a line along the bottom edge */
#define LINE_RGB	0x8c8c8c

static void bar(uint64_t left, uint64_t total)
{
	int w = total ? (int)((uint64_t)gfx_w() * left / total) : 0;
	int y = gfx_h() - LINE_H;

	gfx_clear_rect(w, y, gfx_w() - w, LINE_H);	/* the logo page shows where time has run out */
	if (w > 0)
		gfx_fill(0, y, w, LINE_H, LINE_RGB);
	gfx_flush();
}

#ifdef T70_DIAG
/* Diagnostic build (make DIAG=1): records in misc (diag.c), and two lines
 * printed with the stock console first, the way the stock warning was. */
enum diag_stage { D_ENTRY = 1, D_NO_FB, D_WAIT_OFF, D_DRAWN, D_MENU, D_DONE, D_MENU_DONE, D_MODE };
#define DIAG(stage) t70_diag(stage)

static unsigned hi(const void *p) { return (unsigned)((uintptr_t)p >> 32); }
static unsigned lo(const void *p) { return (unsigned)(uintptr_t)p; }

static void diag_console(const struct t70_cfg *cfg, unsigned keys)
{
	struct lk_fbcon *c = lk_fbcon;
	uint64_t t;

	t70_diag_rec.wait_ms = cfg->wait_ms;
	t70_diag_rec.keys_start = keys;
	lk_console_clear();
	lk_console_goto(lk_console_rows() / 2, 0);
	lk_console_printf("t70 diag: fbcon %x%08x, display ready %u, frame buffer %x%08x + %x\n",
			  hi(c), lo(c), lk_disp_ready, hi(lk_disp_fb), lo(lk_disp_fb), lk_disp_fb_offset);
	if (c)
		lk_console_printf("t70 diag: console %x%08x %ux%u, %u bytes per pixel; wait %u ms, keys %x\n",
				  hi(c->fb), lo(c->fb), c->width, c->height, c->bytes_pp, cfg->wait_ms, keys);
	t70_diag(D_ENTRY);
	t = lk_time_us();
	while (lk_time_us() - t < 2000000)	/* 2 s to read it */
		t70_poll_delay();
}

/* What the first countdown screen left in memory: the first pixel
 * (transparent, 0) and the middle of the bottom line (0xff8c8c8c) in the
 * console page, then the same two in the first page of the display frame
 * buffer. */
static void diag_pixels(void)
{
	struct lk_fbcon *c = lk_fbcon;
	unsigned long bar = ((unsigned long)(c->height - LINE_H) * c->width + c->width / 2) * 4;

	if (c->bytes_pp == 4) {
		t70_diag_rec.px[0] = *(volatile uint32_t *)c->fb;
		t70_diag_rec.px[1] = *(volatile uint32_t *)(c->fb + bar);
		if (lk_disp_fb) {
			t70_diag_rec.px[2] = *(volatile uint32_t *)lk_disp_fb;
			t70_diag_rec.px[3] = *(volatile uint32_t *)(lk_disp_fb + bar);
		}
	}
	t70_diag(D_DRAWN);
}
#else
#define DIAG(stage) do { } while (0)
#endif

/* Normal boots and power-off charging (switched on with the cable in) wait;
 * a mode chosen by the misc message, the RGU boot mode or a key held at
 * power-on is what the user asked for already. */
static int mode_chosen(void)
{
	switch (lk_boot_mode) {
	case LK_MODE_META:
	case LK_MODE_RECOVERY:
	case LK_MODE_FACTORY:
	case LK_MODE_ADVMETA:
	case LK_MODE_ATE_FACTORY:
	case LK_MODE_FASTBOOT:
	case LK_MODE_DOWNLOAD:
		return 1;
	}
	return 0;
}

void t70_orange_wait(void)
{
	struct t70_cfg cfg;
	uint64_t start, limit, now;
	unsigned polls = 0;
	int no_fb;

	if (mode_chosen()) {
		lk_dprintf("t70: boot mode %u, no wait\n", lk_boot_mode);
		DIAG(D_MODE);
		return;
	}
	t70_cfg_load(&cfg);
	limit = (uint64_t)cfg.wait_ms * 1000;
	/* Power still held from switching the tablet on must not open the menu;
	 * a volume key held while booting does. */
	t70_keys_ignore_held(t70_keys() & T70_KEY_POWER);
#ifdef T70_DIAG
	diag_console(&cfg, t70_keys());
	if (limit < 15000000)	/* time to try every key */
		limit = 15000000;
#endif
	start = lk_time_us();
	no_fb = gfx_init();
#ifdef T70_DIAG
	t70_diag_rec.gfx = no_fb;
#endif
	if (no_fb) {
		/* no frame buffer to draw on: only the wait, no menu */
		lk_dprintf("t70: no frame buffer, no boot menu\n");
		DIAG(D_NO_FB);
		while (lk_time_us() - start < limit)
			t70_poll_delay();
		return;
	}
	if (!cfg.wait_ms && !t70_keys()) {
		DIAG(D_WAIT_OFF);
		return;
	}
	gfx_clear();
	bar(limit, limit);
#ifdef T70_DIAG
	diag_pixels();
#endif
	for (;;) {
#ifdef T70_DIAG
		if (t70_diag_keys(gfx_h() - 60))
			gfx_flush();
#endif
		if (t70_keys()) {
			DIAG(D_MENU);
			t70_menu(&cfg);
			DIAG(D_MENU_DONE);
			return;
		}
		now = lk_time_us() - start;
		if (now >= limit)
			break;
		if (++polls % 5 == 0)	/* every 100 ms */
			bar(limit - now, limit);
		t70_poll_delay();
	}
	gfx_clear();
	gfx_flush();
	DIAG(D_DONE);
}
