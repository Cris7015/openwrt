// SPDX-License-Identifier: GPL-2.0-only
/*
 * Block Mapping & Bad Block Tables for EcoNet / Airoha EN75xx NAND.
 *
 * These SoCs use two tables, BBT and BMT. The BBT is factory bad blocks and
 * the BMT is blocks that wore out over time. When a block is added to the BBT,
 * everything following that block is shifted up by one, so adding a block to
 * the BBT after the fact is a no-go.
 *
 * Blocks added to the BBT reduce the user-servicable area directly, while
 * blocks added to the BMT use a pool of reserve blocks that is above the user
 * area. The BBT and BMT tables themselves are also stored at opposite ends of
 * the reserve area.
 *
 * While the BBT can't be changed, it can be reconstructed. To do this, first
 * the BMT must be reconstructed by scanning blocks in the reserve area to
 * identify those whose OOB data contains a back-reference to the block they
 * are mapped to. Once the BMT has been reconstructed, then we can scan all
 * remaining blocks to identify any remaining which are marked bad, and
 * consider them as (probably!) factory bad blocks.
 *
 * Reconstructing the BBT is not very safe because any confusion between a
 * factory bad block and a worn out block will result in wrong offsets and in
 * effect, data loss. Furthermore, bad blocks do not politely identify
 * themselves, generally they error out when we try to read them. Still, we
 * make the best effort we can by tagging worn blocks with 0x55 rather than
 * 0x00 which is used to tag factory blocks. Vendor firmware does not do this,
 * so if the bootloader or vendor OS marks a block bad, it will be
 * indistinguishable from a factory bad block.
 *
 * The layout looks a little bit like this:
 *
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |D D D D F D D D D D D D W D D D D D D D D F D D|B M M 0 0 0 0 0 T|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                     User Area                 |   Reserve Area  |
 *
 * - D: Data block
 * - F: Factory bad block
 * - W: Worn block
 * - B: BBT block
 * - M: Mapped block (replacing a worn block)
 * - 0: Free block
 * - T: BMT table block
 *
 * In this example, there are 22 *usable* blocks in the user area (we include
 * the worn blocks but exclude the factory bad blocks) so this disk will
 * report 2816 KiB.
 *
 * The bottom of the reserve area is decided by counting down from the
 * end until we have REQUIRED_GOOD_BLOCKS non-bad blocks. So when blocks
 * wear out in the reserve area, the bottom is moved down, stealing
 * blocks from the end of the user area and making the disk "shrink".
 * A side-effect of this is if the user puts a valid BBT at the end of
 * their space, there's a chance it might get picked up a THE BBT.
 *
 * Mercifully, blocks in the reserve area are never added to the BBT or
 * BMT, so we don't have any mapping to do in the reserve area, we just
 * have to check every block to see if it's bad before using it.
 *
 * Configuration:
 *
 * This driver is configured by device tree properties, the following
 * properties are available:
 *
 * - econet,bmt;
 *   This boolean property enables the BMT, if it is not present on the
 *   MTD device, the BMT will not be enabled.
 *
 * - econet,esmt-f50l1g41a-oem-fdm;
 *   Opt in to the XR500v OEM FDM layout, where logical bytes 0..3 are stored
 *   at physical OOB bytes 0, 8, 9 and 10. This property requires one of the
 *   static modes below; without it the original BMT scan and writer are used.
 *
 * - econet,bmt-read-only;
 *   Diagnostic mode. Scan and validate the existing BBT/BMT, but do not
 *   erase pending reserve blocks, update tables or enable remapping.
 *
 * - econet,bmt-write-through;
 *   Validate and use the existing BBT/BMT for address translation, while
 *   exposing the MTD writable without dynamic remapping or table updates.
 *   A physical erase/write failure is returned to the caller.  This is useful
 *   for controlled installation on a lab device before the full BMT update
 *   transaction has been validated.
 *
 * - econet,enable-remap;
 *   This boolean property enables remapping of observed worn blocks. It can
 *   be placed either on the device or on a partition within a fixed partitions
 *   table. By default, this module will handle existing mappings but will not
 *   update them. If remapping is enabled, a block may be remapped even if it
 *   is only being read, and remapping carries a risk of lost data, so you
 *   should avoid enabling this on critical partitions like the bootloader, and
 *   you should also avoid enabling this on partitions like UBI which have
 *   their own remapping algorithms.
 *
 * - econet,assert-reserve-size = <u32>;
 *   This allows you to assert that the computed reserve size matches
 *   the bootloader. It is typical for bootloaders to log a message
 *   such as "bmt pool size: 163" on startup. The computed reserve size MUST
 *   match the bootloader, otherwise it will be looking for the BBT in the
 *   be wrong place. If a block in the reserve space wears out, the reserve
 *   size will be increased to account for it, so this property is not
 *   appropriate for production use. But when porting to a new board, it will
 *   ensure that this module exits early if its calculation does not match the
 *   platform.
 *
 * - econet,can-write-factory-bbt;
 *   This boolean property enables updating / rebuilding of the factory BBT.
 *   ANY CHANGE TO THE BBT IMPLIES DATA LOSS. If you enable this in conjunction
 *   with econet,factory-badblocks then it will set the BBT to the configured
 *   bad blocks. If you enable it alone, it will use the bootloader's logic:
 *   Search for a BBT, if one cannot be found then scan the disk for faulty
 *   blocks which have not been mapped as "worn blocks", mark them all as bad,
 *   and create a new BBT with their indexes. This applies to the whole disk
 *   and makes no effort to recover data, it might decide the bootloader is
 *   bad, you were warned.
 *
 * - econet,factory-badblocks = <u32 array>;
 *   This property allows you to specify the factory bad blocks.
 *   If econet,can-write-factory-bbt is unset, this is an assertion which will
 *   cause early exit if the observed BBT does not match the specified bad
 *   blocks. If econet,can-write-factory-bbt is set, this will overwrite the
 *   BBT with the specified bad blocks.
 *
 * - econet,bbt-table-size = <u32>;
 *   Number of entries in the BBT table. Different vendor firmware versions
 *   use different sizes (250 or 1000). The checksum is calculated over the
 *   entire table, so this must match the firmware that created the BBT. If
 *   not specified, defaults to 1000.
 *
 * Copyright (C) 2025 Caleb James DeLisle <cjd@cjdns.fr>
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include "mtk_bmt.h"

#define MAX_BMT_SIZE				256

/* Maximum BBT table size (structure allocation limit). */
#define MAX_BBT_SIZE				1000

/* Default BBT table size if not specified in DTS. */
#define DEFAULT_BBT_TABLE_SIZE			1000

/* Vendor firmware calls this POOL_GOOD_BLOCK_PERCENT */
#define REQUIRED_GOOD_BLOCKS(total_blocks)	((total_blocks) * 8 / 100)

/* This is ours but it is on-disk so we need consensus with ourselves. */
#define BLOCK_WORN_MARK				0x55

/* How hard to try, these must be at least one. */
#define WRITE_TRIES				3
#define ERASE_TRIES				3
/*
 * Number of blocks to try to write updated tables to before failing.
 * Total write attempts will be WRITE_TRIES * UPDATE_TABLE_TRIES
 */
#define UPDATE_TABLE_TRIES			3

/* This is a lot, but if this fails, we're probably losing data. */
#define REMAP_COPY_TRIES			10

#define INITIAL_READ_TRIES			3

/* Max number of FBBs that can be expressed in the devicetree. */
#define MAX_FACTORY_BAD_BLOCKS_OF		32

const char *log_pfx			= "en75_bmt";
const char *name_can_write_factory_bbt	= "econet,can-write-factory-bbt";
const char *name_factory_badblocks	= "econet,factory-badblocks";
const char *name_enable_remap		= "econet,enable-remap";
const char *name_esmt_oem_fdm		= "econet,esmt-f50l1g41a-oem-fdm";
const char *name_read_only		= "econet,bmt-read-only";
const char *name_write_through		= "econet,bmt-write-through";
const char *name_assert_reserve_size	= "econet,assert-reserve-size";
const char *name_bbt_table_size	= "econet,bbt-table-size";

/* To promote readability, most functions must have their inputs passed in. */
#define bmtd dont_directly_reference_mtk_bmtd

/*
 * On disk
 */

struct bmt_table_header {
	/* "BMT" */
	char signature[3];
	/* 1 */
	u8 version;
	/* Unused */
	u8 bad_count;
	/* Number of mappings in table */
	u8 size;
	/* bmt_checksum() */
	u8 checksum;
	/* Unused */
	u8 reserved[13];
} bmt_table_header;
static_assert(sizeof(struct bmt_table_header) == 20);

struct bmt_entry {
	/* The bad block which is being mapped */
	u16	from;
	/* The block in the reserve area that is being used */
	u16	to;
};

struct bmt_table {
	struct bmt_table_header	header;
	struct bmt_entry	table[MAX_BMT_SIZE];
};

