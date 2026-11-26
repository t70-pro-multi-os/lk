/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host test of the boot menu logic (src/orange_wait.c, menu.c, config.c,
 * gfx.c) with the stock lk functions replaced by a simulation: a clock,
 * scripted key presses, a misc partition in memory, the slot, the halt
 * request and a 2240x1400 frame buffer (AddressSanitizer checks every
 * pixel written is inside it). Built twice: the normal build, and the
 * diagnostic one (-DT70_DIAG, its own cases).
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
static int kpd_sparse;	/* keypad bits without a key read 0, not 1 */
volatile uint16_t t70_test_kpd_mem1 = 0xffff;
volatile uint32_t t70_test_secao[0x44];
volatile uint32_t t70_test_musb[0x1000 / 4];
#define MUSB32(o) t70_test_musb[(o) / 4]
static int musb_stuck;		/* transmit queue 1 never stops */
static int phy_off, phy_off_l1intm;
void lk_usb_phy_off(void) { phy_off++; phy_off_l1intm = MUSB32(0xa4); }

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
	uint16_t idle = kpd_sparse ? 0x0003 : 0xffff;
	t70_test_kpd_mem1 = kpd_dead ? 0 : (uint16_t)(idle & ~((k & T70_KEY_UP) ? 1 : 0) & ~((k & T70_KEY_DOWN) ? 2 : 0));
}

