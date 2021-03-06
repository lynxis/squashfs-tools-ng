/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * xattr_writer.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#define SQFS_BUILDING_DLL
#include "config.h"

#include "sqfs/xattr_writer.h"
#include "sqfs/meta_writer.h"
#include "sqfs/super.h"
#include "sqfs/xattr.h"
#include "sqfs/error.h"
#include "sqfs/block.h"
#include "sqfs/io.h"

#include "str_table.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>


#define XATTR_KEY_BUCKETS 31
#define XATTR_VALUE_BUCKETS 511
#define XATTR_INITIAL_PAIR_CAP 128

#define MK_PAIR(key, value) (((sqfs_u64)(key) << 32UL) | (sqfs_u64)(value))
#define GET_KEY(pair) ((pair >> 32UL) & 0x0FFFFFFFFUL)
#define GET_VALUE(pair) (pair & 0x0FFFFFFFFUL)


static const char *hexmap = "0123456789ABCDEF";

static char *to_base32(const void *input, size_t size)
{
	const sqfs_u8 *in = input;
	char *out, *ptr;
	size_t i;

	out = malloc(2 * size + 1);
	if (out == NULL)
		return NULL;

	ptr = out;

	for (i = 0; i < size; ++i) {
		*(ptr++) = hexmap[ in[i]       & 0x0F];
		*(ptr++) = hexmap[(in[i] >> 4) & 0x0F];
	}

	*ptr = '\0';
	return out;
}

static void *from_base32(const char *input, size_t *size_out)
{
	sqfs_u8 lo, hi, *out, *ptr;
	size_t len;

	len = strlen(input);
	*size_out = len / 2;

	out = malloc(*size_out);
	if (out == NULL)
		return NULL;

	ptr = out;

	while (*input != '\0') {
		lo = strchr(hexmap, *(input++)) - hexmap;
		hi = strchr(hexmap, *(input++)) - hexmap;

		*(ptr++) = lo | (hi << 4);
	}

	return out;
}

static int compare_u64(const void *a, const void *b)
{
	sqfs_u64 lhs = *((const sqfs_u64 *)a);
	sqfs_u64 rhs = *((const sqfs_u64 *)b);

	return (lhs < rhs ? -1 : (lhs > rhs ? 1 : 0));
}



typedef struct kv_block_desc_t {
	struct kv_block_desc_t *next;
	size_t start;
	size_t count;

	sqfs_u64 start_ref;
	size_t size_bytes;
} kv_block_desc_t;

struct sqfs_xattr_writer_t {
	sqfs_object_t base;

	str_table_t keys;
	str_table_t values;

	sqfs_u64 *kv_pairs;
	size_t max_pairs;
	size_t num_pairs;

	size_t kv_start;

	kv_block_desc_t *kv_blocks;
	size_t num_blocks;
};


static sqfs_object_t *xattr_writer_copy(const sqfs_object_t *obj)
{
	const sqfs_xattr_writer_t *xwr = (const sqfs_xattr_writer_t *)obj;
	kv_block_desc_t *blk, *it, **next;
	sqfs_xattr_writer_t *copy;

	copy = calloc(1, sizeof(*copy));
	if (copy == NULL)
		return NULL;

	memcpy(copy, xwr, sizeof(*xwr));

	if (str_table_copy(&copy->keys, &xwr->keys))
		goto fail_keys;

	if (str_table_copy(&copy->values, &xwr->values))
		goto fail_values;

	copy->max_pairs = xwr->num_pairs;
	copy->num_pairs = xwr->num_pairs;

	copy->kv_pairs = malloc(sizeof(copy->kv_pairs[0]) * xwr->num_pairs);
	if (copy->kv_pairs == NULL)
		goto fail_pairs;

	memcpy(copy->kv_pairs, xwr->kv_pairs,
	       sizeof(copy->kv_pairs[0]) * xwr->num_pairs);

	next = &(copy->kv_blocks);

	for (it = xwr->kv_blocks; it != NULL; it = it->next) {
		blk = malloc(sizeof(*blk));
		if (blk == NULL)
			goto fail_blk;

		memcpy(blk, it, sizeof(*it));
		blk->next = NULL;

		*next = blk;
		next = &(blk->next);
	}

	return (sqfs_object_t *)copy;
fail_blk:
	while (copy->kv_blocks != NULL) {
		blk = copy->kv_blocks;
		copy->kv_blocks = copy->kv_blocks->next;
		free(blk);
	}
fail_pairs:
	str_table_cleanup(&copy->values);
fail_values:
	str_table_cleanup(&copy->keys);
fail_keys:
	free(copy);
	return NULL;
}