struct bbt_table_header {
	/* "RAWB" */
	char signature[4];
	/* bbt_checksum() (only 16 bits used) */
	u32 checksum;
	/* 1 */
	u8 version;
	/* Number of bad blocks in table */
	u8 size;
	/* Unused (ffff) */
	u8 reserved[2];
};

static_assert(sizeof(struct bbt_table_header) == 12);

struct bbt_table {
	struct bbt_table_header	header;
	/* This must be stored in ascending numerical order */
	u16			table[MAX_BBT_SIZE];
};

static_assert(sizeof(struct bbt_table) == 2012);

/*
 * In memory
 */

/* Use this to differentiate between on-disk indexes and integers */
struct block_index {
	u16 index;
};

static_assert(sizeof(struct block_index) == 2);

enum block_status {
	BS_INVALID,
	BS_AVAILABLE,
	BS_BAD,
	BS_MAPPED,
	BS_BMT,
	BS_BBT,
	BS_NEED_ERASE,
};

struct block_info {
	struct block_index index;
	enum block_status status : 16;
};

static_assert(sizeof(struct block_info) == 4);

struct block_range {
	u16 begin;
	u16 end;
};

/*
 * Context and params
 */

struct en75_bmt_m {
	struct bmt_desc *mtk;

	/* In-memory copy of the BBT */
	struct bbt_table bbt;

	/* BBT table size, from DTS or default */
	u16 bbt_table_size;

	/* In-memory copy of the BMT */
	struct bmt_table bmt;

	/* Array of blocks in reserve area, size = reserve_block_count(ctx) */
	struct block_info *rblocks;

	/* BBT is on the first usable block after this. */
	u16 reserve_area_begin;

	/*
	 * Only allow remapping of blocks within the following ranges.
	 * Expressed as user block index, not mapped through BBT.
	 */
	u16 can_remap_range_count;
	struct block_range *can_remap_ranges;

	/* Incremented each time the BBT is changed, reset when written */
	s8 bbt_dirty;

	/* Incremented each time the BMT is changed, reset when written */
	s8 bmt_dirty;

	/* Unless set, fail any attempt to write the BBT. */
	s8 can_write_factory_bbt;

	/* Use the XR500v ESMT OEM FDM mapping and strict table selection. */
	s8 esmt_oem_fdm;

	/* Scan tables without enabling any write or remap path. */
	s8 read_only;

	/* Allow direct writes, but never change mappings or on-flash tables. */
	s8 write_through;
};

/*
 * In-memory functions (do not read or write)
 */

static u16 bbt_checksum(const struct bbt_table *bbt, u16 table_size)
{
	const u8 *data = (u8 *)bbt->table;
	u16 checksum = bbt->header.version + bbt->header.size;

	for (int i = 0; i < table_size * sizeof(bbt->table[0]); i++)
		checksum += data[i];

	return checksum;
}

static u8 bmt_checksum(const struct bmt_table *bmt, int check_entries)
{
	int length;
	const u8 *data;
	u8 checksum;

	WARN_ON_ONCE(check_entries > MAX_BMT_SIZE);
	length = min(check_entries, MAX_BMT_SIZE) * sizeof(bmt->table[0]);
	data = (u8 *)&bmt->table;
	checksum = bmt->header.version + bmt->header.size;
	for (int i = 0; i < length; i++)
		checksum += data[i];

	return checksum;
}

static int reserve_block_count(const struct en75_bmt_m *ctx)
{
	return ctx->mtk->total_blks - ctx->reserve_area_begin;
}

/* return a block_info or error pointer */
static struct block_info *find_available_block(const struct en75_bmt_m *ctx, bool start_from_end)
{
	int limit = reserve_block_count(ctx);
	int i = 0;
	int d = 1;

	/*
	 * rblocks is populated by scanning in reverse,
	 * so lowest block numbers are at the end.
	 */
	if (!start_from_end) {
		i = limit - 1;
		d = -1;
	}
	for (; i < limit && i >= 0; i += d) {
		if (ctx->rblocks[i].status == BS_AVAILABLE)
			return &ctx->rblocks[i];
	}
	return ERR_PTR(-ENOSPC);
}

static int compare_bbt(const void *a, const void *b)
{
	return *(u16 *)a - *(u16 *)b;
}

static void sort_bbt(struct bbt_table *bbt)
{
	sort(bbt->table, bbt->header.size, sizeof(bbt->table[0]),
	     compare_bbt, NULL);
}

/*
 * When there's a factory bad block, we shift everything above it up by one.
 * We sort the BBT so that we can do this in one pass. The vendor firmware
 * also requires an ascending ordered BBT.
 */
static int get_mapping_block_bbt(const struct en75_bmt_m *ctx, int block)
{
	int size = ctx->bbt.header.size;

	for (int i = 0; i < size; i++)
		if (ctx->bbt.table[i] <= block)
			block++;

	if (block >= ctx->reserve_area_begin || block < 0)
		return -EINVAL;

	return block;
}

static int get_mapping_block(const struct en75_bmt_m *ctx, int block)
{
	block = get_mapping_block_bbt(ctx, block);

	if (block < 0)
		return block;

	for (int i = ctx->bmt.header.size - 1; i >= 0; i--)
		if (ctx->bmt.table[i].from == block)
			return ctx->bmt.table[i].to;

	return block;
}

static bool block_index_is_sane(const struct en75_bmt_m *ctx,
				const struct block_index bi,
				bool user_block)
{
	if (bi.index >= ctx->mtk->total_blks)
		return false;
	if (user_block && bi.index >= ctx->reserve_area_begin)
		return false;
	if (!user_block && bi.index < ctx->reserve_area_begin)
		return false;
	return true;
}

/*
 * OEM logical FDM byte 0 is the physical bad-block marker.  The F50L1G41A
 * then has seven ECC bytes before the first free OOB region, so logical FDM
 * bytes 1..3 live at physical bytes 8..10.
 */
#define OEM_ESMT_FDM_LEN		4
#define OEM_ESMT_FDM_PHYS_LEN		11

static const u8 oem_esmt_fdm_phys[OEM_ESMT_FDM_LEN] = { 0, 8, 9, 10 };

static int oem_esmt_fdm_layout_valid(const struct en75_bmt_m *ctx)
{
	struct mtd_info *mtd = ctx->mtk->mtd;
	struct mtd_oob_region free;

	if (ctx->mtk->oob_offset || mtd->oobsize < OEM_ESMT_FDM_PHYS_LEN ||
	    mtd_ooblayout_free(mtd, 0, &free) ||
	    free.offset != 8 || free.length < 3)
		return -EOPNOTSUPP;

	return 0;
}

static void oem_esmt_fdm_gather(
	const u8 phys[static OEM_ESMT_FDM_PHYS_LEN],
	u8 logical[static OEM_ESMT_FDM_LEN])
{
	for (int i = 0; i < OEM_ESMT_FDM_LEN; i++)
		logical[i] = phys[oem_esmt_fdm_phys[i]];
}

/*
 * Write
 */

static int w_write(const struct en75_bmt_m *ctx,
		   const struct block_index bi,
		   bool user_area,
		   const char *name,
		   size_t len,
		   u8 *buf,
		   bool oob)
{
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB,
		.ooboffs = 0,
		.ooblen = (oob) ? len : 0,
		.oobbuf = (oob) ? buf : NULL,
		.len = (!oob) ? len : 0,
		.datbuf = (!oob) ? buf : NULL,
	};
	int ret;

	if (WARN_ON(!block_index_is_sane(ctx, bi, user_area)))
		return -EINVAL;

	for (int i = 0; i < WRITE_TRIES; i++) {
		ret = ctx->mtk->_write_oob(
			ctx->mtk->mtd,
			((loff_t)bi.index) << ctx->mtk->blk_shift,
			&ops);
		if (!ret)
			break;
	}
	if (ret)
		pr_warn("%s: error writing %s at %d\n",
			log_pfx, name, bi.index);
	return ret;
}

/*
 * Erase & Mark bad
 */

static int w_mark_bad(
	const struct en75_bmt_m *ctx,
	const struct block_index bi,
	bool user_area)
{
	u8 fdm[4] = {BLOCK_WORN_MARK, 0xff, 0xff, 0xff};

	/*
	 * Don't erase first because it's damaged so erase is likely to fail.
	 * In we should only be clearing bits so erase should be unnecessary.
	 */
	return w_write(ctx, bi, user_area, "BAD_BLK",
		       sizeof(fdm), fdm, true);
}

static int w_erase_one(
	const struct en75_bmt_m *ctx,
	const struct block_index bi,
	bool user_area)
{
	int ret = 0;

	if (WARN_ON(!block_index_is_sane(ctx, bi, user_area)))
		return -EINVAL;

	for (int i = 0; i < ERASE_TRIES; i++) {
		ret = bbt_nand_erase(bi.index);
		if (!ret)
			break;
	}

	return ret;
}

