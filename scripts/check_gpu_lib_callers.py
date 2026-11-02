#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build gate: only reviewed objects call into the runtime-loaded GPU libraries.
#
# A DLOPEN_GPU_LIBS build (mk/implib.mk) reaches libdrm, libgbm, libEGL and
# libinput through trampolines, so the binary starts on a system without them.
# Every call path has to ask include/gpu_libs.h first; a path that does not
# gets 0 back from every function of a missing library, which is not a safe
# answer for all of them. This gate keeps the set of objects that reference
# any trampolined function to the list below, each entry checked by hand to
# sit behind a gpu_libs.h test, so a new caller fails the build until it is.
#
# Reads the function names out of the generated <lib>.init.c files and the
# undefined symbols of every object under the build directory.
#
# Usage:
#   scripts/check_gpu_lib_callers.py --build-dir build/pi32 [--nm arm-linux-gnueabihf-nm]
#
# Also fails on what it cannot see: an object nm cannot read, one with no
# symbol table, an LTO object (nm lists only its LTO marker), and an ALLOWED
# entry that no longer calls in. LVGL's DRM and libinput drivers call the
# libraries ungated, so the functions they export are held to the same list.

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import subprocess
import sys
from pathlib import Path

# Object (relative to the build dir, glob) -> where its calls are gated. An
# entry marked with EGL exists only in builds that link the EGL presentation
# path (an egl-variant/ directory); every other entry must match an object.
EGL = "egl"
ALLOWED = {
    "obj/lvgl/src/drivers/display/drm/lv_linux_drm_common.o": "lv_linux_drm_find_device_path() is never called",
    "obj/api/display_backend.o": "panel orientation and no-display diagnostics check Lib::Drm",
    "display/display_backend.o": "the same source, in the splash/watchdog display library",
    "obj/api/display_backend_drm.o": "constructor and is_available() check drm_libs_present()",
    "display/display_backend_drm.o": "the same source, in the splash/watchdog display library",
    "egl-variant/display_backend_drm.o": (EGL, "the EGL compile; drm_libs_present() covers the GPU stack"),
    "obj/lvgl/src/drivers/display/drm/lv_linux_drm.o": "reached only from DisplayBackendDRM",
    "egl-variant/lvgl/src/drivers/display/drm/lv_linux_drm_egl.o": (EGL, "reached only from the EGL DisplayBackendDRM"),
    "obj/lvgl/src/drivers/libinput/lv_libinput.o": "reached only behind Lib::Input in DisplayBackendDRM",
    "obj/application/probe_egl_cmd.o": "run_probe_egl() checks every library before touching one",
    "obj/rendering/gcode_gles_renderer.o": "init_gl() checks Lib::Egl/Gles2, the GBM path Lib::Gbm/Drm",
    "obj/ui/backdrop_blur.o": "gpu_blur() checks all four before the first EGL call",
}

# LVGL's drivers call the libraries ungated: whoever calls the driver is the
# gate. Their exported functions are held to the same reviewed list.
DRIVERS = (
    "obj/lvgl/src/drivers/display/drm/lv_linux_drm.o",
    "egl-variant/lvgl/src/drivers/display/drm/lv_linux_drm_egl.o",
    "obj/lvgl/src/drivers/libinput/lv_libinput.o",
)

# An LTO object holds its code as compiler IR in these sections. Whether nm
# sees its real symbols depends on the LTO plugin it was built with, so such an
# object is refused rather than trusted.
LTO_SECTION_PREFIX = ".gnu.lto_"

SYM_RE = re.compile(r'^\s*"([A-Za-z_][A-Za-z0-9_]*)",\s*$')


def stubbed_functions(build_dir: Path) -> set[str]:
    names: set[str] = set()
    for init_c in sorted((build_dir / "implib").glob("*.init.c")):
        for line in init_c.read_text().splitlines():
            m = SYM_RE.match(line)
            if m:
                names.add(m.group(1))
    return names


class NmError(RuntimeError):
    pass


def lto_objects(readelf: str, objects: list[Path]) -> set[Path]:
    found: set[Path] = set()
    for i in range(0, len(objects), 200):
        batch = objects[i:i + 200]
        out = subprocess.run([readelf, "-SW", *map(str, batch)], capture_output=True, text=True)
        if out.returncode != 0:
            raise NmError(f"{readelf} exited {out.returncode}: {out.stderr.strip()}")
        current = batch[0] if len(batch) == 1 else None
        for line in out.stdout.splitlines():
            if line.startswith("File: "):
                current = Path(line[len("File: "):].strip())
            elif LTO_SECTION_PREFIX in line and current is not None:
                found.add(current)
    return found


