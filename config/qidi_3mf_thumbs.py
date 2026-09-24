#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Materialise QIDI .3mf plate thumbnails under the gcodes root.

QIDI's customized Moonraker hardcodes every uploaded .3mf's thumbnail metadata
at .thumbs/<subdir>/<stem>/plate_N.png (generate_thumb_path in
moonraker/components/file_manager/metadata.py) and extracts no image itself;
the stock screen client is what wrote those files. This script replaces that
duty: it walks the gcodes root, pulls Metadata/plate_N.png out of each .3mf
(a zip written by QIDI Studio) into the path Moonraker expects, and prunes
plate files whose source .3mf is gone. Other .thumbs content (upstream-style
<name>-300x300.png gcode thumbnails) is never touched.

Usage: qidi_3mf_thumbs.py <gcodes-root>

Stdlib only; runs on Python 3.9. Corrupt archives and unwritable destinations
are logged to stderr and skipped, never fatal.
"""

import os
import re
import sys
import zipfile

# Zip members we extract, and the extracted file names we own.
PLATE_MEMBER_RE = re.compile(r"^Metadata/plate_(\d+)\.png$")
PLATE_FILE_RE = re.compile(r"^plate_\d+\.png$")


def thumb_relpath(rel_3mf, plate):
    """Qidi's generate_thumb_path(): .thumbs at the gcodes ROOT, the upload's
    subdir replicated under it, one directory per file stem, one png per plate."""
    stem = os.path.splitext(os.path.basename(rel_3mf))[0]
    sub_dir = os.path.dirname(rel_3mf)
    return os.path.normpath(os.path.join(".thumbs", sub_dir, stem, "plate_%d.png" % plate))


def extract_thumbs(root):
    wrote = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for name in filenames:
            if not name.lower().endswith(".3mf"):
                continue
            src = os.path.join(dirpath, name)
            try:
                src_mtime = os.stat(src).st_mtime
                with zipfile.ZipFile(src) as zf:
                    plates = [
                        (int(PLATE_MEMBER_RE.match(m.filename).group(1)), zf.read(m))
                        for m in zf.infolist()
                        if PLATE_MEMBER_RE.match(m.filename)
                    ]
            except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as exc:
                print("qidi_3mf_thumbs: skipping %s: %s" % (src, exc), file=sys.stderr)
                continue
            rel = os.path.relpath(src, root)
            for plate, data in plates:
                dest = os.path.join(root, thumb_relpath(rel, plate))
                try:
                    if os.path.exists(dest) and os.stat(dest).st_mtime >= src_mtime:
                        continue
                    os.makedirs(os.path.dirname(dest), exist_ok=True)
                    tmp = "%s.tmp.%d" % (dest, os.getpid())
                    with open(tmp, "wb") as fh:
                        fh.write(data)
                    os.replace(tmp, dest)
                    wrote += 1
                except OSError as exc:
                    print("qidi_3mf_thumbs: cannot write %s: %s" % (dest, exc), file=sys.stderr)
    return wrote


def _source_exists(root, thumbs_dirpath, stem):
    """True when <subdir>/<stem>.3mf still exists for this stem directory
    (extension matched case-insensitively, as the extract walk does)."""
    rel = os.path.relpath(thumbs_dirpath, os.path.join(root, ".thumbs"))
    sub_dir = os.path.dirname(rel)
    src_dir = os.path.join(root, sub_dir) if sub_dir else root
    wanted = (stem + ".3mf").lower()
    try:
        return any(f.lower() == wanted for f in os.listdir(src_dir))
    except OSError:
        return False


def prune(root):
    removed = 0
    thumbs = os.path.join(root, ".thumbs")
    if not os.path.isdir(thumbs):
        return 0
    for dirpath, _dirnames, filenames in os.walk(thumbs):
        plates = [f for f in filenames if PLATE_FILE_RE.match(f)]
        if not plates:
            continue
        stem = os.path.basename(dirpath)
        if _source_exists(root, dirpath, stem):
            continue
        for f in plates:
            try:
                os.remove(os.path.join(dirpath, f))
                removed += 1
            except OSError as exc:
                print("qidi_3mf_thumbs: cannot prune %s: %s" % (os.path.join(dirpath, f), exc),
                      file=sys.stderr)
        # Only ever drops a directory that the plate removal just emptied;
        # anything else in it (or a failed remove above) keeps it alive.
        try:
            os.rmdir(dirpath)
        except OSError:
            pass
    return removed


def main(argv):
    if len(argv) != 2:
        print("usage: qidi_3mf_thumbs.py <gcodes-root>", file=sys.stderr)
        return 2
    root = argv[1]
    if not os.path.isdir(root):
        print("qidi_3mf_thumbs: not a directory: %s" % root, file=sys.stderr)
        return 1
    wrote = extract_thumbs(root)
    removed = prune(root)
    if wrote or removed:
        print("qidi_3mf_thumbs: wrote %d, pruned %d under %s" % (wrote, removed, root))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