static void xattr_writer_destroy(sqfs_object_t *obj)
{
	sqfs_xattr_writer_t *xwr = (sqfs_xattr_writer_t *)obj;
	kv_block_desc_t *blk;

	while (xwr->kv_blocks != NULL) {
		blk = xwr->kv_blocks;
		xwr->kv_blocks = xwr->kv_blocks->next;
		free(blk);
	}

	free(xwr->kv_pairs);
	str_table_cleanup(&xwr->values);
	str_table_cleanup(&xwr->keys);
	free(xwr);
}

sqfs_xattr_writer_t *sqfs_xattr_writer_create(void)
{
	sqfs_xattr_writer_t *xwr = calloc(1, sizeof(*xwr));

	if (str_table_init(&xwr->keys, XATTR_KEY_BUCKETS))
		goto fail_keys;

	if (str_table_init(&xwr->values, XATTR_VALUE_BUCKETS))
		goto fail_values;

	xwr->max_pairs = XATTR_INITIAL_PAIR_CAP;
	xwr->kv_pairs = alloc_array(sizeof(xwr->kv_pairs[0]), xwr->max_pairs);

	if (xwr->kv_pairs == NULL)
		goto fail_pairs;

	((sqfs_object_t *)xwr)->copy = xattr_writer_copy;
	((sqfs_object_t *)xwr)->destroy = xattr_writer_destroy;
	return xwr;
fail_pairs:
	str_table_cleanup(&xwr->values);
fail_values:
	str_table_cleanup(&xwr->keys);
fail_keys:
	free(xwr);
	return NULL;
}

int sqfs_xattr_writer_begin(sqfs_xattr_writer_t *xwr)
{
	xwr->kv_start = xwr->num_pairs;
	return 0;
}

int sqfs_xattr_writer_add(sqfs_xattr_writer_t *xwr, const char *key,
			  const void *value, size_t size)
{
	size_t i, key_index, old_value_index, value_index, new_count;
	sqfs_u64 kv_pair, *new;
	char *value_str;
	int err;

	if (sqfs_get_xattr_prefix_id(key) < 0)
		return SQFS_ERROR_UNSUPPORTED;

	/* resolve key and value into unique, incremental IDs */
	err = str_table_get_index(&xwr->keys, key, &key_index);
	if (err)
		return err;

	value_str = to_base32(value, size);
	if (value_str == NULL)
		return SQFS_ERROR_ALLOC;

	err = str_table_get_index(&xwr->values, value_str, &value_index);
	free(value_str);
	if (err)
		return err;

	str_table_add_ref(&xwr->values, value_index);

	if (sizeof(size_t) > sizeof(sqfs_u32)) {
		if (key_index > 0x0FFFFFFFFUL || value_index > 0x0FFFFFFFFUL)
			return SQFS_ERROR_OVERFLOW;
	}

	/* bail if already have the pair, overwrite if we have the key */
	kv_pair = MK_PAIR(key_index, value_index);

	for (i = xwr->kv_start; i < xwr->num_pairs; ++i) {
		if (xwr->kv_pairs[i] == kv_pair)
			return 0;

		if (GET_KEY(xwr->kv_pairs[i]) == key_index) {
			old_value_index = GET_VALUE(xwr->kv_pairs[i]);

			str_table_del_ref(&xwr->values, old_value_index);

			xwr->kv_pairs[i] = kv_pair;
			return 0;
		}
	}

	/* append it to the list */
	if (xwr->max_pairs == xwr->num_pairs) {
		new_count = xwr->max_pairs * 2;
		new = realloc(xwr->kv_pairs,
			      sizeof(xwr->kv_pairs[0]) * new_count);

		if (new == NULL)
			return SQFS_ERROR_ALLOC;

		xwr->kv_pairs = new;
		xwr->max_pairs = new_count;
	}

	xwr->kv_pairs[xwr->num_pairs++] = kv_pair;
	return 0;
}

