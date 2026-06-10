// SPDX-License-Identifier: GPL-2.0
/*
 * fbo_module.c — FBO 3.0 in-kernel pin module.
 *
 * Exposes /sys/kernel/fbo/pin. Writing a package name to it triggers
 * the kernel-side enumeration and pin sequence for that package's
 * static distribution files under /data/app/.
 *
 *   echo com.antutu.ABenchMark > /sys/kernel/fbo/pin
 *
 * Targets kernel >= 5.10. Tested signatures may need adjustment on
 * older Android kernels (notably the filldir_t return type changed
 * from int to bool around 5.18).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/fiemap.h>
#include <linux/cred.h>

#define FBO_DATA_APP   "/data/app"
#define FBO_MAX_PATH   1024
#define FBO_MAX_NAME   256
#define FBO_MAX_FILES  64
#define FBO_MAX_EXT    256

#ifdef CONFIG_ARM64
  #define FBO_ISA "arm64"
#elif defined(CONFIG_ARM)
  #define FBO_ISA "arm"
#elif defined(CONFIG_X86_64)
  #define FBO_ISA "x86_64"
#elif defined(CONFIG_X86)
  #define FBO_ISA "x86"
#else
  #define FBO_ISA "unknown"
#endif

static struct kobject *fbo_kobj;

/* ============================================================ *
 *  Directory iteration: collect entry names, optionally filtered
 *  by a substring (used to find <package>-<hash> inside a hash dir).
 * ============================================================ */

struct fbo_iter_ctx {
	struct dir_context ctx;
	const char        *contains;       /* substring filter; NULL = all */
	char              *names[FBO_MAX_FILES];
	int                n_names;
};

static bool fbo_filldir(struct dir_context *ctx, const char *name, int namlen,
			loff_t offset, u64 ino, unsigned d_type)
{
	struct fbo_iter_ctx *fc = container_of(ctx, struct fbo_iter_ctx, ctx);
	char *copy;

	if (fc->n_names >= FBO_MAX_FILES)
		return false;
	if (namlen >= FBO_MAX_NAME)
		return true;
	if (namlen == 1 && name[0] == '.')
		return true;
	if (namlen == 2 && name[0] == '.' && name[1] == '.')
		return true;

	copy = kmalloc(namlen + 1, GFP_KERNEL);
	if (!copy)
		return false;
	memcpy(copy, name, namlen);
	copy[namlen] = '\0';

	if (fc->contains && !strstr(copy, fc->contains)) {
		kfree(copy);
		return true;
	}

	fc->names[fc->n_names++] = copy;
	return true;
}

/* Read one directory level into a name list. Caller kfree's each name.
 * Returns count or negative errno. */
static int fbo_list_dir(const char *path, const char *contains,
			char **names_out, int max)
{
	struct path p;
	struct file *filp;
	struct fbo_iter_ctx fc = {
		.ctx.actor = fbo_filldir,
		.contains  = contains,
		.n_names   = 0,
	};
	int ret, i, copy_n;

	ret = kern_path(path, LOOKUP_DIRECTORY, &p);
	if (ret)
		return ret;

	filp = dentry_open(&p, O_RDONLY | O_DIRECTORY, current_cred());
	path_put(&p);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	ret = iterate_dir(filp, &fc.ctx);
	fput(filp);
	if (ret < 0) {
		for (i = 0; i < fc.n_names; i++)
			kfree(fc.names[i]);
		return ret;
	}

	copy_n = min(fc.n_names, max);
	for (i = 0; i < copy_n; i++)
		names_out[i] = fc.names[i];
	for (i = copy_n; i < fc.n_names; i++)
		kfree(fc.names[i]);
	return copy_n;
}

/* ============================================================ *
 *  Per-file pin sequence (in-kernel).
 * ============================================================ */

/* OEM-specific UFS vendor pin. Replace this with your real binding —
 * a write to /sys/.../ufs_pin, a SCSI passthrough, or a vendor ioctl. */
static int fbo_ufs_pin(const char *path,
		       const struct fiemap_extent *exts, unsigned n, int pin)
{
	unsigned i;
	pr_info("fbo: ufs_%s %s (%u extent%s)\n",
		pin ? "PIN" : "UNPIN", path, n, n == 1 ? "" : "s");
	for (i = 0; i < n; i++) {
		pr_info("fbo:   lba=0x%llx len=0x%llx flags=0x%x\n",
			(unsigned long long)exts[i].fe_physical,
			(unsigned long long)exts[i].fe_length,
			exts[i].fe_flags);
	}
	return 0;
}

/* Run FIEMAP on a file and feed the extents to the UFS pin path. */
static int fbo_pin_one(const char *path)
{
	struct file *filp;
	struct inode *inode;
	struct fiemap_extent_info fei = { 0 };
	struct fiemap_extent *exts;
	int ret;

	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_warn("fbo: open %s: %ld\n", path, PTR_ERR(filp));
		return PTR_ERR(filp);
	}
	inode = file_inode(filp);

	exts = kcalloc(FBO_MAX_EXT, sizeof(*exts), GFP_KERNEL);
	if (!exts) {
		filp_close(filp, NULL);
		return -ENOMEM;
	}

	fei.fi_flags         = FIEMAP_FLAG_SYNC;
	fei.fi_extents_max   = FBO_MAX_EXT;
	fei.fi_extents_start = (struct fiemap_extent __user *)exts;
	/* NOTE: fi_extents_start is declared as __user but we pass a
	 * kernel pointer. Most fs/fiemap.c paths write through it
	 * via fiemap_fill_next_extent() which uses copy_to_user. On
	 * kernels with set_fs() removed (>= 5.10), this requires a
	 * small patch to fiemap.c to skip the user-access check when
	 * called from kernel context — or use a userspace bounce
	 * buffer via memdup_user-style helpers. */

	if (!inode->i_op->fiemap) {
		pr_warn("fbo: %s: no FIEMAP support on this inode\n", path);
		ret = -EOPNOTSUPP;
		goto out;
	}

	ret = inode->i_op->fiemap(inode, &fei, 0, ~0ULL);
	if (ret < 0) {
		pr_warn("fbo: FIEMAP %s: %d\n", path, ret);
		goto out;
	}

	/*
	 * Step that's missing: F2FS pin to lock the file's blocks
	 * against GC. Requires either:
	 *   - an EXPORT_SYMBOL_GPL on a F2FS helper (small patch)
	 *   - or accepting that pins drift over GC cycles
	 */

	ret = fbo_ufs_pin(path, exts, fei.fi_extents_mapped, 1);