def read_symbols(nm: str, objects: list[Path]) -> dict[Path, tuple[set[str], set[str], bool]]:
    """object -> (undefined, defined globals, unused), for every object nm read."""
    result: dict[Path, tuple[set[str], set[str], bool]] = {}
    for i in range(0, len(objects), 200):
        batch = objects[i:i + 200]
        out = subprocess.run([nm, "-A", "-a", *map(str, batch)], capture_output=True, text=True)
        if out.returncode != 0:
            raise NmError(f"{nm} exited {out.returncode}: {out.stderr.strip()}")
        for line in out.stdout.splitlines():
            path, _, rest = line.partition(":")
            fields = rest.split()
            if len(fields) < 2:
                continue
            kind, name = fields[-2], fields[-1].split("@")[0]
            undef, defined, lto = result.setdefault(Path(path), (set(), set(), False))
            if kind == "U":
                undef.add(name)
            elif kind in "TWDRB" and kind.isupper():
                defined.add(name)
    return result


def required(entry: str, build_dir: Path) -> bool:
    value = ALLOWED[entry]
    return not (isinstance(value, tuple) and value[0] == EGL and not (build_dir / "egl-variant").is_dir())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", required=True, type=Path)
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--readelf", default=None, help="default: --nm with nm replaced by readelf")
    ap.add_argument("--allowed", type=Path,
                    help="JSON {\"allowed\": {glob: reason}, \"drivers\": [path]} replacing the "
                         "built-in lists, for the meta-tests")
    args = ap.parse_args()

    global ALLOWED, DRIVERS
    if args.allowed:
        spec = json.loads(args.allowed.read_text())
        ALLOWED = spec["allowed"]
        DRIVERS = tuple(spec.get("drivers", []))

    build_dir = args.build_dir
    stubbed = stubbed_functions(build_dir)
    if not stubbed:
        print(f"gpu-lib callers: no trampoline tables under {build_dir}/implib", file=sys.stderr)
        return 1

    objects = sorted(p for p in build_dir.rglob("*.o") if "implib" not in p.relative_to(build_dir).parts)
    readelf = args.readelf or re.sub(r"nm$", "readelf", args.nm)
    try:
        symbols = read_symbols(args.nm, objects)
        lto = lto_objects(readelf, objects)
    except NmError as e:
        print(f"gpu-lib callers: {e}", file=sys.stderr)
        return 1

    def rel(p: Path) -> str:
        return p.relative_to(build_dir).as_posix()

    findings: list[str] = []
    for obj in objects:
        entry = symbols.get(obj)
        if obj in lto:
            findings.append(f"{rel(obj)}: an LTO object, so its calls cannot be checked")
        elif entry is None:
            findings.append(f"{rel(obj)}: no symbol table, so its calls cannot be checked")

    driver_api: set[str] = set()
    for obj in objects:
        if rel(obj) in DRIVERS and obj in symbols:
            driver_api |= symbols[obj][1]

    def allowed(o: str) -> bool:
        return any(fnmatch.fnmatch(o, pat) for pat in ALLOWED)

    callers: dict[str, list[str]] = {}
    for obj, (undef, _defined, _lto) in symbols.items():
        used = sorted(undef & (stubbed | driver_api))
        if used:
            callers[rel(obj)] = used

    for obj, syms in sorted(callers.items()):
        if not allowed(obj):
            findings.append(f"{obj}: {', '.join(syms[:6])}{' ...' if len(syms) > 6 else ''}")

    for entry in ALLOWED:
        if not required(entry, build_dir):
            continue
        matched = [o for o in callers if fnmatch.fnmatch(o, entry)]
        if not matched:
            findings.append(f"{entry}: listed in ALLOWED but calls into none of the libraries "
                            "(stale entry, or the object was not built)")

    if findings:
        print("gpu-lib callers: unreviewed or uncheckable calls into a runtime-loaded GPU library:",
              file=sys.stderr)
        for line in findings:
            print(f"  {line}", file=sys.stderr)
        print("Gate each call path on include/gpu_libs.h, then add the object to ALLOWED in\n"
              "scripts/check_gpu_lib_callers.py with where it is gated; drop entries that no\n"
              "longer call in.", file=sys.stderr)
        return 1

    print(f"gpu-lib callers: {len(callers)} reviewed objects call into "
          f"{len(set().union(*callers.values()))} stubbed or driver functions "
          f"({len(stubbed)} stubbed, {len(driver_api)} driver entry points)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
