#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ESP32 native-port audit, Task 2: per-file compile sweep of the app core
# against the ESP-IDF Xtensa toolchain. Categorizes, does NOT fix.
#
# Buckets:
#   A  compiles clean and the file itself never touches spdlog/fmt/json —
#      the shim was only needed to satisfy the repo-wide PCH (lvgl_pch.h
#      unconditionally includes spdlog + hv/json.hpp, so a literal
#      "no-shim" compile of ANY file is impossible; A is therefore defined
#      at file granularity via a usage scan).
#   B  compiles, and uses the shimmed surface (spdlog / fmt / nlohmann json).
#   C  fails on something a small #ifdef or config flip would fix
#      (missing POSIX-ish header, RTTI with -fno-rtti, log-sink backends).
#   D  fails on a fundamentally Linux-bound dependency
#      (SDL/DRM/evdev, BlueZ/DBus, wpa_cli, libusb, camera, libhv internals).
#   ?  compile error the classifier doesn't recognize — listed for manual
#      review; the audit report must not ship with '?' rows.
#
# Flag provenance:
#   - IDF side: the g++ command ESP-IDF generated for cxx_exception_stubs.cpp
#     in build/compile_commands.json (so sysroot/ABI/exception flags are
#     exactly what a real component build uses), with -std forced to gnu++17
#     (the app's standard) and warnings silenced (-w: the audit measures
#     errors, not hygiene).
#   - App side: mirrors the Linux Makefile compile line for printer_state.o,
#     MINUS Linux-only defines/includes (SDL display, systemd, ALSA, libusb,
#     GLES3D, wpa/SDL/libhv include dirs) and PLUS the shim include tree.
#
# Usage:
#   python3 sweep.py --slice          # vertical slice only
#   python3 sweep.py                  # slice + src/printer src/system src/ui
#   python3 sweep.py --files a.cpp b.cpp
#   python3 sweep.py --hv-stub --out audit_sweep_results_pass2.csv
#       Pass 2, "seam carved": adds skeletal hv/Event.h + hv/WebSocketClient.h
#       stand-ins (shim/hv_stub/) so files blocked ONLY by the transitive
#       moonraker_client.h → libhv include reveal what's really underneath.

import argparse
import concurrent.futures
import csv
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

AUDIT = Path(__file__).resolve().parent
REPO = AUDIT.parents[1]
SHIM = AUDIT / "components/helixcore/shim"

SLICE = [
    # UpdateQueue is header-only (include/ui_update_queue.h); printer_state.cpp
    # instantiates it heavily, so the slice covers it without a dedicated TU.
    "src/printer/printer_state.cpp",
    "src/application/static_subject_registry.cpp",
    "src/ui/ui_panel_home.cpp",
]
SWEEP_DIRS = ["src/printer", "src/system", "src/ui"]

APP_DEFINES = [
    '-DHELIX_VERSION="0.0.0-audit"',
    "-DHELIX_VERSION_MAJOR=0",
    "-DHELIX_VERSION_MINOR=0",
    "-DHELIX_VERSION_PATCH=0",
    '-DHELIX_GIT_HASH="audit"',
    '-DINSTALLER_FILENAME="install.sh"',
    "-DHELIX_MAX_FONT_TIER=6",
    # Portable feature logic stays ON so the sweep measures real code, not
    # #ifdef'd-out shells. Linux backends (SDL/systemd/ALSA/libusb/GLES) stay
    # OFF — that is the configuration an ESP32 build would use.
    "-DHELIX_ENABLE_MOCKS",
    "-DHELIX_ENABLE_SCREENSAVER",
    "-DHELIX_HAS_LABEL_PRINTER=1",
    "-DHELIX_HAS_CFS=1",
    "-DHELIX_HAS_IFS=1",
]

APP_INCLUDES = [
    f"-I{REPO}",
    f"-I{REPO}/include",
    f"-I{REPO}/src/generated",
    f"-I{REPO}/build/generated",
    f"-isystem{REPO}/lib",
    f"-isystem{REPO}/lib/glm",
    f"-isystem{REPO}/lib/lvgl",
    f"-isystem{REPO}/lib/lvgl/src",
    # Portable vendored C libs: header-visible so files using them aren't
    # penalized with artificial bucket-C rows (they'd compile on ESP32).
    f"-isystem{REPO}/lib/stb",
    f"-isystem{REPO}/lib/lv_markdown/src",
    f"-isystem{REPO}/lib/lv_markdown/deps/md4c",
    f"-isystem{REPO}/lib/quirc/lib",
    # Deliberately ABSENT: lib/spdlog/include and lib/libhv/* (shim provides
    # spdlog/* and hv/json.hpp; anything else hv/* failing = real signal),
    # SDL2, wpa_supplicant (Linux-bound).
    f"-I{SHIM}/include",
]

