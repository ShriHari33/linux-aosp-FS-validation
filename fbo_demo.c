/*
 * fbo_demo.c — minimal fanotify FAN_OPEN_PERM + FIEMAP demo.
 *
 * Build: gcc -O2 -Wall -o fbo_demo fbo_demo.c
 * Run:   sudo ./fbo_demo /path/to/some/file
 *
 * Then in a second shell:  cat /path/to/some/file
 *
 * What this models, end to end:
 *   1. We mark a specific file's inode with FAN_OPEN_PERM.
 *   2. When anyone opens that file, the kernel BLOCKS that open() inside
 *      the kernel and hands us an fd referring to the file.
 *   3. We FIEMAP the fd to get its (logical -> physical LBA) extent map.
 *      This is exactly the data FBO 3.0 would feed to the UFS pin command.
 *   4. We write FAN_ALLOW back. The kernel unblocks the original open().
 *
 * Things worth trying:
 *   - Comment out the FAN_ALLOW write. Run `cat file` from another shell.
 *     The cat hangs forever — that is the syscall actually paused in the
 *     kernel waiting on you. Kill the daemon: cat unblocks with EPERM.
 *   - Point it at a sparse file (`truncate -s 1G big; dd seek=...`) to
 *     see multiple extents.
 *   - Watch /proc/<pid-of-cat>/stack while it's blocked — you'll see it
 *     parked in fanotify_get_response inside the kernel.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/fanotify.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#define EXTENT_COUNT 32

static void dump_extents(int fd)
{
	struct {
		struct fiemap fm;
		struct fiemap_extent ex[EXTENT_COUNT];
	} buf;

	memset(&buf, 0, sizeof(buf));
	buf.fm.fm_start = 0;
	buf.fm.fm_length = ~0ULL;
	buf.fm.fm_flags = FIEMAP_FLAG_SYNC;
	buf.fm.fm_extent_count = EXTENT_COUNT;

	if (ioctl(fd, FS_IOC_FIEMAP, &buf) < 0) {
		fprintf(stderr, "  FIEMAP failed: %s\n", strerror(errno));
		return;
	}

	printf("  %u extent(s):\n", buf.fm.fm_mapped_extents);
	for (unsigned i = 0; i < buf.fm.fm_mapped_extents; i++) {
		struct fiemap_extent *e = &buf.ex[i];
		printf("    [%u] logical=0x%llx  physical=0x%llx  "
		       "length=0x%llx  flags=0x%x\n",
		       i,
		       (unsigned long long)e->fe_logical,
		       (unsigned long long)e->fe_physical,
		       (unsigned long long)e->fe_length,
		       e->fe_flags);
	}
	/* In real FBO 3.0: ship these (physical, length) pairs as LBA ranges
	 * to the UFS pin path (vendor sysfs / scsi passthrough). */
}

static void path_from_fd(int fd, char *out, size_t outlen)
{
	char link[64];
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(link, out, outlen - 1);
	if (n < 0) {
		snprintf(out, outlen, "<readlink failed: %s>", strerror(errno));
		return;
	}
	out[n] = '\0';
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <path-to-watch>\n", argv[0]);
		return 1;
	}
	const char *target = argv[1];

	int fan = fanotify_init(FAN_CLASS_CONTENT | FAN_CLOEXEC,
				O_RDONLY | O_LARGEFILE);
	if (fan < 0) {
		fprintf(stderr,
			"fanotify_init: %s\n"
			"  hints: need root (CAP_SYS_ADMIN); kernel must have\n"
			"  CONFIG_FANOTIFY=y and CONFIG_FANOTIFY_ACCESS_PERMISSIONS=y.\n"
			"  Check: zcat /proc/config.gz | grep FANOTIFY\n",
			strerror(errno));
		return 1;
	}

	/* Inode mark on a single file. FAN_OPEN_PERM = block-until-we-reply.
	 * FAN_OPEN alone would be observe-only (no syscall pause). */
	if (fanotify_mark(fan,
			  FAN_MARK_ADD,
			  FAN_OPEN_PERM,
			  AT_FDCWD,
			  target) < 0) {
		fprintf(stderr, "fanotify_mark(%s): %s\n",
			target, strerror(errno));
		return 1;
	}

	printf("watching %s\n", target);
	printf("open it from another shell, e.g.:  cat %s\n", target);
	printf("ctrl-C to quit\n\n");

	for (;;) {
		struct fanotify_event_metadata ev;

		ssize_t n = read(fan, &ev, sizeof(ev));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "read: %s\n", strerror(errno));
			break;
		}
		if (n < (ssize_t)sizeof(ev) ||
		    ev.vers != FANOTIFY_METADATA_VERSION) {
			fprintf(stderr, "short/version-mismatched event\n");
			break;
		}

		char path[512];
		path_from_fd(ev.fd, path, sizeof(path));
		printf("EVENT  pid=%d  fd=%d  path=%s\n",
		       ev.pid, ev.fd, path);

		/* The FBO 3.0 work, on the fd the kernel handed us. */
		dump_extents(ev.fd);

		struct fanotify_response resp = {
			.fd = ev.fd,
			.response = FAN_ALLOW,
		};
		if (write(fan, &resp, sizeof(resp)) < 0)
			fprintf(stderr, "write response: %s\n",
				strerror(errno));

		close(ev.fd);
		printf("  -> FAN_ALLOW, opener unblocked\n\n");
	}

	return 0;
}