static void w_erase_pending(const struct en75_bmt_m *ctx)
{
	int rblocks;

	rblocks = reserve_block_count(ctx);
	for (int i = 0; i < rblocks; i++) {
		const struct block_info bif = ctx->rblocks[i];
		int ret;

		if (bif.status != BS_NEED_ERASE)
			continue;

		ret = w_erase_one(ctx, bif.index, false);
		if (ret) {
			if (WARN_ON_ONCE(ret == -EINVAL)) {
				/* Just set status bad so we ignore it */
			} else {
				pr_info("%s: failed to erase block %d, marking bad\n",
					log_pfx, bif.index.index);
				w_mark_bad(ctx, bif.index, false);
			}
			ctx->rblocks[i].status = BS_BAD;
		} else {
			ctx->rblocks[i].status = BS_AVAILABLE;
		}
	}
}

static void mark_for_erasure(struct en75_bmt_m *ctx, enum block_status with_status)
{
	int rblocks = reserve_block_count(ctx);

	for (int i = 0; i < rblocks; i++) {
		if (ctx->rblocks[i].status == with_status)
			ctx->rblocks[i].status = BS_NEED_ERASE;
	}
}

/*
 * Update tables
 */

/*
 * Try on UPDATE_TABLE_TRIES blocks then give up
 * Return a block_info or pointer error.
 */
static struct block_info *w_update_table(
	const struct en75_bmt_m *ctx,
	bool start_from_end,
	const char *name,
	size_t len,
	u8 *buf)
{
	for (int i = 0; i < UPDATE_TABLE_TRIES; i++) {
		struct block_info *bif =
			find_available_block(ctx, start_from_end);
		int ret;

		if (IS_ERR(bif)) {
			pr_err("%s: no space to store %s\n", log_pfx, name);
			return bif;
		}
		ret = w_write(
			ctx, bif->index, false, name,
			len, buf, false);
		if (ret) {
			bif->status = BS_NEED_ERASE;
			continue;
		}
		return bif;
	}
	return ERR_PTR(-EIO);
}

static int w_sync_tables(struct en75_bmt_m *ctx)
{
	w_erase_pending(ctx);

	if (ctx->bmt_dirty) {
		int dirty = ctx->bmt_dirty;
		struct block_info *new_bmt_block;
		int rblocks;

		rblocks = reserve_block_count(ctx);
		for (int i = ctx->bmt.header.size; i < rblocks; i++)
			ctx->bmt.table[i] = (struct bmt_entry){ 0 };
		ctx->bmt.header.checksum =
			bmt_checksum(&ctx->bmt, ctx->bmt.header.size);
		new_bmt_block = w_update_table(
			ctx,
			true,
			"BMT",
			sizeof(ctx->bmt),
			(u8 *)&ctx->bmt);

		/*
		 * If we can't write the BMT, we won't try to write the BBT.
		 * Without the BMT it is impossible to safely reconstruct.
		 */
		if (IS_ERR(new_bmt_block)) {
			pr_err("%s: error writing BMT to block\n", log_pfx);
			return PTR_ERR(new_bmt_block);
		}
		pr_info("%s: BMT written to block %d\n",
			log_pfx, new_bmt_block->index.index);
		mark_for_erasure(ctx, BS_BMT);
		new_bmt_block->status = BS_BMT;

		ctx->bmt_dirty -= dirty;
		WARN_ON(ctx->bmt_dirty);
	}

	if (ctx->bbt_dirty && !ctx->can_write_factory_bbt) {
		WARN_ONCE("%s: BUG: BBT requires update but %s is not set\n",
			  log_pfx, name_can_write_factory_bbt);
	} else if (ctx->bbt_dirty) {
		int dirty = ctx->bbt_dirty;
		struct block_info *new_bbt_block;
		size_t bbt_write_size = sizeof(ctx->bbt.header) +
					ctx->bbt_table_size * sizeof(ctx->bbt.table[0]);

		for (int i = ctx->bbt.header.size; i < ctx->bbt_table_size; i++)
			ctx->bbt.table[i] = 0;
		ctx->bbt.header.checksum = bbt_checksum(&ctx->bbt, ctx->bbt_table_size);
		new_bbt_block = w_update_table(
			ctx,
			false,
			"BBT",
			bbt_write_size,
			(u8 *)&ctx->bbt);

		if (IS_ERR(new_bbt_block)) {
			pr_err("%s: error writing BBT to block\n", log_pfx);
			return PTR_ERR(new_bbt_block);
		}
		pr_info("%s: BBT written to block %d\n",
			log_pfx, new_bbt_block->index.index);
		mark_for_erasure(ctx, BS_BBT);
		new_bbt_block->status = BS_BBT;

		ctx->bbt_dirty -= dirty;
		WARN_ON(ctx->bbt_dirty);
	}

	w_erase_pending(ctx);
	return 0;
}

/*
 * Remap
 */

static int w_make_mapping(const struct en75_bmt_m *ctx,
			  const struct block_info replacement_block,
			  u16 bad_block_index)
{
	u8 fdm[4] = {0xff, 0xff, 0xff, 0xff};

	if (WARN_ON_ONCE(bad_block_index >= ctx->reserve_area_begin))
		return -EINVAL;

	if (WARN_ON_ONCE(replacement_block.status != BS_AVAILABLE))
		return -EINVAL;

	/* vendor firmware uses host order */
	memcpy(&fdm[2], &bad_block_index, sizeof(bad_block_index));
	return w_write(
		ctx,
		replacement_block.index,
		false,
		"REMAP",
		sizeof(fdm),
		fdm,
		true);
}

static int w_remap_block(struct en75_bmt_m *ctx,
			 u16 block,
			 struct block_info *maybe_mapped_block,
			 int copy_len)
{
	int bmt_size = ctx->bmt.header.size;
	bool mapped_already_in_bmt = false;
	int ret;
	struct block_info *new_block;

	if (ctx->bmt.header.size == 0xff) {
		pr_err("%s: BMT full, cannot add more mappings\n", log_pfx);
		return -ENOSPC;
	}

	new_block = find_available_block(ctx, false);
	if (IS_ERR(new_block)) {
		pr_err("%s: no space to remap block %d\n", log_pfx, block);
		return -ENOSPC;
	}

	ret = w_make_mapping(ctx, *new_block, block);
	if (ret) {
		new_block->status = BS_NEED_ERASE;
		return ret;
	}

	if (copy_len) {
		int ret;
		u16 copy_from = block;

		if (maybe_mapped_block)
			copy_from = maybe_mapped_block->index.index;

		for (int i = 0; i < REMAP_COPY_TRIES; i++) {
			ret = bbt_nand_copy(new_block->index.index,
					    copy_from, copy_len);
			if (!ret)
				break;
		}
		if (ret) {
			/*
			 * We can either return an error or continue.
			 * If the user is reading, not returning an error means
			 * they're going to suddenly switch to reading a another
			 * (empty) block and won't know it.
			 *
			 * If the user is erasing, returning an error means we're
			 * not doing our job. In any case, we ideally should be
			 * refusing to read newly remapped blocks until the user
			 * has issued an erase command, but we're using the mtk_bmt
			 * framework which does not support that, so we're going to
			 * have to continue.
			 *
			 * Good luck, user.
			 */
			pr_err("%s: remap copy %d->%d failed LIKELY DATA LOSS!\n",
			       log_pfx, copy_from, new_block->index.index);
		}
	}

	for (int i = 0; i < bmt_size; i++)
		if (ctx->bmt.table[i].from == block) {
			ctx->bmt.table[i].to = new_block->index.index;
			mapped_already_in_bmt = true;
			ctx->bmt_dirty++;
			break;
		}

	if (!mapped_already_in_bmt) {
		ctx->bmt.table[ctx->bmt.header.size++] = (struct bmt_entry) {
			.from = block,
			.to = new_block->index.index
		};
		ctx->bmt_dirty++;
	}

	new_block->status = BS_MAPPED;

	/*
	 * Directly mark the user block bad (if possible), the mapped block we can
	 * try to reclaim, mark it need erase and see if the eraser decides it's bad.
	 */
	w_mark_bad(ctx, (struct block_index) { .index = block }, true);
	if (maybe_mapped_block)
		maybe_mapped_block->status = BS_NEED_ERASE;

	return 0;
}

enum unmap_erase {
	UE_NO_ERASE,
	UE_ATTEMPT_ERASE,
	UE_REQUIRE_ERASE,
};

/*
 * If no mapping in BMT, -EINVAL
 * Attempt erase of block if requested
 * Remove mapping from BMT
 * Set mapped block to needs erase
 */
static int w_unmap_block(struct en75_bmt_m *ctx,
			 u16 block,
			 enum unmap_erase erase)
{
	int bmt_size = ctx->bmt.header.size;
	struct block_info *mapped_block = NULL;
	int bmt_index = 0;

