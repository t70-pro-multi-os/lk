/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* keys.c */
enum t70_key { T70_KEY_UP = 1, T70_KEY_DOWN = 2, T70_KEY_POWER = 4, T70_KEY_OTHER = 8 };
#define T70_POLL_US 20000u
unsigned t70_keys(void);
void t70_poll_delay(void);
void t70_keys_ignore_held(unsigned keys);

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

/* menu.c */
int t70_menu_row(void);
void t70_menu(struct t70_cfg *cfg);
