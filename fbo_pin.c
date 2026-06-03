/*
 * fbo_pin.c — FBO 3.0 static-pin reference implementation.
 *
 * Demonstrates the per-file pin sequence:
 *
 *   open(path, O_RDONLY)
 *     -> ioctl(fd, F2FS_IOC_SET_PIN_FILE, &one)    [best-effort]
 *     -> ioctl(fd, FS_IOC_FIEMAP, &fiemap)
 *     -> ufs_vendor_pin(extents)                   [stubbed for x86]
 *     -> record (path, inode, mtime, extents)
 *
 * Records what was pinned to a text file so 'unpin' can reverse it.
 *
 * Build: gcc -O2 -Wall -o fbo_pin fbo_pin.c
 *
 * Usage:
 *   sudo ./fbo_pin pin   <record-file> <file> [<file>...]
 *   sudo ./fbo_pin unpin <record-file>
 *        ./fbo_pin list  <record-file>
 *
 * On a non-F2FS filesystem (e.g. ext4 on your x86 test machine) the
 * F2FS pin step prints a warning and continues — FIEMAP and the UFS
 * stub still run, so you can validate the rest of the flow.
 *
 * The UFS pin step is a stub that just prints what it would send.
 * Wire ufs_vendor_pin() to your OEM's actual pin interface (sysfs
 * node, scsi passthrough, or vendor ioctl) to make it real.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

/* F2FS_IOC_SET_PIN_FILE lives in <linux/f2fs.h> on recent kernels but
 * isn't always packaged in distro headers. The ioctl number is part
 * of the kernel ABI and stable, so define it locally. */
#ifndef F2FS_IOC_SET_PIN_FILE
#define F2FS_IOC_SET_PIN_FILE _IOW(0xf5, 13, __u32)
#endif
#ifndef F2FS_IOC_GET_PIN_FILE
#define F2FS_IOC_GET_PIN_FILE _IOR(0xf5, 14, __u32)
#endif

#define MAX_EXTENTS 1024

/*
 * Step 1: pin in F2FS sense — tell the filesystem "do not relocate
 * this file's blocks during GC." Required for the UFS-level pin to
 * stay valid over time. Silently ignored on non-F2FS filesystems
 * (ENOTTY); other errors are reported as warnings.
 */
