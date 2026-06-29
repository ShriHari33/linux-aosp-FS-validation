#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Host-side Android F2FS FIEMAP/GC lab harness.

This script is meant to be run from a workstation with adb.  It coordinates a
rooted Android device, package APK/ODEX/VDEX discovery, optional APK install,
FIEMAP snapshots, donor-file pressure, F2FS GC triggering, and before/after
comparison.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


DEFAULT_ATTRS = (
    "gc_urgent",
    "gc_urgent_sleep_time",
    "gc_idle",
    "gc_idle_interval",
    "gc_remaining_trials",
    "gc_reclaimed_segments",
    "gc_segment_mode",
    "migration_granularity",
    "max_victim_search",
    "moved_blocks_foreground",
    "moved_blocks_background",
    "gc_foreground_calls",
    "gc_background_calls",
    "free_segments",
    "dirty_segments",
    "prefree_segments",
    "extension_list",
    "gc_pin_file_thresh",
)


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr, flush=True)


def q(value: str | Path) -> str:
    return shlex.quote(str(value))


def sha1_text(value: str) -> str:
    return hashlib.sha1(value.encode("utf-8")).hexdigest()


def now_stamp() -> str:
    return _dt.datetime.now().strftime("%Y%m%d-%H%M%S")


def normalize_adb_text(value: str) -> str:
    return value.replace("\r\n", "\n").replace("\r", "\n")


class CommandError(RuntimeError):
    def __init__(self, cmd: list[str], rc: int, stdout: str, stderr: str):
        self.cmd = cmd
        self.rc = rc
        self.stdout = stdout
        self.stderr = stderr
        super().__init__(f"command failed rc={rc}: {' '.join(cmd)}")