out:
	kfree(exts);
	filp_close(filp, NULL);
	return ret;
}

/* ============================================================ *
 *  Package -> install directory resolution.
 *
 *  /data/app/<H1>/<package>-<H2>/
 *
 *  We walk /data/app, then for each <H1>, look for a child whose
 *  name contains the package string.
 * ============================================================ */

static int fbo_find_install_dir(const char *pkg, char *out, size_t outlen)
{
	char *outer[FBO_MAX_FILES];
	char *inner[FBO_MAX_FILES];
	int n_outer, n_inner, i, j, ret = -ENOENT;
	char sub[FBO_MAX_PATH];

	n_outer = fbo_list_dir(FBO_DATA_APP, NULL, outer, FBO_MAX_FILES);
	if (n_outer < 0)
		return n_outer;

	for (i = 0; i < n_outer; i++) {
		snprintf(sub, sizeof(sub), "%s/%s", FBO_DATA_APP, outer[i]);
		n_inner = fbo_list_dir(sub, pkg, inner, FBO_MAX_FILES);
		if (n_inner > 0) {
			/* take the first match */
			snprintf(out, outlen, "%s/%s", sub, inner[0]);
			for (j = 0; j < n_inner; j++)
				kfree(inner[j]);
			ret = 0;
			break;
		}
	}

	for (i = 0; i < n_outer; i++)
		kfree(outer[i]);
	return ret;
}

/* ============================================================ *
 *  Top-level: pin a package's distribution files.
 * ============================================================ */

static int fbo_pin_package(const char *pkg)
{
	char install_dir[FBO_MAX_PATH];
	char *files[FBO_MAX_FILES];
	char sub[FBO_MAX_PATH];
	char path[FBO_MAX_PATH];
	int n, i, total = 0;

	if (fbo_find_install_dir(pkg, install_dir, sizeof(install_dir)) < 0) {
		pr_warn("fbo: install dir not found for %s\n", pkg);
		return -ENOENT;
	}
	pr_info("fbo: install dir = %s\n", install_dir);

	/* Pin base.apk + every split (anything ending in .apk in the
	 * install dir). */
	n = fbo_list_dir(install_dir, ".apk", files, FBO_MAX_FILES);
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/%s", install_dir, files[i]);
		if (fbo_pin_one(path) == 0)
			total++;
		kfree(files[i]);
	}

	/* Pin oat/<isa>/* */
	snprintf(sub, sizeof(sub), "%s/oat/%s", install_dir, FBO_ISA);
	n = fbo_list_dir(sub, NULL, files, FBO_MAX_FILES);
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/%s", sub, files[i]);
		if (fbo_pin_one(path) == 0)
			total++;
		kfree(files[i]);
	}

	/* Pin lib/<isa>/* (may not exist; that's fine) */
	snprintf(sub, sizeof(sub), "%s/lib/%s", install_dir, FBO_ISA);
	n = fbo_list_dir(sub, NULL, files, FBO_MAX_FILES);
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/%s", sub, files[i]);
		if (fbo_pin_one(path) == 0)
			total++;
		kfree(files[i]);
	}

	pr_info("fbo: pinned %d file(s) for %s\n", total, pkg);
	return 0;
}

/* ============================================================ *
 *  sysfs interface
 * ============================================================ */

static ssize_t pin_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	char pkg[FBO_MAX_NAME];
	size_t n;
	int ret;

	n = min(count, sizeof(pkg) - 1);
	memcpy(pkg, buf, n);
	pkg[n] = '\0';
	/* trim trailing newline */
	while (n > 0 && (pkg[n - 1] == '\n' || pkg[n - 1] == '\r'))
		pkg[--n] = '\0';

	if (n == 0)
		return -EINVAL;

	pr_info("fbo: pin request: %s\n", pkg);
	ret = fbo_pin_package(pkg);
	if (ret < 0)
		return ret;
	return count;
}

static struct kobj_attribute pin_attr = __ATTR(pin, 0220, NULL, pin_store);

/* ============================================================ *
 *  Module init / exit
 * ============================================================ */

static int __init fbo_init(void)
{
	int ret;

	fbo_kobj = kobject_create_and_add("fbo", kernel_kobj);
	if (!fbo_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(fbo_kobj, &pin_attr.attr);
	if (ret) {
		kobject_put(fbo_kobj);
		return ret;
	}
	pr_info("fbo: loaded (ISA=%s); echo <pkg> > /sys/kernel/fbo/pin\n",
		FBO_ISA);
	return 0;
}

static void __exit fbo_exit(void)
{
	sysfs_remove_file(fbo_kobj, &pin_attr.attr);
	kobject_put(fbo_kobj);
	pr_info("fbo: unloaded\n");
}

module_init(fbo_init);
module_exit(fbo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("FBO 3.0");
MODULE_DESCRIPTION("In-kernel package file enumeration and pin");
