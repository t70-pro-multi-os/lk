/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lk_stock.h"
#include "t70.h"

/*
 * Volume keys: keypad memory register 1 (KPD 0x10010000 + 0x04, mapped by the
 * stock lk at this virtual address), active low. Bit 0 is volume up (keypad
 * code 0) and bit 1 volume down (code 1), as the lk device tree overlay maps
 * them (hw-init-map 115, 114) and the stock lk_detect_key(1) and (0) read
 * them. Only these two bits count, as in the stock lk: the other bits have no
 * key and need not read 1. Both low at once is taken as an unclocked block
 * reading 0, not as two keys.
 * Power is the PMIC key read through the stock lk_detect_key(); the PMIC
 * second key counts as "other".
 */
#ifdef T70_HOST_TEST
extern volatile uint16_t t70_test_kpd_mem1;
#define KPD_MEM1 (&t70_test_kpd_mem1)
#else
#define KPD_MEM1 ((volatile uint16_t *)0xffff000010010004ul)
#endif

static unsigned ignored = 0;	/* in .data: the stock start code does not clear our bss */

unsigned t70_keys(void)
{
	unsigned keys = 0;
	uint16_t mem1 = *KPD_MEM1;

	if (mem1 & 3) {
		if (!(mem1 & 1))
			keys |= T70_KEY_UP;
		if (!(mem1 & 2))
			keys |= T70_KEY_DOWN;
	}
	if (lk_detect_key(LK_KEY_POWER) & 1)
		keys |= T70_KEY_POWER;
	if (lk_detect_key(LK_KEY_PMIC_HOME) & 1)
		keys |= T70_KEY_OTHER;
	ignored &= keys;		/* a released key counts again */
	return keys & ~ignored;
}

uint16_t t70_kpd_mem1(void)
{
	return *KPD_MEM1;
}

/* One key poll interval. The stock lk runs the hardware watchdog with a 30 s
 * timeout, so every wait restarts it (as the stock 5 s warning wait does). */
void t70_poll_delay(void)
{
	lk_wdt_kick();
	lk_delay_us(T70_POLL_US);
}

/* Ignore keys that are held now until they are released. */
void t70_keys_ignore_held(unsigned keys)
{
	ignored |= keys;
}
