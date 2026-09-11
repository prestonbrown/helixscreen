#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check assets/config/platforms.json against the consumers it feeds.

Advisory by default: it reports and exits 0, so it can land before every consumer
reads the manifest. Pass --strict to make findings fail, which is how it gets
wired into CI once the migration is far enough along.

    scripts/check_platform_manifest.py            # report, always exit 0
    scripts/check_platform_manifest.py --strict   # exit 1 on any finding
    scripts/check_platform_manifest.py --quiet    # findings only, no summary
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import platform_manifest as pm  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = REPO_ROOT

REQUIRED_TOP = ["display_name", "panel", "storage", "service", "klipper", "moonraker", "package"]
REQUIRED_PACKAGE = ["asset_name", "docker_target", "in_release_matrix", "detectable"]
REQUIRED_SERVICE = ["init_system", "needs_procd_shim"]


class Findings:
    def __init__(self):
        self.items = []

    def add(self, check, message):
        self.items.append((check, message))

    def __len__(self):
        return len(self.items)


def read(path):
    full = os.path.join(ROOT, path)
    if not os.path.exists(full):
        return None
    with open(full, errors="replace") as f:
        return f.read()


def check_targets_have_entries(manifest, f):
    """Every package-*/release-* target names a platform the manifest describes."""
    cross = read("mk/cross.mk")
    if cross is None:
        f.add("targets", "mk/cross.mk not found")
        return

    targets = set()
    for m in re.finditer(r"^(?:package|release)-([a-z0-9][a-z0-9-]*)\s*:", cross, re.M):
        name = m.group(1)
        if name in ("all", "clean"):
            continue
        targets.add(name)

    known = set(manifest["platforms"])
    for t in sorted(targets - known):
        f.add("targets", f"mk/cross.mk has package/release target for {t!r} with no manifest entry")
    for p in sorted(known - targets):
        entry = manifest["platforms"][p]
        # A platform with no package target of its own is fine when it says so.
        if entry.get("package", {}).get("in_release_matrix", True):
            f.add("targets", f"manifest platform {p!r} claims a release but has no package target")


def check_required_fields(manifest, f):
    for pid, entry in sorted(manifest["platforms"].items()):
        for key in REQUIRED_TOP:
            if key not in entry:
                f.add("fields", f"{pid}: missing required section {key!r}")
        panel = entry.get("panel", {})
        if not panel.get("variable", False):
            for key in ("width", "height"):
                if key not in panel and panel.get("verified", False):
                    f.add("fields", f"{pid}: panel claims verified but has no {key}")
        if "source" not in panel:
            f.add("fields", f"{pid}: panel has no source, so its provenance is unrecorded")
        for key in REQUIRED_PACKAGE:
            if key not in entry.get("package", {}):
                f.add("fields", f"{pid}: package section missing {key!r}")
        for key in REQUIRED_SERVICE:
            if key not in entry.get("service", {}):
                f.add("fields", f"{pid}: service section missing {key!r}")


def check_unverified_panels(manifest, f):
    for pid, entry in sorted(manifest["platforms"].items()):
        panel = entry.get("panel", {})
        if panel.get("variable", False):
            continue
        if not panel.get("verified", False):
            f.add("provenance", f"{pid}: panel geometry is not hardware-verified")


def check_package_prereqs(manifest, f):
    """Each package-<p> must depend on gen-splash-3d-<p>, not another platform's."""
    cross = read("mk/cross.mk")
    if cross is None:
        return
    for m in re.finditer(r"^package-([a-z0-9][a-z0-9-]*)\s*:(.*)$", cross, re.M):
        pid, prereqs = m.group(1), m.group(2)
        if pid in ("all", "clean"):
            continue
        found = re.findall(r"gen-splash-3d(?:-([a-z0-9-]+))?", prereqs)
        if not found:
            f.add("prereqs", f"package-{pid} has no gen-splash-3d prerequisite")
            continue
        for suffix in found:
            if suffix != pid:
                named = f"gen-splash-3d-{suffix}" if suffix else "gen-splash-3d"
                f.add(
                    "prereqs",
                    f"package-{pid} depends on {named}, which generates another platform's classes",
                )


