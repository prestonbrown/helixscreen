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
        for key in ("root", "root_fallback", "previous_root"):
            value = storage.get(key)
            if value and value.startswith("/"):
                roots.add(value)
        for value in storage.get("root_by_firmware", {}).values():
            for token in re.findall(r"(/[\w./-]*helixscreen)", value or ""):
                roots.add(token)

    # The C++ side reads one shared list, so a consumer satisfies this by using
    # that header rather than by spelling every root itself. The header is then
    # the only place the roots appear, and it is checked like any other consumer.
    shared_header = "include/helix_install_roots.h"
    consumers = {
        "scripts/lib/installer/common.sh": "HELIX_INSTALL_DIRS",
        "scripts/lib/installer/platform.sh": "_HELIX_KNOWN_INSTALL_DIRS",
        shared_header: "kInstallRoots",
        "src/system/log_collector.cpp": "log roots",
        "src/system/update_checker.cpp": "find_local_installer",
        "src/system/debug_bundle_collector.cpp": "crash.txt config dirs",
    }
    for path, label in consumers.items():
        text = read(path)
        if text is None:
            f.add("install-roots", f"{path} not found")
            continue
        if path != shared_header and "helix_install_roots.h" in text:
            continue
        if path == shared_header:
            # Scope the match to kInstallRoots. kStateRoots sits in the same file
            # and a state root is often a payload root plus a suffix, so a
            # whole-file search reports a list as complete when it is not.
            text = _array_block(text, "kInstallRoots")
        for root in sorted(roots):
            if root not in text:
                f.add("install-roots", f"{path} ({label}) does not list {root}")


def _array_block(text, name):
    """The body of a C++ array initialiser, or "" when it is not there."""
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    return m.group(1) if m else ""


def check_state_roots(manifest, f):
    """Every state root must be swept on uninstall and known to the app.

    State outlives the payload by design, so nothing else will remove it: a root
    missing from the sweep is a tree left on the device forever, and one missing
    from kStateRoots is logs a debug bundle cannot find.
    """
    declared = set()
    for entry in manifest["platforms"].values():
        storage = entry.get("storage", {})
        for key in ("state_root", "previous_state_root"):
            value = storage.get(key)
            if value and value.startswith("/"):
                declared.add(value)

    text = read("scripts/lib/installer/common.sh")
    if text is None:
        f.add("state-roots", "scripts/lib/installer/common.sh not found")
    else:
        m = re.search(r'^HELIX_STATE_DIRS="([^"]*)"', text, re.M)
        if not m:
            f.add("state-roots", "HELIX_STATE_DIRS is not a plain assignment any more")
        else:
            listed = set(m.group(1).split())
            for root in sorted(declared - listed):
                f.add("state-roots", f"HELIX_STATE_DIRS does not sweep {root}")

    header = read("include/helix_install_roots.h")
    if header is None:
        f.add("state-roots", "include/helix_install_roots.h not found")
    else:
        block = _array_block(header, "kStateRoots")
        for root in sorted(declared):
            if root not in block:
                f.add("state-roots", f"kStateRoots does not list {root}")


def check_installer_branch_roots(manifest, f):
    """The installer's per-platform literals must be the manifest's values.

    The installer runs where python is only probed for, never assumed, so it
    cannot read the manifest and keeps literals instead. That makes the manifest
    decoration unless something holds the two together, and a root that drifts
    is an install landing somewhere no uninstall sweeps.
    """
    text = read("scripts/lib/installer/platform.sh")
    if text is None:
        f.add("installer-roots", "scripts/lib/installer/platform.sh not found")
        return

    # Each branch runs from its own `[ "$platform" = "<id>" ]` test to the next.
    marks = [
        (m.group(1), m.end())
        for m in re.finditer(r'\[ "\$platform" = "([\w-]+)" \]; then', text)
    ]
    bounds = {}
    for i, (pid, start) in enumerate(marks):
        end = marks[i + 1][1] if i + 1 < len(marks) else len(text)
        bounds.setdefault(pid, text[start:end])

    fields = (
        ("root", "INSTALL_DIR"),
        ("previous_root", "PREVIOUS_INSTALL_DIR"),
        ("previous_state_root", "PREVIOUS_STATE_DIR"),
    )
    for pid, entry in sorted(manifest["platforms"].items()):
        block = bounds.get(pid)
        if block is None:
            continue  # no branch of its own; a build-only or derived platform
        storage = entry.get("storage", {})
        for key, var in fields:
            want = storage.get(key)
            found = re.findall(r'^\s*%s="([^"]*)"' % var, block, re.M)
            if not want:
                if found and any(v for v in found):
                    f.add(
                        "installer-roots",
                        f"{pid}: platform.sh sets {var}={found[0]} but the "
                        f"manifest declares no storage.{key}",
                    )
                continue
            if len(found) != 1:
                # Zero means the branch derives it; more than one means a case
                # statement picks between them. Either way there is no single
                # literal to hold to the manifest.
                continue
            if found[0] != want:
                f.add(
                    "installer-roots",
                    f"{pid}: platform.sh sets {var}={found[0]}, manifest "
                    f"storage.{key} is {want}",
                )