# First missing header decides C vs D. Substring match against the header path.
D_HEADERS = [
    "SDL", "drm", "gbm", "EGL/", "GLES", "GL/gl", "X11/",
    "systemd/", "libudev", "udev.h",
    "linux/input", "linux/uinput", "linux/videodev2", "linux/spi", "linux/fb",
    "alsa/", "asoundlib",
    "bluetooth/", "sd-bus", "dbus/",
    "wpa_ctrl", "common/wpa_ctrl",
    "libusb",
    "curl/",
    "hv/",  # hv/json.hpp is shimmed; any other hv/* = libhv network internals
    "pipewire", "pulse/",
    "dlfcn.h",    # dlopen() plugin/BlueZ loading — no dynamic linking on ESP-IDF
    "ucontext.h", # signal-context register dumps (crash handler) — Linux process model
]
C_HEADERS = [
    "spdlog/sinks/",  # log backend setup — replaced by esp_log on a port
    "sys/statvfs", "sys/vfs", "sys/mount", "sys/sysinfo", "sys/utsname",
    "sys/wait", "sys/prctl", "sys/inotify", "sys/eventfd", "sys/epoll",
    "sys/un.h", "sys/resource", "sys/syscall", "sys/mman",
    "pwd.h", "grp.h", "ifaddrs", "net/if", "netdb.h", "net/route",
    "syslog.h", "execinfo", "mntent", "termios", "glob.h", "fnmatch",
    "arpa/inet", "netinet/", "poll.h",
]

# Files whose real fate is Linux-bound even when the FIRST error is a generic
# header (e.g. BlueZ RFCOMM code trips on poll.h before reaching bluetooth/*).
# Applied only when the header classifier said C or ?.
D_FILE_PATTERNS = re.compile(r"(^|/)(bt_|bluetooth|makeid_|niimbot_)")


def build_flags(jobs_dir: Path):
    cc_path = AUDIT / "build/compile_commands.json"
    entries = json.loads(cc_path.read_text())
    base = next(e for e in entries if e["file"].endswith("cxx_exception_stubs.cpp"))
    toks = shlex.split(base["command"])
    compiler = toks[0]

    flags, skip = [], False
    for t in toks[1:]:
        if skip:
            skip = False
            continue
        if t == "-o":
            skip = True
            continue
        if t in ("-c",) or t.endswith(".cpp"):
            continue
        if t.startswith("-std=") or t in ("-Os", "-Wall", "-Wextra") or t.startswith("-Werror") or t.startswith("-Wno-"):
            continue
        if t.startswith("-fdiagnostics-color"):
            continue
        flags.append(t)

    flags += [
        "-std=gnu++17", "-O0", "-w",
        # helixcore component surface (same as Task 1's component build)
        f"-I{AUDIT}/components/helixcore",
        f"-I{REPO}/lib/lvgl",
        f"-I{REPO}/lib/helix-xml",
        "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_KCONFIG_IGNORE",
    ]
    flags += APP_DEFINES + APP_INCLUDES
    flags += ["-include", f"{REPO}/include/lvgl_pch.h"]
    return compiler, flags


USES_SHIM_RE = re.compile(
    r"spdlog::|SPDLOG_|\bfmt::|nlohmann|hv::Json|[<\"]spdlog/|[<\"]hv/json\.hpp"
)
MISSING_RE = re.compile(r"fatal error: ([^\n:]+): No such file")
ERROR_LINE_RE = re.compile(r"^[^\n]*error:[^\n]*$", re.M)
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[mK]")
# "path:line:col: fatal error: ..." — path attributes WHERE the failure fired,
# so transitive blockage (a shared header, not the swept file) is visible.
ERR_LOC_RE = re.compile(r"^([^\s:][^:\n]*):\d+:(?:\d+:)? *(?:fatal )?error", re.M)


def classify_failure(stderr: str):
    m = MISSING_RE.search(stderr)
    if m:
        hdr = m.group(1).strip()
        for pat in D_HEADERS:
            if pat in hdr:
                return "D", hdr
        for pat in C_HEADERS:
            if pat in hdr:
                return "C", hdr
        return "?", hdr
    if "-fno-rtti" in stderr or re.search(r"\b(dynamic_cast|typeid)\b.*(rtti|RTTI)", stderr):
        return "C", "rtti (CONFIG_COMPILER_CXX_RTTI=y flips this)"
    # Xtensa newlib defines int32_t as 'long int' — std::min/max/clamp with
    # mixed int32_t/int arguments fails template deduction. Cast-level fix.
    if re.search(r"no matching function for call to '(min|max|clamp)\(", stderr):
        return "C", "int32_t=long on Xtensa (std::min/max/clamp arg mix)"
    if "'thread' is not a member of 'std'" in stderr:
        return "C", "missing #include <thread> (transitive include differs)"
    if re.search(r"'(SA_\w+|SIGRT\w+)' was not declared", stderr):
        return "C", "POSIX signal API (newlib lacks SA_* flags)"
    if "'timegm' was not declared" in stderr:
        return "C", "timegm() missing in newlib (portable reimpl exists)"
    m = ERROR_LINE_RE.search(stderr)
    return "?", (m.group(0).strip()[:200] if m else stderr.strip()[:200])