def check_no_hardcoded_sizes(manifest, f):
    """The per-platform splash targets must not restate a size class."""
    images = read("mk/images.mk")
    if images is None:
        return
    classes = set(manifest["size_classes"]["splash_composite_height"])
    for m in re.finditer(r"--sizes\s+([a-z_ ]+)", images):
        named = [c for c in m.group(1).split() if c in classes]
        if named:
            f.add(
                "literals",
                f"mk/images.mk hardcodes --sizes {' '.join(named)}; derive it from the manifest",
            )


def check_install_roots(manifest, f):
    """The hand-kept install-root lists must cover every root the manifest names."""
    roots = set()
    for pid, entry in manifest["platforms"].items():
        storage = entry.get("storage", {})
        for key in ("root", "root_fallback"):
            value = storage.get(key)
            if value and value.startswith("/"):
                roots.add(value)
        for value in storage.get("root_by_firmware", {}).values():
            for token in re.findall(r"(/[\w./-]*helixscreen)", value or ""):
                roots.add(token)

    consumers = {
        "scripts/lib/installer/common.sh": "HELIX_INSTALL_DIRS",
        "scripts/lib/installer/platform.sh": "_HELIX_KNOWN_INSTALL_DIRS",
        "src/system/log_collector.cpp": "log roots",
        "src/system/update_checker.cpp": "find_local_installer",
        "src/system/debug_bundle_collector.cpp": "crash.txt config dirs",
    }
    for path, label in consumers.items():
        text = read(path)
        if text is None:
            f.add("install-roots", f"{path} not found")
            continue
        for root in sorted(roots):
            if root not in text:
                f.add("install-roots", f"{path} ({label}) does not list {root}")


def check_renders_exist(manifest, f, build_dir):
    """Derived classes must have a render under build/, when a build exists."""
    prerendered = os.path.join(ROOT, build_dir, "assets", "images", "prerendered")
    if not os.path.isdir(prerendered):
        return  # No build yet: a fresh clone must not go red.
    present = set(os.listdir(prerendered))
    for pid in sorted(manifest["platforms"]):
        for cls in pm.splash_3d_classes_for(manifest, pid):
            for mode in ("light", "dark"):
                name = f"splash-3d-{mode}-{cls}.bin"
                if name not in present:
                    f.add("renders", f"{pid} selects {cls} but {build_dir}/.../{name} is missing")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--strict", action="store_true", help="exit 1 when anything is reported")
    ap.add_argument("--quiet", action="store_true", help="print findings only")
    ap.add_argument("--build-dir", default="build", help="where to look for renders")
    ap.add_argument(
        "--root",
        default=REPO_ROOT,
        help="tree to inspect; lets the meta-tests exercise findings against a copy "
        "instead of editing the working tree",
    )
    args = ap.parse_args()

    global ROOT
    ROOT = args.root

    try:
        manifest = pm.load(os.path.join(ROOT, "assets", "config", "platforms.json"))
    except FileNotFoundError:
        print("check_platform_manifest: assets/config/platforms.json not found", file=sys.stderr)
        return 1 if args.strict else 0

    f = Findings()
    check_targets_have_entries(manifest, f)
    check_required_fields(manifest, f)
    check_unverified_panels(manifest, f)
    check_package_prereqs(manifest, f)
    check_no_hardcoded_sizes(manifest, f)
    check_install_roots(manifest, f)
    check_renders_exist(manifest, f, args.build_dir)

    if not args.quiet:
        print("Platform manifest check (%s)" % ("strict" if args.strict else "advisory"))
        print("  %d platforms, %d findings" % (len(manifest["platforms"]), len(f)))
        print()

    current = None
    for check, message in f.items:
        if check != current:
            print(f"[{check}]")
            current = check
        print(f"  {message}")

    if not args.quiet:
        print()
        if len(f) == 0:
            print("No findings.")
        elif args.strict:
            print("%d finding(s); --strict, so this is a failure." % len(f))
        else:
            print("%d finding(s); advisory, so this does not fail the build." % len(f))

    return 1 if (args.strict and len(f)) else 0


if __name__ == "__main__":
    sys.exit(main())
