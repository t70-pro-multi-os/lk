/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stock lk (G3D1 V1.08) functions and data used by our code; see names.txt. */
#pragma once
#include <stddef.h>
#include <stdint.h>

uint64_t lk_time_us(void);
void lk_delay_us(uint32_t us);
int lk_detect_key(int key);
void lk_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void lk_console_goto(int row, int col);
int lk_console_rows(void);
void lk_display_update(void);
int lk_dprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern volatile uint8_t lk_boot_mode;

/* lk_detect_key() indices (stock key table at data 0xbc530). */
enum lk_key {
	LK_KEY_KPD_CODE17 = 0,	/* keypad code 17: no such key on the T70 */
	LK_KEY_KPD_CODE0 = 1,	/* keypad code 0 */
	LK_KEY_POWER = 2,	/* PMIC power key */
	LK_KEY_PMIC_HOME = 3,	/* PMIC second key */
};

const char *lk_get_slot_suffix(void);
int lk_set_active_slot(const char *suffix);

enum { LK_HALT_REBOOT = 1, LK_HALT_SHUTDOWN = 2 };
enum { LK_REASON_COLD = 1, LK_REASON_SW_RESET = 7 };
void lk_platform_halt(int action, int reason) __attribute__((noreturn));

/* Boot mode for the next boot, kept in TOPRGU across a watchdog reset. */
enum { LK_RGU_RECOVERY = 2, LK_RGU_BOOTLOADER = 3 };
void lk_rgu_set_boot_mode(uint8_t mode);

void *lk_bio_open(const char *name);
long lk_bio_read(void *dev, void *buf, long offset, unsigned long len);
long lk_bio_write(void *dev, const void *buf, long offset, unsigned long len);
void lk_bio_close(void *dev);
void *lk_malloc(unsigned long size);
void lk_free(void *ptr);
void *memset(void *s, int c, size_t n);
