/*
 * fbo_pin.c — FBO 3.0 static-pin reference implementation.
 *
 * Build: gcc -O2 -Wall -o fbo_pin fbo_pin.c
 *
 * Per-file pin sequence:
 *   open(path, O_RDONLY)
 *     -> ioctl(fd, F2FS_IOC_SET_PIN_FILE, &one)    [best-effort]
 *     -> ioctl(fd, FS_IOC_FIEMAP, &fiemap)
 *     -> ufs_vendor_pin(extents)                   [stubbed]
 *     -> record (path, inode, mtime, extents)
 *
 * Commands:
 *   pin   <record-file> <file> [<file>...]   pin explicit files
 *   app   <record-file> <package>            enumerate + pin (Android-side)
 *   unpin <record-file>                      reverse a previous pin
 *   list  <record-file>                      show what's recorded
 *
 * The 'app' command runs the enumeration internally:
 *   - popen("pm path <pkg>")           for base.apk + splits
 *   - dirname(first apk)               for the install directory
 *   - popen("getprop ro.product.cpu.abi") + abi_to_isa()
 *   - opendir on <dir>/oat/<isa>       for AOT artifacts
 *   - opendir on <dir>/lib/<isa>       for extracted native libs (if any)
 * and then calls the same pin sequence as the explicit 'pin' command.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#ifndef F2FS_IOC_SET_PIN_FILE
#define F2FS_IOC_SET_PIN_FILE _IOW(0xf5, 13, __u32)
#endif

#define MAX_EXTENTS 1024

/* -------------------------------------------------------------------------
 * Generic helpers
 * ------------------------------------------------------------------------- */

/* Run a shell command via popen and collect each output line into a
 * dynamically-grown array of strings. On success returns the line count
 * and sets *out to the (caller-owned) array. Returns -1 on failure.
 *
 * Caller frees: each line, then the array itself. */
static int popen_lines(const char *cmd, char ***out)
{
	FILE *p = popen(cmd, "r");
	if (!p)
		return -1;

	char **arr = NULL;
	int n = 0, cap = 0;
	char buf[4096];

	while (fgets(buf, sizeof(buf), p)) {
		size_t len = strlen(buf);
		while (len > 0 && (buf[len - 1] == '\n' ||
				   buf[len - 1] == '\r'))
			buf[--len] = 0;
		if (len == 0)
			continue;

		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			char **na = realloc(arr, cap * sizeof(*arr));
			if (!na) {
				pclose(p);
				for (int i = 0; i < n; i++)
					free(arr[i]);
				free(arr);
				return -1;
			}
			arr = na;
		}
		arr[n++] = strdup(buf);
	}
	pclose(p);
	*out = arr;
	return n;
}

static void free_lines(char **arr, int n)
{
	if (!arr)
		return;
	for (int i = 0; i < n; i++)
		free(arr[i]);
	free(arr);
}

/* Append entries from a directory (one level, regular files only) to a
 * growing string vector. Missing directory is not an error. */
static void dir_append_files(const char *dir, char ***vec, int *n, int *cap)
{
	DIR *d = opendir(dir);
	if (!d)
		return;

	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;

		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);

		struct stat st;
		if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
			continue;

		if (*n == *cap) {
			*cap = *cap ? *cap * 2 : 16;
			*vec = realloc(*vec, *cap * sizeof(**vec));
		}
		(*vec)[(*n)++] = strdup(path);
	}
	closedir(d);
}

/* -------------------------------------------------------------------------
 * Android-specific helpers
 * ------------------------------------------------------------------------- */

/* Map full Android ABI ("arm64-v8a") to the short ISA name ("arm64") used
 * for the on-disk extracted-lib and OAT directories. */
static const char *abi_to_isa(const char *abi)
{
	if (strcmp(abi, "arm64-v8a") == 0)   return "arm64";
	if (strcmp(abi, "armeabi-v7a") == 0) return "arm";
	return abi;  /* x86, x86_64 unchanged */
}

static int read_device_abi(char *out, size_t outlen)
{
	char **lines = NULL;
	int n = popen_lines("getprop ro.product.cpu.abi", &lines);
	if (n < 1) {
		free_lines(lines, n);
		return -1;
	}
	snprintf(out, outlen, "%s", lines[0]);
	free_lines(lines, n);
	return 0;
}

/* Enumerate the static distribution files for an installed Android
 * package: base APK + every installed split + every file under
 * oat/<isa>/ + every file under lib/<isa>/ (if extracted).
 *
 * On success returns count of paths and sets *out to a caller-owned
 * array of strdup'd absolute paths. Returns -1 on failure. */