class Lab:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.outdir = Path(args.output or f"f2fs-gc-lab-{now_stamp()}").resolve()
        self.logdir = self.outdir / "logs"
        self.snapdir = self.outdir / "snapshots"
        self.helper_installed = False
        self.data_mount: dict[str, str] = {}
        self.sysfs_dir = ""
        self.proc_dir = ""
        self.targets: list[str] = []
        self.warnings: list[str] = []

    def adb_base(self) -> list[str]:
        cmd = [self.args.adb]
        if self.args.serial:
            cmd += ["-s", self.args.serial]
        return cmd

    def run(self, cmd: list[str], *, check: bool = True,
            cwd: str | Path | None = None) -> subprocess.CompletedProcess[str]:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd) if cwd else None,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        proc.stdout = normalize_adb_text(proc.stdout)
        proc.stderr = normalize_adb_text(proc.stderr)
        if check and proc.returncode != 0:
            raise CommandError(cmd, proc.returncode, proc.stdout, proc.stderr)
        return proc

    def adb(self, args: list[str], *, check: bool = True) -> str:
        proc = self.run(self.adb_base() + args, check=check)
        return proc.stdout

    def shell(self, script: str, *, root: bool = False,
              check: bool = True) -> str:
        if root:
            su_words = shlex.split(self.args.su_command)
            cmd = self.adb_base() + ["shell"] + su_words + [script]
        else:
            cmd = self.adb_base() + ["shell", "sh", "-c", script]
        proc = self.run(cmd, check=check)
        return proc.stdout

    def root(self, script: str, *, check: bool = True) -> str:
        return self.shell(script, root=True, check=check)

    def write_text(self, rel: str, value: str) -> None:
        path = self.outdir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value, encoding="utf-8")

    def write_json(self, rel: str, value: Any) -> None:
        path = self.outdir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")

    def append_warning(self, msg: str) -> None:
        self.warnings.append(msg)
        eprint(f"warning: {msg}")

    def setup_output(self) -> None:
        self.logdir.mkdir(parents=True, exist_ok=True)
        self.snapdir.mkdir(parents=True, exist_ok=True)
        self.write_json("run_args.json", vars(self.args))

    def verify_device(self) -> None:
        eprint("checking adb device and root shell")
        self.adb(["wait-for-device"])
        uid = self.root("id -u", check=True).strip()
        if uid != "0":
            raise RuntimeError(
                f"root shell did not return uid 0; got {uid!r}. "
                "Use --su-command if your su syntax differs."
            )

    def install_helper(self) -> None:
        if not self.args.helper:
            return
        helper = Path(self.args.helper).resolve()
        if not helper.exists():
            raise FileNotFoundError(helper)

        eprint(f"pushing helper to {self.args.device_helper}")
        self.adb(["push", str(helper), self.args.device_helper])
        self.root(f"chmod 755 {q(self.args.device_helper)}")
        out = self.root(f"[ -x {q(self.args.device_helper)} ] && echo ok")
        self.helper_installed = out.strip() == "ok"
        if not self.helper_installed:
            raise RuntimeError("helper push succeeded, but device helper is not executable")

    def discover_mounts(self) -> None:
        eprint("discovering /data F2FS mount")
        mounts = self.root("cat /proc/mounts")
        self.write_text("logs/proc_mounts.txt", mounts)
        for line in mounts.splitlines():
            fields = line.split()
            if len(fields) >= 4 and fields[1] == "/data":
                self.data_mount = {
                    "device": fields[0],
                    "mountpoint": fields[1],
                    "fstype": fields[2],
                    "options": fields[3],
                }
                break
        if not self.data_mount:
            raise RuntimeError("could not find /data in /proc/mounts")
        if self.data_mount["fstype"] != "f2fs":
            self.append_warning(
                f"/data fstype is {self.data_mount['fstype']}, not f2fs"
            )

        script = r'''
dev=$(awk '$2=="/data"{print $1; exit}' /proc/mounts)
base=${dev##*/}
for d in "/sys/fs/f2fs/$base" /sys/fs/f2fs/*; do
    [ -d "$d" ] && { echo "$d"; exit 0; }
done
'''
        self.sysfs_dir = self.root(script, check=False).strip().splitlines()[:1]
        self.sysfs_dir = self.sysfs_dir[0] if self.sysfs_dir else ""

        script = r'''
dev=$(awk '$2=="/data"{print $1; exit}' /proc/mounts)
base=${dev##*/}
for d in "/proc/fs/f2fs/$base" /proc/fs/f2fs/*; do
    [ -d "$d" ] && { echo "$d"; exit 0; }
done
'''
        self.proc_dir = self.root(script, check=False).strip().splitlines()[:1]
        self.proc_dir = self.proc_dir[0] if self.proc_dir else ""

        self.write_json("f2fs_mount.json", {
            "data_mount": self.data_mount,
            "sysfs_dir": self.sysfs_dir,
            "proc_dir": self.proc_dir,
        })

    def collect_f2fs_state(self, label: str, *, segment_info: bool) -> dict[str, Any]:
        eprint(f"collecting F2FS state: {label}")
        state: dict[str, Any] = {
            "label": label,
            "data_mount": self.data_mount,
            "sysfs_dir": self.sysfs_dir,
            "proc_dir": self.proc_dir,
            "attrs": {},
        }

        if self.sysfs_dir:
            attr_script = ["set +e"]
            for attr in DEFAULT_ATTRS:
                path = f"{self.sysfs_dir}/{attr}"
                attr_script.append(
                    f"if [ -e {q(path)} ]; then "
                    f"printf '%s\\t' {q(attr)}; cat {q(path)}; "
                    "printf '\\n__END_ATTR__\\n'; fi"
                )
            out = self.root("\n".join(attr_script), check=False)
            self.write_text(f"logs/f2fs_attrs_{label}.txt", out)
            for chunk in out.split("\n__END_ATTR__\n"):
                if "\t" not in chunk:
                    continue
                name, value = chunk.split("\t", 1)
                state["attrs"][name.strip()] = value.strip()

        if self.proc_dir:
            status = self.root(f"cat {q(self.proc_dir + '/status')}",
                               check=False)
            self.write_text(f"logs/f2fs_status_{label}.txt", status)
            state["status"] = parse_f2fs_status(status)
            if segment_info:
                seg = self.root(f"cat {q(self.proc_dir + '/segment_info')}",
                                check=False)
                self.write_text(f"logs/segment_info_{label}.txt", seg)
                state["segment_info_file"] = f"logs/segment_info_{label}.txt"

        self.write_json(f"f2fs_state_{label}.json", state)
        return state

    def compile_package(self) -> None:
        if self.args.skip_dexopt:
            return
        eprint("forcing package dexopt before baseline snapshot")
        pkg = q(self.args.package)
        mode = q(self.args.compile_mode)
        out = self.root(f"cmd package compile -m {mode} -f {pkg}", check=False)
        self.write_text("logs/package_compile.txt", out)

    def install_apks_if_requested(self) -> None:
        if not self.args.install_apk:
            return
        eprint("installing APK set with adb")
        apks = [str(Path(p).resolve()) for p in self.args.install_apk]
        for apk in apks:
            if not Path(apk).exists():
                raise FileNotFoundError(apk)

        if self.args.uninstall_first:
            self.adb(["uninstall", self.args.package], check=False)

        if len(apks) == 1:
            cmd = self.adb_base() + ["install", "-r", apks[0]]
        else:
            cmd = self.adb_base() + ["install-multiple", "-r"] + apks
        proc = self.run(cmd, check=False)
        self.write_text("logs/adb_install_stdout.txt", proc.stdout)
        self.write_text("logs/adb_install_stderr.txt", proc.stderr)
        if proc.returncode != 0:
            raise CommandError(cmd, proc.returncode, proc.stdout, proc.stderr)

    def donor_script(self, action: str) -> str:
        donor_dir = q(self.args.donor_dir)
        count = int(self.args.donor_count)
        mib = int(self.args.donor_mib)
        exts = " ".join(q(x.strip().lstrip(".")) for x in self.args.donor_exts.split(",") if x.strip())

        if action == "create":
            return f'''
set -e
dir={donor_dir}
rm -rf "$dir"
mkdir -p "$dir"
i=0
while [ "$i" -lt {count} ]; do
    for ext in {exts}; do
        dd if=/dev/zero of="$dir/donor_${{i}}.${{ext}}" bs=1048576 count={mib} conv=fsync >/dev/null 2>&1
    done
    i=$((i + 1))
done
sync
du -sm "$dir" 2>/dev/null || true
'''
        if action == "delete":
            return f'''
set +e
dir={donor_dir}
rm -f "$dir"/donor_*
rmdir "$dir" 2>/dev/null
sync
'''
        raise ValueError(action)

    def create_donors(self, label: str) -> None:
        if self.args.donor_count <= 0 or self.args.donor_mib <= 0:
            return
        eprint(f"creating donor files: {label}")
        out = self.root(self.donor_script("create"), check=False)
        self.write_text(f"logs/donors_create_{label}.txt", out)

    def delete_donors(self, label: str) -> None:
        eprint(f"deleting donor files: {label}")
        out = self.root(self.donor_script("delete"), check=False)
        self.write_text(f"logs/donors_delete_{label}.txt", out)

    def discover_targets(self) -> list[str]:
        eprint("discovering APK/ODEX/VDEX targets")
        pkg = q(self.args.package)
        script = f'''
set +e
pkg={pkg}
pm path "$pkg" 2>/dev/null | sed 's/^package://'
for apk in $(pm path "$pkg" 2>/dev/null | sed 's/^package://'); do
    d=${{apk%/*}}
    find "$d" -xdev -type f \\( -name '*.apk' -o -name '*.odex' -o -name '*.vdex' \\) -print 2>/dev/null
done
find /data/app -xdev -type f \\( -name '*.apk' -o -name '*.odex' -o -name '*.vdex' \\) -path "*$pkg*" -print 2>/dev/null
find /data/dalvik-cache -type f \\( -name "*$pkg*.odex" -o -name "*$pkg*.vdex" \\) -print 2>/dev/null
'''
        out = self.root(script, check=False)
        candidates = []
        seen = set()
        for raw in out.splitlines() + list(self.args.extra_path or []):
            path = raw.strip()
            if not path or path in seen:
                continue
            seen.add(path)
            candidates.append(path)

        existing = []
        for path in candidates:
            ok = self.root(f"[ -f {q(path)} ] && echo ok", check=False).strip()
            if ok == "ok":
                existing.append(path)

        if not existing:
            raise RuntimeError(
                "no APK/ODEX/VDEX targets found; use --extra-path for explicit files"
            )

        self.targets = sorted(existing)
        self.write_text("targets.txt", "\n".join(self.targets) + "\n")
        return self.targets

    def has_filefrag(self) -> bool:
        out = self.root("command -v filefrag >/dev/null 2>&1 && echo yes",
                        check=False)
        return out.strip() == "yes"

    def snapshot_file(self, path: str, label: str) -> dict[str, Any]:
        safe = sha1_text(path)[:12] + "_" + re.sub(r"[^A-Za-z0-9_.-]", "_", Path(path).name)
        entry: dict[str, Any] = {"path": path}

        stat_script = f'''
p={q(path)}
if [ ! -e "$p" ]; then echo missing; exit 1; fi
stat -c 'dev=%d inode=%i size=%s mtime=%Y mode=%f path=%n' "$p" 2>/dev/null || ls -ln "$p"
sha256sum "$p" 2>/dev/null || toybox sha256sum "$p" 2>/dev/null
'''
        stat_out = self.root(stat_script, check=False)
        entry["stat_raw"] = stat_out.strip()
        entry["stat"] = parse_stat_line(stat_out)
        entry["sha256"] = parse_sha256(stat_out)
        self.write_text(f"snapshots/{label}_{safe}.stat.txt", stat_out)

        if self.helper_installed:
            fm = self.root(f"{q(self.args.device_helper)} fiemap-json {q(path)}",
                           check=False)
            entry["fiemap_raw"] = fm.strip()
            try:
                entry["fiemap"] = json.loads(fm)
            except json.JSONDecodeError:
                entry["fiemap_error"] = fm.strip()
        elif self.has_filefrag():
            fm = self.root(f"filefrag -v {q(path)}", check=False)
            entry["fiemap_raw"] = fm.strip()
            entry["fiemap_tool"] = "filefrag"
        else:
            fm = "no helper binary and no filefrag on device"
            entry["fiemap_raw"] = fm
            entry["fiemap_error"] = fm
            self.append_warning(f"cannot collect FIEMAP for {path}: {fm}")

        self.write_text(f"snapshots/{label}_{safe}.fiemap.txt",
                        entry.get("fiemap_raw", "") + "\n")
        return entry

    def snapshot(self, label: str) -> dict[str, Any]:
        eprint(f"taking {label} snapshot")
        snap = {
            "label": label,
            "timestamp": _dt.datetime.now().isoformat(),
            "targets": [self.snapshot_file(path, label) for path in self.targets],
        }
        self.write_json(f"snapshots/{label}.json", snap)
        return snap

    def reset_gc_counters(self) -> None:
        if not self.sysfs_dir:
            return
        script = f'''
set +e
sys={q(self.sysfs_dir)}
for mode in 0 1 2 3 4 5 6; do
    [ -w "$sys/gc_segment_mode" ] && echo "$mode" > "$sys/gc_segment_mode"
    [ -w "$sys/gc_reclaimed_segments" ] && echo 0 > "$sys/gc_reclaimed_segments"
done
'''
        self.root(script, check=False)

    def trigger_sysfs_gc(self) -> None:
        if not self.sysfs_dir:
            self.append_warning("no /sys/fs/f2fs directory found; skipping gc_urgent")
            return
        eprint("triggering F2FS gc_urgent")
        script = f'''
set +e
sys={q(self.sysfs_dir)}
old_urgent=$(cat "$sys/gc_urgent" 2>/dev/null)
old_sleep=$(cat "$sys/gc_urgent_sleep_time" 2>/dev/null)
[ -w "$sys/gc_urgent_sleep_time" ] && echo {int(self.args.urgent_sleep_ms)} > "$sys/gc_urgent_sleep_time"
[ -w "$sys/gc_remaining_trials" ] && echo {int(self.args.gc_trials)} > "$sys/gc_remaining_trials"
[ -w "$sys/gc_urgent" ] && echo 1 > "$sys/gc_urgent"
sleep {int(self.args.gc_seconds)}
[ -w "$sys/gc_urgent" ] && echo 0 > "$sys/gc_urgent"
[ -n "$old_sleep" ] && [ -w "$sys/gc_urgent_sleep_time" ] && echo "$old_sleep" > "$sys/gc_urgent_sleep_time"
[ -n "$old_urgent" ] && [ -w "$sys/gc_urgent" ] && echo "$old_urgent" > "$sys/gc_urgent"
sync
'''
        out = self.root(script, check=False)
        self.write_text("logs/gc_urgent.txt", out)

    def trigger_helper_gc(self) -> None:
        if not self.helper_installed or not self.targets:
            return

        if self.args.range_gc:
            eprint("triggering targeted F2FS GC range ioctls")
            logs = []
            for path in self.targets:
                out = self.root(
                    f"{q(self.args.device_helper)} gc-range --sync {q(path)}",
                    check=False,
                )
                logs.append(out)
            self.write_text("logs/helper_gc_range.txt", "\n".join(logs))

        if self.args.gc_one_trials > 0:
            eprint("triggering ordinary F2FS GC ioctl loop")
            anchor = self.targets[0]
            script = f'''
set +e
i=0
while [ "$i" -lt {int(self.args.gc_one_trials)} ]; do
    {q(self.args.device_helper)} gc-one --sync {q(anchor)} >/dev/null 2>&1
    i=$((i + 1))
done
sync
'''
            out = self.root(script, check=False)
            self.write_text("logs/helper_gc_one.txt", out)

    def compare(self, before: dict[str, Any], after: dict[str, Any],
                before_state: dict[str, Any], after_state: dict[str, Any]) -> dict[str, Any]:
        eprint("comparing snapshots")
        after_by_path = {x["path"]: x for x in after["targets"]}
        rows = []
        for b in before["targets"]:
            path = b["path"]
            a = after_by_path.get(path)
            if not a:
                rows.append({"path": path, "missing_after": True})
                continue
            row = {
                "path": path,
                "same_sha256": b.get("sha256") == a.get("sha256"),
                "same_inode": same_inode(b, a),
                "same_size": b.get("stat", {}).get("size") == a.get("stat", {}).get("size"),
                "fiemap_changed": fiemap_signature(b) != fiemap_signature(a),
                "before_extent_count": extent_count(b),
                "after_extent_count": extent_count(a),
            }
            rows.append(row)

        summary = {
            "rows": rows,
            "warnings": self.warnings,
            "counter_delta": counter_delta(before_state, after_state),
        }
        self.write_json("summary.json", summary)
        self.write_text("summary.txt", render_summary(summary))
        return summary

    def run_lab(self) -> None:
        self.setup_output()
        self.verify_device()
        self.install_helper()
        self.discover_mounts()

        if self.args.install_apk and self.args.stage_donors_before_install:
            self.create_donors("before_install")
        self.install_apks_if_requested()
        self.compile_package()
        if self.args.install_apk and self.args.stage_donors_before_install:
            self.delete_donors("after_install_before_baseline")

        self.discover_targets()
        before_state = self.collect_f2fs_state(
            "before", segment_info=self.args.collect_segment_info
        )
        before = self.snapshot("before")

        self.reset_gc_counters()

        if not self.args.install_apk:
            self.create_donors("post_install")
            self.delete_donors("post_install")

        self.trigger_helper_gc()
        self.trigger_sysfs_gc()
        time.sleep(max(0, self.args.settle_seconds))

        after_state = self.collect_f2fs_state(
            "after", segment_info=self.args.collect_segment_info
        )
        after = self.snapshot("after")
        self.compare(before, after, before_state, after_state)
        self.write_text("warnings.txt", "\n".join(self.warnings) + ("\n" if self.warnings else ""))


