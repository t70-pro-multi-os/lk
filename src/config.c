/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lk_stock.h"
#include "t70.h"

#define BLOCK 512u
#define MISC_MESSAGE_SIZE 0x800u	/* bootloader_message at misc offset 0 */

static uint32_t crc32(const void *data, unsigned len)
{
	const uint8_t *p = data;
	uint32_t crc = ~0u;

	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

static void copy(void *dst, const void *src, unsigned len)
{
	for (unsigned i = 0; i < len; i++)
		((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
}

/* Read or write misc through a heap buffer (the lk thread stack is small). */
static int misc_rw(long offset, uint8_t *heap_buf, unsigned len, int write)
{
	void *dev = lk_bio_open("misc");
	long done;

	if (!dev)
		return -1;
	done = write ? lk_bio_write(dev, heap_buf, offset, len) : lk_bio_read(dev, heap_buf, offset, len);
	lk_bio_close(dev);
	return done == (long)len ? 0 : -1;
}

static void defaults(struct t70_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->magic = T70_CFG_MAGIC;
	cfg->version = T70_CFG_VERSION;
	cfg->size = sizeof(*cfg);
	cfg->wait_ms = 5000;	/* the stock orange-state wait */
}

void t70_cfg_load(struct t70_cfg *cfg)
{
	uint8_t *block = lk_malloc(BLOCK);

	defaults(cfg);
	if (!block)
		return;
	if (misc_rw(T70_CFG_OFFSET, block, BLOCK, 0) == 0) {
		struct t70_cfg got;

		copy(&got, block, sizeof(got));
		if (got.magic == T70_CFG_MAGIC && got.version == T70_CFG_VERSION &&
		    got.size == sizeof(got) &&
		    got.crc32 == crc32(&got, sizeof(got) - sizeof(got.crc32)) &&
		    got.wait_ms <= 30000)
			copy(cfg, &got, sizeof(got));
	}
	lk_free(block);
}

int t70_cfg_save(const struct t70_cfg *cfg)
{
	uint8_t *block = lk_malloc(BLOCK);
	struct t70_cfg out;
	int ret;

	if (!block)
		return -1;
	copy(&out, cfg, sizeof(out));
	out.magic = T70_CFG_MAGIC;
	out.version = T70_CFG_VERSION;
	out.size = sizeof(out);
	out.crc32 = crc32(&out, sizeof(out) - sizeof(out.crc32));
	memset(block, 0, BLOCK);
	copy(block, &out, sizeof(out));
	ret = misc_rw(T70_CFG_OFFSET, block, BLOCK, 1);
	lk_free(block);
	return ret;
}

/* Write the bootloader_message command (misc offset 0), as the stock
 * fastboot "reboot-recovery" / "reboot-fastboot" handlers do. */
int t70_misc_command(const char *command)
{
	uint8_t *msg = lk_malloc(MISC_MESSAGE_SIZE);
	unsigned i;
	int ret;

	if (!msg)
		return -1;
	memset(msg, 0, MISC_MESSAGE_SIZE);
	for (i = 0; command[i] && i < 31; i++)
		msg[i] = command[i];
	ret = misc_rw(0, msg, MISC_MESSAGE_SIZE, 1);
	lk_free(msg);
	return ret;
}
