/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * restore_fstree.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#include "rdsquashfs.h"

#ifdef _WIN32
static int create_node(const sqfs_tree_node_t *n, const char *name)
{
	WCHAR *wpath = NULL;
	DWORD length;
	HANDLE fh;

	length = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0) + 1;
	if (length <= 0)
		goto fail;

	wpath = alloc_array(sizeof(wpath[0]), length);
	if (wpath == NULL) {
		perror(name);
		return -1;
	}

	MultiByteToWideChar(CP_UTF8, 0, name, -1, wpath, length);
	wpath[length - 1] = '\0';

	switch (n->inode->base.mode & S_IFMT) {
	case S_IFDIR:
		if (!CreateDirectoryW(wpath, NULL))
			goto fail;
		break;
	case S_IFREG:
		fh = CreateFileW(wpath, GENERIC_READ,
				 FILE_SHARE_READ | FILE_SHARE_WRITE,
				 NULL, CREATE_NEW, 0, NULL);

		if (fh == INVALID_HANDLE_VALUE)
			goto fail;

		CloseHandle(fh);
		break;
	default:
		break;
	}

	free(wpath);
	return 0;
fail:
	fprintf(stderr, "Creating %s: %ld\n", name, GetLastError());
	free(wpath);
	return -1;
}
#else
static int create_node(const sqfs_tree_node_t *n, const char *name)
{
	sqfs_u32 devno;
	int fd;

	switch (n->inode->base.mode & S_IFMT) {
	case S_IFDIR:
		if (mkdir(name, 0755) && errno != EEXIST) {
			fprintf(stderr, "mkdir %s: %s\n",
				name, strerror(errno));
			return -1;
		}
		break;
	case S_IFLNK:
		if (symlink(n->inode->slink_target, name)) {
			fprintf(stderr, "ln -s %s %s: %s\n",
				n->inode->slink_target, name,
				strerror(errno));
			return -1;
		}
		break;
	case S_IFSOCK:
	case S_IFIFO:
		if (mknod(name, (n->inode->base.mode & S_IFMT) | 0700, 0)) {
			fprintf(stderr, "creating %s: %s\n",
				name, strerror(errno));
			return -1;
		}
		break;
	case S_IFBLK:
	case S_IFCHR:
		if (n->inode->base.type == SQFS_INODE_EXT_BDEV ||
		    n->inode->base.type == SQFS_INODE_EXT_CDEV) {
			devno = n->inode->data.dev_ext.devno;
		} else {
			devno = n->inode->data.dev.devno;
		}

		if (mknod(name, n->inode->base.mode & S_IFMT, devno)) {
			fprintf(stderr, "creating device %s: %s\n",
				name, strerror(errno));
			return -1;
		}
		break;
	case S_IFREG:
		fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);

		if (fd < 0) {
			fprintf(stderr, "creating %s: %s\n",
				name, strerror(errno));
			return -1;
		}

		close(fd);
		break;
	default:
		break;
	}

	return 0;
}
#endif

static int create_node_dfs(const sqfs_tree_node_t *n, int flags)
{
	const sqfs_tree_node_t *c;
	char *name;
	int ret;

	if (!is_filename_sane((const char *)n->name)) {
		fprintf(stderr, "Found an entry named '%s', skipping.\n",
			n->name);
		return 0;
	}

	name = sqfs_tree_node_get_path(n);
	if (name == NULL) {
		fprintf(stderr, "Constructing full path for '%s': %s\n",
			(const char *)n->name, strerror(errno));
		return -1;
	}

	ret = canonicalize_name(name);
	assert(ret == 0);

	if (!(flags & UNPACK_QUIET))
		printf("creating %s\n", name);

	ret = create_node(n, name);
	free(name);
	if (ret)
		return -1;

	if (S_ISDIR(n->inode->base.mode)) {
		for (c = n->children; c != NULL; c = c->next) {
			if (create_node_dfs(c, flags))
				return -1;
		}
	}
	return 0;
}

