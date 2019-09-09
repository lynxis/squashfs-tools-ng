/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dir_writer.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#define SQFS_BUILDING_DLL
#include "config.h"

#include "sqfs/meta_writer.h"
#include "sqfs/inode.h"
#include "sqfs/error.h"
#include "sqfs/data.h"
#include "sqfs/dir.h"
#include "util.h"

#include <sys/stat.h>
#include <endian.h>
#include <stdlib.h>
#include <string.h>

typedef struct dir_entry_t {
	struct dir_entry_t *next;
	uint64_t inode_ref;
	uint32_t inode_num;
	uint16_t type;
	size_t name_len;
	char name[];
} dir_entry_t;

typedef struct index_ent_t {
	struct index_ent_t *next;
	dir_entry_t *ent;
	uint64_t block;
	uint32_t index;
} index_ent_t;

struct sqfs_dir_writer_t {
	dir_entry_t *list;
	dir_entry_t *list_end;

	index_ent_t *idx;
	index_ent_t *idx_end;

	uint64_t dir_ref;
	size_t dir_size;
	size_t idx_size;
	sqfs_meta_writer_t *dm;
};

static int get_type(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFSOCK: return SQFS_INODE_SOCKET;
	case S_IFIFO:  return SQFS_INODE_FIFO;
	case S_IFLNK:  return SQFS_INODE_SLINK;
	case S_IFBLK:  return SQFS_INODE_BDEV;
	case S_IFCHR:  return SQFS_INODE_CDEV;
	case S_IFDIR:  return SQFS_INODE_DIR;
	case S_IFREG:  return SQFS_INODE_FILE;
	}

	return SQFS_ERROR_UNSUPPORTED;
}

static void writer_reset(sqfs_dir_writer_t *writer)
{
	dir_entry_t *ent;
	index_ent_t *idx;

	while (writer->idx != NULL) {
		idx = writer->idx;
		writer->idx = idx->next;
		free(idx);
	}

	while (writer->list != NULL) {
		ent = writer->list;
		writer->list = ent->next;
		free(ent);
	}

	writer->list_end = NULL;
	writer->idx_end = NULL;
	writer->dir_ref = 0;
	writer->dir_size = 0;
	writer->idx_size = 0;
}

sqfs_dir_writer_t *sqfs_dir_writer_create(sqfs_meta_writer_t *dm)
{
	sqfs_dir_writer_t *writer = calloc(1, sizeof(*writer));

	if (writer == NULL)
		return NULL;

	writer->dm = dm;
	return writer;
}

void sqfs_dir_writer_destroy(sqfs_dir_writer_t *writer)
{
	writer_reset(writer);
	free(writer);
}

int sqfs_dir_writer_begin(sqfs_dir_writer_t *writer)
{
	uint32_t offset;
	uint64_t block;

	writer_reset(writer);

	sqfs_meta_writer_get_position(writer->dm, &block, &offset);
	writer->dir_ref = (block << 16) | offset;
	return 0;
}

int sqfs_dir_writer_add_entry(sqfs_dir_writer_t *writer, const char *name,
			      uint32_t inode_num, uint64_t inode_ref,
			      mode_t mode)
{
	dir_entry_t *ent;
	int type;

	type = get_type(mode);
	if (type < 0)
		return type;

	ent = alloc_flex(sizeof(*ent), 1, strlen(name));
	if (ent == NULL)
		return SQFS_ERROR_ALLOC;

	ent->inode_ref = inode_ref;
	ent->inode_num = inode_num;
	ent->type = type;
	ent->name_len = strlen(name);
	memcpy(ent->name, name, ent->name_len);

	if (writer->list_end == NULL) {
		writer->list = writer->list_end = ent;
	} else {
		writer->list_end->next = ent;
		writer->list_end = ent;
	}

	writer->dir_size += sizeof(ent) + ent->name_len;
	return 0;
}

