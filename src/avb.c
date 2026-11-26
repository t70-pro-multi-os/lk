/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Boot images that AVB cannot read, on an unlocked device.
 *
 * The stock boot (f_05af38) verifies boot, vendor_boot and init_boot with
 * libavb (avb_slot_verify, f_089944) and boots the images it loaded. On an
 * unlocked device it passes ALLOW_VERIFICATION_ERROR and does not look at the
 * result, so unsigned images and wrong hashes boot (orange state). libavb lets
 * only verification errors through, though. An image with no AVB footer in a
 * partition vbmeta chains to (a boot image written without avbtool), or a
 * vbmeta it cannot parse, ends the verification with no slot data at all:
 * f_05b534 then hands no images on (it returns 2 at once), the orange-state
 * wait still runs, and the boot goes on with no kernel to start.
 *
 * Hooked in place of that avb_slot_verify call: when it gives no slot data on
 * an unlocked device, load the requested partitions as they are, as libavb
 * itself does when vbmeta has verification disabled
 * (load_requested_partitions, f_08b1dc), and hand them on in the same form.
 * Only as much is read as the image header says: the libavb allocator is a
 * 140 MiB pool that never frees (f_08be78), and whole partitions
 * (64 + 64 + 8 MiB) would not fit after the failed attempt.
 *
 * A locked device, and every verification that gives slot data, is left
 * exactly as the stock does it.
 */
#include <stdbool.h>
#include "lk_stock.h"
#include "t70.h"

/* libavb (AvbOps, AvbPartitionData, AvbSlotVerifyData) as the stock lk has it. */
struct avb_ops {
	void *user_data, *ab_ops, *atx_ops;
	int (*read_from_partition)(struct avb_ops *ops, const char *partition, int64_t offset,
				   size_t num_bytes, void *buffer, size_t *out_num_read);
	void *get_preloaded_partition, *write_to_partition, *validate_vbmeta_public_key;
	void *read_rollback_index, *write_rollback_index;
	int (*read_is_device_unlocked)(struct avb_ops *ops, bool *out_is_unlocked);
	void *get_unique_guid_for_partition;
	int (*get_size_of_partition)(struct avb_ops *ops, const char *partition, uint64_t *out_size);
};

struct avb_partition_data {
	char *partition_name;	/* without the slot suffix */
	uint8_t *data;
	size_t data_size;
	bool preloaded;
	int verify_result;
};

struct avb_slot_data {
	char *ab_suffix;
	void *vbmeta_images;
	size_t num_vbmeta_images;
	struct avb_partition_data *loaded_partitions;
	size_t num_loaded_partitions;
	char *cmdline;		/* the stock appends it to the kernel command line */
	uint64_t rollback_indexes[32];
	int resolved_hashtree_error_mode;
};

_Static_assert(offsetof(struct avb_ops, read_from_partition) == 0x18, "AvbOps");
_Static_assert(offsetof(struct avb_ops, read_is_device_unlocked) == 0x48, "AvbOps");
_Static_assert(offsetof(struct avb_ops, get_size_of_partition) == 0x58, "AvbOps");
_Static_assert(sizeof(struct avb_partition_data) == 0x20, "AvbPartitionData");
_Static_assert(offsetof(struct avb_slot_data, cmdline) == 0x28, "AvbSlotVerifyData");
_Static_assert(offsetof(struct avb_slot_data, resolved_hashtree_error_mode) == 0x130, "AvbSlotVerifyData");
_Static_assert(sizeof(struct avb_slot_data) == 0x138, "AvbSlotVerifyData");

#define AVB_ALLOW_VERIFICATION_ERROR	1u	/* AVB_SLOT_VERIFY_FLAGS_*: set by the stock when unlocked */
#define MAX_LOADED			32	/* libavb MAX_NUMBER_OF_LOADED_PARTITIONS */
#define HEADER				4096

/* What libavb itself puts on the command line for an unlocked device. The
 * stock then looks for androidboot.vbmeta.hash_alg, .size and .digest in it
 * (f_08f638): a key that is not there is skipped, but a command line shorter
 * than the key is an error. */
static const char unlocked_cmdline[] = "androidboot.vbmeta.device_state=unlocked";

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t up(uint64_t n, uint32_t page)
{
	return (n + page - 1) / page * page;
}

static int starts(const uint8_t *p, const char *magic)
{
	for (; *magic; p++, magic++)
		if (*p != (uint8_t)*magic)
			return 0;
	return 1;
}

