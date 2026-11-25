/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Called in place of the stock orange-state warning screen (hooks.txt): the
 * stock lk would clear the screen, print the unlocked-device warning and wait
 * 5 s. We wait the configured time (default 5 s, the stock wait) with only a
 * countdown on the screen; any key during the wait, or a volume key held
 * when it starts, opens the boot menu. With the wait set to 0 and no key
 * held the screen is left as it is.
 */
#include "lk_stock.h"
#include "t70.h"

#define POLL_US 20000u

void t70_orange_wait(void)
{
	struct t70_cfg cfg;
	uint64_t start, limit, now;
	unsigned shown = ~0u;
	int row;

	t70_cfg_load(&cfg);
	/* Power still held from switching the tablet on must not open the menu;
	 * a volume key held while booting does. */
	t70_keys_ignore_held(t70_keys() & T70_KEY_POWER);
	if (!cfg.wait_ms && !t70_keys())
		return;
	lk_console_clear();
	row = lk_console_rows() / 2;
	start = lk_time_us();
	limit = (uint64_t)cfg.wait_ms * 1000;
	for (;;) {
		if (t70_keys()) {
			t70_menu(&cfg);
			return;
		}
		now = lk_time_us() - start;
		if (now >= limit)
			break;
		unsigned left = (unsigned)((limit - now + 999999) / 1000000);
		if (left != shown) {
			shown = left;
			lk_console_goto(row, 0);
			lk_console_printf("Press any key for the boot menu (%u)   \n", left);
			lk_display_update();
		}
		lk_delay_us(POLL_US);
	}
	lk_console_goto(row, 0);
	lk_console_printf("                                          \n");
	lk_display_update();
}