int sqfs_xattr_writer_end(sqfs_xattr_writer_t *xwr, sqfs_u32 *out)
{
	kv_block_desc_t *blk, *blk_prev;
	size_t i, count, value_idx;
	sqfs_u32 index;
	int ret;

	count = xwr->num_pairs - xwr->kv_start;
	if (count == 0) {
		*out = 0xFFFFFFFF;
		return 0;
	}

	qsort(xwr->kv_pairs + xwr->kv_start, count,
	      sizeof(xwr->kv_pairs[0]), compare_u64);

	blk_prev = NULL;
	blk = xwr->kv_blocks;
	index = 0;

	while (blk != NULL) {
		if (blk->count == count) {
			ret = memcmp(xwr->kv_pairs + blk->start,
				     xwr->kv_pairs + xwr->kv_start,
				     sizeof(xwr->kv_pairs[0]) * count);

			if (ret == 0)
				break;
		}

		if (index == 0xFFFFFFFF)
			return SQFS_ERROR_OVERFLOW;

		++index;
		blk_prev = blk;
		blk = blk->next;
	}

	if (blk != NULL) {
		for (i = 0; i < count; ++i) {
			value_idx = GET_VALUE(xwr->kv_pairs[xwr->kv_start + i]);
			str_table_del_ref(&xwr->values, value_idx);

			value_idx = GET_VALUE(xwr->kv_pairs[blk->start + i]);
			str_table_add_ref(&xwr->values, value_idx);
		}

		xwr->num_pairs = xwr->kv_start;
	} else {
		blk = calloc(1, sizeof(*blk));
		if (blk == NULL)
			return SQFS_ERROR_ALLOC;

		blk->start = xwr->kv_start;
		blk->count = count;

		if (blk_prev == NULL) {
			xwr->kv_blocks = blk;
		} else {
			blk_prev->next = blk;
		}

		xwr->num_blocks += 1;
	}

	*out = index;
	return 0;
}

/*****************************************************************************/

static sqfs_s32 write_key(sqfs_meta_writer_t *mw, const char *key,
			  bool value_is_ool)
{
	sqfs_xattr_entry_t kent;
	int type, err;
	size_t len;

	type = sqfs_get_xattr_prefix_id(key);
	assert(type >= 0);

	key = strchr(key, '.');
	assert(key != NULL);
	++key;
	len = strlen(key);

	if (value_is_ool)
		type |= SQFS_XATTR_FLAG_OOL;

	memset(&kent, 0, sizeof(kent));
	kent.type = htole16(type);
	kent.size = htole16(len);

	err = sqfs_meta_writer_append(mw, &kent, sizeof(kent));
	if (err)
		return err;

	err = sqfs_meta_writer_append(mw, key, len);
	if (err)
		return err;

	return sizeof(kent) + len;
}

static sqfs_s32 write_value(sqfs_meta_writer_t *mw, const char *value_str,
			    sqfs_u64 *value_ref_out)
{
	sqfs_xattr_value_t vent;
	sqfs_u32 offset;
	sqfs_u64 block;
	size_t size;
	void *value;
	int err;

	value = from_base32(value_str, &size);
	if (value == NULL)
		return SQFS_ERROR_ALLOC;

	memset(&vent, 0, sizeof(vent));
	vent.size = htole32(size);

	sqfs_meta_writer_get_position(mw, &block, &offset);
	*value_ref_out = (block << 16) | (offset & 0xFFFF);

	err = sqfs_meta_writer_append(mw, &vent, sizeof(vent));
	if (err)
		goto fail;

	err = sqfs_meta_writer_append(mw, value, size);
	if (err)
		goto fail;

	free(value);
	return sizeof(vent) + size;
fail:
	free(value);
	return err;
}

