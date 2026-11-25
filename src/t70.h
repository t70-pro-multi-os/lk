/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* keys.c */
enum t70_key { T70_KEY_UP = 1, T70_KEY_DOWN = 2, T70_KEY_POWER = 4, T70_KEY_OTHER = 8 };
#define T70_POLL_US 20000u
unsigned t70_keys(void);
void t70_poll_delay(void);
void t70_keys_ignore_held(unsigned keys);
uint16_t t70_kpd_mem1(void);	/* the raw keypad register, for the diagnostic build */

/* config.c: our settings in misc (vendor bootloader space). */
#define T70_CFG_OFFSET	0x3000	/* misc 2-16 KiB is for the bootloader; 0-0x900 is in use */
#define T70_CFG_MAGIC	0x4d303754u	/* "T70M" */
#define T70_CFG_VERSION	1

struct t70_cfg {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	uint32_t wait_ms;	/* how long "any key" opens the menu */
	uint32_t reserved[12];
	uint32_t crc32;		/* over everything before it */
};

void t70_cfg_load(struct t70_cfg *cfg);
int t70_cfg_save(const struct t70_cfg *cfg);
int t70_misc_command(const char *command);
int t70_misc_rw(long offset, uint8_t *heap_buf, unsigned len, int write);

/* diag.c: diagnostic build only (make DIAG=1). What the boot wait saw, in misc. */
#define T70_DIAG_OFFSET	0x3200	/* one 512-byte block after our settings */
#define T70_DIAG_MAGIC	0x44303754u	/* "T70D" */

struct t70_diag {
	uint32_t magic;
	uint32_t seq;		/* counts boots of the diagnostic build */
	uint32_t size_of;
	uint32_t stage;		/* last step reached (orange_wait.c, enum diag_stage) */
	uint64_t t_entry;	/* lk_time_us() at the first record */
	uint64_t t_last;	/* at this record */
	uint64_t fbcon;		/* lk_fbcon */
	uint64_t fb;		/* lk_fbcon fields */
	uint32_t size, colours, bytes_pp, width, height;
	uint32_t disp_ready;	/* lk_disp_ready */
	uint64_t disp_fb;	/* lk_disp_fb */
	uint32_t disp_off;	/* lk_disp_fb_offset */
	uint32_t boot_mode;	/* lk_boot_mode */
	int32_t gfx;		/* gfx_init() */
	uint32_t wait_ms;	/* from our settings */
	uint32_t keys_start;	/* t70_keys() when the wait began */
	uint32_t flushes;	/* gfx_flush() calls */
	uint32_t misc_write;	/* result of the previous record's write (0 ok) */
	uint32_t px[4];		/* read back after drawing the countdown, see orange_wait.c */
	uint32_t kpd_samples;	/* raw keypad register reads (t70_diag_keys) */
	uint32_t kpd_and;	/* all reads ANDed: bits ever 0 (pressed) are 0 */
	uint32_t kpd_or;	/* all reads ORed */
	uint32_t det_or;	/* bit n: stock lk_detect_key(n) seen pressed, n = 0..3 */
};

#ifdef T70_DIAG
extern struct t70_diag t70_diag_rec;
void t70_diag(unsigned stage);
int t70_diag_keys(int y);	/* show the raw keys on a line at y; 1 if redrawn */
#endif

/* download.c: USB download modes for PC tools */
void t70_reboot_brom(unsigned seconds) __attribute__((noreturn));
void t70_reboot_preloader(void) __attribute__((noreturn));

/* fastboot.c */
int t70_fastboot_start(void);

/* menu.c */
void t70_menu(struct t70_cfg *cfg);