def parse_stat_line(text: str) -> dict[str, str]:
    for line in text.splitlines():
        if line.startswith("dev="):
            result = {}
            for part in line.split():
                if "=" in part:
                    k, v = part.split("=", 1)
                    if k == "path":
                        break
                    result[k] = v
            return result
    return {}


def parse_sha256(text: str) -> str:
    for line in text.splitlines():
        fields = line.split()
        if fields and re.fullmatch(r"[0-9a-fA-F]{64}", fields[0]):
            return fields[0].lower()
    return ""


def fiemap_signature(entry: dict[str, Any]) -> Any:
    fm = entry.get("fiemap")
    if isinstance(fm, dict) and isinstance(fm.get("extents"), list):
        return [
            (x.get("logical"), x.get("physical"), x.get("length"), x.get("flags"))
            for x in fm["extents"]
        ]
    return entry.get("fiemap_raw", "")


def extent_count(entry: dict[str, Any]) -> int | None:
    fm = entry.get("fiemap")
    if isinstance(fm, dict):
        return int(fm.get("extent_count", 0))
    return None


def same_inode(before: dict[str, Any], after: dict[str, Any]) -> bool:
    bs = before.get("stat", {})
    ast = after.get("stat", {})
    return bool(bs) and bool(ast) and bs.get("dev") == ast.get("dev") and bs.get("inode") == ast.get("inode")


