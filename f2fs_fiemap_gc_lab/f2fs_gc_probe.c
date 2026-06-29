// SPDX-License-Identifier: GPL-2.0
/*
 * f2fs_gc_probe - small Android/root helper for FIEMAP and F2FS GC ioctls.
 *
 * Build for Android with an NDK clang, push to /data/local/tmp, and run as
 * root through adb shell su -c.  The host Python harness in this directory can
 * drive this helper automatically once a device binary is supplied.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef FIEMAP_FLAG_SYNC
#define FIEMAP_FLAG_SYNC 0x00000001
#endif

#ifndef FIEMAP_EXTENT_LAST
#define FIEMAP_EXTENT_LAST 0x00000001
#endif

struct fiemap_extent {
	uint64_t fe_logical;
	uint64_t fe_physical;
	uint64_t fe_length;
	uint64_t fe_reserved64[2];
	uint32_t fe_flags;
	uint32_t fe_reserved[3];
};

struct fiemap {
	uint64_t fm_start;
	uint64_t fm_length;
	uint32_t fm_flags;
	uint32_t fm_mapped_extents;
	uint32_t fm_extent_count;
	uint32_t fm_reserved;
	struct fiemap_extent fm_extents[];
};

#ifndef FS_IOC_FIEMAP
#define FS_IOC_FIEMAP _IOWR('f', 11, struct fiemap)
#endif

#define F2FS_IOCTL_MAGIC 0xf5

struct f2fs_gc_range {
	uint32_t sync;
	uint64_t start;
	uint64_t len;
};

#ifndef F2FS_IOC_GARBAGE_COLLECT
#define F2FS_IOC_GARBAGE_COLLECT _IOW(F2FS_IOCTL_MAGIC, 6, uint32_t)
#endif

#ifndef F2FS_IOC_GARBAGE_COLLECT_RANGE
#define F2FS_IOC_GARBAGE_COLLECT_RANGE \
	_IOW(F2FS_IOCTL_MAGIC, 11, struct f2fs_gc_range)
#endif

#define F2FS_BLOCK_SIZE 4096ULL
#define FIEMAP_BATCH 256

struct extent {
	uint64_t logical;
	uint64_t physical;
	uint64_t length;
	uint32_t flags;
};

struct extent_vec {
	struct extent *items;
	size_t nr;
	size_t cap;
};

static void usage(FILE *out, const char *prog)
{
	fprintf(out,
		"Usage:\n"
		"  %s fiemap-json PATH\n"
		"  %s gc-range [--sync|--async] PATH\n"
		"  %s gc-one [--sync|--async] PATH\n\n"
		"Commands:\n"
		"  fiemap-json  Print stat data and extents as one JSON object.\n"
		"  gc-range     Run F2FS_IOC_GARBAGE_COLLECT_RANGE for each extent.\n"
		"  gc-one       Run one ordinary F2FS_IOC_GARBAGE_COLLECT call.\n",
		prog, prog, prog);
}

static int parse_sync_arg(int argc, char **argv, int *idx)
{
	int sync = 1;

	if (*idx < argc && strcmp(argv[*idx], "--async") == 0) {
		sync = 0;
		(*idx)++;
	} else if (*idx < argc && strcmp(argv[*idx], "--sync") == 0) {
		sync = 1;
		(*idx)++;
	}

	return sync;
}

static void json_escape(const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		switch (c) {
		case '\\':
			fputs("\\\\", stdout);
			break;
		case '"':
			fputs("\\\"", stdout);
			break;
		case '\n':
			fputs("\\n", stdout);
			break;
		case '\r':
			fputs("\\r", stdout);
			break;
		case '\t':
			fputs("\\t", stdout);
			break;
		default:
			if (c < 0x20)
				printf("\\u%04x", c);
			else
				putchar(c);
		}
	}
}

static int push_extent(struct extent_vec *vec, const struct fiemap_extent *fe)
{
	struct extent *tmp;

	if (vec->nr == vec->cap) {
		size_t new_cap = vec->cap ? vec->cap * 2 : FIEMAP_BATCH;

		tmp = realloc(vec->items, new_cap * sizeof(*vec->items));
		if (!tmp)
			return -ENOMEM;
		vec->items = tmp;
		vec->cap = new_cap;
	}

	vec->items[vec->nr].logical = fe->fe_logical;
	vec->items[vec->nr].physical = fe->fe_physical;
	vec->items[vec->nr].length = fe->fe_length;
	vec->items[vec->nr].flags = fe->fe_flags;
	vec->nr++;
	return 0;
}

static int collect_fiemap(int fd, struct extent_vec *vec)
{
	uint64_t start = 0;

	while (1) {
		size_t bytes = sizeof(struct fiemap) +
			FIEMAP_BATCH * sizeof(struct fiemap_extent);
		struct fiemap *fm = calloc(1, bytes);
		uint64_t next = start;
		uint32_t i;

		if (!fm)
			return -ENOMEM;

		fm->fm_start = start;
		fm->fm_length = UINT64_MAX - start;
		fm->fm_flags = FIEMAP_FLAG_SYNC;
		fm->fm_extent_count = FIEMAP_BATCH;

		if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
			int err = -errno;

			free(fm);
			return err;
		}

		if (fm->fm_mapped_extents == 0) {
			free(fm);
			return 0;
		}

		for (i = 0; i < fm->fm_mapped_extents; i++) {
			const struct fiemap_extent *fe = &fm->fm_extents[i];
			int ret = push_extent(vec, fe);

			if (ret) {
				free(fm);
				return ret;
			}

			next = fe->fe_logical + fe->fe_length;
			if (fe->fe_flags & FIEMAP_EXTENT_LAST) {
				free(fm);
				return 0;
			}
		}

		free(fm);
		if (next <= start)
			return -EIO;
		start = next;
	}
}

static int open_target(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
	return fd;
}

static int cmd_fiemap_json(const char *path)
{
	struct extent_vec vec = { 0 };
	struct stat st;
	int fd;
	int ret;
	size_t i;

	fd = open_target(path);
	if (fd < 0)
		return 1;

	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "fstat(%s): %s\n", path, strerror(errno));
		close(fd);
		return 1;
	}

	ret = collect_fiemap(fd, &vec);
	close(fd);
	if (ret) {
		fprintf(stderr, "FIEMAP(%s): %s\n", path, strerror(-ret));
		free(vec.items);
		return 1;
	}

	printf("{\"path\":\"");
	json_escape(path);
	printf("\",\"dev\":%" PRIu64 ",\"inode\":%" PRIu64
	       ",\"size\":%" PRIu64 ",\"mtime\":%" PRIu64
	       ",\"mode\":%u,\"block_size\":%" PRIu64
	       ",\"extent_count\":%zu,\"extents\":[",
	       (uint64_t)st.st_dev, (uint64_t)st.st_ino,
	       (uint64_t)st.st_size, (uint64_t)st.st_mtime,
	       (unsigned int)st.st_mode, (uint64_t)F2FS_BLOCK_SIZE, vec.nr);

	for (i = 0; i < vec.nr; i++) {
		if (i)
			putchar(',');
		printf("{\"logical\":%" PRIu64 ",\"physical\":%" PRIu64
		       ",\"length\":%" PRIu64 ",\"flags\":%u}",
		       vec.items[i].logical, vec.items[i].physical,
		       vec.items[i].length, vec.items[i].flags);
	}
	puts("]}");

	free(vec.items);
	return 0;
}

static int cmd_gc_one(const char *path, int sync)
{
	uint32_t value = sync ? 1 : 0;
	int fd = open_target(path);
	int ret;

	if (fd < 0)
		return 1;

	ret = ioctl(fd, F2FS_IOC_GARBAGE_COLLECT, &value);
	if (ret < 0) {
		fprintf(stderr, "F2FS_IOC_GARBAGE_COLLECT(%s): %s\n",
			path, strerror(errno));
		close(fd);
		return 1;
	}

	printf("{\"path\":\"");
	json_escape(path);
	printf("\",\"sync\":%u,\"ret\":0}\n", value);
	close(fd);
	return 0;
}

static int cmd_gc_range(const char *path, int sync)
{
	struct extent_vec vec = { 0 };
	int fd = open_target(path);
	int ret;
	size_t i;
	int failures = 0;

	if (fd < 0)
		return 1;

	ret = collect_fiemap(fd, &vec);
	if (ret) {
		fprintf(stderr, "FIEMAP(%s): %s\n", path, strerror(-ret));
		close(fd);
		free(vec.items);
		return 1;
	}

	printf("{\"path\":\"");
	json_escape(path);
	printf("\",\"sync\":%u,\"ranges\":[", sync ? 1 : 0);

	for (i = 0; i < vec.nr; i++) {
		struct f2fs_gc_range range;
		uint64_t offset = vec.items[i].physical % F2FS_BLOCK_SIZE;
		uint64_t blocks = (offset + vec.items[i].length +
				   F2FS_BLOCK_SIZE - 1) / F2FS_BLOCK_SIZE;
		int saved_errno = 0;

		range.sync = sync ? 1 : 0;
		range.start = vec.items[i].physical / F2FS_BLOCK_SIZE;
		range.len = blocks ? blocks - 1 : 0;

		ret = ioctl(fd, F2FS_IOC_GARBAGE_COLLECT_RANGE, &range);
		if (ret < 0) {
			saved_errno = errno;
			failures++;
		}

		if (i)
			putchar(',');
		printf("{\"start\":%" PRIu64 ",\"len\":%" PRIu64
		       ",\"ret\":%d,\"errno\":%d}",
		       range.start, range.len, ret, saved_errno);
	}

	puts("]}");

	close(fd);
	free(vec.items);
	return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *cmd;
	int idx = 2;
	int sync;

	if (argc < 3) {
		usage(stderr, argv[0]);
		return 2;
	}

	cmd = argv[1];

	if (strcmp(cmd, "fiemap-json") == 0) {
		if (argc != 3) {
			usage(stderr, argv[0]);
			return 2;
		}
		return cmd_fiemap_json(argv[2]);
	}

	if (strcmp(cmd, "gc-range") == 0) {
		sync = parse_sync_arg(argc, argv, &idx);
		if (idx + 1 != argc) {
			usage(stderr, argv[0]);
			return 2;
		}
		return cmd_gc_range(argv[idx], sync);
	}

	if (strcmp(cmd, "gc-one") == 0) {
		sync = parse_sync_arg(argc, argv, &idx);
		if (idx + 1 != argc) {
			usage(stderr, argv[0]);
			return 2;
		}
		return cmd_gc_one(argv[idx], sync);
	}

	usage(stderr, argv[0]);
	return 2;
}
