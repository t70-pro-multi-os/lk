/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Called where the stock lk waits 5 s under the orange-state warning.
 * For now it keeps the stock behaviour exactly: a 5 s busy wait. The boot
 * menu ("any key during the wait") is built on this hook.
 */
#include "lk_stock.h"

#define ORANGE_WAIT_US 5000000u

void t70_orange_wait(void)
{
	lk_delay_us(ORANGE_WAIT_US);
}