def parse_f2fs_status(text: str) -> dict[str, int]:
    status: dict[str, int] = {}
    m = re.search(r"Main\s*:\s*0x([0-9a-fA-F]+)", text)
    if m:
        status["main_blkaddr"] = int(m.group(1), 16)
    m = re.search(r"Segs/Sections\s*:\s*(\d+)", text)
    if m:
        status["segs_per_sec"] = int(m.group(1))
    m = re.search(r"Block size\s*:\s*(\d+)\s*KB", text)
    if m:
        status["block_size"] = int(m.group(1)) * 1024
    m = re.search(r"Segment size\s*:\s*(\d+)\s*MB", text)
    if m:
        status["segment_size"] = int(m.group(1)) * 1024 * 1024
    return status


def counter_delta(before_state: dict[str, Any], after_state: dict[str, Any]) -> dict[str, int]:
    delta: dict[str, int] = {}
    before = before_state.get("attrs", {})
    after = after_state.get("attrs", {})
    for key in (
        "moved_blocks_foreground",
        "moved_blocks_background",
        "gc_foreground_calls",
        "gc_background_calls",
        "free_segments",
        "dirty_segments",
        "prefree_segments",
    ):
        try:
            delta[key] = int(after.get(key, "0").splitlines()[0]) - int(before.get(key, "0").splitlines()[0])
        except (ValueError, IndexError):
            continue
    return delta