	for (; bmt_index < bmt_size; bmt_index++) {
		int rblocks;

		rblocks = reserve_block_count(ctx);
		if (ctx->bmt.table[bmt_index].from != block)
			continue;

		for (int j = 0; j < rblocks; j++) {
			if (ctx->rblocks[j].index.index == ctx->bmt.table[bmt_index].to) {
				mapped_block = &ctx->rblocks[j];
				break;
			}
		}
		break;
	}
	if (!mapped_block) {
		pr_err("%s: block %d not mapped\n", log_pfx, block);
		return -EINVAL;
	}
	WARN_ON_ONCE(mapped_block->status != BS_MAPPED);
	if (erase > UE_NO_ERASE) {
		int ret;

		ret = w_erase_one(ctx, mapped_block->index, false);
		if (ret) {
			if (erase == UE_REQUIRE_ERASE) {
				pr_err("%s: unmap block %d: erase failed\n",
				       log_pfx, block);
				return ret;
			}
			pr_warn("%s: unmap block %d: erase failed\n",
				log_pfx, block);
		}
	}

	ctx->bmt.table[bmt_index] = ctx->bmt.table[--bmt_size];
	ctx->bmt.table[bmt_size] = (struct bmt_entry) { 0 };
	ctx->bmt.header.size = bmt_size;
	ctx->bmt_dirty++;

	mapped_block->status = BS_NEED_ERASE;
	return 0;
}

/*
 * Init functions
 */

enum block_is_bad {
	/* Good block */
	BB_GOOD,
	/* Probably marked bad at the factory (or by vendor firmware!) */
	BB_FACTORY_BAD,
	/* Marked bad by us */
	BB_WORN,
	/* Reserve-pool block retired through OEM logical FDM byte 1. */
	BB_POOL_BAD,
	/* We don't know */
	BB_UNKNOWN_BAD,
};

static enum block_is_bad fdm_is_bad(const struct en75_bmt_m *ctx,
				    const u8 fdm[static 4])
{
	if (!ctx->esmt_oem_fdm) {
		if (fdm[0] == 0xff && fdm[1] == 0xff)
			return BB_GOOD;
		if (fdm[0] == BLOCK_WORN_MARK)
			return BB_WORN;
		if (fdm[0] == 0x00 || fdm[1] == 0x00)
			return BB_FACTORY_BAD;
		return BB_UNKNOWN_BAD;
	}

	if (fdm[0] == 0xff)
		return fdm[1] == 0xff ? BB_GOOD : BB_POOL_BAD;
	if (fdm[0] == BLOCK_WORN_MARK)
		return BB_WORN;
	if (fdm[0] == 0x00)
		return BB_FACTORY_BAD;
	return BB_UNKNOWN_BAD;
}

static bool fdm_raw_bad_for_geometry(const u8 fdm[static 4])
{
	return fdm[0] != 0xff;
}

static bool fdm_skips_pool_block(const u8 fdm[static 4])
{
	return fdm[0] != 0xff || fdm[1] != 0xff;
}

static bool fdm_is_mapped(const u8 fdm[static 4])
{
	if (fdm_skips_pool_block(fdm))
		return false;
	return fdm[2] != 0xff || fdm[3] != 0xff;
}

/*
 * The ESMT F50L1G41A layout used by the OEM does not store the first four
 * logical FDM bytes contiguously.  Its physical OOB layout starts with the
 * bad-block marker at byte 0, followed by ECC bytes 1..7 and user metadata at
 * bytes 8..15.  The OEM compacts those areas, so logical FDM bytes 0..3 are
 * physical bytes 0, 8, 9 and 10.
 *
 * Only boards which explicitly opt in use this mapping. The legacy scan and
 * writer continue to use the original contiguous FDM representation.
 */
static int r_read_oem_esmt_fdm_page(const struct en75_bmt_m *ctx, u16 block,
				    u8 *data, u32 datalen,
				    u8 fdm[static 4])
{
	struct mtd_info *mtd = ctx->mtk->mtd;
	u8 phys[OEM_ESMT_FDM_PHYS_LEN];
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB,
		.ooboffs = 0,
		.oobbuf = phys,
		.ooblen = sizeof(phys),
		.datbuf = data,
		.len = datalen,
	};
	int ret;

	ret = oem_esmt_fdm_layout_valid(ctx);
	if (ret)
		return ret;

	memset(data, 0xff, datalen);
	memset(phys, 0xff, sizeof(phys));
	memset(fdm, 0xff, 4);
	ret = ctx->mtk->_read_oob(mtd,
				   ((loff_t)block) << ctx->mtk->blk_shift,
				   &ops);
	if (ret < 0)
		return ret;
	if (ops.retlen != datalen || ops.oobretlen != sizeof(phys))
		return -EIO;

	oem_esmt_fdm_gather(phys, fdm);

	return ret;
}

static int r_read_oem_esmt_fdm_page_normalized(
	const struct en75_bmt_m *ctx, u16 block, u8 *data, u32 datalen,
	u8 fdm[static 4])
{
	int ret = r_read_oem_esmt_fdm_page(ctx, block, data, datalen, fdm);

	if (ret > 0) {
		pr_info("%s: %d bitflips\n", __func__, ret);
		return 0;
	}

	return ret;
}

static int r_read_fdm_page_normalized(const struct en75_bmt_m *ctx,
				      u16 block, u8 *data, u32 datalen,
				      u8 fdm[static 4])
{
	if (ctx->esmt_oem_fdm)
		return r_read_oem_esmt_fdm_page_normalized(ctx, block, data,
							   datalen, fdm);

	return bbt_nand_read(blk_pg(block), data, datalen, fdm, 4);
}

/*
 * Read the first-page marker window without asking the ECC engine to decode
 * page data.  The OEM uses logical FDM byte 0 alone while calculating the
 * reserve geometry, even when that block's data is no longer decodable.
 */
static int r_read_oem_esmt_fdm_raw(const struct en75_bmt_m *ctx, u16 block,
				   u8 fdm[static OEM_ESMT_FDM_LEN])
{
	struct mtd_info *mtd = ctx->mtk->mtd;
	u8 phys[OEM_ESMT_FDM_PHYS_LEN];
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_RAW,
		.ooboffs = 0,
		.oobbuf = phys,
		.ooblen = sizeof(phys),
	};
	int ret;

	ret = oem_esmt_fdm_layout_valid(ctx);
	if (ret)
		return ret;

	memset(phys, 0xff, sizeof(phys));
	memset(fdm, 0xff, OEM_ESMT_FDM_LEN);
	ret = ctx->mtk->_read_oob(mtd,
				   ((loff_t)block) << ctx->mtk->blk_shift,
				   &ops);
	if (ret < 0)
		return ret;
	if (ops.retlen || ops.oobretlen != sizeof(phys))
		return -EIO;

	oem_esmt_fdm_gather(phys, fdm);
	return 0;
}

static void r_reconstruct_bmt(struct en75_bmt_m *ctx)
{
	int reserve_area_begin = ctx->reserve_area_begin;

	memset(&ctx->bmt, 0xff, sizeof(ctx->bmt.header));
	/*
	 * The table must be zero because the vendor firmware checksums
	 * over a number of entries equal to the number of blocks in the
	 * reserve area (note that when a block in the reserve area fails,
	 * this number will increase on next boot!). But we and the vendor
	 * firmware both store the entire table, and zeroed entries do not
	 * affect the checksum.
	 */
	memset(&ctx->bmt, 0x00, sizeof(ctx->bmt.table));
	memcpy(&ctx->bmt.header.signature, "BMT", 3);
	ctx->bmt.header.version = 1;
	ctx->bmt.header.size = 0;
	ctx->bmt_dirty++;

	for (int i = ctx->mtk->total_blks - 1; i >= reserve_area_begin; i--) {
		unsigned short mapped_block;
		u8 fdm[4];
		int ret;

		ret = r_read_fdm_page_normalized(
			ctx, i, ctx->mtk->data_buf,
			ctx->mtk->pg_size, fdm);
		if (ret < 0 || fdm_is_bad(ctx, fdm))
			continue;

		/* Vendor firmware uses host order. */
		memcpy(&mapped_block, &fdm[2], 2);
		if (mapped_block >= reserve_area_begin)
			continue;
		pr_info("%s: Found mapping %d->%d\n", log_pfx, mapped_block, i);
		ctx->bmt.table[ctx->bmt.header.size++] = (struct bmt_entry) {
			.from = mapped_block,
			.to = i
		};
	}
}

static int r_reconstruct_bbt(struct bbt_table *bbt_out, const struct en75_bmt_m *ctx)
{
	int reserve_area_begin = ctx->reserve_area_begin;
	int bmt_size = ctx->bmt.header.size;

	/* Need the BMT to exist in order to reconstruct the BBT. */
	if (WARN_ON_ONCE(!ctx->bmt.header.version))
		return -EINVAL;

	memset(bbt_out, 0xff, sizeof(bbt_out->header));
	/* Vendor firmware checksums the entire table, no matter how much is used. */
	memset(bbt_out->table, 0x00, ctx->bbt_table_size * sizeof(bbt_out->table[0]));
	memcpy(bbt_out->header.signature, "RAWB", 4);
	bbt_out->header.version = 1;
	bbt_out->header.size = 0;

	for (int i = 0; i < reserve_area_begin; i++) {
		bool is_mapped = false;
		int ret;
		u8 fdm[4];

		for (int j = 0; j < bmt_size; j++)
			if (ctx->bmt.table[j].from == i) {
				is_mapped = true;
				break;
			}

		if (is_mapped)
			continue;

		ret = r_read_fdm_page_normalized(
			ctx, i, ctx->mtk->data_buf,
			ctx->mtk->pg_size, fdm);
		if (!ret) {
			enum block_is_bad status = fdm_is_bad(ctx, fdm);

			if (status == BB_GOOD || status == BB_WORN ||
			    status == BB_POOL_BAD)
				continue;
		}

		pr_info("%s: Found factory bad block %d\n", log_pfx, i);
		bbt_out->table[bbt_out->header.size++] = (u16)i;
	}
	sort_bbt(bbt_out);
	return 0;
}