/* Bytes of the image whose first 4 KiB are h: boot or init_boot (header v3
 * or v4) and vendor_boot (v3 or v4) by their headers, anything else the whole
 * partition. Never more than the partition. */
uint64_t t70_avb_image_size(const uint8_t *h, uint64_t part_size)
{
	uint64_t n = part_size;
	uint32_t v, page;

	if (starts(h, "ANDROID!")) {
		v = le32(h + 40);
		if (v == 3 || v == 4) {
			n = HEADER + up(le32(h + 8), HEADER) + up(le32(h + 12), HEADER);
			if (v == 4)
				n += up(le32(h + 1580), HEADER);	/* boot signature */
		}
	} else if (starts(h, "VNDRBOOT")) {
		v = le32(h + 8);
		page = le32(h + 12);
		if ((v == 3 || v == 4) && page >= 2048 && page <= 65536 && !(page & (page - 1))) {
			n = up(le32(h + 2096), page)		/* header */
			  + up(le32(h + 24), page)		/* vendor ramdisks */
			  + up(le32(h + 2100), page);		/* dtb */
			if (v == 4)
				n += up(le32(h + 2112), page)	/* vendor ramdisk table */
				   + up(le32(h + 2124), page);	/* bootconfig */
		}
	}
	return n && n <= part_size ? n : part_size;
}

static int load(struct avb_ops *ops, const char *name, const char *suffix, struct avb_partition_data *out)
{
	char part[48];
	unsigned i = 0;
	uint64_t part_size, size;
	size_t got = 0;
	uint8_t *h, *data;
	int r;

	for (const char *s = name; *s; s++)
		if (i < sizeof(part) - 1)
			part[i++] = *s;
	for (const char *s = suffix; s && *s; s++)
		if (i < sizeof(part) - 1)
			part[i++] = *s;
	part[i] = 0;

	if (ops->get_size_of_partition(ops, part, &part_size) || part_size < HEADER) {
		lk_dprintf("t70: avb: %s: no such partition\n", part);
		return 1;
	}
	h = lk_malloc(HEADER);
	if (!h)
		return 1;
	r = ops->read_from_partition(ops, part, 0, HEADER, h, &got);
	size = t70_avb_image_size(h, part_size);
	lk_free(h);
	if (r || got != HEADER) {
		lk_dprintf("t70: avb: %s: header not read (%d)\n", part, r);
		return 1;
	}
	data = lk_avb_malloc(size);
	if (!data) {
		lk_dprintf("t70: avb: %s: no memory for 0x%llx bytes\n", part, (unsigned long long)size);
		return 1;
	}
	got = 0;
	r = ops->read_from_partition(ops, part, 0, size, data, &got);
	if (r || got != size) {
		lk_dprintf("t70: avb: %s: read failed (%d)\n", part, r);
		return 1;
	}
	out->partition_name = lk_avb_strdup(name);
	out->data = data;
	out->data_size = size;
	out->preloaded = false;
	lk_dprintf("t70: avb: %s: 0x%llx bytes loaded as they are\n", part, (unsigned long long)size);
	return !out->partition_name;
}

int t70_avb_slot_verify(void *ops_, const char *const *partitions, const char *suffix,
			unsigned flags, int hashtree_mode, void **out)
{
	struct avb_ops *ops = ops_;
	struct avb_slot_data *d;
	bool unlocked = false;
	size_t n = 0;
	int r = lk_avb_slot_verify(ops, partitions, suffix, flags, hashtree_mode, out);

	if (!out || *out || !(flags & AVB_ALLOW_VERIFICATION_ERROR))
		return r;
	if (ops->read_is_device_unlocked(ops, &unlocked) || !unlocked)
		return r;
	lk_dprintf("t70: avb: verification gave no images (%d); unlocked device: loading them as they are\n", r);

	d = lk_avb_calloc(sizeof(*d));
	if (!d)
		return r;
	d->loaded_partitions = lk_avb_calloc(MAX_LOADED * sizeof(*d->loaded_partitions));
	d->ab_suffix = lk_avb_strdup(suffix ? suffix : "");
	d->cmdline = lk_avb_strdup(unlocked_cmdline);
	if (!d->loaded_partitions || !d->ab_suffix || !d->cmdline)
		return r;
	for (unsigned i = 0; partitions[i] && n < MAX_LOADED; i++)
		if (!load(ops, partitions[i], suffix, &d->loaded_partitions[n]))
			n++;
	if (!n)
		return r;
	d->num_loaded_partitions = n;
	d->resolved_hashtree_error_mode = hashtree_mode;
	*out = d;
	return r;	/* the stock does not look at it on an unlocked device */
}
