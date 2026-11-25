/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Diagnostic build only (make DIAG=1): what the boot wait saw, written to
 * misc at T70_DIAG_OFFSET at each step, for reading from Android afterwards:
 *
 *   dd if=/dev/block/by-name/misc bs=512 skip=25 count=1 | xxd
 *
 * (tools/lk_diag.py prints it). The normal build has none of this.
 */
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
#endif
