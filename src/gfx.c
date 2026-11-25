/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Drawing into the frame buffer of the stock lk text console (the one the
 * orange-state warning used): its address, size and bytes per pixel come
 * from the stock console state, and lk_disp_update() cleans the cache and
 * shows it, as the stock console does after printing.
 *
 * With 4 bytes per pixel the top byte is alpha: the console page lies over
 * the page with the boot logo, so 0 shows the logo and 0xff hides it. The
 * stock console writes its text with 0xff and clears to 0. Every colour we
 * draw is opaque; gfx_clear() makes the whole page transparent again.
 *
 * Text uses the stock 8x16 font. Enlarged glyphs are smoothed with Scale2x
 * (2x, and twice for 4x) and Scale3x, so diagonals and curves stay round.
 */
#include "gfx.h"
#include "lk_stock.h"
#include "t70.h"

#ifdef T70_HOST_TEST
void t70_test_text(const char *s);
#endif

static struct {
	uint8_t *fb;
	int w, h, bpp, pitch;
} g = { 0, 0, 0, 0, 0 };	/* in .data: the stock start code does not clear our bss */

int gfx_init(void)
{
	struct lk_fbcon *c = lk_fbcon;

	if (!c || !c->fb || (c->bytes_pp != 4 && c->bytes_pp != 2) ||
	    c->width < 1600 || c->height < 1200 || c->width > 8192 || c->height > 8192)
		return -1;
	g.fb = c->fb;
	g.w = (int)c->width;
	g.h = (int)c->height;
	g.bpp = (int)c->bytes_pp;
	g.pitch = g.w * g.bpp;
	return 0;
}

int gfx_w(void) { return g.w; }
int gfx_h(void) { return g.h; }

static uint32_t pixel(uint32_t rgb)
{
	if (g.bpp == 2)
		return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
	return 0xff000000u | (rgb & 0xffffff);	/* opaque, as the stock console writes text */
}

static void span(int x, int y, int n, uint32_t v)
{
	if (y < 0 || y >= g.h)
		return;
	if (x < 0) {
		n += x;
		x = 0;
	}
	if (n > g.w - x)
		n = g.w - x;
	if (n <= 0)
		return;
	uint8_t *p = g.fb + (long)y * g.pitch + (long)x * g.bpp;
	if (g.bpp == 2) {
		for (uint16_t *q = (uint16_t *)p; n > 0; n--)
			*q++ = (uint16_t)v;
		return;
	}
	uint32_t *q = (uint32_t *)p;
	if (((uintptr_t)q & 7) && n > 0) {
		*q++ = v;
		n--;
	}
	uint64_t vv = ((uint64_t)v << 32) | v;
	for (uint64_t *d = (uint64_t *)q; n >= 2; n -= 2) {
		*d++ = vv;
		q += 2;
	}
	if (n)
		*q = v;
}

void gfx_fill(int x, int y, int w, int h, uint32_t rgb)
{
	uint32_t v = pixel(rgb);

	for (int j = 0; j < h; j++)
		span(x, y + j, w, v);
}

/* Rectangle with round corners of radius r. */
void gfx_round(int x, int y, int w, int h, int r, uint32_t rgb)
{
	uint32_t v = pixel(rgb);

	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	for (int j = 0; j < h; j++) {
		int inset = 0;
		int dy = j < r ? r - j : j >= h - r ? j - (h - r) + 1 : 0;
		if (dy) {
			/* first column whose pixel centre is inside the corner circle */
			int cy = 2 * dy - 1;
			while (inset < r) {
				int cx = 2 * (r - inset) - 1;
				if (cx * cx + cy * cy <= 4 * r * r)
					break;
				inset++;
			}
		}
		span(x + inset, y + j, w - 2 * inset, v);
	}
}

/* ---- text ---- */

/* Bitmaps up to 32 pixels wide, one uint32_t per row, bit x = column x. */
static int bit(const uint32_t *rows, int w, int h, int x, int y)
{
	return x >= 0 && y >= 0 && x < w && y < h && ((rows[y] >> x) & 1);
}

static void scale2x(const uint32_t *s, int w, int h, uint32_t *d)
{
	for (int y = 0; y < h; y++) {
		uint32_t r0 = 0, r1 = 0;
		for (int x = 0; x < w; x++) {
			int e = bit(s, w, h, x, y), b = bit(s, w, h, x, y - 1), hh = bit(s, w, h, x, y + 1);
			int dd = bit(s, w, h, x - 1, y), f = bit(s, w, h, x + 1, y);
			int e0 = e, e1 = e, e2 = e, e3 = e;
			if (b != hh && dd != f) {
				e0 = dd == b ? dd : e;
				e1 = b == f ? f : e;
				e2 = dd == hh ? dd : e;
				e3 = hh == f ? f : e;
			}
			r0 |= (uint32_t)e0 << (2 * x) | (uint32_t)e1 << (2 * x + 1);
			r1 |= (uint32_t)e2 << (2 * x) | (uint32_t)e3 << (2 * x + 1);
		}
		d[2 * y] = r0;
		d[2 * y + 1] = r1;
	}
}