def sweep_file(compiler, flags, rel):
    src = REPO / rel
    cmd = [compiler] + flags + ["-c", str(src), "-o", "/dev/null"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return rel, "?", "compile timeout (600s)", "", ""
    if p.returncode == 0:
        text = src.read_text(errors="replace")
        if USES_SHIM_RE.search(text):
            return rel, "B", "spdlog/fmt/json via shim", "", ""
        return rel, "A", "", "", ""
    stderr = ANSI_RE.sub("", p.stderr)
    bucket, blocking = classify_failure(stderr)
    if bucket in ("C", "?") and D_FILE_PATTERNS.search(rel):
        bucket, blocking = "D", f"Bluetooth stack (BlueZ/RFCOMM); first error: {blocking}"
    via = ""
    m = ERR_LOC_RE.search(stderr)
    if m:
        loc = m.group(1)
        try:
            loc = str(Path(loc).resolve().relative_to(REPO))
        except ValueError:
            pass
        if loc != rel:
            via = loc
    first_err = ""
    m = ERROR_LINE_RE.search(stderr)
    if m:
        first_err = m.group(0).strip()[:300]
    return rel, bucket, blocking, via, first_err


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slice", action="store_true", help="vertical slice only")
    ap.add_argument("--files", nargs="*", help="explicit repo-relative files")
    ap.add_argument("--jobs", type=int, default=max(2, (os.cpu_count() or 4) - 2))
    ap.add_argument("--out", default=str(AUDIT / "audit_sweep_results.csv"))
    ap.add_argument("--hv-stub", action="store_true",
                    help="pass 2: carve the moonraker_client.h seam with skeletal hv headers")
    args = ap.parse_args()

    if args.files:
        targets = args.files
    elif args.slice:
        targets = SLICE
    else:
        targets = list(SLICE)
        seen = set(targets)
        for d in SWEEP_DIRS:
            for p in sorted((REPO / d).rglob("*.cpp")):
                rel = str(p.relative_to(REPO))
                if rel not in seen:
                    seen.add(rel)
                    targets.append(rel)

    compiler, flags = build_flags(AUDIT / "build")
    if args.hv_stub:
        flags.append(f"-I{SHIM}/hv_stub")
    print(f"sweep: {len(targets)} files, {args.jobs} jobs"
          + (" [pass 2: hv seam carved]" if args.hv_stub else ""), flush=True)

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(sweep_file, compiler, flags, t): t for t in targets}
        done = 0
        for fut in concurrent.futures.as_completed(futs):
            results.append(fut.result())
            done += 1
            if done % 25 == 0:
                print(f"  {done}/{len(targets)}", flush=True)

    results.sort(key=lambda r: r[0])
    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file", "bucket", "blocking", "via", "first_error"])
        w.writerows(results)

    # Summary: bucket counts per top-level directory
    summary = {}
    for rel, bucket, _, _, _ in results:
        top = "/".join(rel.split("/")[:2])
        summary.setdefault(top, {}).setdefault(bucket, 0)
        summary[top][bucket] += 1
    print(f"\n{'dir':<18} {'A':>4} {'B':>4} {'C':>4} {'D':>4} {'?':>4}  total")
    tot = {}
    for top in sorted(summary):
        row = summary[top]
        print(f"{top:<18} " + " ".join(f"{row.get(b, 0):>4}" for b in "ABCD?")
              + f"  {sum(row.values()):>5}")
        for b, n in row.items():
            tot[b] = tot.get(b, 0) + n
    print(f"{'TOTAL':<18} " + " ".join(f"{tot.get(b, 0):>4}" for b in "ABCD?")
          + f"  {sum(tot.values()):>5}")
    # Transitive-blockage attribution: which shared headers do the blocking
    vias = {}
    for rel, bucket, _, via, _ in results:
        if bucket in ("C", "D") and via:
            vias.setdefault(via, 0)
            vias[via] += 1
    if vias:
        print("\nblocked transitively via (top 15):")
        for via, n in sorted(vias.items(), key=lambda kv: -kv[1])[:15]:
            print(f"  {n:>4}  {via}")

    unclassified = [r for r in results if r[1] == "?"]
    if unclassified:
        print(f"\n{len(unclassified)} unclassified ('?') files — review these:")
        for rel, _, blocking, _, _ in unclassified[:40]:
            print(f"  {rel}: {blocking}")
    print(f"\nCSV: {args.out}")


if __name__ == "__main__":
    main()