uint64_t lk_time_us(void) { return now_us; }
void lk_wdt_kick(void) { last_kick = now_us; }
static int wdt_on = 1;
void lk_wdt_enable(int on) { wdt_on = on; }
static const char *fb_reply;
static void *boot_set;
static int fb_registered_boot, fb_continued;
static void (*fb_boot_handler)(const char *, void *, unsigned);
void lk_fastboot_register(const char *p, void *h, int a, int b)
{
	(void)a; (void)b;
	if (!strcmp(p, "boot")) {
		fb_registered_boot = 1;
		fb_boot_handler = (void (*)(const char *, void *, unsigned))h;
	}
}
void lk_fastboot_fail(const char *r) { fb_reply = r; }
void lk_fastboot_okay(const char *r) { (void)r; }
void lk_fb_cmd_continue(const char *a, void *d, unsigned s) { (void)a; (void)d; (void)s; fb_continued = 1; }
void lk_boot_images_check(void *b, void *v, void *i) { (void)v; (void)i; boot_set = b; }
void lk_boot_images_set(void *b, void *v, void *i) { (void)v; (void)i; boot_set = b; }
static int buffer_freed;
static void *header_from;
int lk_boot_header_copy(void *i, const char *p) { (void)p; header_from = i; return 0; }
int lk_mblock_free(unsigned long pa) { (void)pa; buffer_freed++; return 0; }
int lk_vmm_free_region(void *a, void *v) { (void)a; (void)v; buffer_freed++; return 0; }
void lk_delay_us(uint32_t us)
{
	now_us += us;
	if (musb_stuck)
		MUSB32(0xa00) |= 0x8000;
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
/* the display state and the stock text console, as the diagnostic build uses them */
volatile uint8_t lk_disp_ready = 1;
uint8_t *volatile lk_disp_fb;
volatile uint32_t lk_disp_fb_offset;
static char console[4096];
int lk_console_rows(void) { return 87; }
void lk_console_goto(int row, int col) { (void)row; (void)col; }
void lk_console_clear(void) { console[0] = 0; }
void lk_console_printf(const char *fmt, ...)
{
	va_list ap;
	size_t n = strlen(console);

	va_start(ap, fmt);
	vsnprintf(console + n, sizeof(console) - n, fmt, ap);
	va_end(ap);
}
void t70_test_text(const char *s)
{
	if (strlen(screen) + strlen(s) + 1 < sizeof(screen) - 1) {
		strcat(screen, s);
		strcat(screen, "\n");
	}
}
/* all zero: transparent, the boot logo shows */
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

/* libavb as the stock lk has it (src/avb.c): avb_slot_verify gives what the
 * case sets; the pool allocator fails past avb_pool bytes. Partitions of the
 * slot live in avb_parts. */
static int avb_result, avb_verify_calls;
static void *avb_data;
static size_t avb_pool, avb_used;
static int avb_reads, avb_unlocked_op = 1;
static struct { const char *name; uint8_t *data; uint64_t size; } avb_parts[4];
int lk_avb_slot_verify(void *o, const char *const *p, const char *s, unsigned f, int m, void **d)
{
	(void)o; (void)p; (void)s; (void)f; (void)m;
	avb_verify_calls++;
	*d = avb_data;
	return avb_result;
}
static uint8_t avb_pool_mem[1 << 20] __attribute__((aligned(64)));
void *lk_avb_malloc(size_t n)
{
	void *p;

	n = (n + 63) & ~(size_t)63;
	if (avb_used + n > avb_pool || avb_used + n > sizeof(avb_pool_mem))
		return NULL;
	p = avb_pool_mem + avb_used;
	avb_used += n;
	return p;
}
void *lk_avb_calloc(size_t n)
{
	void *p = lk_avb_malloc(n);
	return p ? memset(p, 0, n) : NULL;
}
char *lk_avb_strdup(const char *s)
{
	char *p = lk_avb_malloc(strlen(s) + 1);
	return p ? strcpy(p, s) : NULL;
}
struct test_avb_ops {
	void *user_data, *ab_ops, *atx_ops;
	int (*read)(struct test_avb_ops *, const char *, int64_t, size_t, void *, size_t *);
	void *preloaded, *write, *validate_key, *read_rollback, *write_rollback;
	int (*unlocked)(struct test_avb_ops *, _Bool *);
	void *guid;
	int (*size)(struct test_avb_ops *, const char *, uint64_t *);
};
static int avb_find(const char *name)
{
	for (int i = 0; i < 4; i++)
		if (avb_parts[i].name && !strcmp(avb_parts[i].name, name))
			return i;
	return -1;
}
static int avb_op_read(struct test_avb_ops *o, const char *name, int64_t off, size_t n, void *buf, size_t *got)
{
	int i = avb_find(name);
	(void)o;
	if (i < 0 || off < 0 || (uint64_t)off + n > avb_parts[i].size)
		return 2;
	memcpy(buf, avb_parts[i].data + off, n);
	*got = n;
	avb_reads++;
	return 0;
}
static int avb_op_unlocked(struct test_avb_ops *o, _Bool *out) { (void)o; *out = avb_unlocked_op; return 0; }
static int avb_op_size(struct test_avb_ops *o, const char *name, uint64_t *out)
{
	int i = avb_find(name);
	(void)o;
	if (i < 0)
		return 2;
	*out = avb_parts[i].size;
	return 0;
}
static struct test_avb_ops avb_ops = { .read = avb_op_read, .unlocked = avb_op_unlocked, .size = avb_op_size };
struct test_avb_part { char *name; uint8_t *data; size_t size; _Bool preloaded; int verify_result; };
struct test_avb_slot { char *suffix; void *vbmeta; size_t nvbmeta; struct test_avb_part *parts; size_t nparts;
	char *cmdline; uint64_t rollback[32]; int hashtree_mode; };
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

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
	console[0] = 0;
#ifdef T70_DIAG
	memset(&t70_diag_rec, 0, sizeof(t70_diag_rec));	/* a new boot */
#endif
	kpd_dead = 0;
	kpd_sparse = 0;
	lk_boot_mode = LK_MODE_NORMAL;
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
	lk_disp_fb = fbcon.fb;
	enum { UP = T70_KEY_UP, DOWN = T70_KEY_DOWN, PWR = T70_KEY_POWER };
#ifdef T70_DIAG
	/* Diagnostic build: 2 s of stock console text first, then the normal
	 * wait, with a record in misc at every step. */
	struct t70_diag d;

	name = "diag: no key";
	reset(); memset(misc, 0, sizeof(misc)); run();
	memcpy(&d, misc + T70_DIAG_OFFSET, sizeof(d));
	CHECK(!halted && now_us >= 17000000 && now_us < 17200000, "not 2 s + 15 s");
	CHECK(strstr(console, "t70 diag: fbcon ") && strstr(console, "2240x1400, 4 bytes per pixel; wait 5000 ms"),
	      "console lines");
	CHECK(d.magic == T70_DIAG_MAGIC && d.seq == 1 && d.size_of == sizeof(d) && d.stage == 6, "record");
	CHECK(d.gfx == 0 && d.width == 2240 && d.height == 1400 && d.bytes_pp == 4 && d.disp_ready == 1, "frame buffer");
	CHECK(d.px[0] == 0 && d.px[1] == 0xff8c8c8c && d.px[3] == 0xff8c8c8c && d.flushes > 40, "pixels / flushes");
	CHECK(d.t_entry < 100000 && d.t_last >= 17000000 && d.misc_write == 0, "times");
	CHECK(d.kpd_samples > 500 && d.kpd_and == 0xffff && d.kpd_or == 0xffff && d.det_or == 0, "key record");
	CHECK(strstr(screen, "keypad ffff   vol-down 0  vol-up 0  power 0  home 0") != NULL, "key line");
	CHECK(misc[0] == 0 && misc[0x800] == 0 && misc[T70_CFG_OFFSET] == 0, "touched other misc data");

	name = "diag: the next boot counts on";
	reset(); run();
	memcpy(&d, misc + T70_DIAG_OFFSET, sizeof(d));
	CHECK(d.seq == 2 && d.stage == 6, "not counted");

	name = "diag: menu, then continue";
	reset(); press(3000, 100, DOWN); press(4000, 100, PWR); run();
	memcpy(&d, misc + T70_DIAG_OFFSET, sizeof(d));
	CHECK(!halted && strstr(screen, "Boot options") && d.stage == 7, "menu not recorded");
	CHECK(d.kpd_and == 0xfffd && (d.det_or & 4), "volume down / power not recorded");
	CHECK(strstr(screen, "keypad fffd") != NULL, "key line does not follow the keypad");

	name = "diag: no frame buffer";
	lk_fbcon = NULL;
	reset(); run();
	memcpy(&d, misc + T70_DIAG_OFFSET, sizeof(d));
	CHECK(d.stage == 2 && d.gfx == -1 && d.fbcon == 0 && strstr(console, "fbcon 0") && !strstr(console, "console"),
	      "no frame buffer not recorded");
	lk_fbcon = &fbcon;

	printf("%s\n", failures ? "FAILED" : "ALL PASS (diagnostic build)");
	return failures != 0;
#endif

	name = "no key: stock 5 s wait, then boot";
	reset(); run();
	CHECK(!halted, "halted");
	CHECK(now_us >= 5000000 && now_us < 5100000, "wait is not 5 s");
	CHECK(!screen[0], "text drawn over the logo");
	CHECK(flushes > 40 && fb_black(), "bar not running or screen not left black");

	name = "key in the wait, POWER on Continue: boot this slot";
	reset(); press(1000, 200, DOWN); press(2000, 100, PWR); run();
	CHECK(!halted, "halted");
	CHECK(strstr(screen, "Boot options") != NULL, "menu not drawn");
	CHECK(strstr(screen, "Continue booting\nslot A\n") != NULL, "slot A not shown");
	CHECK(fb_black(), "screen not black after the menu");

	name = "Boot slot only sets the flag; Continue then restarts into slot B";
	reset(); memset(misc, 0, sizeof(misc)); strcpy(slot, "_a");
	press(500, 100, UP); press(1000, 100, DOWN); press(1500, 100, PWR); press(2000, 100, UP);
	press(2500, 100, PWR); run();
	CHECK(halted && halt_action == LK_HALT_REBOOT && halt_reason == LK_REASON_COLD && now_us > 2500000,
	      "no cold reboot after Continue");
	CHECK(misc[0x800] == '_' && misc[0x801] == 'b', "slot b not set");
	CHECK(strstr(screen, "Boot slot\nB  (next boot)\n") && strstr(screen, "Continue booting\nslot B, restarts\n") &&
	      strstr(screen, "Restarting to slot B"), "labels");

	name = "Boot slot twice: back to this slot, Continue boots on";
	reset(); memset(misc, 0, sizeof(misc));
	press(500, 100, UP); press(1000, 100, DOWN); press(1500, 100, PWR); press(2000, 100, PWR);
	press(2500, 100, UP); press(3000, 100, PWR); run();
	CHECK(!halted && misc[0x801] == 'a', "did not boot on in slot A");

	name = "Boot slot, then no key for 60 s: restart into slot B";
	reset(); memset(misc, 0, sizeof(misc));
	press(500, 100, UP); press(1000, 100, DOWN); press(1500, 100, PWR); run();
	CHECK(halted && halt_reason == LK_REASON_COLD && misc[0x801] == 'b' && now_us > 60000000, "no restart");
	/* the menu and the message are opaque (alpha 0xff), also where dimmed */
	CHECK(((uint32_t *)fbcon.fb)[0] >> 24 == 0xff && ((uint32_t *)fbcon.fb)[2240 * 700 + 1120] >> 24 == 0xff,
	      "menu or message not opaque");

	name = "from slot B, to slot A";
	reset(); strcpy(slot, "_b");
	press(500, 100, PWR); press(1000, 100, DOWN); press(1500, 100, PWR); press(2000, 100, UP);
	press(2500, 100, PWR); run();
	CHECK(halted && misc[0x801] == 'a', "slot a not set");
	CHECK(strstr(screen, "Continue booting\nslot B\n") && strstr(screen, "Boot slot\nB  (this boot)\n"), "labels");
	CHECK(strstr(screen, "Restarting to slot A") != NULL, "no message before the restart");
	strcpy(slot, "_a");

	name = "recovery";
	reset(); memset(misc, 0, 0x800);
	press(100, 100, DOWN); for (int i = 0; i < 2; i++) press(400 + i * 300, 100, DOWN);
	press(1500, 100, PWR); run();
	CHECK(halted && halt_reason == LK_REASON_SW_RESET && rgu_mode == LK_RGU_RECOVERY, "no recovery reboot");
	CHECK(!strcmp((char *)misc, "boot-recovery"), "misc command");

	name = "fastboot (bootloader), going up past the top";
	reset();
	press(100, 100, UP); for (int i = 0; i < 8; i++) press(400 + i * 300, 100, UP);
	press(3500, 100, PWR); run();
	CHECK(halted && rgu_mode == LK_RGU_BOOTLOADER && halt_reason == LK_REASON_SW_RESET, "no fastboot reboot");

	name = "fastbootd";
	reset(); memset(misc, 0, 0x800);
	press(100, 100, UP); for (int i = 0; i < 4; i++) press(400 + i * 300, 100, DOWN);
	press(2500, 100, PWR); run();
	CHECK(halted && rgu_mode == LK_RGU_RECOVERY && !strcmp((char *)misc, "boot-fastboot"), "no fastbootd");

	name = "factory mode: the boot goes on in factory mode";
	reset(); press(100, 100, DOWN); for (int i = 0; i < 5; i++) press(400 + i * 300, 100, DOWN);
	press(2500, 100, PWR); run();
	CHECK(!halted && lk_boot_mode == LK_MODE_FACTORY && fb_black() && now_us < 3000000, "not factory");
	CHECK(strstr(screen, "Factory mode\ntest menu\n") != NULL, "label");

	name = "preloader: a cold restart";
	reset(); press(100, 100, DOWN); for (int i = 0; i < 6; i++) press(400 + i * 300, 100, DOWN);
	press(2800, 100, PWR); run();
	CHECK(halted && halt_action == LK_HALT_REBOOT && halt_reason == LK_REASON_COLD && rgu_mode == -1, "no cold restart");

	name = "BootROM download: BOOT_MISC0 set, watchdog reset";
	reset(); memset((void *)t70_test_secao, 0, sizeof(t70_test_secao));
	press(100, 100, DOWN); for (int i = 0; i < 7; i++) press(400 + i * 300, 100, DOWN);
	press(3100, 100, PWR); run();
	CHECK(halted && halt_reason == LK_REASON_SW_RESET, "no watchdog reset");
	CHECK(t70_test_secao[0x80 / 4] == (0x444c0000u | 60u << 2 | 1) && (t70_test_secao[0x108 / 4] & 1) &&
	      t70_test_secao[0x100 / 4] == 0, "download request not set");

	name = "wait setting 5 -> 3 s, saved in misc, used next boot";
	reset(); memset(misc, 0, sizeof(misc));
	press(100, 100, UP); for (int i = 0; i < 8; i++) press(400 + i * 300, 100, DOWN);
	press(3500, 100, PWR); press(4000, 100, UP); for (int i = 0; i < 8; i++) press(4300 + i * 300, 100, DOWN);
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

	name = "volume keys with the other keypad bits reading 0";
	reset(); kpd_sparse = 1;
	press(1000, 100, UP); press(1500, 100, DOWN); press(1800, 100, UP); press(2200, 100, PWR); run();
	CHECK(!halted && strstr(screen, "Boot options") && now_us < 3000000, "volume ignored");

	name = "power bouncing as it is let go does not select";
	reset(); press(1000, 100, PWR); press(1130, 20, PWR); run();
	CHECK(!halted && strstr(screen, "Boot options") && now_us > 60000000, "bounce selected Continue");

	name = "recovery boot: no wait, no menu";
	reset(); lk_boot_mode = LK_MODE_RECOVERY; press(200, 300, DOWN); run();
	CHECK(!halted && !screen[0] && flushes == 0 && now_us < 100000, "waited or drew");

	name = "recovery boot, volume held: the menu; Continue clears misc, restarts normally";
	reset(); memset(misc, 0, sizeof(misc));
	strcpy((char *)misc, "boot-recovery"); strcpy((char *)misc + 0x40, "recovery\n--wipe_data");
	misc[0x800] = '_'; misc[0x801] = 'b';
	lk_boot_mode = LK_MODE_RECOVERY; press(0, 300, DOWN); press(1000, 100, PWR); run();
	CHECK(strstr(screen, "Continue booting\nslot A, normal, restarts\n") != NULL, "label");
	CHECK(halted && halt_action == LK_HALT_REBOOT && halt_reason == LK_REASON_COLD && rgu_mode == -1,
	      "no plain restart");
	CHECK(misc[0] == 0 && misc[0x40] == 0 && misc[0x801] == 'b', "misc message not cleared, or more cleared");

	name = "recovery boot, volume held, menu left alone: recovery goes on";
	reset(); memset(misc, 0, sizeof(misc)); strcpy((char *)misc, "boot-recovery");
	lk_boot_mode = LK_MODE_RECOVERY; press(0, 300, DOWN); run();
	CHECK(!halted && strstr(screen, "Boot options") && now_us > 60000000, "menu not shown or not left");
	CHECK(!strcmp((char *)misc, "boot-recovery") && lk_boot_mode == LK_MODE_RECOVERY, "recovery changed");

	name = "fastboot boot, volume held: the menu; Continue goes on in that mode";
	reset(); memset(misc, 0, sizeof(misc));
	lk_boot_mode = LK_MODE_FASTBOOT; press(0, 300, UP); press(1000, 100, PWR); run();
	CHECK(!halted && strstr(screen, "Continue booting\nslot A\n") && lk_boot_mode == LK_MODE_FASTBOOT &&
	      misc[0] == 0, "did not go on");

	name = "factory and fastboot modes: no wait";
	reset(); lk_boot_mode = LK_MODE_FACTORY; run();
	CHECK(flushes == 0 && now_us < 100000, "factory waited");
	reset(); lk_boot_mode = LK_MODE_FASTBOOT; run();
	CHECK(flushes == 0 && now_us < 100000, "fastboot waited");

	name = "switched on by the charger cable: the normal wait";
	reset(); lk_boot_mode = LK_MODE_KPOC; run();
	CHECK(now_us >= 5000000 && now_us < 5100000 && flushes > 40, "no wait");

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

	name = "fastboot: the hardware watchdog is stopped, no idle timer";
	wdt_on = 1;
	CHECK(t70_fastboot_start() == 0 && wdt_on == 0, "watchdog still on");

	name = "fastboot boot: registered with continue, checks the image, boots it";
	{
		static uint8_t img[4096 * 4];
		int part = 0x1234;

		t70_fastboot_register_continue("continue", (void *)0, 0, 0);
		CHECK(fb_registered_boot && fb_boot_handler, "boot not registered");
		t70_boot_images_set(&part, 0, 0);
		CHECK(boot_set == &part, "partition image not used without fastboot boot");
		t70_fastboot_buffer_free(0); t70_fastboot_buffer_unmap(0, 0);
		CHECK(buffer_freed == 2, "buffer not freed after a plain fastboot");
		memcpy(img, "ANDROID!", 8);
		img[8] = 0x10; img[9] = 0x30;		/* kernel 0x3010 bytes: 4 pages, 20 KiB with the header */
		img[40] = 4;
		CHECK(t70_boot_image_problem(img, sizeof(img)) != NULL, "too large image accepted");
		fb_boot_handler("", img, sizeof(img));
		CHECK(fb_reply && !fb_continued, "too large image booted");
		img[9] = 0x10;				/* 0x1010 bytes: 2 pages */
		CHECK(t70_boot_image_problem(img, sizeof(img)) == NULL, "good image refused");
		img[40] = 2;
		CHECK(t70_boot_image_problem(img, sizeof(img)) != NULL, "header v2 accepted");
		img[40] = 4;
		img[0] = 'X';
		CHECK(t70_boot_image_problem(img, sizeof(img)) != NULL, "bad magic accepted");
		img[0] = 'A';
		fb_reply = NULL;
		fb_boot_handler("", img, sizeof(img));
		CHECK(!fb_reply && fb_continued, "good image not booted");
		t70_boot_images_check(&part, 0, 0);
		CHECK(boot_set == img, "downloaded image not checked");
		t70_boot_images_set(&part, 0, 0);
		CHECK(boot_set == img, "downloaded image not used");
		t70_boot_header_copy(&part, "boot");
		CHECK(header_from == img, "boot header not taken from the downloaded image");
		t70_boot_header_copy(&part, "vendor_boot");
		CHECK(header_from == &part, "vendor_boot header replaced");
		t70_boot_header_copy(&part, "init_boot");
		CHECK(header_from == &part, "init_boot header replaced");
		buffer_freed = 0; t70_fastboot_buffer_free(0); t70_fastboot_buffer_unmap(0, 0);
		CHECK(buffer_freed == 0, "buffer freed under the boot image");
	}

	name = "usb stop: controller stopped and cleared before the PHY goes off";
	{
		volatile uint8_t *m = (volatile uint8_t *)t70_test_musb;
		uint64_t t0;

		memset((void *)t70_test_musb, 0, sizeof(t70_test_musb));
		MUSB32(0xa4) = 0x2f;			/* L1INTM as fastboot leaves it */
		m[0x0b] = 0xf7; m[0x06] = 3; m[0x08] = 3;	/* interrupt enables */
		MUSB32(0x204) = 1;			/* HSDMA channel 0 */
		MUSB32(0x810) = 0x8001; MUSB32(0x814) = 0x1234;	/* receive queue 1, active */
		MUSB32(0xa00) = 0x8001; MUSB32(0xa04) = 0x5678;	/* transmit queue 1, active */
		MUSB32(0xa10) = 0x0001;			/* transmit queue 2, not active */
		MUSB32(0xb00) = 0x10001; MUSB32(0x800) = 0x10001; MUSB32(0x808) = 0x1000000;
		m[0x01] = 0x60;				/* SOFTCONN left on */
		phy_off = 0;
		t70_usb_stop();
		CHECK(MUSB32(0xa4) == 0 && m[0x0b] == 0 && m[0x06] == 0 && m[0x08] == 0, "interrupts still enabled");
		CHECK(MUSB32(0x204) == 0, "HSDMA channel still enabled");
		CHECK(MUSB32(0x810) == 4 && MUSB32(0xa00) == 4, "active queues not stopped");
		CHECK(MUSB32(0x814) == 0 && MUSB32(0xa04) == 0, "queue descriptors kept");
		CHECK(MUSB32(0xa10) == 1, "inactive queue written");
		CHECK(MUSB32(0xb00) == 0 && MUSB32(0x800) == 0 && MUSB32(0x808) == 0, "queue enables kept");
		CHECK(MUSB32(0xc0c) == 0x1e03ffff && MUSB32(0xc9c) == 0xff00ffff && MUSB32(0xcbc) == 0x00ffffff &&
		      MUSB32(0xc6c) == 0xff, "QMU interrupts not masked");
		CHECK(!(m[0x01] & 0x40), "SOFTCONN still on");
		CHECK(phy_off == 1 && phy_off_l1intm == 0, "PHY not powered down, or before the controller was cleared");

		name = "usb stop: a queue that does not stop does not hang the boot";
		MUSB32(0xa00) = 0x8001; MUSB32(0xa04) = 0x5678;
		musb_stuck = 1; phy_off = 0; t0 = now_us;
		t70_usb_stop();
		musb_stuck = 0;
		CHECK(phy_off == 1 && now_us - t0 <= 11000, "waited too long for the queue");
		CHECK(MUSB32(0xa04) == 0x5678, "descriptors of a running queue cleared");
	}

	name = "avb: header sizes";
	{
		static uint8_t h[4096];
		const uint64_t part = 0x4000000;

		CHECK(t70_avb_image_size(h, part) == part, "unknown image not the whole partition");
		memcpy(h, "ANDROID!", 8); put32(h + 8, 0x5001); put32(h + 12, 0x2000); put32(h + 40, 4); put32(h + 1580, 0x1000);
		CHECK(t70_avb_image_size(h, part) == 0x1000 + 0x6000 + 0x2000 + 0x1000, "boot v4 size");
		put32(h + 40, 3);
		CHECK(t70_avb_image_size(h, part) == 0x1000 + 0x6000 + 0x2000, "boot v3 size (no signature)");
		put32(h + 40, 2);
		CHECK(t70_avb_image_size(h, part) == part, "boot v2 not the whole partition");
		put32(h + 40, 4); put32(h + 8, 0x7fffffff);
		CHECK(t70_avb_image_size(h, 0x800000) == 0x800000, "size past the partition not capped");
		memset(h, 0, sizeof(h));
		memcpy(h, "VNDRBOOT", 8); put32(h + 8, 4); put32(h + 12, 4096); put32(h + 24, 0x10001);
		put32(h + 2096, 2128); put32(h + 2100, 0x3000); put32(h + 2112, 0x80); put32(h + 2124, 0x10);
		CHECK(t70_avb_image_size(h, part) == 0x1000 + 0x11000 + 0x3000 + 0x1000 + 0x1000, "vendor_boot v4 size");
		put32(h + 12, 3000);
		CHECK(t70_avb_image_size(h, part) == part, "odd vendor_boot page size used");
		memset(h, 0, sizeof(h));
	}

	name = "avb: unlocked, no slot data: boot, vendor_boot, init_boot loaded by their headers";
	{
		static uint8_t boot[0x40000], vboot[0x40000], iboot[0x8000];
		const char *const req[] = { "boot", "vendor_boot", "init_boot", NULL };
		struct test_avb_slot *d = NULL, given;
		int r;

		for (unsigned i = 0; i < sizeof(boot); i++) boot[i] = vboot[i] = (uint8_t)(i * 7 + 1);
		for (unsigned i = 0; i < sizeof(iboot); i++) iboot[i] = (uint8_t)(i * 3);
		memcpy(boot, "ANDROID!", 8); put32(boot + 8, 0x3000); put32(boot + 12, 0); put32(boot + 40, 4); put32(boot + 1580, 0);
		memset(boot + 1580, 0, 4);
		memcpy(vboot, "VNDRBOOT", 8); put32(vboot + 8, 4); put32(vboot + 12, 4096); put32(vboot + 24, 0x2000);
		put32(vboot + 2096, 2128); put32(vboot + 2100, 0x1000); put32(vboot + 2112, 0); put32(vboot + 2124, 0);
		avb_parts[0].name = "boot_b"; avb_parts[0].data = boot; avb_parts[0].size = sizeof(boot);
		avb_parts[1].name = "vendor_boot_b"; avb_parts[1].data = vboot; avb_parts[1].size = sizeof(vboot);
		avb_parts[2].name = "init_boot_b"; avb_parts[2].data = iboot; avb_parts[2].size = sizeof(iboot);

		/* locked: the stock result as it is, nothing read */
		avb_result = 3; avb_data = NULL; avb_pool = 1 << 24; avb_used = 0; avb_reads = 0;
		r = t70_avb_slot_verify(&avb_ops, req, "_b", 0, 0, (void **)&d);
		CHECK(r == 3 && !d && avb_reads == 0, "locked device: loaded without verification");

		/* unlocked and verification gave slot data: kept */
		avb_data = &given;
		r = t70_avb_slot_verify(&avb_ops, req, "_b", 1, 0, (void **)&d);
		CHECK(r == 3 && d == &given && avb_reads == 0, "slot data from verification replaced");

		/* the flag but the device says locked: nothing */
		avb_data = NULL; avb_unlocked_op = 0; d = NULL;
		r = t70_avb_slot_verify(&avb_ops, req, "_b", 1, 0, (void **)&d);
		CHECK(!d && avb_reads == 0, "loaded on a device that says it is locked");
		avb_unlocked_op = 1;

		/* unlocked, no slot data: loaded */
		avb_verify_calls = 0;
		r = t70_avb_slot_verify(&avb_ops, req, "_b", 1 | 4, 2, (void **)&d);
		CHECK(avb_verify_calls == 1 && r == 3, "verification not called once, or its result changed");
		CHECK(d && d->nparts == 3 && d->nvbmeta == 0, "not three partitions");
		if (d && d->nparts == 3) {
			CHECK(!strcmp(d->parts[0].name, "boot") && !strcmp(d->parts[1].name, "vendor_boot") &&
			      !strcmp(d->parts[2].name, "init_boot"), "names (without suffix)");
			CHECK(d->parts[0].size == 0x1000 + 0x3000, "boot not sized by its header");
			CHECK(d->parts[1].size == 0x1000 + 0x2000 + 0x1000, "vendor_boot not sized by its header");
			CHECK(d->parts[2].size == sizeof(iboot), "unknown image not the whole partition");
			CHECK(!memcmp(d->parts[0].data, boot, d->parts[0].size) &&
			      !memcmp(d->parts[1].data, vboot, d->parts[1].size) &&
			      !memcmp(d->parts[2].data, iboot, d->parts[2].size), "data not the partitions'");
			CHECK(!d->parts[0].preloaded && !d->parts[0].verify_result, "flags");
			CHECK(!strcmp(d->suffix, "_b") && d->hashtree_mode == 2, "suffix or hashtree mode");
			CHECK(d->cmdline && strlen(d->cmdline) >= strlen("androidboot.vbmeta.hash_alg") &&
			      strstr(d->cmdline, "androidboot.vbmeta.device_state=unlocked"), "command line");
		}

		name = "avb: a missing partition is skipped, no memory gives no slot data";
		avb_parts[2].name = NULL; d = NULL;
		t70_avb_slot_verify(&avb_ops, req, "_b", 1, 0, (void **)&d);
		CHECK(d && d->nparts == 2, "missing init_boot not skipped");
		avb_pool = avb_used + 0x2000; d = NULL;
		t70_avb_slot_verify(&avb_ops, req, "_b", 1, 0, (void **)&d);
		CHECK(!d, "slot data without images");
		memset(avb_parts, 0, sizeof(avb_parts));
	}

	name = "no frame buffer: the wait only, no menu";
	lk_fbcon = NULL;
	reset(); press(1000, 100, DOWN); press(2000, 100, PWR); run();
	CHECK(!halted && !screen[0] && flushes == 0 && now_us >= 5000000 && now_us < 5100000, "drew or did not wait");
	lk_fbcon = &fbcon;

	printf("%s\n", failures ? "FAILED" : "ALL PASS");
	return failures != 0;
}