static void scale3x(const uint32_t *s, int w, int h, uint32_t *d)
{
	for (int y = 0; y < h; y++) {
		uint32_t r0 = 0, r1 = 0, r2 = 0;
		for (int x = 0; x < w; x++) {
			int a = bit(s, w, h, x - 1, y - 1), b = bit(s, w, h, x, y - 1), c = bit(s, w, h, x + 1, y - 1);
			int dd = bit(s, w, h, x - 1, y), e = bit(s, w, h, x, y), f = bit(s, w, h, x + 1, y);
			int gg = bit(s, w, h, x - 1, y + 1), hh = bit(s, w, h, x, y + 1), i = bit(s, w, h, x + 1, y + 1);
			int o[9] = { e, e, e, e, e, e, e, e, e };
			if (b != hh && dd != f) {
				o[0] = dd == b ? dd : e;
				o[1] = (dd == b && e != c) || (b == f && e != a) ? b : e;
				o[2] = b == f ? f : e;
				o[3] = (dd == b && e != gg) || (dd == hh && e != a) ? dd : e;
				o[5] = (b == f && e != i) || (hh == f && e != c) ? f : e;
				o[6] = dd == hh ? dd : e;
				o[7] = (dd == hh && e != i) || (hh == f && e != gg) ? hh : e;
				o[8] = hh == f ? f : e;
			}
			for (int k = 0; k < 3; k++) {
				r0 |= (uint32_t)o[k] << (3 * x + k);
				r1 |= (uint32_t)o[3 + k] << (3 * x + k);
				r2 |= (uint32_t)o[6 + k] << (3 * x + k);
			}
		}
		d[3 * y] = r0;
		d[3 * y + 1] = r1;
		d[3 * y + 2] = r2;
	}
}

static void glyph(int x0, int y0, int scale, uint32_t v, unsigned char ch)
{
	uint32_t a[64], b[64], *rows = a;
	int w = 8, h = 16;

	for (int y = 0; y < 16; y++) {
		uint8_t bits = lk_font_8x16[ch * 16 + y];
		uint32_t r = 0;
		for (int x = 0; x < 8; x++)
			if (bits & (0x80 >> x))
				r |= 1u << x;
		a[y] = r;
	}
	if (scale == 2 || scale == 4) {
		scale2x(a, w, h, b);
		rows = b;
		w *= 2;
		h *= 2;
		if (scale == 4) {
			scale2x(b, w, h, a);
			rows = a;
			w *= 2;
			h *= 2;
		}
	} else if (scale == 3) {
		scale3x(a, w, h, b);
		rows = b;
		w *= 3;
		h *= 3;
	}
	for (int y = 0; y < h; y++) {
		uint32_t r = rows[y];
		for (int x = 0; x < w;) {
			if (!((r >> x) & 1)) {
				x++;
				continue;
			}
			int s = x;
			while (x < w && ((r >> x) & 1))
				x++;
			span(x0 + s, y0 + y, x - s, v);
		}
	}
}

int gfx_text_w(int scale, const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n * 8 * scale;
}

int gfx_text(int x, int y, int scale, uint32_t rgb, const char *s)
{
	uint32_t v = pixel(rgb);
	int x0 = x;

	if (scale < 1 || scale > 4)
		scale = 1;
#ifdef T70_HOST_TEST
	t70_test_text(s);
#endif
	for (; *s; s++, x += 8 * scale)
		if (*s != ' ')
			glyph(x, y, scale, v, (unsigned char)*s);
	return x - x0;
}

void gfx_text_center(int y, int scale, uint32_t rgb, const char *s)
{
	gfx_text((g.w - gfx_text_w(scale, s)) / 2, y, scale, rgb, s);
}

/* The whole page transparent: the boot logo underneath shows (RGB565 has
 * no alpha: black). */
void gfx_clear(void)
{
	if (g.bpp == 4)
		memset(g.fb, 0, (unsigned long)g.pitch * g.h);
	else
		gfx_fill(0, 0, g.w, g.h, 0);
}

/* A rectangle made transparent (RGB565: black). */
void gfx_clear_rect(int x, int y, int w, int h)
{
	uint32_t v = g.bpp == 4 ? 0 : pixel(0);

	for (int j = 0; j < h; j++)
		span(x, y + j, w, v);
}

/* Everything on the screen to 3/8 of its brightness; alpha is kept. */
void gfx_dim(void)
{
	for (int y = 0; y < g.h; y++) {
		uint8_t *row = g.fb + (long)y * g.pitch;
		if (g.bpp == 4) {
			uint32_t *p = (uint32_t *)row;
			for (int x = 0; x < g.w; x++)
				p[x] = (p[x] & 0xff000000u) + ((p[x] >> 2) & 0x3f3f3f) + ((p[x] >> 3) & 0x1f1f1f);
		} else {
			uint16_t *p = (uint16_t *)row;
			for (int x = 0; x < g.w; x++)
				p[x] = (uint16_t)(((p[x] >> 2) & 0x39e7) + ((p[x] >> 3) & 0x18e3));
		}
	}
}

void gfx_flush(void)
{
#ifdef T70_DIAG
	t70_diag_rec.flushes++;
#endif
	lk_disp_update(0, 0, g.w, g.h);
}
