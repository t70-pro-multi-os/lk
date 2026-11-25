/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Called in place of the stock orange-state warning screen (hooks.txt): the
 * stock lk would clear the screen, print the unlocked-device warning and wait
 * 5 s. We wait the configured time (default 5 s, the stock wait) on a black
 * screen with a hint and a bar running down at the bottom; any key during
 * the wait, or a volume key held when it starts, opens the boot menu. With
 * the wait set to 0 and no key held the screen is left as it is.
 */
#include "gfx.h"
#include "lk_stock.h"
#include "t70.h"

#define BAR_W	360
#define BAR_H	6

static void bar(uint64_t left, uint64_t total)
{
	int x = (gfx_w() - BAR_W) / 2, y = gfx_h() - 150;
	int w = total ? (int)((uint64_t)BAR_W * left / total) : 0;

	gfx_round(x, y, BAR_W, BAR_H, 3, 0x262626);
	if (w > 0)
		gfx_round(x, y, w, BAR_H, 3, 0xb4b4b4);
	gfx_flush();
}

void t70_orange_wait(void)
{
	struct t70_cfg cfg;
	uint64_t start, limit, now;
	unsigned polls = 0;

	t70_cfg_load(&cfg);
	limit = (uint64_t)cfg.wait_ms * 1000;
	/* Power still held from switching the tablet on must not open the menu;
	 * a volume key held while booting does. */
	t70_keys_ignore_held(t70_keys() & T70_KEY_POWER);
	start = lk_time_us();
	if (gfx_init()) {
		/* no frame buffer to draw on: only the wait, no menu */
		lk_dprintf("t70: no frame buffer, no boot menu\n");
		while (lk_time_us() - start < limit)
			t70_poll_delay();
		return;
	}
	if (!cfg.wait_ms && !t70_keys())
		return;
	gfx_fill(0, 0, gfx_w(), gfx_h(), 0);
	gfx_text_center(gfx_h() - 220, 2, 0x8c8c8c, "Press any key for boot options");
	bar(limit, limit);
	for (;;) {
		if (t70_keys()) {
			t70_menu(&cfg);
			return;
		}
		now = lk_time_us() - start;
		if (now >= limit)
			break;
		if (++polls % 5 == 0)	/* every 100 ms */
			bar(limit - now, limit);
		t70_poll_delay();
	}
	gfx_fill(0, 0, gfx_w(), gfx_h(), 0);
	gfx_flush();
}