static int enumerate_package(const char *pkg, char ***out)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "pm path %s 2>/dev/null", pkg);

	char **lines = NULL;
	int nl = popen_lines(cmd, &lines);
	if (nl < 1) {
		fprintf(stderr,
			"enumerate: package not installed or `pm` unavailable: %s\n",
			pkg);
		free_lines(lines, nl);
		return -1;
	}

	char **files = NULL;
	int nf = 0, cap = 0;

	/* Strip "package:" prefix from each APK path. */
	for (int i = 0; i < nl; i++) {
		const char *p = lines[i];
		if (strncmp(p, "package:", 8) == 0)
			p += 8;
		if (nf == cap) {
			cap = cap ? cap * 2 : 16;
			files = realloc(files, cap * sizeof(*files));
		}
		files[nf++] = strdup(p);
	}

	/* Derive install directory from the first APK. */
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", files[0]);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = 0;

	free_lines(lines, nl);

	/* OAT + extracted native libs, both keyed on ISA short form. */
	char abi[64];
	if (read_device_abi(abi, sizeof(abi)) == 0) {
		const char *isa = abi_to_isa(abi);
		char sub[2048];

		snprintf(sub, sizeof(sub), "%s/oat/%s", dir, isa);
		dir_append_files(sub, &files, &nf, &cap);

		snprintf(sub, sizeof(sub), "%s/lib/%s", dir, isa);
		dir_append_files(sub, &files, &nf, &cap);
	} else {
		fprintf(stderr,
			"enumerate: ro.product.cpu.abi unavailable; "
			"skipping oat/ and lib/ enumeration\n");
	}

	*out = files;
	return nf;
}

/* Read /proc/<pid>/maps and return a deduplicated list of mapped file
 * paths that match the given prefix (e.g. "/data/app/"). The maps file
 * contains one mapping per line:
 *   ADDR-ADDR PERMS OFFSET DEV INO PATH
 * We take the last whitespace-separated field as the path. Anonymous
 * mappings, [stack], [heap], /dev/, /memfd: are filtered out. */
static int read_proc_maps(int pid, const char *prefix,
			  char ***out)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}

	char **vec = NULL;
	int n = 0, cap = 0;
	char line[4096];

	while (fgets(line, sizeof(line), f)) {
		/* Locate the path: it starts at first '/' after the
		 * five whitespace-separated fields. Easier to grab the
		 * last token and validate it. */
		size_t len = strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = 0;

		char *p = strrchr(line, ' ');
		if (!p) continue;
		p++;
		if (*p != '/') continue;             /* anon mapping */
		if (strncmp(p, "/dev/", 5) == 0) continue;
		if (strncmp(p, "/memfd:", 7) == 0) continue;
		if (strncmp(p, prefix, strlen(prefix)) != 0) continue;

		/* Linear dedupe — sets are small (dozens). */
		int dup = 0;
		for (int i = 0; i < n; i++) {
			if (strcmp(vec[i], p) == 0) { dup = 1; break; }
		}
		if (dup) continue;

		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			vec = realloc(vec, cap * sizeof(*vec));
		}
		vec[n++] = strdup(p);
	}
	fclose(f);

	*out = vec;
	return n;
}

/* Resolve the launchable component of a package via the platform's
 * own resolver. Output looks like "com.x.y/com.x.y.MainActivity". */
static int resolve_activity(const char *pkg, char *out, size_t outlen)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "cmd package resolve-activity --brief %s 2>/dev/null", pkg);

	char **lines = NULL;
	int n = popen_lines(cmd, &lines);
	if (n < 1) {
		free_lines(lines, n);
		return -1;
	}
	/* Last non-empty line is the component. */
	const char *comp = lines[n - 1];
	if (!strchr(comp, '/')) {
		free_lines(lines, n);
		return -1;
	}
	snprintf(out, outlen, "%s", comp);
	free_lines(lines, n);
	return 0;
}

/* Wait up to <secs> for the package's process to appear, return pid or -1. */
static int wait_for_pid(const char *pkg, int secs)
{
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", pkg);
	for (int i = 0; i < secs; i++) {
		char **lines = NULL;
		int n = popen_lines(cmd, &lines);
		if (n >= 1) {
			int pid = atoi(lines[0]);
			free_lines(lines, n);
			if (pid > 0) return pid;
		}
		free_lines(lines, n);
		sleep(1);
	}
	return -1;
}

/* -------------------------------------------------------------------------
 * Pin / FIEMAP / record
 * ------------------------------------------------------------------------- */

static int do_f2fs_pin(int fd, const char *path, int pin)
{
	__u32 v = pin ? 1 : 0;
	if (ioctl(fd, F2FS_IOC_SET_PIN_FILE, &v) < 0) {
		if (errno != ENOTTY)
			fprintf(stderr, "  warning: F2FS pin on %s: %s\n",
				path, strerror(errno));
		return -1;
	}
	return 0;
}

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