static int do_f2fs_pin(int fd, const char *path, int pin)
{
	__u32 v = pin ? 1 : 0;
	if (ioctl(fd, F2FS_IOC_SET_PIN_FILE, &v) < 0) {
		if (errno != ENOTTY)
			fprintf(stderr,
				"  warning: F2FS pin on %s: %s\n",
				path, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Step 2: read the file's (logical-offset -> physical-LBA, length)
 * extent map via the standard FIEMAP ioctl. The returned LBAs are
 * what the UFS pin command takes.
 */
static int fiemap_file(int fd, const char *path,
		       struct fiemap_extent *out, unsigned *out_n)
{
	size_t bytes = sizeof(struct fiemap) +
		       MAX_EXTENTS * sizeof(struct fiemap_extent);
	struct fiemap *fm = calloc(1, bytes);
	if (!fm) {
		perror("calloc");
		return -1;
	}

	fm->fm_start        = 0;
	fm->fm_length       = ~0ULL;
	fm->fm_flags        = FIEMAP_FLAG_SYNC;
	fm->fm_extent_count = MAX_EXTENTS;

	if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
		fprintf(stderr, "  FIEMAP on %s: %s\n",
			path, strerror(errno));
		free(fm);
		return -1;
	}

	unsigned n = fm->fm_mapped_extents;
	if (n > MAX_EXTENTS)
		n = MAX_EXTENTS;
	memcpy(out, fm->fm_extents, n * sizeof(struct fiemap_extent));
	*out_n = n;
	free(fm);
	return 0;
}

/*
 * Step 3: ship LBA ranges to the UFS pin path. Vendor-specific in
 * reality. On real hardware this would be one of:
 *   - write(open("/sys/class/.../ufs_pin", O_WR), buf, len)
 *   - ioctl(scsi_fd, SG_IO, &cmd) with a vendor opcode
 *   - ioctl on a vendor char device
 * For the demo we just print what we'd send.
 */
static int ufs_vendor_pin(const char *path,
			  const struct fiemap_extent *exts, unsigned n,
			  int pin)
{
	const char *op = pin ? "PIN" : "UNPIN";
	(void)path;
	for (unsigned i = 0; i < n; i++) {
		printf("  ufs_%s  lba=0x%llx  len=0x%llx  flags=0x%x\n",
		       op,
		       (unsigned long long)exts[i].fe_physical,
		       (unsigned long long)exts[i].fe_length,
		       exts[i].fe_flags);
	}
	return 0;
}

/*
 * Step 4: persist what we pinned so unpin can reverse the operation.
 * Text format chosen for inspectability; production code would use
 * a structured binary or sqlite store.
 */
static int record_write(FILE *rec, const char *path,
			ino_t ino, time_t mtime,
			const struct fiemap_extent *exts, unsigned n)
{
	fprintf(rec, "FILE %s ino=%llu mtime=%lld nextents=%u\n",
		path, (unsigned long long)ino,
		(long long)mtime, n);
	for (unsigned i = 0; i < n; i++) {
		fprintf(rec, "  EXT phys=0x%llx len=0x%llx flags=0x%x\n",
			(unsigned long long)exts[i].fe_physical,
			(unsigned long long)exts[i].fe_length,
			exts[i].fe_flags);
	}
	return 0;
}

static int cmd_pin(const char *recpath, char **paths, int npaths)
{
	FILE *rec = fopen(recpath, "w");
	if (!rec) {
		perror(recpath);
		return 1;
	}

	int ok_count = 0;
	for (int i = 0; i < npaths; i++) {
		const char *path = paths[i];
		printf("PIN %s\n", path);

		int fd = open(path, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "  open: %s\n", strerror(errno));
			continue;
		}

		struct stat st;
		if (fstat(fd, &st) < 0) {
			perror("  fstat");
			close(fd);
			continue;
		}

		do_f2fs_pin(fd, path, 1);

		struct fiemap_extent exts[MAX_EXTENTS];
		unsigned n = 0;
		if (fiemap_file(fd, path, exts, &n) < 0) {
			close(fd);
			continue;
		}

		ufs_vendor_pin(path, exts, n, 1);
		record_write(rec, path, st.st_ino, st.st_mtime, exts, n);

		close(fd);
		printf("  done — %u extent(s) pinned\n\n", n);
		ok_count++;
	}

	fclose(rec);
	printf("pinned %d / %d files; record at %s\n",
	       ok_count, npaths, recpath);
	return 0;
}

static int cmd_unpin(const char *recpath)
{
	FILE *rec = fopen(recpath, "r");
	if (!rec) {
		perror(recpath);
		return 1;
	}

	char line[1024];
	char path[512] = {0};

	/* First pass: issue UFS unpin for each recorded extent. */
	while (fgets(line, sizeof(line), rec)) {
		if (strncmp(line, "FILE ", 5) == 0) {
			sscanf(line, "FILE %511s", path);
			printf("UNPIN %s\n", path);
		} else if (strncmp(line, "  EXT ", 6) == 0) {
			struct fiemap_extent e = {0};
			unsigned long long phys, len;
			unsigned flags;
			if (sscanf(line,
				   "  EXT phys=0x%llx len=0x%llx flags=0x%x",
				   &phys, &len, &flags) == 3) {
				e.fe_physical = phys;
				e.fe_length   = len;
				e.fe_flags    = flags;
				ufs_vendor_pin(path, &e, 1, 0);
			}
		}
	}

	/* Second pass: clear F2FS pin so the file is GC-eligible again. */
	rewind(rec);
	while (fgets(line, sizeof(line), rec)) {
		if (strncmp(line, "FILE ", 5) != 0)
			continue;
		sscanf(line, "FILE %511s", path);
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			do_f2fs_pin(fd, path, 0);
			close(fd);
		}
	}

	fclose(rec);
	return 0;
}

static int cmd_list(const char *recpath)
{
	FILE *rec = fopen(recpath, "r");
	if (!rec) {
		perror(recpath);
		return 1;
	}
	char line[1024];
	while (fgets(line, sizeof(line), rec))
		fputs(line, stdout);
	fclose(rec);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
			"usage:\n"
			"  %s pin   <record-file> <file>...\n"
			"  %s unpin <record-file>\n"
			"  %s list  <record-file>\n",
			argv[0], argv[0], argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "pin") == 0 && argc >= 4)
		return cmd_pin(argv[2], &argv[3], argc - 3);
	if (strcmp(argv[1], "unpin") == 0)
		return cmd_unpin(argv[2]);
	if (strcmp(argv[1], "list") == 0)
		return cmd_list(argv[2]);

	fprintf(stderr, "unknown command: %s\n", argv[1]);
	return 1;
}
