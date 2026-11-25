/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Boot menu, drawn into the stock lk frame buffer (gfx.c): a title, the
 * choices in groups, the selected one on a highlighted bar, and the keys at
 * the bottom. Volume keys move, power selects. Text is ASCII and code page
 * 437 symbols: the stock font has no Hangul.
 */
#include "gfx.h"
#include "lk_stock.h"
#include "t70.h"

#define IDLE_US		60000000ull	/* no key for 60 s: continue booting */

enum action {
	ACT_CONTINUE,
	ACT_OTHER_SLOT,
	ACT_RECOVERY,
	ACT_FASTBOOT,
	ACT_FASTBOOTD,
	ACT_WAIT,
	ACT_REBOOT,
	ACT_POWEROFF,
	ACT_COUNT
};

static const uint32_t wait_choices[] = { 5000, 3000, 1000, 0 };
static int dimmed = 0;	/* the menu behind a message box is darkened (in .data) */

/* Colours: greys and one green that stays green whichever order the panel
 * takes red and blue in (not confirmed on the device yet). */
#define C_BG		0x111111
#define C_ROW		0x262626
#define C_LINE		0x303030
#define C_DIALOG	0x1c1c1c
#define C_TEXT		0xf5f5f5
#define C_TEXT_DIM	0xc2c2c2
#define C_TEXT2		0x8c8c8c
#define C_TEXT3		0x5f5f5f
#define C_ACCENT	0x4cd480

/* Layout for the 2240x1400 panel: one centred column. */
#define COL_W		1280
#define ROW_H		76
#define LIST_Y		300
#define KEY_GAP		18	/* key cap to its meaning */
#define PAIR_GAP	72	/* between the two key hints */

static const char *const group[ACT_COUNT] = {
	[ACT_CONTINUE] = "START",
	[ACT_RECOVERY] = "MODES",
	[ACT_WAIT] = "SETTINGS",
	[ACT_REBOOT] = "POWER",
};

static int col_x(void)
{
	return (gfx_w() - COL_W) / 2;
}

static int item_y(int item)
{
	int y = LIST_Y;

	for (int i = 0;; i++) {
		if (group[i])
			y += (i ? 30 : 0) + 32 + 14;	/* gap, group title, gap */
		if (i == item)
			return y;
		y += ROW_H;
	}
}

static const char *slot_name(const char *suffix)
{
	return (suffix && suffix[0] == '_' && suffix[1] == 'b') ? "B" : "A";
}

/* dst = a + b + c; dst has room for 47 characters */
static const char *join(char *dst, const char *a, const char *b, const char *c)
{
	const char *part[3] = { a, b, c };
	int n = 0;

	for (int k = 0; k < 3; k++)
		for (const char *p = part[k]; p && *p && n < 47; p++)
			dst[n++] = *p;
	dst[n] = 0;
	return dst;
}

static const char *wait_text(uint32_t ms, char *buf)
{
	if (!ms)
		return "off";
	buf[0] = (char)('0' + ms / 1000);
	buf[1] = ' ';
	buf[2] = 's';
	buf[3] = 0;
	return buf;
}

