/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Screen demo: runs the real menu code (src/) on the build host against a
 * text console the size of the stock one (2240x1400 panel, 8x16 font:
 * 280 columns, 87 rows) and prints every screen update as text. A scripted
 * user opens the menu, moves to "Switch to slot B" and selects it.
 * tools/lk_demo.py draws the frames with the font of your stock lk.
 *
 *   demo_screen "<stock line 1>" "<stock line 2>"   (the orange-state text)
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lk_stock.h"
#include "t70.h"

#define ROWS 87
#define COLS 280

static char grid[ROWS][COLS];
static int cur_row, cur_col;

static uint64_t now_us;
static struct press { uint64_t from, to; unsigned keys; } script[16];
static int nscript;
static uint8_t misc[64 * 1024];
static jmp_buf halt_jump;
volatile uint8_t lk_boot_mode;
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

static void sync_kpd(void)
{
	unsigned k = held();
	t70_test_kpd_mem1 = (uint16_t)(0xffff & ~((k & T70_KEY_UP) ? 1 : 0) & ~((k & T70_KEY_DOWN) ? 2 : 0));
}

uint64_t lk_time_us(void) { return now_us; }
void lk_delay_us(uint32_t us) { now_us += us; sync_kpd(); }
int lk_detect_key(int key) { return key == LK_KEY_POWER && (held() & T70_KEY_POWER); }

/* The stock console: goto() moves the cursor, printf() draws characters, '\n' starts the next row. */
int lk_console_rows(void) { return ROWS; }
void lk_console_goto(int row, int col)
{
	if (row <= ROWS && col <= COLS) {
		cur_row = row;
		cur_col = col;
	}
}
void lk_console_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	for (const char *s = buf; *s; s++) {
		if (*s == '\n' || cur_col >= COLS) {
			cur_col = 0;
			cur_row++;
			if (*s == '\n')
				continue;
		}
		if (cur_row < ROWS)
			grid[cur_row][cur_col] = *s;
		cur_col++;
	}
}
void lk_display_update(void)
{
	printf("F %llu\n", (unsigned long long)(now_us / 1000));
	for (int r = 0; r < ROWS; r++)
		printf("%.*s\n", COLS, grid[r]);
}
int lk_dprintf(const char *fmt, ...) { (void)fmt; return 0; }
const char *lk_get_slot_suffix(void) { return "_a"; }
int lk_set_active_slot(const char *suffix) { (void)suffix; return 0; }
void lk_platform_halt(int action, int reason) { (void)action; (void)reason; longjmp(halt_jump, 1); }
void lk_rgu_set_boot_mode(uint8_t mode) { (void)mode; }
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

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <stock line 1> <stock line 2>\n", argv[0]);
		return 2;
	}
	memset(grid, ' ', sizeof(grid));
	/* What the stock lk does before the hooked wait (f_05b948): clear the
	 * console, go to the middle row and print the orange-state warning. */
	lk_console_goto(ROWS / 2, 0);
	lk_console_printf("%s", argv[1]);
	lk_console_printf("%s", argv[2]);
	lk_display_update();

	press(1500, 150, T70_KEY_DOWN);		/* any key: open the menu */
	press(3000, 150, T70_KEY_DOWN);		/* move to "Switch to slot B" */
	press(4500, 150, T70_KEY_POWER);	/* select */
	sync_kpd();
	if (!setjmp(halt_jump))
		t70_orange_wait();
	return 0;
}