static size_t get_conseq_entry_count(uint32_t offset, dir_entry_t *head)
{
	size_t size, count = 0;
	dir_entry_t *it;
	int32_t diff;

	size = (offset + sizeof(sqfs_dir_header_t)) % SQFS_META_BLOCK_SIZE;

	for (it = head; it != NULL; it = it->next) {
		if ((it->inode_ref >> 16) != (head->inode_ref >> 16))
			break;

		diff = it->inode_num - head->inode_num;

		if (diff > 32767 || diff < -32767)
			break;

		size += sizeof(sqfs_dir_entry_t) + it->name_len;

		if (count > 0 && size > SQFS_META_BLOCK_SIZE)
			break;

		count += 1;

		if (count == SQFS_MAX_DIR_ENT)
			break;
	}

	return count;
}

static int add_header(sqfs_dir_writer_t *writer, size_t count,
		      dir_entry_t *ref, uint64_t block)
{
	sqfs_dir_header_t hdr;
	index_ent_t *idx;
	int err;

	hdr.count = htole32(count - 1);
	hdr.start_block = htole32(ref->inode_ref >> 16);
	hdr.inode_number = htole32(ref->inode_num);

	err = sqfs_meta_writer_append(writer->dm, &hdr, sizeof(hdr));
	if (err)
		return err;

	idx = calloc(1, sizeof(*idx));
	if (idx == NULL)
		return SQFS_ERROR_ALLOC;

	idx->ent = ref;
	idx->block = block;
	idx->index = writer->dir_size;

	if (writer->idx_end == NULL) {
		writer->idx = writer->idx_end = idx;
	} else {
		writer->idx_end->next = idx;
		writer->idx_end = idx;
	}

	writer->dir_size += sizeof(hdr);
	writer->idx_size += 1;
	return 0;
}

int sqfs_dir_writer_end(sqfs_dir_writer_t *writer)
{
	dir_entry_t *it, *first;
	sqfs_dir_entry_t ent;
	uint16_t *diff_u16;
	size_t i, count;
	uint32_t offset;
	uint64_t block;
	int err;

	for (it = writer->list; it != NULL; ) {
		sqfs_meta_writer_get_position(writer->dm, &block, &offset);
		count = get_conseq_entry_count(offset, it);

		err = add_header(writer, count, it, block);
		if (err)
			return err;

		first = it;

		for (i = 0; i < count; ++i) {
			ent.offset = htole16(it->inode_ref & 0x0000FFFF);
			ent.inode_diff = it->inode_num - first->inode_num;
			ent.type = htole16(it->type);
			ent.size = htole16(it->name_len - 1);

			diff_u16 = (uint16_t *)&ent.inode_diff;
			*diff_u16 = htole16(*diff_u16);

			err = sqfs_meta_writer_append(writer->dm, &ent,
						      sizeof(ent));
			if (err)
				return err;

			err = sqfs_meta_writer_append(writer->dm, it->name,
						      it->name_len);
			if (err)
				return err;

			it = it->next;
		}
	}

	return 0;
}

size_t sqfs_dir_writer_get_size(sqfs_dir_writer_t *writer)
{
	return writer->dir_size;
}

uint64_t sqfs_dir_writer_get_dir_reference(sqfs_dir_writer_t *writer)
{
	return writer->dir_ref;
}

size_t sqfs_dir_writer_get_index_size(sqfs_dir_writer_t *writer)
{
	return writer->idx_size;
}

int sqfs_dir_writer_write_index(sqfs_dir_writer_t *writer,
				sqfs_meta_writer_t *im)
{
	sqfs_dir_index_t ent;
	index_ent_t *idx;
	int err;

	for (idx = writer->idx; idx != NULL; idx = idx->next) {
		ent.start_block = htole32(idx->block);
		ent.index = htole32(idx->index);
		ent.size = htole32(idx->ent->name_len - 1);

		err = sqfs_meta_writer_append(im, &ent, sizeof(ent));
		if (err)
			return err;

		err = sqfs_meta_writer_append(im, idx->ent->name,
					      idx->ent->name_len);
		if (err)
			return err;
	}

	return 0;
}