static void draw_item(int i, int selected, const struct t70_cfg *cfg)
{
	const char *here = slot_name(lk_get_slot_suffix());
	const char *other = here[0] == 'A' ? "B" : "A";
	char lbuf[48], rbuf[48], wbuf[8];
	const char *label = "", *right = "";
	int x = col_x(), y = item_y(i);

	switch (i) {
	case ACT_CONTINUE:
		label = "Continue booting";
		right = join(rbuf, "slot ", here, 0);
		break;
	case ACT_OTHER_SLOT:
		label = join(lbuf, "Switch to slot ", other, 0);
		right = "restarts";
		break;
	case ACT_RECOVERY:
		label = "Recovery";
		break;
	case ACT_FASTBOOT:
		label = "Fastboot";
		right = "bootloader";
		break;
	case ACT_FASTBOOTD:
		label = "Fastbootd";
		right = "userspace";
		break;
	case ACT_WAIT:
		label = "Key wait at boot";
		right = wait_text(cfg->wait_ms, wbuf);
		break;
	case ACT_REBOOT:
		label = "Restart";
		break;
	case ACT_POWEROFF:
		label = "Power off";
		break;
	}
	gfx_fill(x, y, COL_W, ROW_H, C_BG);
	if (selected) {
		gfx_round(x, y, COL_W, ROW_H, 14, C_ROW);
		gfx_round(x + 16, y + 20, 6, ROW_H - 40, 3, C_ACCENT);
	}
	/* 3x label and 2x note on one baseline (row 12 of the 16-row font) */
	int ty = y + (ROW_H - 48) / 2;
	gfx_text(x + 48, ty, 3, selected ? C_TEXT : C_TEXT_DIM, label);
	if (right[0])
		gfx_text(x + COL_W - 40 - gfx_text_w(2, right), ty + 12, 2,
			 selected ? C_ACCENT : C_TEXT2, right);
}

static int keycap_w(const char *key)
{
	return gfx_text_w(2, key) + 32;
}

static int keycap(int x, int y, const char *key)
{
	int w = keycap_w(key), h = 52;

	gfx_round(x, y, w, h, 10, C_LINE);
	gfx_round(x + 2, y + 2, w - 4, h - 4, 8, C_BG);
	gfx_text(x + 16, y + 10, 2, C_TEXT_DIM, key);
	return w;
}

static void draw(int sel, const struct t70_cfg *cfg)
{
	static const char *const hint[2][2] = { { "VOL \x1e\x1f", "Move" }, { "POWER", "Select" } };
	const char *here = slot_name(lk_get_slot_suffix());
	char sub[48];
	int x = col_x();

	dimmed = 0;
	gfx_fill(0, 0, gfx_w(), gfx_h(), C_BG);
	gfx_text(x, 112, 4, C_TEXT, "Boot options");
	gfx_text(x, 200, 2, C_TEXT2, join(sub, "T70 Pro   \x07   slot ", here, "   \x07   unlocked"));
	gfx_fill(x, 262, COL_W, 2, C_LINE);
	for (int i = 0; i < ACT_COUNT; i++) {
		if (group[i])
			gfx_text(x + 16, item_y(i) - 46, 2, C_TEXT3, group[i]);
		draw_item(i, i == sel, cfg);
	}
	/* the keys, centred at the bottom */
	int w = PAIR_GAP;
	for (int k = 0; k < 2; k++)
		w += keycap_w(hint[k][0]) + KEY_GAP + gfx_text_w(2, hint[k][1]);
	int kx = (gfx_w() - w) / 2, ky = gfx_h() - 136;
	for (int k = 0; k < 2; k++) {
		kx += keycap(kx, ky, hint[k][0]) + KEY_GAP;
		kx += gfx_text(kx, ky + 10, 2, C_TEXT2, hint[k][1]) + PAIR_GAP;
	}
	gfx_flush();
}

/* A message box in the middle of the screen. */
static void notice(const char *title, const char *text)
{
	int w = 1040, h = 280, x = (gfx_w() - w) / 2, y = (gfx_h() - h) / 2;

	if (!dimmed)
		gfx_dim();
	dimmed = 1;
	gfx_round(x, y, w, h, 24, C_LINE);
	gfx_round(x + 2, y + 2, w - 4, h - 4, 22, C_DIALOG);
	gfx_text_center(y + 76, 3, C_TEXT, title);
	gfx_text_center(y + 164, 2, C_TEXT2, text);
	gfx_flush();
}

static void pause_ms(unsigned ms)
{
	uint64_t start = lk_time_us();

	while (lk_time_us() - start < (uint64_t)ms * 1000)
		t70_poll_delay();
}

/* Wait until the keys are released; a key stuck for 3 s is ignored from then on. */
static void wait_release(void)
{
	uint64_t start = lk_time_us();
	unsigned keys;

	while ((keys = t70_keys())) {
		if (lk_time_us() - start > 3000000) {
			lk_dprintf("t70: keys %x stuck, ignored\n", keys);
			t70_keys_ignore_held(keys);
			break;
		}
		t70_poll_delay();
	}
}

