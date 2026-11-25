/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host test of the boot menu logic (src/orange_wait.c, menu.c, config.c,
 * gfx.c) with the stock lk functions replaced by a simulation: a clock,
 * scripted key presses, a misc partition in memory, the slot, the halt
 * request and a 2240x1400 frame buffer (AddressSanitizer checks every
 * pixel written is inside it).
 * Build and run: make test
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lk_stock.h"
#include "t70.h"

/* ---- simulation ---- */
static uint64_t now_us;
static struct press { uint64_t from, to; unsigned keys; } script[32];
static int nscript;
static uint8_t misc[512 * 1024];
static char slot[3] = "_a";
static int halted, halt_action, halt_reason, rgu_mode = -1, flushes;
static jmp_buf halt_jump;
static uint64_t last_kick, kick_gap;
static char screen[65536];	/* every text drawn, one per line */
volatile uint8_t lk_boot_mode;

static int kpd_dead;
volatile uint16_t t70_test_kpd_mem1 = 0xffff;

static void press(uint64_t at_ms, uint64_t len_ms, unsigned keys)
{
	script[nscript++] = (struct press){ at_ms * 1000, (at_ms + len_ms) * 1000, keys };
}

static unsigned held(void)
{
	unsigned k = 0;
	for (int i = 0; i < nscript; i++)
		if (now_us >= script[i].from && now_us < script[i].to)
			k |= script[i].keys;
	return k;
}

/* The keypad register as the hardware shows it (active low). */
static void sync_kpd(void)
{
	unsigned k = held();
	t70_test_kpd_mem1 = kpd_dead ? 0 : (uint16_t)(0xffff & ~((k & T70_KEY_UP) ? 1 : 0) & ~((k & T70_KEY_DOWN) ? 2 : 0));
}

uint64_t lk_time_us(void) { return now_us; }
void lk_wdt_kick(void) { last_kick = now_us; }
void lk_delay_us(uint32_t us)
{
	now_us += us;
	if (now_us - last_kick > kick_gap)
		kick_gap = now_us - last_kick;
	sync_kpd();
}
int lk_detect_key(int key)
{
	return (key == LK_KEY_POWER && (held() & T70_KEY_POWER)) ||
	       (key == LK_KEY_PMIC_HOME && (held() & T70_KEY_OTHER));
}
static struct lk_fbcon fbcon = { NULL, 2240 * 1400 * 4, 3, 4, 2240, 1400 };
struct lk_fbcon *volatile lk_fbcon = &fbcon;
extern uint8_t *const sim_font;
void lk_disp_update(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; flushes++; }
void t70_test_text(const char *s)
{
	if (strlen(screen) + strlen(s) + 1 < sizeof(screen) - 1) {
		strcat(screen, s);
		strcat(screen, "\n");
	}
}
static int fb_black(void)
{
	for (uint32_t i = 0; i < fbcon.size; i++)
		if (fbcon.fb[i])
			return 0;
	return 1;
}
int lk_dprintf(const char *fmt, ...) { (void)fmt; return 0; }
const char *lk_get_slot_suffix(void) { return slot; }
int lk_set_active_slot(const char *suffix)
{
	if (strcmp(suffix, "_a") && strcmp(suffix, "_b"))
		return -1;
	misc[0x800] = suffix[0];	/* stands in for boot_ctrl */
	misc[0x801] = suffix[1];
	return 0;
}
void lk_platform_halt(int action, int reason)
{
	halted = 1;
	halt_action = action;
	halt_reason = reason;
	longjmp(halt_jump, 1);
}
void lk_rgu_set_boot_mode(uint8_t mode) { rgu_mode = mode; }
void *lk_bio_open(const char *name) { return strcmp(name, "misc") ? NULL : misc; }
long lk_bio_read(void *dev, void *buf, long off, unsigned long len)
{
	memcpy(buf, (uint8_t *)dev + off, len);
	return (long)len;
}
long lk_bio_write(void *dev, const void *buf, long off, unsigned long len)
{
	memcpy((uint8_t *)dev + off, buf, len);
	return (long)len;
}
void lk_bio_close(void *dev) { (void)dev; }
void *lk_malloc(unsigned long size) { return malloc(size); }
void lk_free(void *p) { free(p); }

