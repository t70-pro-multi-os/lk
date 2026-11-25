/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Fastboot: the watchdog fix and "fastboot boot".
 *
 * Watchdog: the stock lk starts the hardware watchdog with a 30 s timeout
 * (f_0037f8) and only its key boot menu (f_00da08) stops it; image loading
 * and the orange-state wait restart it, the fastboot command loop (f_007b4c)
 * never does. So the tablet reset about 30 s after entering fastboot. Hooked
 * in place of the loop's call for its idle timeout (f_00509c, always 0: no
 * idle timer), we stop the watchdog as the stock boot menu does, and return 0.
 *
 * "fastboot boot <boot.img>": missing from the stock fastboot. Registered
 * next to "continue" (hook at its registration); it checks the downloaded
 * image, keeps its address and does what "continue" does (OKAY, leave
 * fastboot, boot). The normal boot then loads and verifies this slot's
 * partitions as always (an unlocked device boots on with a verification
 * error). Where f_05b534 hands the loaded images on, three hooks put the
 * downloaded image in place of the boot partition: the header copy the boot
 * code takes its sizes and offsets from (f_00c3a0, taken for each loaded
 * partition), the header check (f_00d2d8) and the image addresses
 * (f_00d268). vendor_boot and init_boot stay this slot's.
 * The stock fastboot frees its 128 MiB download buffer when it leaves (two
 * more hooks in f_008228); after "fastboot boot" it is kept, as the image
 * is in it.
 */
#include "lk_stock.h"
#include "t70.h"

int t70_fastboot_start(void)
{
	lk_wdt_enable(0);
	lk_dprintf("t70: fastboot: hardware watchdog stopped\n");
	return 0;
}

static const uint8_t *ram_boot = 0;	/* in .data: our bss is not cleared */

static uint32_t le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned long pages(uint32_t n)
{
	return ((unsigned long)n + 4095) & ~4095ul;
}

/* NULL if img is a boot image (header v3 or v4, 4 KiB pages) that fits in
 * sz bytes; otherwise what is wrong with it. */
const char *t70_boot_image_problem(const void *data, unsigned long sz)
{
	const uint8_t *img = data;
	static const char magic[] = "ANDROID!";
	unsigned long need;
	uint32_t version;

	if (!img || sz < 4096)
		return "image too small";
	for (int i = 0; i < 8; i++)
		if (img[i] != (uint8_t)magic[i])
			return "not an Android boot image";
	version = le32(img + 40);
	if (version < 3 || version > 4)
		return "boot image header v3 or v4 needed";
	need = 4096 + pages(le32(img + 8)) + pages(le32(img + 12));
	if (version == 4)
		need += pages(le32(img + 1580));	/* boot signature */
	if (le32(img + 8) == 0)
		return "boot image without a kernel";
	if (need > sz)
		return "boot image larger than the download";
	return 0;
}

static void cmd_boot(const char *arg, void *data, unsigned sz)
{
	const char *problem = t70_boot_image_problem(data, sz);

	if (problem) {
		lk_fastboot_fail(problem);
		return;
	}
	ram_boot = data;
	lk_dprintf("t70: fastboot boot: %u bytes at %p in place of the boot partition\n", sz, data);
	lk_fb_cmd_continue(arg, data, sz);	/* OKAY, leave fastboot, boot */
}

/* Hook at the registration of "continue": register it, then "boot".
 * The third argument is "allowed under the security control" (the stock
 * registers getvar, flash and reboot with 1; "continue" has 0 and is
 * refused with "No support by security control" on this tablet), the fourth
 * "needs an unlocked device" (0: the stock lock check stays out of it; an
 * unlocked bootloader is what lets the boot go on past verification). */
void t70_fastboot_register_continue(const char *name, void *handler, int a, int b)
{
	lk_fastboot_register(name, handler, a, b);
	lk_fastboot_register("boot", (void *)cmd_boot, 1, 0);
}

/* Hooks where the stock fastboot gives its download buffer back. */
int t70_fastboot_buffer_free(unsigned long pa)
{
	if (ram_boot) {
		lk_dprintf("t70: fastboot buffer kept for the boot image\n");
		return 0;
	}
	return lk_mblock_free(pa);
}

int t70_fastboot_buffer_unmap(void *aspace, void *va)
{
	return ram_boot ? 0 : lk_vmm_free_region(aspace, va);
}

static int is_boot(const char *name)
{
	return name && name[0] == 'b' && name[1] == 'o' && name[2] == 'o' && name[3] == 't' && !name[4];
}

/* Hooks in f_05b534 where the verified images are handed on. */
int t70_boot_header_copy(void *data, const char *name)
{
	if (ram_boot && is_boot(name)) {
		lk_dprintf("t70: boot header from the fastboot boot image\n");
		data = (void *)ram_boot;
	}
	return lk_boot_header_copy(data, name);
}

void t70_boot_images_check(void *boot, void *vendor_boot, void *init_boot)
{
	lk_boot_images_check(ram_boot ? (void *)ram_boot : boot, vendor_boot, init_boot);
}

void t70_boot_images_set(void *boot, void *vendor_boot, void *init_boot)
{
	if (ram_boot)
		lk_dprintf("t70: booting the image from fastboot boot\n");
	lk_boot_images_set(ram_boot ? (void *)ram_boot : boot, vendor_boot, init_boot);
}
