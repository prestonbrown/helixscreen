#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read assets/config/platforms.json and answer derived questions about a platform.

Build-time counterpart to src/system/prerender_size_class.cpp. Both evaluate the
same ladder, which lives as data in the manifest's "size_classes" section, and
tests/unit/test_prerender_size_class.cpp asserts the compiled selector agrees
with it so the two cannot drift apart.

Used as a library by scripts/check_platform_manifest.py and as a CLI by
mk/images.mk, which asks it which splash classes a platform needs:

    scripts/platform_manifest.py splash-3d-sizes k2   -> medium
    scripts/platform_manifest.py splash-3d-sizes k1   -> medium small
    scripts/platform_manifest.py splash-3d-sizes pi   -> (empty: every class)
"""

import json
import os
import sys

MANIFEST_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "assets",
    "config",
    "platforms.json",
)


class ManifestError(Exception):
    pass


def load(path=MANIFEST_PATH):
    with open(path) as f:
        return json.load(f)


def platform(manifest, platform_id):
    try:
        return manifest["platforms"][platform_id]
    except KeyError:
        known = ", ".join(sorted(manifest["platforms"]))
        raise ManifestError(f"unknown platform {platform_id!r}; known: {known}")


def has_fixed_panel(entry):
    """A platform whose panel geometry is known at package time."""
    panel = entry.get("panel", {})
    return not panel.get("variable", False) and "width" in panel and "height" in panel


def effective_resolution(entry):
    """Panel geometry after software rotation, as the app will see it.

    K1 and K2 ship portrait panels used rotated, so the panel size and the
    resolution that selects an asset are not the same number.
    """
    panel = entry["panel"]
    w, h = panel["width"], panel["height"]
    if panel.get("rotate", 0) in (90, 270):
        return h, w
    return w, h


def _match(rule, width, height):
    narrow = min(width, height) if height else width
    if "min_width" in rule and width < rule["min_width"]:
        return False
    if "max_width" in rule and width > rule["max_width"]:
        return False
    if "min_height" in rule and height < rule["min_height"]:
        return False
    if "max_height" in rule and height > rule["max_height"]:
        return False
    if "min_narrow" in rule and narrow < rule["min_narrow"]:
        return False
    if "max_narrow" in rule and narrow > rule["max_narrow"]:
        return False
    return True


def _first_match(rules, width, height, key):
    for rule in rules:
        if _match(rule, width, height):
            return rule[key]
    raise ManifestError(f"no rule matched {width}x{height}; the ladder needs a catch-all")


def splash_class(manifest, width, height):
    """The size class a resolution selects. One ladder for 2D and 3D alike."""
    return _first_match(manifest["size_classes"]["splash"], width, height, "class")


def has_2d_logo(manifest, class_name):
    """Whether that class has a 2D logo render, or falls back to scaling the PNG."""
    return class_name in manifest["size_classes"]["splash_2d_logo_classes"]


def printer_image_size(manifest, width):
    return _first_match(manifest["size_classes"]["printer_image"], width, 0, "size")


def splash_3d_classes_for(manifest, platform_id):
    """Every 3D splash class a platform's package must contain.

    An empty list means "no restriction, generate all of them" - the right answer
    for a platform whose panel is not known until runtime.
    """
    entry = platform(manifest, platform_id)
    if not has_fixed_panel(entry):
        return []
    width, height = effective_resolution(entry)
    classes = [splash_class(manifest, width, height)]
    for extra in entry.get("package", {}).get("extra_splash_3d_classes", []):
        if extra not in classes:
            classes.append(extra)
    return classes


def composite_height(manifest, class_name):
    return manifest["size_classes"]["splash_composite_height"].get(class_name, 0)


def _cmd_splash_3d_sizes(manifest, args):
    print(" ".join(splash_3d_classes_for(manifest, args[0])))


def _cmd_splash_2d_size(manifest, args):
    entry = platform(manifest, args[0])
    if not has_fixed_panel(entry):
        print("")
        return
    cls = splash_class(manifest, *effective_resolution(entry))
    print(cls if has_2d_logo(manifest, cls) else "")


def _cmd_printer_image_size(manifest, args):
    entry = platform(manifest, args[0])
    if not has_fixed_panel(entry):
        print("")
        return
    print(printer_image_size(manifest, effective_resolution(entry)[0]))


def _cmd_effective_resolution(manifest, args):
    entry = platform(manifest, args[0])
    if not has_fixed_panel(entry):
        print("")
        return
    print("%dx%d" % effective_resolution(entry))


def _cmd_list(manifest, args):
    for pid in sorted(manifest["platforms"]):
        print(pid)


def _cmd_get(manifest, args):
    """Dotted lookup into one platform's entry: `get k2 storage.root`."""
    node = platform(manifest, args[0])
    for part in args[1].split("."):
        if not isinstance(node, dict) or part not in node:
            print("")
            return
        node = node[part]
    if isinstance(node, list):
        print(" ".join(str(x) for x in node))
    elif isinstance(node, bool):
        print("true" if node else "false")
    elif node is None:
        print("")
    else:
        print(node)


COMMANDS = {
    "splash-3d-sizes": (_cmd_splash_3d_sizes, 1),
    "splash-2d-size": (_cmd_splash_2d_size, 1),
    "printer-image-size": (_cmd_printer_image_size, 1),
    "effective-resolution": (_cmd_effective_resolution, 1),
    "list": (_cmd_list, 0),
    "get": (_cmd_get, 2),
}


def main(argv):
    if len(argv) < 2 or argv[1] not in COMMANDS:
        print(__doc__, file=sys.stderr)
        print("commands: " + ", ".join(sorted(COMMANDS)), file=sys.stderr)
        return 2
    handler, argc = COMMANDS[argv[1]]
    args = argv[2:]
    if len(args) != argc:
        print(f"{argv[1]} takes {argc} argument(s), got {len(args)}", file=sys.stderr)
        return 2
    try:
        handler(load(), args)
    except ManifestError as e:
        print(f"platform_manifest: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
