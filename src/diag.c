/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Diagnostic build only (make DIAG=1): what the boot wait saw, written to
 * misc at T70_DIAG_OFFSET at each step, for reading from Android afterwards:
 *
 *   dd if=/dev/block/by-name/misc bs=512 skip=25 count=1 | xxd
 *
 * (tools/lk_diag.py prints it). The normal build has none of this.
 */
#include "gfx.h"
#include "lk_stock.h"
#include "t70.h"

#ifdef T70_DIAG
struct t70_diag t70_diag_rec = { .magic = 0 };	/* in .data: our bss is not cleared */

static void copy(void *dst, const void *src, unsigned len)
{
	for (unsigned i = 0; i < len; i++)
		((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
}

void t70_diag(unsigned stage)
{
	struct t70_diag *d = &t70_diag_rec;
	struct lk_fbcon *c = lk_fbcon;
	uint8_t *block = lk_malloc(512);

	if (!block)
		return;
	if (d->magic != T70_DIAG_MAGIC) {
		/* first record of this boot: number it after the last one */
		struct t70_diag old;

		old.magic = 0;
		if (t70_misc_rw(T70_DIAG_OFFSET, block, 512, 0) == 0)
			copy(&old, block, sizeof(old));
		d->seq = old.magic == T70_DIAG_MAGIC ? old.seq + 1 : 1;
		d->magic = T70_DIAG_MAGIC;
		d->size_of = sizeof(*d);
		d->t_entry = lk_time_us();
	}
	d->stage = stage;
	d->t_last = lk_time_us();
	d->fbcon = (uint64_t)(uintptr_t)c;
	if (c) {
		d->fb = (uint64_t)(uintptr_t)c->fb;
		d->size = c->size;
		d->colours = c->colours;
		d->bytes_pp = c->bytes_pp;
		d->width = c->width;
		d->height = c->height;
	}
	d->disp_ready = lk_disp_ready;
	d->disp_fb = (uint64_t)(uintptr_t)lk_disp_fb;
	d->disp_off = lk_disp_fb_offset;
	d->boot_mode = lk_boot_mode;
	memset(block, 0, 512);
	copy(block, d, sizeof(*d));
	d->misc_write = (uint32_t)t70_misc_rw(T70_DIAG_OFFSET, block, 512, 1);
	lk_free(block);
}

static char *put(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static char *hex4(char *p, unsigned v)
{
	for (int i = 12; i >= 0; i -= 4)
		*p++ = "0123456789abcdef"[(v >> i) & 15];
	return p;
}

/* The raw key inputs on one line: the keypad register and the stock
 * lk_detect_key() 0-3 (volume down, volume up, power, PMIC home). */
int t70_diag_keys(int y)
{
	static uint32_t shown = 0xffffffffu;	/* in .data */
	struct t70_diag *d = &t70_diag_rec;
	unsigned mem1 = t70_kpd_mem1(), det = 0;
	char line[80], *p = line;

	for (int i = 0; i < 4; i++)
		if (lk_detect_key(i) & 1)
			det |= 1u << i;
	d->kpd_and = d->kpd_samples ? d->kpd_and & mem1 : mem1;
	d->kpd_or |= mem1;
	d->kpd_samples++;
	d->det_or |= det;
	if ((mem1 | det << 16) == shown)
		return 0;
	shown = mem1 | det << 16;
	p = put(p, "keypad ");
	p = hex4(p, mem1);
	p = put(p, "   vol-down ");
	*p++ = (char)('0' + (det & 1));
	p = put(p, "  vol-up ");
	*p++ = (char)('0' + (det >> 1 & 1));
	p = put(p, "  power ");
	*p++ = (char)('0' + (det >> 2 & 1));
	p = put(p, "  home ");
	*p++ = (char)('0' + (det >> 3 & 1));
	*p = 0;
	gfx_fill(0, y - 8, gfx_w(), 48, 0x000000);
	gfx_text(40, y, 2, 0xc2c2c2, line);
	return 1;
}
#endif