#ifdef HAVE_SYS_XATTR_H
static int set_xattr(const char *path, sqfs_xattr_reader_t *xattr,
		     const sqfs_tree_node_t *n)
{
	sqfs_xattr_value_t *value;
	sqfs_xattr_entry_t *key;
	sqfs_xattr_id_t desc;
	sqfs_u32 index;
	size_t i;
	int ret;

	sqfs_inode_get_xattr_index(n->inode, &index);

	if (index == 0xFFFFFFFF)
		return 0;

	if (sqfs_xattr_reader_get_desc(xattr, index, &desc)) {
		fputs("Error resolving xattr index\n", stderr);
		return -1;
	}

	if (sqfs_xattr_reader_seek_kv(xattr, &desc)) {
		fputs("Error locating xattr key-value pairs\n", stderr);
		return -1;
	}

	for (i = 0; i < desc.count; ++i) {
		if (sqfs_xattr_reader_read_key(xattr, &key)) {
			fputs("Error reading xattr key\n", stderr);
			return -1;
		}

		if (sqfs_xattr_reader_read_value(xattr, key, &value)) {
			fputs("Error reading xattr value\n", stderr);
			free(key);
			return -1;
		}

		ret = lsetxattr(path, (const char *)key->key,
				value->value, value->size, 0);
		if (ret) {
			fprintf(stderr, "setting xattr '%s' on %s: %s\n",
				key->key, path, strerror(errno));
		}

		free(key);
		free(value);
		if (ret)
			return -1;
	}

	return 0;
}
#endif

static int set_attribs(sqfs_xattr_reader_t *xattr,
		       const sqfs_tree_node_t *n, int flags)
{
	const sqfs_tree_node_t *c;
	char *path;
	int ret;

	if (!is_filename_sane((const char *)n->name))
		return 0;

	if (S_ISDIR(n->inode->base.mode)) {
		for (c = n->children; c != NULL; c = c->next) {
			if (set_attribs(xattr, c, flags))
				return -1;
		}
	}

	path = sqfs_tree_node_get_path(n);
	if (path == NULL) {
		fprintf(stderr, "Reconstructing full path: %s\n",
			strerror(errno));
		return -1;
	}

	ret = canonicalize_name(path);
	assert(ret == 0);

#ifdef HAVE_SYS_XATTR_H
	if ((flags & UNPACK_SET_XATTR) && xattr != NULL) {
		if (set_xattr(path, xattr, n))
			goto fail;
	}
#endif

#ifndef _WIN32
	if (flags & UNPACK_SET_TIMES) {
		struct timespec times[2];

		memset(times, 0, sizeof(times));
		times[0].tv_sec = n->inode->base.mod_time;
		times[1].tv_sec = n->inode->base.mod_time;

		if (utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW)) {
			fprintf(stderr, "setting timestamp on %s: %s\n",
				path, strerror(errno));
			goto fail;
		}
	}

	if (flags & UNPACK_CHOWN) {
		if (fchownat(AT_FDCWD, path, n->uid, n->gid,
			     AT_SYMLINK_NOFOLLOW)) {
			fprintf(stderr, "chown %s: %s\n",
				path, strerror(errno));
			goto fail;
		}
	}

	if (flags & UNPACK_CHMOD && !S_ISLNK(n->inode->base.mode)) {
		if (fchmodat(AT_FDCWD, path,
			     n->inode->base.mode & ~S_IFMT, 0)) {
			fprintf(stderr, "chmod %s: %s\n",
				path, strerror(errno));
			goto fail;
		}
	}
#endif
	free(path);
	return 0;
fail:
	free(path);
	return -1;
}

int restore_fstree(sqfs_tree_node_t *root, int flags)
{
	sqfs_tree_node_t *n, *old_parent;

	/* make sure fstree_get_path() stops at this node */
	old_parent = root->parent;
	root->parent = NULL;

	if (S_ISDIR(root->inode->base.mode)) {
		for (n = root->children; n != NULL; n = n->next) {
			if (create_node_dfs(n, flags))
				return -1;
		}
	} else {
		if (create_node_dfs(root, flags))
			return -1;
	}

	root->parent = old_parent;
	return 0;
}

int update_tree_attribs(sqfs_xattr_reader_t *xattr,
			const sqfs_tree_node_t *root, int flags)
{
	const sqfs_tree_node_t *n;

	if ((flags & (UNPACK_CHOWN | UNPACK_CHMOD |
		      UNPACK_SET_TIMES | UNPACK_SET_XATTR)) == 0) {
		return 0;
	}

	if (S_ISDIR(root->inode->base.mode)) {
		for (n = root->children; n != NULL; n = n->next) {
			if (set_attribs(xattr, n, flags))
				return -1;
		}
	} else {
		if (set_attribs(xattr, root, flags))
			return -1;
	}

	return 0;
}
