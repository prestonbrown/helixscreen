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
    scripts/platform_manifest.py splash-3d-sizes mips -> medium small (its candidate boards)
    scripts/platform_manifest.py splash-3d-sizes pi   -> (empty: every class)
"""

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from translation_packs import UNLOADED_TRANSLATION_FILES  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "translations"))
import cjk_filename_fonts  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

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


def font_breakpoint(manifest, width, height):
    """The breakpoint tier the panel's fonts come from: the splash ladder's
    cramped-axis rules, without the rules that pick a canvas by shape."""
    rules = [r for r in manifest["size_classes"]["splash"] if set(r) <= {"class", "max_narrow"}]
    return _first_match(rules, width, height, "class")


def filename_font_sizes_for(manifest, platform_id):
    """Pixel sizes of the CJK filename glyphs this package's panels can draw,
    or None when the panel is unbounded and every size ships."""
    panels = candidate_panels(manifest, platform_id)
    if not panels:
        return None
    by_tier = cjk_filename_fonts.sizes_by_tier(Path(REPO_ROOT))
    sizes = set()
    for entry in panels:
        sizes.update(by_tier[font_breakpoint(manifest, *effective_resolution(entry))])
    return sorted(sizes)


def _prune_cjk_filename_fonts(manifest, platform_id, root):
    """(doomed paths, notes) for the CJK filename bins. Raises when a size the
    panel draws has no bin staged, so a prune can never ship without one."""
    cjk_dir = os.path.join(root, "assets", "fonts", "cjk")
    if not os.path.isdir(cjk_dir):
        return [], ["no CJK fonts staged"]
    bin_dir = os.path.join(root, str(cjk_filename_fonts.FILENAME_BIN_DIR))
    wanted = filename_font_sizes_for(manifest, platform_id)
    if wanted is None:
        wanted = cjk_filename_fonts.all_sizes(Path(REPO_ROOT))
    keep = {cjk_filename_fonts.bin_name(px) for px in wanted}
    staged = set(os.listdir(bin_dir)) if os.path.isdir(bin_dir) else set()
    missing = sorted(keep - staged)
    if missing:
        raise ManifestError(f"{platform_id}: CJK filename font(s) its panel draws are not "
                            f"staged: {', '.join(missing)}")
    doomed = [os.path.join(bin_dir, n) for n in sorted(staged)
              if n.endswith(".bin") and n not in keep]
    return doomed, [f"keeping CJK filename glyphs at {'/'.join(map(str, wanted))}px"]


def splash_portrait_class(manifest, width, height):
    """The largest portrait canvas that fits a portrait panel, or None.

    The ladder above picks a landscape canvas from the narrow axis, which runs
    past the sides of a panel taller than it is wide.
    """
    if height <= width:
        return None
    classes = manifest["size_classes"]
    for name in classes["splash_portrait"]:
        if (classes["splash_composite_width"][name] <= width
                and classes["splash_composite_height"][name] <= height):
            return name
    return None


def splash_3d_class(manifest, width, height):
    """The full-screen canvas class a resolution takes: portrait when one fits."""
    return splash_portrait_class(manifest, width, height) or splash_class(manifest, width, height)


def has_2d_logo(manifest, class_name):
    """Whether that class has a 2D logo render, or falls back to scaling the PNG."""
    return class_name in manifest["size_classes"]["splash_2d_logo_classes"]


def printer_image_size(manifest, width):
    return _first_match(manifest["size_classes"]["printer_image"], width, 0, "size")


def candidate_panels(manifest, platform_id):
    """The panels this package can land on, as manifest-shaped entries.

    A fixed panel is its own only candidate. A variable panel that names
    `candidates` is one binary serving several known boards, so its package
    needs exactly what those boards need. A candidate is either another
    platform's id, which brings that platform's package hedges with it, or an
    inline panel for a board with no platform entry of its own. An empty list
    means the panel is unbounded: user-supplied hardware of any size.
    """
    entry = platform(manifest, platform_id)
    if has_fixed_panel(entry):
        return [entry]
    out = []
    for cand in entry.get("panel", {}).get("candidates", []):
        if isinstance(cand, str):
            resolved = platform(manifest, cand)
            name = cand
        else:
            resolved = {"panel": cand}
            name = cand.get("board", "inline panel")
        if not has_fixed_panel(resolved):
            raise ManifestError(f"{platform_id}: candidate {name!r} has no fixed panel")
        out.append(resolved)
    return out


def _union(lists):
    out = []
    for items in lists:
        for item in items:
            if item not in out:
                out.append(item)
    return out


def splash_3d_classes_for(manifest, platform_id):
    """Every 3D splash class a platform's package must contain.

    An empty list means "no restriction, generate all of them" - the right answer
    for a platform whose panel is not known until runtime.
    """
    per_panel = []
    for entry in candidate_panels(manifest, platform_id):
        classes = [splash_3d_class(manifest, *effective_resolution(entry))]
        per_panel.append(classes + entry.get("package", {}).get("extra_splash_3d_classes", []))
    return _union(per_panel)


def printer_image_sizes_for(manifest, platform_id):
    """Every prerendered printer tier a platform's package must contain.

    Empty for an unbounded panel, which keeps every tier.
    """
    per_panel = []
    for entry in candidate_panels(manifest, platform_id):
        size = printer_image_size(manifest, effective_resolution(entry)[0])
        per_panel.append([size] + entry.get("package", {}).get("extra_printer_image_sizes", []))
    return sorted(_union(per_panel))


def _kb(paths):
    return sum(os.path.getsize(x) for x in paths) // 1024


def _remove_all(paths, root):
    """Delete the files, then any directory they emptied."""
    for path in paths:
        os.remove(path)
    for path in sorted({os.path.dirname(p) for p in paths}, key=len, reverse=True):
        while os.path.isdir(path) and not os.listdir(path) and os.path.abspath(path) != os.path.abspath(root):
            os.rmdir(path)
            path = os.path.dirname(path)


def prune_assets(manifest, platform_id, root, dry_run=False):  # noqa: C901
    """Remove staged assets the platform's panel can never ask for.

    Generating fewer classes is not enough on its own: a release copies whatever
    build/ happens to hold, so a tree left over from another platform's build
    ships with it. This is the step that actually bounds the payload.

    Returns (removed_paths, notes). A platform whose panel is unbounded keeps
    every size class and its source PNGs.
    """
    entry = platform(manifest, platform_id)
    notes = []
    doomed = []

    # Tracker music is playable only where the tracker player is compiled in.
    # This is asked of the PLATFORM, not of the host running the release, which
    # is the distinction the Makefile-flag version of this check could not make.
    if not entry.get("sound", {}).get("has_tracker", True):
        sounds = os.path.join(root, "assets", "sounds")
        if os.path.isdir(sounds):
            for dirpath, _dirnames, filenames in os.walk(sounds):
                doomed.extend(os.path.join(dirpath, n) for n in filenames)
            notes.append("dropped assets/sounds: no tracker player on this platform")

    # Translation files no locale loads (translation_packs.py).
    translations = os.path.join(root, "ui_xml", "translations")
    for name in UNLOADED_TRANSLATION_FILES:
        path = os.path.join(translations, name)
        if os.path.isfile(path):
            doomed.append(path)
            notes.append(f"dropped ui_xml/translations/{name}: no locale loads it")

    font_doomed, font_notes = _prune_cjk_filename_fonts(manifest, platform_id, root)
    doomed.extend(font_doomed)
    notes.extend(font_notes)

    keep_classes = splash_3d_classes_for(manifest, platform_id)
    keep_sizes = printer_image_sizes_for(manifest, platform_id)

    if not keep_sizes:
        # generate_cached_printer_image() reads the source PNG whenever the home
        # widget is larger than the biggest tier, which an unbounded panel can be.
        freed = _kb(doomed)
        if not dry_run:
            _remove_all(doomed, root)
        notes.append("panel is not known until runtime; keeping every size class and the "
                     "source PNGs the image cache reads above the largest render")
        notes.append(f"removed {len(doomed)} file(s), {freed} KB")
        return doomed, notes

    # Splash canvases and logos for classes this panel cannot select.
    splash_dir = os.path.join(root, "assets", "images", "prerendered")
    for name in sorted(os.listdir(splash_dir)) if os.path.isdir(splash_dir) else []:
        cls = None
        if name.startswith("splash-3d-") and name.endswith(".bin"):
            # splash-3d-<mode>-<class>.bin
            cls = name[len("splash-3d-"):-len(".bin")].split("-", 1)[-1]
        elif name.startswith("splash-logo-") and name.endswith(".bin"):
            cls = name[len("splash-logo-"):-len(".bin")]
        if cls is not None and cls not in keep_classes:
            doomed.append(os.path.join(splash_dir, name))

    # Printer art at sizes no candidate panel selects.
    printer_dir = os.path.join(root, "assets", "images", "printers")
    prerendered = os.path.join(printer_dir, "prerendered")
    have_bin = {size: set() for size in keep_sizes}
    for name in sorted(os.listdir(prerendered)) if os.path.isdir(prerendered) else []:
        if not name.endswith(".bin"):
            continue
        stem, _, size = name[:-len(".bin")].rpartition("-")
        if not size.isdigit():
            continue
        if int(size) in have_bin:
            have_bin[int(size)].add(stem)
        else:
            doomed.append(os.path.join(prerendered, name))

    # Source PNGs are a fallback for a printer with no render. Dropping them is
    # only safe where every one of them has a render at every size being kept;
    # otherwise that printer would silently degrade to the generic image.
    sizes_text = "/".join(str(s) for s in keep_sizes) + "px"
    pngs = []
    if os.path.isdir(printer_dir):
        pngs = [n for n in sorted(os.listdir(printer_dir)) if n.endswith(".png")]
    uncovered = [n for n in pngs
                 if any(n[:-len(".png")] not in stems for stems in have_bin.values())]
    if pngs and not uncovered:
        doomed.extend(os.path.join(printer_dir, n) for n in pngs)
        notes.append(f"dropped {len(pngs)} source PNG(s); every one has a {sizes_text} render")
    elif uncovered:
        notes.append(
            f"kept source PNGs: {len(uncovered)} printer(s) lack a {sizes_text} render "
            f"({', '.join(uncovered[:3])}{'...' if len(uncovered) > 3 else ''})")

    notes.insert(0, f"keeping splash {sorted(keep_classes)} and {sizes_text} printer art")
    freed = _kb(doomed)
    if not dry_run:
        _remove_all(doomed, root)
    notes.append(f"removed {len(doomed)} file(s), {freed} KB")
    return doomed, notes


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


def _cmd_prune_assets(manifest, args):
    platform_id, root = args
    if not os.path.isdir(root):
        raise ManifestError(f"staged release root {root!r} does not exist")
    _removed, notes = prune_assets(manifest, platform_id, root)
    for note in notes:
        print(f"  {note}")


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
    "prune-assets": (_cmd_prune_assets, 2),
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