static sqfs_s32 write_value_ool(sqfs_meta_writer_t *mw, sqfs_u64 location)
{
	sqfs_xattr_value_t vent;
	sqfs_u64 ref;
	int err;

	memset(&vent, 0, sizeof(vent));
	vent.size = htole32(sizeof(location));
	ref = htole64(location);

	err = sqfs_meta_writer_append(mw, &vent, sizeof(vent));
	if (err)
		return err;

	err = sqfs_meta_writer_append(mw, &ref, sizeof(ref));
	if (err)
		return err;

	return sizeof(vent) + sizeof(ref);
}

static bool should_store_ool(const char *val_str, size_t refcount)
{
	if (refcount < 2)
		return false;

	/*
	  Storing in line needs this many bytes: refcount * len

	  Storing out-of-line needs this many: len + (refcount - 1) * 8

	  Out-of-line prefereable iff refcount > 1 and:
	      refcount * len > len + (refcount - 1) * 8
	   => refcount * len - len > (refcount - 1) * 8
	   => (refcount - 1) * len > (refcount - 1) * 8
	   => len > 8
	 */
	return (strlen(val_str) / 2) > sizeof(sqfs_u64);
}

static int write_block_pairs(sqfs_xattr_writer_t *xwr, sqfs_meta_writer_t *mw,
			     kv_block_desc_t *blk, sqfs_u64 *ool_locations)
{
	sqfs_u32 key_idx, val_idx;
	const char *key_str, *value_str;
	sqfs_s32 diff, total = 0;
	size_t i, refcount;
	sqfs_u64 ref;

	for (i = 0; i < blk->count; ++i) {
		key_idx = GET_KEY(xwr->kv_pairs[blk->start + i]);
		val_idx = GET_VALUE(xwr->kv_pairs[blk->start + i]);

		key_str = str_table_get_string(&xwr->keys, key_idx);
		value_str = str_table_get_string(&xwr->values, val_idx);

		if (ool_locations[val_idx] == 0xFFFFFFFFFFFFFFFFUL) {
			diff = write_key(mw, key_str, false);
			if (diff < 0)
				return diff;
			total += diff;

			diff = write_value(mw, value_str, &ref);
			if (diff < 0)
				return diff;
			total += diff;

			refcount = str_table_get_ref_count(&xwr->values,
							   val_idx);

			if (should_store_ool(value_str, refcount))
				ool_locations[val_idx] = ref;
		} else {
			diff = write_key(mw, key_str, true);
			if (diff < 0)
				return diff;
			total += diff;

			diff = write_value_ool(mw, ool_locations[val_idx]);
			if (diff < 0)
				return diff;
			total += diff;
		}
	}

	return total;
}

static int write_kv_pairs(sqfs_xattr_writer_t *xwr, sqfs_meta_writer_t *mw)
{
	sqfs_u64 block, *ool_locations;
	kv_block_desc_t *blk;
	sqfs_u32 offset;
	sqfs_s32 size;
	size_t i;

	ool_locations = alloc_array(sizeof(ool_locations[0]),
				    xwr->values.num_strings);
	if (ool_locations == NULL)
		return SQFS_ERROR_ALLOC;

	for (i = 0; i < xwr->values.num_strings; ++i)
		ool_locations[i] = 0xFFFFFFFFFFFFFFFFUL;

	for (blk = xwr->kv_blocks; blk != NULL; blk = blk->next) {
		sqfs_meta_writer_get_position(mw, &block, &offset);
		blk->start_ref = (block << 16) | (offset & 0xFFFF);

		size = write_block_pairs(xwr, mw, blk, ool_locations);
		if (size < 0) {
			free(ool_locations);
			return size;
		}

		blk->size_bytes = size;
	}

	free(ool_locations);
	return sqfs_meta_writer_flush(mw);
}