void t70_orange_wait(void);

/* ---- cases ---- */
static int failures;
static const char *name;

static void reset(void)
{
	now_us = 0;
	nscript = 0;
	halted = halt_action = halt_reason = 0;
	rgu_mode = -1;
	flushes = 0;
	if (fbcon.fb)
		memset(fbcon.fb, 0x5a, fbcon.size);	/* what was on the screen before */
	last_kick = kick_gap = 0;
	screen[0] = 0;
	kpd_dead = 0;
	sync_kpd();
}

static void run(void)
{
	sync_kpd();
	if (!setjmp(halt_jump))
		t70_orange_wait();
	/* The stock lk sets the watchdog to 30 s: every wait must restart it. */
	if (kick_gap > 100000) {
		printf("FAIL %s: watchdog not restarted for %llu ms\n", name, (unsigned long long)(kick_gap / 1000));
		failures++;
	}
}

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s: %s\n", name, what); failures++; } } while (0)

int main(void)
{
	/* a font with bits in every glyph, so all the drawing paths run */
	for (int i = 0; i < 256 * 16; i++)
		sim_font[i] = (uint8_t)(0x18 | (i * 37));
	fbcon.fb = malloc(fbcon.size);
	enum { UP = T70_KEY_UP, DOWN = T70_KEY_DOWN, PWR = T70_KEY_POWER };

	name = "no key: stock 5 s wait, then boot";
	reset(); run();
	CHECK(!halted, "halted");
	CHECK(now_us >= 5000000 && now_us < 5100000, "wait is not 5 s");
	CHECK(strstr(screen, "Press any key for boot options") != NULL, "no hint");
	CHECK(flushes > 40 && fb_black(), "bar not running or screen not left black");

	name = "key in the wait, POWER on Continue: boot this slot";
	reset(); press(1000, 200, DOWN); press(2000, 100, PWR); run();
	CHECK(!halted, "halted");
	CHECK(strstr(screen, "Boot options") != NULL, "menu not drawn");
	CHECK(strstr(screen, "Continue booting\nslot A\n") != NULL, "slot A not shown");
	CHECK(fb_black(), "screen not black after the menu");

	name = "switch to slot B";
	reset(); memset(misc, 0, sizeof(misc)); strcpy(slot, "_a");
	press(500, 100, UP); press(1000, 100, DOWN); press(1500, 100, PWR); run();
	CHECK(halted && halt_action == LK_HALT_REBOOT && halt_reason == LK_REASON_COLD, "no cold reboot");
	CHECK(misc[0x800] == '_' && misc[0x801] == 'b', "slot b not set");

	name = "from slot B, switch to slot A";
	reset(); strcpy(slot, "_b");
	press(500, 100, PWR); press(1000, 100, DOWN); press(1500, 100, PWR); run();
	CHECK(halted && misc[0x801] == 'a', "slot a not set");
	CHECK(strstr(screen, "Continue booting\nslot B\n") && strstr(screen, "Switch to slot A"), "labels");
	CHECK(strstr(screen, "Slot A is active") != NULL, "no message before the restart");
	strcpy(slot, "_a");

	name = "recovery";
	reset(); memset(misc, 0, 0x800);
	press(100, 100, DOWN); for (int i = 0; i < 2; i++) press(400 + i * 300, 100, DOWN);
	press(1500, 100, PWR); run();
	CHECK(halted && halt_reason == LK_REASON_SW_RESET && rgu_mode == LK_RGU_RECOVERY, "no recovery reboot");
	CHECK(!strcmp((char *)misc, "boot-recovery"), "misc command");

	name = "fastboot (bootloader), going up past the top";
	reset();
	press(100, 100, UP); for (int i = 0; i < 5; i++) press(400 + i * 300, 100, UP);
	press(2500, 100, PWR); run();
	CHECK(halted && rgu_mode == LK_RGU_BOOTLOADER && halt_reason == LK_REASON_SW_RESET, "no fastboot reboot");

	name = "fastbootd";
	reset(); memset(misc, 0, 0x800);
	press(100, 100, UP); for (int i = 0; i < 4; i++) press(400 + i * 300, 100, DOWN);
	press(2500, 100, PWR); run();
	CHECK(halted && rgu_mode == LK_RGU_RECOVERY && !strcmp((char *)misc, "boot-fastboot"), "no fastbootd");

	name = "wait setting 5 -> 3 s, saved in misc, used next boot";
	reset(); memset(misc, 0, sizeof(misc));
	press(100, 100, UP); for (int i = 0; i < 5; i++) press(400 + i * 300, 100, DOWN);
	press(2500, 100, PWR); press(3000, 100, UP); for (int i = 0; i < 5; i++) press(3300 + i * 300, 100, DOWN);
	run();
	CHECK(!halted, "halted");
	{
		struct t70_cfg c;
		t70_cfg_load(&c);
		CHECK(c.wait_ms == 3000, "not saved as 3 s");
		uint32_t magic;
		memcpy(&magic, misc + T70_CFG_OFFSET, 4);
		CHECK(magic == T70_CFG_MAGIC, "not at misc 0x3000");
		CHECK(misc[0] == 0 && misc[0x800] == 0, "touched other misc data");
	}
	reset(); run();
	CHECK(now_us >= 3000000 && now_us < 3100000, "next boot does not wait 3 s");

	name = "corrupt setting falls back to 5 s";
	misc[T70_CFG_OFFSET + 8] ^= 1;
	reset(); run();
	CHECK(now_us >= 5000000 && now_us < 5100000, "no fallback");

	name = "menu idle 60 s: continue";
	reset(); press(100, 100, DOWN); run();
	CHECK(!halted && now_us > 60000000, "did not continue");

	name = "power off";
	reset(); press(100, 100, UP); press(500, 100, UP); press(1000, 100, PWR); run();
	CHECK(halted && halt_action == LK_HALT_SHUTDOWN, "no power off");

	name = "volume key held while booting opens the menu";
	reset(); press(0, 300, DOWN); press(1000, 100, PWR); run();
	CHECK(!halted && strstr(screen, "Boot options"), "menu not opened / held key selected");

	name = "power still held from switching on does not open the menu";
	reset(); press(0, 1500, PWR); run();
	CHECK(!halted && !strstr(screen, "Boot options"), "menu opened");
	CHECK(now_us >= 5000000 && now_us < 5100000, "not the normal wait");

	name = "power pressed again after release opens the menu";
	reset(); press(0, 800, PWR); press(2000, 100, PWR); press(3000, 100, PWR); run();
	CHECK(!halted && strstr(screen, "Boot options"), "menu not opened");

	name = "keypad reading 0 (not clocked) is ignored";
	reset(); kpd_dead = 1; run();
	CHECK(!halted && !strstr(screen, "Boot options") && now_us >= 5000000, "menu opened");

	name = "a stuck key is ignored after 3 s";
	reset(); press(100, 1000000, DOWN); press(5000, 100, PWR); run();
	CHECK(!halted && strstr(screen, "Boot options") && now_us < 10000000, "menu stuck");

	name = "wait 0, no key: no wait, screen left as it is";
	{
		struct t70_cfg c;
		t70_cfg_load(&c);
		c.wait_ms = 0;
		t70_cfg_save(&c);
	}
	reset(); run();
	CHECK(!halted && flushes == 0 && !screen[0] && now_us < 100000, "waited or drew");

	name = "wait 0, volume held while booting: menu";
	reset(); press(0, 300, DOWN); press(1000, 100, PWR); run();
	CHECK(!halted && strstr(screen, "Boot options") && strstr(screen, "off"), "menu not opened");
	memset(misc + T70_CFG_OFFSET, 0, 512);

	name = "no frame buffer: the wait only, no menu";
	lk_fbcon = NULL;
	reset(); press(1000, 100, DOWN); press(2000, 100, PWR); run();
	CHECK(!halted && !screen[0] && flushes == 0 && now_us >= 5000000 && now_us < 5100000, "drew or did not wait");
	lk_fbcon = &fbcon;

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures != 0;
}