static bool block_is_erased(u8 *data, u32 datalen, u8 *oob, u32 ooblen)
{
	for (int i = 0; i < datalen; i++)
		if (data[i] != 0xff)
			return false;
	for (int i = 0; i < ooblen; i++)
		if (oob[i] != 0xff)
			return false;
	return true;
}

static int try_parse_bbt(struct bbt_table *out, u8 *buf, int len, u16 table_size)
{
	static struct bbt_table workspace;
	size_t bbt_size = sizeof(workspace.header) + table_size * sizeof(workspace.table[0]);

	if (len < bbt_size)
		return -EINVAL;

	memset(&workspace, 0, sizeof(workspace));
	memcpy(&workspace, buf, bbt_size);

	if (strncmp(workspace.header.signature, "RAWB", 4))
		return -EINVAL;

	if (workspace.header.checksum != bbt_checksum(&workspace, table_size))
		return -EINVAL;

	sort_bbt(&workspace);

	if (out)
		memcpy(out, &workspace, bbt_size);
	return 0;
}

static int try_parse_bmt(struct bmt_table *out, u8 *buf, int len)
{
	static struct bmt_table workspace;

	if (len < sizeof(*out))
		return -EINVAL;

	memcpy(&workspace, buf, sizeof(workspace));

	if (strncmp(workspace.header.signature, "BMT", 3))
		return -EINVAL;

	/*
	 * The vendor firmware checksums over rblocks entries, but zero
	 * values do not affect the checksum so this works.
	 * We don't know rblocks while we're scanning and in any case
	 * it's a moving target, if a block fails in the reserve area,
	 * rblocks will increase by one. So we use the size from the
	 * header and if the vendor firmware left some trash in the
	 * buffer after the last entry, we're going to have an invalid
	 * checksum.
	 */
	if (workspace.header.checksum !=
		bmt_checksum(&workspace, workspace.header.size))
		return -EINVAL;

	if (out)
		memcpy(out, &workspace, sizeof(workspace));
	return 0;
}

union table_candidate {
	struct bmt_table bmt;
	struct bbt_table bbt;
};

/*
 * Read a first page without normalising a positive MTD return value.  The
 * generic bbt_nand_read() helper converts corrected-bit counts to zero, which
 * is useful normally but hides evidence needed by the read-only diagnostic.
 */
static int r_read_candidate_page(const struct en75_bmt_m *ctx, u16 block,
				 u8 fdm[static 4], int *attempts)
{
	u8 *data_buf = ctx->mtk->data_buf;
	u32 pg_size = ctx->mtk->pg_size;
	int ret = -EIO;

	for (int i = 0; i < INITIAL_READ_TRIES; i++) {
		ret = r_read_oem_esmt_fdm_page(ctx, block, data_buf,
					       pg_size, fdm);
		*attempts = i + 1;
		if (ret >= 0)
			break;
	}

	return ret;
}

static bool r_copy_bmt_candidate(struct bmt_table *out, const u8 *buf, int len)
{
	if (len < sizeof(*out) || memcmp(buf, "BMT", 3))
		return false;

	memcpy(out, buf, sizeof(*out));
	return true;
}

static bool r_copy_bbt_candidate(struct bbt_table *out, const u8 *buf, int len,
				 u16 table_size)
{
	size_t bbt_size = sizeof(out->header) +
			  table_size * sizeof(out->table[0]);

	if (len < bbt_size || memcmp(buf, "RAWB", 4))
		return false;

	memset(out, 0, sizeof(*out));
	memcpy(out, buf, bbt_size);
	return true;
}

static bool r_bmt_ranges_valid(const struct en75_bmt_m *ctx,
			       const struct bmt_table *candidate)
{
	int pool_size = reserve_block_count(ctx);

	if (candidate->header.size > pool_size)
		return false;

	for (int i = 0; i < candidate->header.size; i++) {
		const struct bmt_entry entry = candidate->table[i];

		if (entry.from >= ctx->reserve_area_begin ||
		    entry.to < ctx->reserve_area_begin ||
		    entry.to >= ctx->mtk->total_blks)
			return false;
	}

	return true;
}

static bool r_bbt_ranges_valid(const struct en75_bmt_m *ctx,
			       const struct bbt_table *candidate)
{
	if (candidate->header.size > ctx->bbt_table_size)
		return false;

	for (int i = 0; i < candidate->header.size; i++)
		if (candidate->table[i] >= ctx->reserve_area_begin)
			return false;

	return true;
}

/* Compare the complete checksum domain, excluding unused header fields. */
static bool r_bmt_candidates_identical(const struct bmt_table *a,
				       const struct bmt_table *b,
				       int pool_size)
{
	return a->header.version == b->header.version &&
	       a->header.size == b->header.size &&
	       a->header.checksum == b->header.checksum &&
	       !memcmp(a->table, b->table,
		       pool_size * sizeof(a->table[0]));
}

static bool r_bbt_candidates_identical(const struct bbt_table *a,
				       const struct bbt_table *b,
				       u16 table_size)
{
	return a->header.version == b->header.version &&
	       a->header.size == b->header.size &&
	       a->header.checksum == b->header.checksum &&
	       !memcmp(a->table, b->table,
		       table_size * sizeof(a->table[0]));
}

/*
 * Repeat the reserve scan after its exact size is known.  This mirrors the
 * OEM's scan direction and copy preference: highest acceptable BMT, lowest
 * acceptable BBT.  Selection here is intentionally stricter than the OEM:
 * marked, unreadable, unsupported-version or out-of-range tables are never
 * promoted into the in-memory state.
 */
