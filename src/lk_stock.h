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
void lk_console_clear(void);

/* The console frame buffer state, set up by the stock lk (f_04381c). */
struct lk_fbcon {
	uint8_t *fb;		/* CPU address of the first pixel; rows are width * bytes_pp apart */
	uint32_t size;
	uint32_t colours;	/* console colour scheme (3: grey on black) */
	uint32_t bytes_pp;
	uint32_t width;
	uint32_t height;
};
extern struct lk_fbcon *volatile lk_fbcon;
extern const uint8_t lk_font_8x16[256 * 16];
void lk_disp_update(int x, int y, int w, int h);
extern volatile uint8_t lk_disp_ready;		/* bit 0: the display has its frame buffer */
extern uint8_t *volatile lk_disp_fb;		/* the frame buffer (three pages) */
extern volatile uint32_t lk_disp_fb_offset;	/* the console page in it */
void lk_wdt_kick(void);
void lk_wdt_enable(int on);
void lk_fastboot_register(const char *prefix, void *handler, int a, int b);
void lk_fastboot_fail(const char *reason);
void lk_fastboot_okay(const char *info);
void lk_fb_cmd_continue(const char *arg, void *data, unsigned size);
void lk_boot_images_check(void *boot, void *vendor_boot, void *init_boot);
void lk_boot_images_set(void *boot, void *vendor_boot, void *init_boot);
int lk_mblock_free(unsigned long pa);
int lk_vmm_free_region(void *aspace, void *va);
int lk_dprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern volatile uint8_t lk_boot_mode;
/* lk_boot_mode values (set before the mt_boot app runs, f_00d3d0). */
enum {
	LK_MODE_NORMAL = 0,
	LK_MODE_META = 1,
	LK_MODE_RECOVERY = 2,	/* misc message, RGU recovery mode or volume down held */
	LK_MODE_FACTORY = 4,	/* volume up held */
	LK_MODE_ADVMETA = 5,
	LK_MODE_ATE_FACTORY = 6,
	LK_MODE_KPOC = 8,	/* switched on by the charger cable */
	LK_MODE_FASTBOOT = 99,	/* RGU bootloader mode */
	LK_MODE_DOWNLOAD = 100,
};

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
