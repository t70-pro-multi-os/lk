/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Stock fastboot fixes.
 *
 * The stock lk starts the hardware watchdog with a 30 s timeout (f_0037f8)
 * and only its key boot menu (f_00da08) stops it; image loading and the
 * orange-state wait restart it, the fastboot command loop (f_007b4c) never
 * does. So the tablet reset about 30 s after entering fastboot, often while
 * a command was running. Hooked in place of the loop's call for its idle
 * timeout (f_00509c, always 0: no idle timer), we stop the watchdog as the
 * stock boot menu does, and return 0 as that call did.
 */
#include "lk_stock.h"
#include "t70.h"

int t70_fastboot_start(void)
{
	lk_wdt_enable(0);
	lk_dprintf("t70: fastboot: hardware watchdog stopped\n");
	return 0;
}
