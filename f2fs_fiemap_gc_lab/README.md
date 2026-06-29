# Android F2FS FIEMAP GC Lab

This directory contains a host-side harness and a small native helper for
testing whether Android package artifacts move at the FIEMAP level during F2FS
garbage collection.

Target files:

- `base.apk`
- split `*.apk`
- `base.odex`
- `base.vdex`
- explicitly supplied `--extra-path` files

The success condition for a GC relocation test is:

- file content hash stays the same,
- device/inode stays the same,
- FIEMAP physical extents change,
- F2FS moved-block or GC counters increase.

## Files

- `f2fs_fiemap_gc_lab.py`: host-side `adb` orchestrator.
- `f2fs_gc_probe.c`: native helper for `FS_IOC_FIEMAP`,
  `F2FS_IOC_GARBAGE_COLLECT`, and `F2FS_IOC_GARBAGE_COLLECT_RANGE`.
- `Makefile`: helper/PDF build targets.
- `f2fs_fiemap_gc_lab.tex`: detailed guide source.
- `f2fs_fiemap_gc_lab.pdf`: generated guide after `make pdf`.

## Build the Android helper

```sh
cd tools/f2fs_fiemap_gc_lab
export ANDROID_NDK_HOME=/path/to/android-ndk
make android ANDROID_ARCH=arm64 ANDROID_API=30
```

The harness can push the resulting binary:

```sh
python3 f2fs_fiemap_gc_lab.py \
  --package your.package.name \
  --helper ./f2fs_gc_probe.android \
  --range-gc \
  --collect-segment-info
```

## Controlled APK install workflow

This is the recommended workflow when you want repeatability.  Donor files are
kept alive during install, then removed before GC.  That gives F2FS a better
chance of placing package artifacts in sections that later become partially
invalid.

```sh
python3 f2fs_fiemap_gc_lab.py \
  --package your.package.name \
  --helper ./f2fs_gc_probe.android \
  --install-apk /path/to/base.apk \
  --install-apk /path/to/split_config.apk \
  --range-gc \
  --gc-one-trials 128 \
  --gc-seconds 60 \
  --donor-count 64 \
  --donor-mib 4 \
  --collect-segment-info
```

For a single APK, provide one `--install-apk`.  For split APKs, repeat
`--install-apk`; the harness uses `adb install-multiple -r`.

## Existing Play Store install workflow

This is lower probability because the harness cannot change how the package
artifacts were originally allocated.

```sh
python3 f2fs_fiemap_gc_lab.py \
  --package your.package.name \
  --helper ./f2fs_gc_probe.android \
  --range-gc \
  --gc-one-trials 128 \
  --gc-seconds 60 \
  --donor-count 64 \
  --donor-mib 4
```

## Output

Each run creates a local directory such as `f2fs-gc-lab-20260629-143000`.
Important files:

- `summary.txt`: quick verdict per target.
- `summary.json`: machine-readable result.
- `targets.txt`: files under test.
- `snapshots/before.json` and `snapshots/after.json`: stat, hash, FIEMAP data.
- `logs/f2fs_attrs_before.txt` and `logs/f2fs_attrs_after.txt`: sysfs counters.
- `logs/segment_info_before.txt` and `logs/segment_info_after.txt`: only when
  `--collect-segment-info` is used.

`summary.txt` marks a file as `MOVED` only when the FIEMAP signature differs
between before and after snapshots.