def check_printer_image_sizes(manifest, f):
    """A printer-image size must be derived, never written down.

    Packaging keeps exactly one prerendered tier per platform, the one the
    panel's width selects, and deletes the other. So a literal size names a file
    that is present on some devices and absent on the rest, and the absence is
    invisible in a checkout: the repo tree has the source PNGs that a miss falls
    back to, and a package has none.

    `src/system/prerender_size_class.cpp` holds the rule itself and is the one
    place the numbers belong.
    """
    sizes = {str(r["size"]) for r in manifest["size_classes"]["printer_image"]}
    rule_owner = os.path.join("src", "system", "prerender_size_class.cpp")

    for base in ("src", "include"):
        root = os.path.join(ROOT, base)
        if not os.path.isdir(root):
            continue
        for dirpath, _dirs, files in os.walk(root):
            for name in files:
                if not name.endswith((".cpp", ".h")):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, ROOT)
                if rel == rule_owner:
                    continue
                try:
                    with open(full, encoding="utf-8") as fh:
                        text = fh.read()
                except OSError:
                    continue

                # A literal where a display width belongs. This is the shape that
                # hides best: 480 is not a size, it silently means the small tier.
                for m in re.finditer(r"get_prerendered_printer_path\([^,()]+,\s*(\d+)\s*\)", text):
                    f.add(
                        "printer-image-sizes",
                        f"{rel} passes the literal width {m.group(1)} to "
                        f"get_prerendered_printer_path; ask the display instead",
                    )

                # A tier size spelled into a SHIPPED filename. The custom-image
                # cache also names both sizes and is deliberately excluded: it is
                # written on the device, holds every size it needs, and nothing
                # prunes it.
                lines = text.splitlines()
                for i, raw in enumerate(lines):
                    # Doc comments spell out example filenames, which is the
                    # clearest way to describe these functions and not a size
                    # anything reads.
                    stripped = raw.strip()
                    if stripped.startswith(("//", "*", "/*")):
                        continue
                    line = raw.split("//", 1)[0]
                    near = "\n".join(lines[max(0, i - 2):i + 3])
                    if "assets/images/printers" not in near and "PRERENDERED_BASE_PATH" not in near:
                        continue
                    for m in re.finditer(r"(?<![\w.])(\d+)(?![\w.])", line):
                        value = m.group(1)
                        if value in sizes:
                            f.add(
                                "printer-image-sizes",
                                f"{rel}:{i + 1} builds a shipped prerendered name with "
                                f"the literal size {value}; derive it from the display width",
                            )


def check_shell_roots(manifest, f):
    """The shell sweep list must name every root the manifest declares.

    The C++ side reads one shared header; the installer cannot, because it runs
    on devices where python is only probed for, never assumed. So the shell keeps
    a literal list and this is what holds it to the manifest.
    """
    text = read("scripts/lib/installer/common.sh")
    if text is None:
        f.add("shell-roots", "scripts/lib/installer/common.sh not found")
        return
    m = re.search(r'^HELIX_INSTALL_DIRS="([^"]*)"', text, re.M)
    if not m:
        f.add("shell-roots", "HELIX_INSTALL_DIRS is not a plain assignment any more")
        return
    listed = set(m.group(1).split())

    declared = set()
    for entry in manifest["platforms"].values():
        storage = entry.get("storage", {})
        for key in ("root", "previous_root"):
            value = storage.get(key)
            if value and value.startswith("/"):
                declared.add(value)
        for cond in storage.get("root_by_firmware", {}).values():
            for token in re.findall(r"(/[\w./-]*helixscreen)", cond or ""):
                declared.add(token)

    for root in sorted(declared - listed):
        f.add("shell-roots", f"HELIX_INSTALL_DIRS does not sweep {root}")

    # The C++ header may legitimately know more (a shape no installer arm
    # produces), but the asymmetry is worth naming rather than leaving implicit:
    # a root the app reads logs from but the uninstaller never removes is a tree
    # left behind on every uninstall.
    header = read("include/helix_install_roots.h")
    if header:
        # kInstallRoots only. kHomeInstallRoots are $KLIPPER_HOME fallbacks for a
        # Pi-class box, not fixed platform roots, and an uninstall sweeping a
        # user's home directory would be a different and worse bug.
        block = re.search(r"kInstallRoots\[\]\s*=\s*\{(.*?)\};", header, re.S)
        cxx = set(re.findall(r'"(/[\w./-]*helixscreen)"', block.group(1) if block else ""))
        for root in sorted(cxx - listed):
            f.add(
                "shell-roots",
                f"{root} is searched by the app as a payload root, but no "
                f"uninstall sweeps it",
            )


def _under(path, root):
    """True when `path` is `root` or lives inside it."""
    if not path or not root or not path.startswith("/") or not root.startswith("/"):
        return False
    root = root.rstrip("/")
    return path == root or path.startswith(root + "/")


def check_state_outside_payload(manifest, f):
    """Cache and logs must not live inside the payload.

    The payload is what an update replaces, and not only by our own hand: a
    Moonraker `type: web` entry does shutil.rmtree(path) before extracting, and
    that path is the install root. Anything under it is deleted on every update,
    so logs disappear exactly when someone needs them and the thumbnail cache is
    rebuilt from nothing.
    """
    for pid, entry in sorted(manifest["platforms"].items()):
        storage = entry.get("storage", {})
        root = storage.get("root", "")
        if not root.startswith("/"):
            continue  # discovered at runtime; the XDG cascade keeps state elsewhere

        for field in ("cache_dir", "log_file"):
            value = storage.get(field)
            if not value:
                continue
            # A log FILE sits in a directory; judge the directory.
            probe = value if field == "cache_dir" else os.path.dirname(value)
            if _under(probe, root):
                f.add(
                    "state-in-payload",
                    f"{pid}: {field} {value} is inside the payload root {root}, "
                    f"which every update deletes",
                )


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
    check_printer_image_sizes(manifest, f)
    check_install_roots(manifest, f)
    check_shell_roots(manifest, f)
    check_installer_branch_roots(manifest, f)
    check_state_outside_payload(manifest, f)
    check_state_roots(manifest, f)
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