static int r_select_table_copies(struct en75_bmt_m *ctx)
{
	union table_candidate *candidate;
	int pool_size = reserve_block_count(ctx);
	int selected_bmt = -1;
	int selected_bbt = -1;
	bool bmt_conflict = false;
	bool bbt_conflict = false;
	u8 fdm[4];

	if (pool_size <= 0 || pool_size > MAX_BMT_SIZE) {
		pr_err("%s: reserve size %d cannot be checked as an OEM BMT domain\n",
		       log_pfx, pool_size);
		return -EOVERFLOW;
	}

	candidate = kmalloc(sizeof(*candidate), GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;

	memset(&ctx->bmt, 0, sizeof(ctx->bmt));
	memset(&ctx->bbt, 0, sizeof(ctx->bbt));

	/* OEM load_bmt_data(): scan from the end and keep the first valid copy. */
	for (int block = ctx->mtk->total_blks - 1;
	     block >= ctx->reserve_area_begin; block--) {
		const char *relation = "rejected";
		bool current_ok;
		bool oem_ok;
		bool ranges_ok;
		bool acceptable;
		int attempts = 0;
		int ret;

		ret = r_read_candidate_page(ctx, block, fdm, &attempts);
		if (!r_copy_bmt_candidate(&candidate->bmt,
					  ctx->mtk->data_buf,
					  ctx->mtk->pg_size))
			continue;

		current_ok = candidate->bmt.header.checksum ==
			bmt_checksum(&candidate->bmt, candidate->bmt.header.size);
		oem_ok = candidate->bmt.header.checksum ==
			bmt_checksum(&candidate->bmt, pool_size);
		ranges_ok = r_bmt_ranges_valid(ctx, &candidate->bmt);
		acceptable = ret >= 0 && !fdm_skips_pool_block(fdm) &&
			     candidate->bmt.header.version == 1 &&
			     oem_ok && ranges_ok;

		if (acceptable && selected_bmt < 0) {
			memcpy(&ctx->bmt, &candidate->bmt, sizeof(ctx->bmt));
			selected_bmt = block;
			relation = "selected-highest";
		} else if (acceptable) {
			if (r_bmt_candidates_identical(
				    &ctx->bmt, &candidate->bmt, pool_size)) {
				relation = "identical";
			} else {
				relation = "CONFLICT";
				bmt_conflict = true;
			}
		}

		pr_info("%s: BMT candidate block=%d ret=%d attempts=%d fdm=%02x:%02x:%02x:%02x version=%u size=%u current-csum=%s oem-csum[%d]=%s ranges=%s relation=%s\n",
			log_pfx, block, ret, attempts,
			fdm[0], fdm[1], fdm[2], fdm[3],
			candidate->bmt.header.version,
			candidate->bmt.header.size,
			current_ok ? "ok" : "bad", pool_size,
			oem_ok ? "ok" : "bad",
			ranges_ok ? "ok" : "bad", relation);
	}

	/* OEM load_bbt_data(): scan from the pool start and keep the first. */
	for (int block = ctx->reserve_area_begin;
	     block < ctx->mtk->total_blks; block++) {
		const char *relation = "rejected";
		bool checksum_ok;
		bool ranges_ok;
		bool acceptable;
		int attempts = 0;
		int ret;

		ret = r_read_candidate_page(ctx, block, fdm, &attempts);
		if (!r_copy_bbt_candidate(&candidate->bbt,
					  ctx->mtk->data_buf,
					  ctx->mtk->pg_size,
					  ctx->bbt_table_size))
			continue;

		checksum_ok = candidate->bbt.header.checksum ==
			bbt_checksum(&candidate->bbt, ctx->bbt_table_size);
		ranges_ok = r_bbt_ranges_valid(ctx, &candidate->bbt);
		acceptable = ret >= 0 && !fdm_skips_pool_block(fdm) &&
			     candidate->bbt.header.version == 1 &&
			     checksum_ok && ranges_ok;

		if (acceptable && selected_bbt < 0) {
			memcpy(&ctx->bbt, &candidate->bbt, sizeof(ctx->bbt));
			selected_bbt = block;
			relation = "selected-lowest";
		} else if (acceptable) {
			if (r_bbt_candidates_identical(
				    &ctx->bbt, &candidate->bbt,
				    ctx->bbt_table_size)) {
				relation = "identical";
			} else {
				relation = "CONFLICT";
				bbt_conflict = true;
			}
		}

		pr_info("%s: BBT candidate block=%d ret=%d attempts=%d fdm=%02x:%02x:%02x:%02x version=%u size=%u csum[%u]=%s ranges=%s relation=%s\n",
			log_pfx, block, ret, attempts,
			fdm[0], fdm[1], fdm[2], fdm[3],
			candidate->bbt.header.version,
			candidate->bbt.header.size,
			ctx->bbt_table_size,
			checksum_ok ? "ok" : "bad",
			ranges_ok ? "ok" : "bad", relation);
	}

	if (selected_bmt >= 0)
		pr_info("%s: selected high-to-low diagnostic BMT copy at block %d\n",
			log_pfx, selected_bmt);
	if (selected_bbt >= 0) {
		pr_info("%s: selected low-to-high diagnostic BBT copy at block %d\n",
			log_pfx, selected_bbt);
		sort_bbt(&ctx->bbt);
	}

	kfree(candidate);
	if (bmt_conflict || bbt_conflict) {
		pr_err("%s: conflicting valid table copies; refusing attach\n",
		       log_pfx);
		return -EUCLEAN;
	}
	return 0;
}

static int r_scan_reserve_legacy(struct en75_bmt_m *ctx)
{
	u16 total_blks = ctx->mtk->total_blks;
	int cursor = total_blks - 1;
	int good_blocks = 0;
	int rblock = 0;
	int rblocks_available = 0;
	u8 fdm[4];

	for (; cursor > 0; cursor--) {
		int ret;
		u8 *data_buf = ctx->mtk->data_buf;
		u32 pg_size = ctx->mtk->pg_size;

		if (rblock >= rblocks_available) {
			rblocks_available += cursor;
			ctx->rblocks = krealloc(
				ctx->rblocks,
				rblocks_available * sizeof(*ctx->rblocks),
				GFP_KERNEL);
			if (!ctx->rblocks)
				return -ENOMEM;
		}

		for (int i = 0; i < INITIAL_READ_TRIES; i++) {
			ret = bbt_nand_read(blk_pg(cursor),
					    data_buf,
					    pg_size,
					    fdm, sizeof(fdm));
			if (!ret)
				break;
		}
		struct block_info bif = {
			.index = { .index = cursor },
			.status = BS_INVALID
		};

		if (ret || fdm_is_bad(ctx, fdm)) {
			pr_info("%s: skipping bad block %d in reserve area\n",
				log_pfx, cursor);
			bif.status = BS_BAD;
		} else if (fdm_is_mapped(fdm)) {
			pr_debug("%s: found mapped block %d\n", log_pfx, cursor);
			bif.status = BS_MAPPED;
		} else if (!try_parse_bbt(&ctx->bbt, data_buf, pg_size,
					  ctx->bbt_table_size)) {
			pr_info("%s: found BBT in block %d\n", log_pfx, cursor);
			bif.status = BS_BBT;
		} else if (!try_parse_bmt(&ctx->bmt, data_buf, pg_size)) {
			pr_info("%s: found BMT in block %d\n", log_pfx, cursor);
			bif.status = BS_BMT;
		} else if (block_is_erased(data_buf, pg_size, fdm,
					   sizeof(fdm))) {
			pr_debug("%s: found available block %d\n", log_pfx, cursor);
			bif.status = BS_AVAILABLE;
		} else {
			pr_debug("%s: found block needing erase %d\n",
				 log_pfx, cursor);
			bif.status = BS_NEED_ERASE;
		}

		ctx->rblocks[rblock++] = bif;
		good_blocks += (bif.status != BS_BAD);

		if (good_blocks >= REQUIRED_GOOD_BLOCKS(total_blks))
			break;
	}
	if (!cursor) {
		pr_err("%s: not enough valid blocks found, need %d got %d\n",
		       log_pfx, REQUIRED_GOOD_BLOCKS(total_blks), good_blocks);
		return -ENOSPC;
	}
	ctx->reserve_area_begin = cursor;
	return 0;
}

static int r_scan_reserve_esmt(struct en75_bmt_m *ctx)
{
	u16 total_blks = ctx->mtk->total_blks;
	int cursor = total_blks - 1;
	int raw_good_blocks = 0;
	int rblock = 0;
	u8 raw_fdm[OEM_ESMT_FDM_LEN];

	/*
	 * Phase A mirrors calc_bmt_pool_size(): only logical FDM byte 0
	 * contributes to geometry.  Logical byte 1 retires a pool block, but it
	 * must not move reserve_area_begin or change the BMT checksum domain.
	 */
	for (; cursor > 0; cursor--) {
		int ret = -EIO;

		for (int i = 0; i < INITIAL_READ_TRIES; i++) {
			ret = r_read_oem_esmt_fdm_raw(ctx, cursor, raw_fdm);
			if (!ret)
				break;
		}
		if (ret) {
			pr_err("%s: cannot determine raw marker for reserve block %d: %d\n",
			       log_pfx, cursor, ret);
			return ret;
		}

		if (!fdm_raw_bad_for_geometry(raw_fdm) &&
		    ++raw_good_blocks >= REQUIRED_GOOD_BLOCKS(total_blks))
			break;
	}
	if (!cursor) {
		pr_err("%s: not enough raw-good blocks found, need %d got %d\n",
		       log_pfx, REQUIRED_GOOD_BLOCKS(total_blks), raw_good_blocks);
		return -ENOSPC;
	}

	ctx->reserve_area_begin = cursor;
	ctx->rblocks = kcalloc(reserve_block_count(ctx),
			       sizeof(*ctx->rblocks), GFP_KERNEL);
	if (!ctx->rblocks)
		return -ENOMEM;

	/*
	 * Phase B classifies the now-fixed pool.  Both OEM marker bytes make a
	 * block unavailable, but neither a logical-byte-1 marker nor a data ECC
	 * failure is allowed to retroactively alter the geometry from phase A.
	 */
	for (int block = total_blks - 1; block >= ctx->reserve_area_begin;
	     block--) {
		u8 page_fdm[OEM_ESMT_FDM_LEN];
		u8 *data_buf = ctx->mtk->data_buf;
		u32 pg_size = ctx->mtk->pg_size;
		int marker_ret = -EIO;
		int data_ret = -EIO;

		for (int i = 0; i < INITIAL_READ_TRIES; i++) {
			marker_ret = r_read_oem_esmt_fdm_raw(ctx, block, raw_fdm);
			if (!marker_ret)
				break;
		}

		if (!marker_ret && !fdm_skips_pool_block(raw_fdm)) {
			for (int i = 0; i < INITIAL_READ_TRIES; i++) {
				data_ret = r_read_oem_esmt_fdm_page_normalized(
					ctx, block, data_buf, pg_size, page_fdm);
				if (!data_ret)
					break;
			}
		}

		struct block_info bif = {
			.index = { .index = block },
			.status = BS_INVALID
		};

		if (marker_ret) {
			pr_warn("%s: marker read failed for reserve block %d: %d\n",
				log_pfx, block, marker_ret);
			bif.status = BS_BAD;
		} else if (fdm_skips_pool_block(raw_fdm)) {
			pr_info("%s: skipping OEM-marked block %d in reserve area\n",
				log_pfx, block);
			bif.status = BS_BAD;
		} else if (data_ret) {
			pr_info("%s: skipping unreadable block %d in reserve area: %d\n",
				log_pfx, block, data_ret);
			bif.status = BS_BAD;
		} else if (memcmp(raw_fdm, page_fdm, OEM_ESMT_FDM_LEN)) {
			pr_warn("%s: inconsistent raw/ECC FDM for reserve block %d\n",
				log_pfx, block);
			bif.status = BS_BAD;
		} else if (!try_parse_bbt(NULL, data_buf, pg_size,
					  ctx->bbt_table_size)) {
			pr_info("%s: found BBT in block %d\n", log_pfx, block);
			bif.status = BS_BBT;
		} else if (!try_parse_bmt(NULL, data_buf, pg_size)) {
			pr_info("%s: found BMT in block %d\n", log_pfx, block);
			bif.status = BS_BMT;
		} else if (fdm_is_mapped(raw_fdm)) {
			pr_debug("%s: found mapped block %d\n", log_pfx, block);
			bif.status = BS_MAPPED;
		} else if (block_is_erased(data_buf, pg_size, raw_fdm,
					   sizeof(raw_fdm))) {
			pr_debug("%s: found available block %d\n", log_pfx, block);
			bif.status = BS_AVAILABLE;
		} else {
			pr_debug("%s: found block needing erase %d\n", log_pfx, block);
			bif.status = BS_NEED_ERASE;
		}

		ctx->rblocks[rblock++] = bif;
	}

	return 0;
}

static int w_factory_badblocks(struct en75_bmt_m *ctx, const u32 *blocks, int count)
{
	if (WARN_ON_ONCE(!ctx->bbt.header.version))
		return -EINVAL;
	if (WARN_ON_ONCE(!ctx->bmt.header.version))
		return -EINVAL;

	for (int i = 0; i < count; i++) {
		if (blocks[i] >= ctx->reserve_area_begin) {
			pr_err("%s: factory bad block %d not in user area\n",
			       log_pfx, blocks[i]);
			return -EINVAL;
		}
	}

	if (count > ctx->bbt_table_size) {
		pr_err("%s: Can't set %d factory bad blocks, limit is %d\n",
		       log_pfx, count, ctx->bbt_table_size);
		return -ENOSPC;
	}

	/* If ctx->can_write_factory_bbt is not set, this is just an assertion. */
	if (!ctx->can_write_factory_bbt) {
		if (ctx->bbt.header.size != count) {
			pr_err("%s: factory bad block count mismatch %d != %d\n",
			       log_pfx, ctx->bbt.header.size, count);
			return -EIO;
		}
		for (int i = 0; i < count; i++)
			for (int j = 0; j < ctx->bbt.header.size; j++) {
				if (ctx->bbt.table[j] == blocks[i])
					break;
				if (j == ctx->bbt.header.size - 1) {
					pr_err("%s: factory bad block %d not in BBT\n",
					       log_pfx, blocks[i]);
					return -EIO;
				}
			}
		return 0;
	}

	/*
	 * Clear the BBT, and un-bad the blocks that will not be added back.
	 * We have to clear it because if we're adding a block, we can't
	 * properly check if it's mapped in the BMT unless it's removed from
	 * the BBT.
	 */
	for (int i = ctx->bbt.header.size - 1; i >= 0; i--) {
		int j = 0;

		for (; j < count; j++)
			if (ctx->bbt.table[i] == blocks[j])
				break;

		ctx->bbt.header.size--;
		ctx->bbt.table[i] = ctx->bbt.table[ctx->bbt.header.size];
		ctx->bbt.table[ctx->bbt.header.size] = 0;

		/* It's going to be added back so let it stay bad. */
		if (j < count)
			continue;

		/*
		 * Try to erase the bad marker, if it's impossible to erase
		 * then the BMT is going to catch it and map it out later.
		 * If you're running w_factory_badblocks(), then you're
		 * changing offsets so data loss is already guaranteed.
		 */
		w_erase_one(ctx,
			    (struct block_index) { .index = ctx->bbt.table[i] },
			    true);
	}

	/* Unmap any block that is mapped in the BMT */
	for (int i = 0; i < count; i++) {
		int mapped_block;
		int ret;

		mapped_block = get_mapping_block(ctx, blocks[i]);
		if (mapped_block < ctx->reserve_area_begin) {
			pr_info("%s: factory bad block %d not mapped, no unmap needed\n",
				log_pfx, blocks[i]);
			continue; /* not mapped */
		}

		pr_info("%s: unmapping block %d to set as factory bad block\n",
			log_pfx, blocks[i]);
		ret = w_unmap_block(ctx, blocks[i], UE_NO_ERASE);
		if (ret) {
			pr_warn("%s: failed unmapping %d to set as factory bad block\n",
				log_pfx, blocks[i]);
		}
	}

	/* Add everything in the list to the BBT */
	for (int i = 0; i < count; i++) {
		int j = 0;

		for (; j < ctx->bbt.header.size; j++)
			if (ctx->bbt.table[j] == blocks[i])
				break;

		if (j < ctx->bbt.header.size)
			continue;

		ctx->bbt.table[ctx->bbt.header.size++] = blocks[i];

		w_mark_bad(ctx, (struct block_index) { .index = blocks[i] }, true);
	}

	sort_bbt(&ctx->bbt);
	ctx->bbt_dirty++;

	return 0;
}

static int add_remap_range(struct en75_bmt_m *ctx, u16 begin_block, u16 size_blocks)
{
	if (((int)begin_block) + size_blocks > ctx->reserve_area_begin) {
		pr_err("%s: remap range %d->%d exceeds user area\n",
		       log_pfx, begin_block, size_blocks);
		return -EINVAL;
	}
	ctx->can_remap_ranges = krealloc(
		ctx->can_remap_ranges,
		(ctx->can_remap_range_count + 1) * sizeof(*ctx->can_remap_ranges),
		GFP_KERNEL);
	if (!ctx->can_remap_ranges)
		return -ENOMEM;
	ctx->can_remap_ranges[ctx->can_remap_range_count++] = (struct block_range) {
		.begin = begin_block,
		.end = begin_block + size_blocks
	};
	return 0;
}

static int w_init(struct en75_bmt_m *ctx, struct device_node *np)
{
	struct device_node *parts_np;
	u32 factory_badblocks[MAX_FACTORY_BAD_BLOCKS_OF];
	int factory_badblocks_count = -1;
	int assert_reserve_size = -1;
	bool child_remap = false;
	u32 bbt_table_size;
	int ret;

	if (of_property_read_u32(np, name_bbt_table_size, &bbt_table_size))
		bbt_table_size = DEFAULT_BBT_TABLE_SIZE;

	if (bbt_table_size > MAX_BBT_SIZE) {
		pr_err("%s: %s=%d exceeds MAX_BBT_SIZE=%d\n",
		       log_pfx, name_bbt_table_size, bbt_table_size, MAX_BBT_SIZE);
		return -EINVAL;
	}
	ctx->bbt_table_size = bbt_table_size;
	ctx->esmt_oem_fdm = of_property_read_bool(np, name_esmt_oem_fdm);
	ctx->read_only = of_property_read_bool(np, name_read_only);
	ctx->write_through = of_property_read_bool(np, name_write_through);
	if (ctx->read_only && ctx->write_through) {
		pr_err("%s: %s and %s are mutually exclusive\n", log_pfx,
		       name_read_only, name_write_through);
		return -EINVAL;
	}
	if ((ctx->read_only || ctx->write_through) && !ctx->esmt_oem_fdm) {
		pr_err("%s: %s or %s requires %s\n", log_pfx,
		       name_read_only, name_write_through, name_esmt_oem_fdm);
		return -EINVAL;
	}
	if (ctx->esmt_oem_fdm && !ctx->read_only && !ctx->write_through) {
		pr_err("%s: %s requires a static BMT mode\n", log_pfx,
		       name_esmt_oem_fdm);
		return -EINVAL;
	}

	if (ctx->esmt_oem_fdm)
		ret = r_scan_reserve_esmt(ctx);
	else
		ret = r_scan_reserve_legacy(ctx);
	if (ret)
		return ret;

	if (!of_property_read_u32(np, name_assert_reserve_size, &assert_reserve_size)) {
		if (assert_reserve_size != reserve_block_count(ctx)) {
			pr_err("%s: reserve area size mismatch %d != %d\n",
			       log_pfx, assert_reserve_size, reserve_block_count(ctx));
			return -EINVAL;
		}
	}
	if (ctx->esmt_oem_fdm) {
		ret = r_select_table_copies(ctx);
		if (ret)
			return ret;
	}
	if (ctx->write_through &&
	    (!ctx->bmt.header.version || !ctx->bbt.header.version)) {
		pr_err("%s: write-through requires existing valid BMT and BBT\n",
		       log_pfx);
		return -EIO;
	}

	/* Diagnostic mode must override even a contradictory write property. */
	ctx->can_write_factory_bbt = 0;
	if (!ctx->read_only && !ctx->write_through &&
	    of_property_read_bool(np, name_can_write_factory_bbt))
		ctx->can_write_factory_bbt = 1;

	ret = of_property_read_variable_u32_array(np, name_factory_badblocks,
						  factory_badblocks,
						  0, MAX_FACTORY_BAD_BLOCKS_OF);
	if (ret >= 0)
		factory_badblocks_count = ret;

	if (ctx->write_through) {
		parts_np = of_get_child_by_name(np, "partitions");
		for_each_child_of_node_scoped(parts_np, part_np) {
			if (of_property_read_bool(part_np, name_enable_remap)) {
				child_remap = true;
				break;
			}
		}
		if (parts_np)
			of_node_put(parts_np);

		if (of_property_read_bool(np, name_can_write_factory_bbt) ||
		    factory_badblocks_count >= 0 ||
		    of_property_read_bool(np, name_enable_remap) || child_remap) {
			pr_err("%s: write-through conflicts with BBT/remap write properties\n",
			       log_pfx);
			return -EINVAL;
		}
	}

	if (ctx->read_only) {
		pr_info("%s: read-only diagnostic mode; remapping disabled\n",
			log_pfx);
	} else if (ctx->write_through) {
		pr_warn("%s: writable pass-through mode; dynamic remapping and table updates disabled\n",
			log_pfx);
	} else if (of_property_read_bool(np, name_enable_remap)) {
		add_remap_range(ctx, 0, ctx->reserve_area_begin);
	} else {
		struct device_node *parts_np;

		parts_np = of_get_child_by_name(np, "partitions");
		for_each_child_of_node_scoped(parts_np, part_np) {
			u32 start;
			u32 size;
			const __be32 *reg;
			int ret;

			if (!of_property_read_bool(np, name_enable_remap))
				continue;

			reg = of_get_property(part_np, "reg", NULL);
			if (!reg) {
				pr_warn("%s: can't enable-remap on %pOF, no reg property\n",
					log_pfx, part_np);
				continue;
			}

			start = be32_to_cpup(reg);
			if (start & ((1 << ctx->mtk->blk_shift) - 1)) {
				pr_warn("%s: can't enable-remap on %pOF start not aligned\n",
					log_pfx, part_np);
				continue;
			}
			size = be32_to_cpup(&reg[1]);
			if (size & ((1 << ctx->mtk->blk_shift) - 1)) {
				pr_warn("%s: can't enable-remap on %pOF size not aligned\n",
					log_pfx, part_np);
				continue;
			}
			ret = add_remap_range(ctx,
					      start >> ctx->mtk->blk_shift,
					      size >> ctx->mtk->blk_shift);
			if (ret)
				pr_warn("%s: failed enable-remap on %pOF: %d\n",
					log_pfx, part_np, ret);
			else
				pr_info("%s: enable-remap set for %pOF\n",
					log_pfx, part_np);
		}
		if (parts_np)
			of_node_put(parts_np);
	}

	if (ctx->bbt.header.version) {
		if (!ctx->bmt.header.version) {
			pr_info("%s: BBT found, BMT missing or corrupted\n", log_pfx);
			r_reconstruct_bmt(ctx);
		} else {
			pr_info("%s: BBT & BMT found\n", log_pfx);
		}
	} else if (!ctx->can_write_factory_bbt) {
		pr_err("%s: BBT not found and %s is unset, giving up\n",
		       log_pfx, name_can_write_factory_bbt);
		return -EIO;
	} else if (factory_badblocks_count > -1) {
		pr_info("%s: BBT not found, reconstructing from %s\n",
			log_pfx, name_factory_badblocks);
		if (!ctx->bmt.header.version)
			r_reconstruct_bmt(ctx);
	} else if (ctx->bmt.header.version) {
		pr_info("%s: BBT not found, BMT found, attempting reconstruction\n",
			log_pfx);
		ret = r_reconstruct_bbt(&ctx->bbt, ctx);
		if (ret)
			return ret;
	} else {
		pr_warn("%s: No BBT or BMT found, attempting reconstruction, LIKELY DATA LOSS!\n",
			log_pfx);
		r_reconstruct_bmt(ctx);
		ret = r_reconstruct_bbt(&ctx->bbt, ctx);
		if (ret)
			return ret;
	}

	if (factory_badblocks_count > -1) {
		int ret = w_factory_badblocks(ctx,
					      factory_badblocks,
					      factory_badblocks_count);
		if (ret)
			return ret;
	}

	pr_info("%s: blocks: total: %d, user: %d, factory_bad: %d, worn: %d reserve: %d\n",
		log_pfx,
		ctx->mtk->total_blks,
		ctx->reserve_area_begin - ctx->bbt.header.size,
		ctx->bbt.header.size,
		ctx->bmt.header.size,
		reserve_block_count(ctx)
	);

	for (int i = 0; i < ctx->bbt.header.size; i++)
		pr_info(" - BBT factory bad block: %d\n", ctx->bbt.table[i]);
	for (int i = 0; i < ctx->bmt.header.size; i++)
		pr_info(" - BMT mapped worn block: %d->%d\n",
			ctx->bmt.table[i].from, ctx->bmt.table[i].to);

	ctx->mtk->mtd->size =
		(ctx->reserve_area_begin - ctx->bbt.header.size) << ctx->mtk->blk_shift;
	pr_info("%s: %u MiB usable space", log_pfx, (u32)ctx->mtk->mtd->size >> 20);

	return 0;
}

/*
 * Public functions (only these have direct access to the context)
 */

static struct en75_bmt_m en75_bmt_m;

static int pub_init(struct device_node *np)
{
	int ret;

	ret = w_init(&en75_bmt_m, np);
	if (!ret && !en75_bmt_m.read_only && !en75_bmt_m.write_through)
		w_sync_tables(&en75_bmt_m);
	else if (!ret && en75_bmt_m.read_only)
		pr_info("%s: read-only diagnostic mode; table sync skipped\n",
			log_pfx);
	else if (!ret)
		pr_info("%s: writable pass-through mode; table sync skipped\n",
			log_pfx);
	return ret;
}

/*
 * If we return true, mtk_bmt will retry the operation and if it continues
 * to fail, it will call us back 9 more times. If we return false, the user
 * gets an error immediately.
 *
 * mtk_bmt might be calling us because:
 * 1. user tried to read and it failed
 * 2. user tried to read and there was a "concerning" amount of bit errors
 *    in this case, the user does not get an error if we return false.
 * 3. user tried to write or erase and it failed
 *
 */
static bool pub_remap_block(
	u16 user_block,
	u16 mapped_block_idx,
	int copy_len)
{
	u16 block;
	struct block_info *maybe_mapped_block = NULL;
	int ret;

	if (en75_bmt_m.read_only)
		return false;
	if (en75_bmt_m.write_through) {
		pr_err_ratelimited("%s: block %u failed in pass-through mode; dynamic remap disabled\n",
				   log_pfx, user_block);
		return false;
	}

	for (int i = 0; i < en75_bmt_m.can_remap_range_count; i++) {
		if (user_block >= en75_bmt_m.can_remap_ranges[i].begin &&
		    user_block < en75_bmt_m.can_remap_ranges[i].end)
			goto in_range;
	}
	return false;

in_range:

	block = get_mapping_block_bbt(&en75_bmt_m, user_block);
	if (block < 0 || user_block >= en75_bmt_m.reserve_area_begin) {
		pr_info("%s: remap: block %d out of range\n",
			log_pfx, user_block);
		return false;
	}

	if (mapped_block_idx != block) {
		int rblocks = reserve_block_count(&en75_bmt_m);

		for (int i = 0; i < rblocks; i++)
			if (en75_bmt_m.rblocks[i].index.index == mapped_block_idx) {
				maybe_mapped_block = &en75_bmt_m.rblocks[i];
				break;
			}
		if (WARN_ON_ONCE(!maybe_mapped_block))
			return false;
	}

	ret = w_remap_block(&en75_bmt_m, block, maybe_mapped_block, copy_len);
	w_sync_tables(&en75_bmt_m);
	if (ret == -ENOSPC)
		return false;

	return true;
}

static void pub_unmap_block(u16 user_block)
{
	int block;

	if (en75_bmt_m.read_only || en75_bmt_m.write_through)
		return;

	block = get_mapping_block_bbt(&en75_bmt_m, user_block);
	if (block < 0 || user_block >= en75_bmt_m.reserve_area_begin) {
		pr_info("%s: unmap: block %d out of range\n",
			log_pfx, user_block);
		return;
	}
	w_unmap_block(&en75_bmt_m, block, UE_REQUIRE_ERASE);
	w_sync_tables(&en75_bmt_m);
}

static int pub_debug(void *data, u64 val)
{
	return 0;
}

static int pub_get_mapping_block(int user_block)
{
	return get_mapping_block(&en75_bmt_m, user_block);
}

#undef bmtd
static struct en75_bmt_m en75_bmt_m = {
	.mtk = &bmtd,
};

const struct mtk_bmt_ops en75_bmt_ops = {
	.init = pub_init,
	.remap_block = pub_remap_block,
	.unmap_block = pub_unmap_block,
	.get_mapping_block = pub_get_mapping_block,
	.debug = pub_debug,
};