static int do_action(int act, struct t70_cfg *cfg)
{
	const char *here = lk_get_slot_suffix();
	char t[48];

	switch (act) {
	case ACT_CONTINUE:
		return 1;
	case ACT_OTHER_SLOT: {
		const char *target = (here && here[1] == 'b') ? "_a" : "_b";

		notice(join(t, "Switching to slot ", slot_name(target), 0), "Setting the active slot");
		if (lk_set_active_slot(target)) {
			notice(join(t, "Could not switch to slot ", slot_name(target), 0), "Back to the menu");
			pause_ms(2500);
			return 0;
		}
		notice(join(t, "Slot ", slot_name(target), " is active"), "Restarting...");
		lk_platform_halt(LK_HALT_REBOOT, LK_REASON_COLD);
	}
	case ACT_RECOVERY:
	case ACT_FASTBOOTD:
		notice(act == ACT_RECOVERY ? "Restarting to recovery" : "Restarting to fastbootd", "Please wait");
		if (t70_misc_command(act == ACT_RECOVERY ? "boot-recovery" : "boot-fastboot")) {
			notice("Could not write to misc", "Back to the menu");
			pause_ms(2500);
			return 0;
		}
		lk_rgu_set_boot_mode(LK_RGU_RECOVERY);
		lk_platform_halt(LK_HALT_REBOOT, LK_REASON_SW_RESET);
	case ACT_FASTBOOT:
		notice("Restarting to fastboot", "Please wait");
		lk_rgu_set_boot_mode(LK_RGU_BOOTLOADER);
		lk_platform_halt(LK_HALT_REBOOT, LK_REASON_SW_RESET);
	case ACT_WAIT: {
		unsigned i = 0, n = sizeof(wait_choices) / sizeof(wait_choices[0]);

		while (i < n && wait_choices[i] != cfg->wait_ms)
			i++;
		cfg->wait_ms = wait_choices[(i + 1) % n];
		if (t70_cfg_save(cfg)) {
			notice("Could not save the setting", "misc partition");
			pause_ms(2500);
		}
		return 0;
	}
	case ACT_REBOOT:
		notice("Restarting", "Please wait");
		lk_platform_halt(LK_HALT_REBOOT, LK_REASON_COLD);
	case ACT_POWEROFF:
		notice("Powering off", "Goodbye");
		lk_platform_halt(LK_HALT_SHUTDOWN, 0);
	}
	return 0;
}

void t70_menu(struct t70_cfg *cfg)
{
	int sel = ACT_CONTINUE;
	unsigned prev;
	uint64_t idle;

	lk_dprintf("t70: boot menu\n");
	draw(sel, cfg);
	wait_release();
	prev = 0;
	idle = lk_time_us();
	for (;;) {
		unsigned keys = t70_keys();
		unsigned pressed = keys & ~prev;
		int old = sel;

		prev = keys;
		if (pressed)
			idle = lk_time_us();
		else if (lk_time_us() - idle > IDLE_US)
			break;
		if (pressed & T70_KEY_UP) {
			sel = (sel + ACT_COUNT - 1) % ACT_COUNT;
		} else if (pressed & (T70_KEY_DOWN | T70_KEY_OTHER)) {
			sel = (sel + 1) % ACT_COUNT;
		} else if (pressed & T70_KEY_POWER) {
			lk_dprintf("t70: menu action %d\n", sel);
			if (do_action(sel, cfg))
				break;
			wait_release();
			prev = 0;
			draw(sel, cfg);
		}
		if (sel != old) {
			draw_item(old, 0, cfg);
			draw_item(sel, 1, cfg);
			gfx_flush();
		}
		t70_poll_delay();
	}
	gfx_fill(0, 0, gfx_w(), gfx_h(), 0);	/* black, as the stock warning left it */
	gfx_flush();
}
