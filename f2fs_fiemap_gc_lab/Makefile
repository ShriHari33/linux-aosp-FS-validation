# SPDX-License-Identifier: GPL-2.0

ANDROID_API ?= 30
ANDROID_ARCH ?= arm64
NDK_PREBUILT ?= linux-x86_64

ifeq ($(ANDROID_ARCH),arm64)
ANDROID_TRIPLE := aarch64-linux-android
else ifeq ($(ANDROID_ARCH),arm)
ANDROID_TRIPLE := armv7a-linux-androideabi
else ifeq ($(ANDROID_ARCH),x86_64)
ANDROID_TRIPLE := x86_64-linux-android
else ifeq ($(ANDROID_ARCH),x86)
ANDROID_TRIPLE := i686-linux-android
else
$(error Unsupported ANDROID_ARCH=$(ANDROID_ARCH))
endif

ANDROID_CC ?= $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/$(NDK_PREBUILT)/bin/$(ANDROID_TRIPLE)$(ANDROID_API)-clang

.PHONY: all android host clean pdf

all: host

host: f2fs_gc_probe.host

android: f2fs_gc_probe.android

f2fs_gc_probe.host: f2fs_gc_probe.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

f2fs_gc_probe.android: f2fs_gc_probe.c
	@test -n "$(ANDROID_NDK_HOME)" || { echo "Set ANDROID_NDK_HOME first"; exit 1; }
	$(ANDROID_CC) -O2 -Wall -Wextra -static -o $@ $<

pdf: f2fs_fiemap_gc_lab.pdf

f2fs_fiemap_gc_lab.pdf: f2fs_fiemap_gc_lab.tex
	pdflatex -interaction=nonstopmode -halt-on-error $<
	pdflatex -interaction=nonstopmode -halt-on-error $<

clean:
	rm -f f2fs_gc_probe.host f2fs_gc_probe.android
	rm -f f2fs_fiemap_gc_lab.aux f2fs_fiemap_gc_lab.log f2fs_fiemap_gc_lab.out f2fs_fiemap_gc_lab.pdf
