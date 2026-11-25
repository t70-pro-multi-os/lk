/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * USB download modes for PC tools.
 *
 * BROM: the stock preloader's emergency download (taken when volume up,
 * volume down and power are held at power-on) unlocks the always-on security
 * block, sets the BootROM download request in BOOT_MISC0 and resets with the
 * watchdog; the BootROM then waits for a tool on USB. We do the same with a
 * longer wait (the stock one waits 5 s).
 *
 *   SECURITY_AO 0x1001a000: +0x100 MISC_LOCK_KEY (0xad98 unlocks)
 *                           +0x108 RST_CON (bit 0 set)
 *                           +0x080 BOOT_MISC0 = "DL" << 16 | seconds << 2 | 1
 *
 * Preloader: the preloader looks for a tool on USB at every start (2.5 s to
 * be enumerated, then 8 s for the handshake) but skips it while the power key
 * is held, as it often is after a forced power-off. A restart from the menu
 * has no key held, so a tool already waiting on the PC gets the preloader.
 */
#include "lk_stock.h"
#include "t70.h"

#ifdef T70_HOST_TEST
extern volatile uint32_t t70_test_secao[0x44];
#define SECAO t70_test_secao
#else
#define SECAO ((volatile uint32_t *)0xffff00001001a000ul)	/* stock lk device mapping */
#endif

#define MISC_LOCK_KEY	(0x100 / 4)
#define RST_CON		(0x108 / 4)
#define BOOT_MISC0	(0x080 / 4)

void t70_reboot_brom(unsigned seconds)
{
	if (seconds > 0x3ffe)
		seconds = 0x3ffe;
	SECAO[MISC_LOCK_KEY] = 0xad98;
	SECAO[RST_CON] |= 1;
	SECAO[MISC_LOCK_KEY] = 0;
	SECAO[BOOT_MISC0] = 0x444c0000u | (seconds << 2) | 1;
	lk_platform_halt(LK_HALT_REBOOT, LK_REASON_SW_RESET);
}

void t70_reboot_preloader(void)
{
	lk_platform_halt(LK_HALT_REBOOT, LK_REASON_COLD);
}