def render_summary(summary: dict[str, Any]) -> str:
    lines = []
    lines.append("F2FS FIEMAP GC lab summary")
    lines.append("")
    lines.append("Per-file result:")
    for row in summary["rows"]:
        if row.get("missing_after"):
            lines.append(f"  MISSING after: {row['path']}")
            continue
        verdict = "MOVED" if row["fiemap_changed"] else "same-map"
        lines.append(
            f"  {verdict:8} sha={str(row['same_sha256']).lower():5} "
            f"inode={str(row['same_inode']).lower():5} "
            f"extents={row.get('before_extent_count')}->{row.get('after_extent_count')} "
            f"{row['path']}"
        )
    lines.append("")
    lines.append("Counter delta:")
    for key, value in summary.get("counter_delta", {}).items():
        lines.append(f"  {key}: {value}")
    if summary.get("warnings"):
        lines.append("")
        lines.append("Warnings:")
        for warning in summary["warnings"]:
            lines.append(f"  {warning}")
    return "\n".join(lines) + "\n"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate FIEMAP movement of Android APK/ODEX/VDEX files under F2FS GC."
    )
    parser.add_argument("--package", required=True,
                        help="Android package name, for example com.example.game")
    parser.add_argument("--adb", default="adb", help="adb executable")
    parser.add_argument("--serial", help="adb device serial")
    parser.add_argument("--su-command", default="su -c",
                        help="root command words used after 'adb shell' (default: 'su -c')")
    parser.add_argument("--output", help="local output directory")

    parser.add_argument("--helper",
                        help="local Android binary built from f2fs_gc_probe.c")
    parser.add_argument("--device-helper", default="/data/local/tmp/f2fs_gc_probe",
                        help="device path where helper will be pushed")

    parser.add_argument("--install-apk", action="append",
                        help="APK path to install. Repeat for split APKs.")
    parser.add_argument("--uninstall-first", action="store_true",
                        help="adb uninstall package before install")
    parser.add_argument("--stage-donors-before-install", action=argparse.BooleanOptionalAction,
                        default=True,
                        help="with --install-apk, keep donor files alive during install")
    parser.add_argument("--extra-path", action="append",
                        help="explicit device file to include as a target")

    parser.add_argument("--skip-dexopt", action="store_true",
                        help="skip cmd package compile before baseline snapshot")
    parser.add_argument("--compile-mode", default="speed",
                        help="cmd package compile mode (default: speed)")

    parser.add_argument("--donor-dir", default="/data/local/tmp/f2fs_gc_lab_donors",
                        help="device donor directory on /data")
    parser.add_argument("--donor-count", type=int, default=12,
                        help="number of donor rounds")
    parser.add_argument("--donor-mib", type=int, default=8,
                        help="MiB per donor file per extension")
    parser.add_argument("--donor-exts", default="apk,vdex,odex",
                        help="comma-separated donor extensions")

    parser.add_argument("--range-gc", action="store_true",
                        help="use helper gc-range for each target extent")
    parser.add_argument("--gc-one-trials", type=int, default=32,
                        help="ordinary F2FS GC ioctl iterations through helper")
    parser.add_argument("--gc-trials", type=int, default=1000,
                        help="gc_remaining_trials value for gc_urgent")
    parser.add_argument("--gc-seconds", type=int, default=20,
                        help="seconds to keep gc_urgent enabled")
    parser.add_argument("--urgent-sleep-ms", type=int, default=20,
                        help="gc_urgent_sleep_time while running")
    parser.add_argument("--settle-seconds", type=int, default=2,
                        help="sleep after GC before after snapshot")
    parser.add_argument("--collect-segment-info", action="store_true",
                        help="collect full /proc/fs/f2fs/<dev>/segment_info")
    return parser


def main(argv: list[str]) -> int:
    args = build_parser().parse_args(argv)
    lab = Lab(args)
    try:
        lab.run_lab()
    except CommandError as err:
        eprint(str(err))
        eprint("stdout:")
        eprint(err.stdout)
        eprint("stderr:")
        eprint(err.stderr)
        return 1
    except Exception as err:
        eprint(f"error: {err}")
        return 1

    print(lab.outdir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
