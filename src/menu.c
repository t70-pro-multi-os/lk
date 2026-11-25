/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Boot menu on the stock lk text console, under the orange-state warning.
 * Volume keys move, power selects. Text is ASCII: the lk font has no Hangul.
 */
#include "lk_stock.h"
#include "t70.h"

#define POLL_US		20000u
#define IDLE_US		60000000ull	/* no key for 60 s: continue booting */
#define WIDTH		52

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

/* First row below the stock warning (rows/2: "Orange State", blank, the
 * warning line), with one blank row between. */
int t70_menu_row(void)
{
	return lk_console_rows() / 2 + 4;
}

/* Print one full-width line at a console row, padding over older text. */
static void line(int row, const char *prefix, const char *text, const char *arg)
{
	char buf[WIDTH + 1];
	int n = 0;

	for (const char *s = prefix; *s && n < WIDTH; s++)
		buf[n++] = *s;
	for (const char *s = text; *s && n < WIDTH; s++) {
		if (*s == '%' && s[1] == 's' && arg) {
			for (const char *a = arg; *a && n < WIDTH; a++)
				buf[n++] = *a;
			s++;
			continue;
		}
		buf[n++] = *s;
	}
	while (n < WIDTH)
		buf[n++] = ' ';
	buf[n] = 0;
	lk_console_goto(row, 0);
	lk_console_printf("%s\n", buf);
}

static const char *slot_name(const char *suffix)
{
	return (suffix && suffix[0] == '_' && suffix[1] == 'b') ? "B" : "A";
}

static void wait_label(uint32_t ms, char out[8])
{
	out[0] = (char)('0' + ms / 1000);
	out[1] = ' ';
	out[2] = 's';
	out[3] = 0;
}

static void draw(int sel, const struct t70_cfg *cfg)
{
	static const char *const text[ACT_COUNT] = {
		[ACT_CONTINUE] = "Continue: boot slot %s",
		[ACT_OTHER_SLOT] = "Switch to slot %s and reboot",
		[ACT_RECOVERY] = "Recovery",
		[ACT_FASTBOOT] = "Fastboot (bootloader)",
		[ACT_FASTBOOTD] = "Fastbootd (userspace)",
		[ACT_WAIT] = "Menu key wait: %s (select to change)",
		[ACT_REBOOT] = "Reboot",
		[ACT_POWEROFF] = "Power off",
	};
	const char *here = slot_name(lk_get_slot_suffix());
	const char *other = here[0] == 'A' ? "B" : "A";
	char wait[8];
	int row = t70_menu_row();

	wait_label(cfg->wait_ms, wait);
	line(row++, "", "T70 boot menu   VOL: move   POWER: select", 0);
	line(row++, "", "", 0);
	for (int i = 0; i < ACT_COUNT; i++) {
		const char *arg = i == ACT_CONTINUE ? here : i == ACT_OTHER_SLOT ? other :
				  i == ACT_WAIT ? wait : 0;
		line(row++, i == sel ? " > " : "   ", text[i], arg);
	}
	line(row++, "", "", 0);
	lk_display_update();
}

static void status(const char *text, const char *arg)
{
	line(t70_menu_row() + ACT_COUNT + 3, "", text, arg);
	lk_display_update();
}

static void clear_menu(void)
{
	for (int row = t70_menu_row(); row < t70_menu_row() + ACT_COUNT + 4; row++)
		line(row, "", "", 0);
	lk_display_update();
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
		lk_delay_us(POLL_US);
	}
}

static int do_action(int act, struct t70_cfg *cfg)
{
	const char *here = lk_get_slot_suffix();

	switch (act) {
	case ACT_CONTINUE:
		return 1;
	case ACT_OTHER_SLOT: {
		const char *target = (here && here[1] == 'b') ? "_a" : "_b";

		status("Setting slot %s active...", slot_name(target));
		if (lk_set_active_slot(target)) {
			status("Could not set slot %s active", slot_name(target));
			return 0;
		}
		status("Slot %s is active. Rebooting...", slot_name(target));
		lk_platform_halt(LK_HALT_REBOOT, LK_REASON_COLD);
	}
	case ACT_RECOVERY:
	case ACT_FASTBOOTD:
		status("Rebooting to %s...", act == ACT_RECOVERY ? "recovery" : "fastbootd");
		if (t70_misc_command(act == ACT_RECOVERY ? "boot-recovery" : "boot-fastboot")) {
			status("Could not write the misc message", 0);
			return 0;
		}
		lk_rgu_set_boot_mode(LK_RGU_RECOVERY);
		lk_platform_halt(LK_HALT_REBOOT, LK_REASON_SW_RESET);
	case ACT_FASTBOOT:
		status("Rebooting to fastboot...", 0);
		lk_rgu_set_boot_mode(LK_RGU_BOOTLOADER);
		lk_platform_halt(LK_HALT_REBOOT, LK_REASON_SW_RESET);
	case ACT_WAIT: {
		unsigned i = 0;
		char wait[8];

		while (i < sizeof(wait_choices) / sizeof(wait_choices[0]) && wait_choices[i] != cfg->wait_ms)
			i++;
		cfg->wait_ms = wait_choices[(i + 1) % (sizeof(wait_choices) / sizeof(wait_choices[0]))];
		wait_label(cfg->wait_ms, wait);
		status(t70_cfg_save(cfg) ? "Could not save to misc" : "Saved: menu key wait %s", wait);
		return 0;
	}
	case ACT_REBOOT:
		status("Rebooting...", 0);
		lk_platform_halt(LK_HALT_REBOOT, LK_REASON_COLD);
	case ACT_POWEROFF:
		status("Powering off...", 0);
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

		prev = keys;
		if (pressed)
			idle = lk_time_us();
		else if (lk_time_us() - idle > IDLE_US)
			break;
		if (pressed & T70_KEY_UP) {
			sel = (sel + ACT_COUNT - 1) % ACT_COUNT;
			draw(sel, cfg);
		} else if (pressed & (T70_KEY_DOWN | T70_KEY_OTHER)) {
			sel = (sel + 1) % ACT_COUNT;
			draw(sel, cfg);
		} else if (pressed & T70_KEY_POWER) {
			lk_dprintf("t70: menu action %d\n", sel);
			if (do_action(sel, cfg))
				break;
			wait_release();
			prev = 0;
			draw(sel, cfg);
		}
		lk_delay_us(POLL_US);
	}
	clear_menu();
}