static int record_write(FILE *rec, const char *path,
			ino_t ino, time_t mtime,
			const struct fiemap_extent *exts, unsigned n)
{
	fprintf(rec, "FILE %s ino=%llu mtime=%lld nextents=%u\n",
		path, (unsigned long long)ino, (long long)mtime, n);
	for (unsigned i = 0; i < n; i++) {
		fprintf(rec, "  EXT phys=0x%llx len=0x%llx flags=0x%x\n",
			(unsigned long long)exts[i].fe_physical,
			(unsigned long long)exts[i].fe_length,
			exts[i].fe_flags);
	}
	return 0;
}

/* Pin a list of files into a record file. Shared by 'pin' and 'app'. */
static int pin_files(const char *recpath, char **paths, int npaths)
{
	FILE *rec = fopen(recpath, "w");
	if (!rec) {
		perror(recpath);
		return 1;
	}

	int ok = 0;
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
		ok++;
	}

	fclose(rec);
	printf("pinned %d / %d files; record at %s\n", ok, npaths, recpath);
	return 0;
}

/* -------------------------------------------------------------------------
 * Command dispatch
 * ------------------------------------------------------------------------- */

static int cmd_pin(const char *recpath, char **paths, int npaths)
{
	return pin_files(recpath, paths, npaths);
}

static int cmd_app(const char *recpath, const char *pkg)
{
	char **files = NULL;
	int n = enumerate_package(pkg, &files);
	if (n < 0)
		return 1;

	printf("enumerated %d file(s) for %s:\n", n, pkg);
	for (int i = 0; i < n; i++)
		printf("  %s\n", files[i]);
	printf("\n");

	int rc = pin_files(recpath, files, n);

	for (int i = 0; i < n; i++)
		free(files[i]);
	free(files);
	return rc;
}

/* observe: launch the package, read its /proc/<pid>/maps, and pin
 * every mapped file under /data/app/. This is the runtime-discovery
 * complement to 'app' (static discovery). The launch itself is cold;
 * the pin benefits subsequent launches. */
static int cmd_observe(const char *recpath, const char *pkg,
		       const char *activity_opt)
{
	char activity[256];
	if (activity_opt && strchr(activity_opt, '/')) {
		snprintf(activity, sizeof(activity), "%s", activity_opt);
	} else if (resolve_activity(pkg, activity, sizeof(activity)) < 0) {
		fprintf(stderr,
			"observe: could not resolve activity for %s; "
			"pass it as 4th arg\n", pkg);
		return 1;
	}

	char cmd[512];
	int unused;
	snprintf(cmd, sizeof(cmd), "am force-stop %s >/dev/null 2>&1", pkg);
	unused = system(cmd);
	sleep(1);

	snprintf(cmd, sizeof(cmd), "am start -n %s >/dev/null 2>&1", activity);
	unused = system(cmd);
	(void)unused;

	int pid = wait_for_pid(pkg, 10);
	if (pid < 0) {
		fprintf(stderr, "observe: process never appeared for %s\n", pkg);
		return 1;
	}

	/* Let launch-phase mmaps settle. */
	sleep(3);

	char **files = NULL;
	int n = read_proc_maps(pid, "/data/app/", &files);
	if (n < 0) return 1;
	if (n == 0) {
		fprintf(stderr,
			"observe: no /data/app/ files mapped — wrong package "
			"or app launch failed?\n");
		return 1;
	}

	printf("observed %d mapped file(s) under /data/app/ for %s (pid %d):\n",
	       n, pkg, pid);
	for (int i = 0; i < n; i++)
		printf("  %s\n", files[i]);
	printf("\n");

	int rc = pin_files(recpath, files, n);

	for (int i = 0; i < n; i++) free(files[i]);
	free(files);
	return rc;
}

static int cmd_unpin(const char *recpath)
{
	FILE *rec = fopen(recpath, "r");
	if (!rec) {
		perror(recpath);
		return 1;
	}

	char line[1024], path[512] = {0};

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
			"  %s pin     <record-file> <file>...           pin explicit files\n"
			"  %s app     <record-file> <package>           static enum + pin\n"
			"  %s observe <record-file> <package> [<act>]   launch + observe + pin\n"
			"  %s unpin   <record-file>                     reverse a previous pin\n"
			"  %s list    <record-file>                     dump the record\n",
			argv[0], argv[0], argv[0], argv[0], argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "pin") == 0 && argc >= 4)
		return cmd_pin(argv[2], &argv[3], argc - 3);
	if (strcmp(argv[1], "app") == 0 && argc == 4)
		return cmd_app(argv[2], argv[3]);
	if (strcmp(argv[1], "observe") == 0 && (argc == 4 || argc == 5))
		return cmd_observe(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
	if (strcmp(argv[1], "unpin") == 0)
		return cmd_unpin(argv[2]);
	if (strcmp(argv[1], "list") == 0)
		return cmd_list(argv[2]);

	fprintf(stderr, "unknown command or wrong arity: %s\n", argv[1]);
	return 1;
}