static int write_id_table(sqfs_xattr_writer_t *xwr, sqfs_meta_writer_t *mw,
			  sqfs_u64 *locations)
{
	sqfs_xattr_id_t id_ent;
	kv_block_desc_t *blk;
	sqfs_u32 offset;
	sqfs_u64 block;
	size_t i = 0;
	int err;

	locations[i++] = 0;

	for (blk = xwr->kv_blocks; blk != NULL; blk = blk->next) {
		memset(&id_ent, 0, sizeof(id_ent));
		id_ent.xattr = htole64(blk->start_ref);
		id_ent.count = htole32(blk->count);
		id_ent.size = htole32(blk->size_bytes);

		err = sqfs_meta_writer_append(mw, &id_ent, sizeof(id_ent));
		if (err)
			return err;

		sqfs_meta_writer_get_position(mw, &block, &offset);
		if (block != locations[i - 1])
			locations[i++] = block;
	}

	return sqfs_meta_writer_flush(mw);
}

static int write_location_table(sqfs_xattr_writer_t *xwr, sqfs_u64 kv_start,
				sqfs_file_t *file, const sqfs_super_t *super,
				sqfs_u64 *locations, size_t loc_count)
{
	sqfs_xattr_id_table_t idtbl;
	int err;

	memset(&idtbl, 0, sizeof(idtbl));
	idtbl.xattr_table_start = htole64(kv_start);
	idtbl.xattr_ids = htole32(xwr->num_blocks);

	err = file->write_at(file, super->xattr_id_table_start,
			     &idtbl, sizeof(idtbl));
	if (err)
		return err;

	return file->write_at(file, super->xattr_id_table_start + sizeof(idtbl),
			      locations, sizeof(locations[0]) * loc_count);
}

static int alloc_location_table(sqfs_xattr_writer_t *xwr, sqfs_u64 **tbl_out,
				size_t *szout)
{
	sqfs_u64 *locations;
	size_t size, count;

	if (SZ_MUL_OV(xwr->num_blocks, sizeof(sqfs_xattr_id_t), &size))
		return SQFS_ERROR_OVERFLOW;

	count = size / SQFS_META_BLOCK_SIZE;
	if (size % SQFS_META_BLOCK_SIZE)
		++count;

	locations = alloc_array(sizeof(sqfs_u64), count);
	if (locations == NULL)
		return SQFS_ERROR_ALLOC;

	*tbl_out = locations;
	*szout = count;
	return 0;
}

int sqfs_xattr_writer_flush(sqfs_xattr_writer_t *xwr, sqfs_file_t *file,
			    sqfs_super_t *super, sqfs_compressor_t *cmp)
{
	sqfs_u64 *locations = NULL, kv_start, id_start;
	sqfs_meta_writer_t *mw;
	size_t i, count;
	int err;

	if (xwr->num_pairs == 0 || xwr->num_blocks == 0) {
		super->xattr_id_table_start = 0xFFFFFFFFFFFFFFFFUL;
		super->flags |= SQFS_FLAG_NO_XATTRS;
		return 0;
	}

	mw = sqfs_meta_writer_create(file, cmp, 0);
	if (mw == NULL)
		return SQFS_ERROR_ALLOC;

	kv_start = file->get_size(file);
	err = write_kv_pairs(xwr, mw);
	if (err)
		goto out;

	sqfs_meta_writer_reset(mw);

	id_start = file->get_size(file);
	err = alloc_location_table(xwr, &locations, &count);
	if (err)
		goto out;

	err = write_id_table(xwr, mw, locations);
	if (err)
		goto out;

	super->xattr_id_table_start = file->get_size(file);
	super->flags &= ~SQFS_FLAG_NO_XATTRS;

	for (i = 0; i < count; ++i)
		locations[i] = htole64(locations[i] + id_start);

	err = write_location_table(xwr, kv_start, file, super,
				   locations, count);
out:
	free(locations);
	sqfs_destroy(mw);
	return err;
}
