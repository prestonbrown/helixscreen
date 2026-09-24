# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for config/qidi_3mf_thumbs.py.

The helper materialises the .3mf plate thumbnails QIDI's customized Moonraker
points metadata at (.thumbs/<subdir>/<stem>/plate_N.png under the gcodes root),
which nothing writes once the stock screen client is stopped
(prestonbrown/helixscreen#1713).
"""

import importlib.util
import io
import os
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "config" / "qidi_3mf_thumbs.py"


def run_helper(root):
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(root)],
        capture_output=True,
        text=True,
    )


def make_3mf(path, plates, extra_members=()):
    """Write a .3mf zip; plates maps plate number -> png bytes."""
    with zipfile.ZipFile(path, "w") as zf:
        for n, data in plates.items():
            zf.writestr("Metadata/plate_%d.png" % n, data)
        for name, data in extra_members:
            zf.writestr(name, data)
    return path


PNG1 = b"\x89PNG plate one"
PNG2 = b"\x89PNG plate two"


@pytest.fixture()
def root(tmp_path):
    gcodes = tmp_path / "gcodes"
    gcodes.mkdir()
    return gcodes


def test_extracts_plate_thumbnail_to_qidi_thumbs_path(root):
    make_3mf(root / "Foo (PLA).gcode.3mf", {1: PNG1})

    res = run_helper(root)
    assert res.returncode == 0, res.stderr

    dest = root / ".thumbs" / "Foo (PLA).gcode" / "plate_1.png"
    assert dest.read_bytes() == PNG1


def test_subdir_upload_keeps_thumbs_at_the_gcodes_root(root):
    subdir = root / "USB"
    subdir.mkdir()
    make_3mf(subdir / "Bar.3mf", {1: PNG1})

    assert run_helper(root).returncode == 0

    # Qidi's layout puts .thumbs at the ROOT, with the upload's subdir
    # replicated under it (not a per-directory .thumbs like upstream).
    dest = root / ".thumbs" / "USB" / "Bar" / "plate_1.png"
    assert dest.read_bytes() == PNG1
    assert not (subdir / ".thumbs").exists()


def test_cyrillic_name_and_multi_plate_zip(root):
    make_3mf(root / "Журнал «Тест».3mf", {1: PNG1, 2: PNG2})

    assert run_helper(root).returncode == 0

    stem = "Журнал «Тест»"
    assert (root / ".thumbs" / stem / "plate_1.png").read_bytes() == PNG1
    assert (root / ".thumbs" / stem / "plate_2.png").read_bytes() == PNG2


def test_case_insensitive_extension(root):
    make_3mf(root / "Upper.3MF", {1: PNG1})

    assert run_helper(root).returncode == 0
    assert (root / ".thumbs" / "Upper" / "plate_1.png").read_bytes() == PNG1


def test_non_plate_members_are_ignored(root):
    make_3mf(root / "Only.3mf", {}, extra_members=[("Metadata/thumbnail.png", b"x"), ("3D/3dmodel.model", b"y")])

    assert run_helper(root).returncode == 0
    assert not (root / ".thumbs").exists()


def test_dot_directories_are_not_walked(root):
    hidden = root / ".cache"
    hidden.mkdir()
    make_3mf(hidden / "Ghost.3mf", {1: PNG1})

    assert run_helper(root).returncode == 0
    assert not (root / ".thumbs" / "Ghost").exists()


def test_corrupt_zip_is_logged_and_skipped(root):
    make_3mf(root / "Good.3mf", {1: PNG1})
    (root / "Bad.3mf").write_bytes(b"this is not a zip file")

    res = run_helper(root)
    assert res.returncode == 0
    assert "Bad.3mf" in res.stderr
    assert (root / ".thumbs" / "Good" / "plate_1.png").read_bytes() == PNG1


def test_second_run_is_stat_only(root):
    src = make_3mf(root / "Again.3mf", {1: PNG1})
    assert run_helper(root).returncode == 0
    dest = root / ".thumbs" / "Again" / "plate_1.png"
    before = dest.stat().st_mtime_ns

    assert run_helper(root).returncode == 0

    assert dest.stat().st_mtime_ns == before
    leftovers = [p.name for p in dest.parent.iterdir() if p.name != "plate_1.png"]
    assert leftovers == []


def test_stale_thumbnail_is_refreshed(root):
    src = make_3mf(root / "Stale.3mf", {1: b"\x89PNG old"})
    dest = root / ".thumbs" / "Stale" / "plate_1.png"
    dest.parent.mkdir(parents=True)
    dest.write_bytes(b"\x89PNG old")
    old = src.stat().st_mtime - 500
    os.utime(dest, (old, old))

    make_3mf(src, {1: PNG1})
    assert run_helper(root).returncode == 0

    assert dest.read_bytes() == PNG1


def test_prunes_orphaned_plates_and_keeps_neighbour_thumbs(root):
    make_3mf(root / "Live.3mf", {1: PNG1})
    orphan = root / ".thumbs" / "Gone"
    orphan.mkdir(parents=True)
    (orphan / "plate_1.png").write_bytes(PNG1)
    (orphan / "plate_7.png").write_bytes(PNG1)
    # Upstream-style gcode thumbnail in .thumbs proper: must survive.
    (root / ".thumbs" / "Live-300x300.png").write_bytes(PNG1)

    assert run_helper(root).returncode == 0

    assert (root / ".thumbs" / "Live" / "plate_1.png").exists()
    assert not orphan.exists()
    assert (root / ".thumbs" / "Live-300x300.png").exists()


def test_orphan_stem_dir_with_other_files_is_emptied_of_plates_only(root):
    orphan = root / ".thumbs" / "Keeper"
    orphan.mkdir(parents=True)
    (orphan / "plate_2.png").write_bytes(PNG1)
    (orphan / "notes.txt").write_bytes(b"not ours")

    assert run_helper(root).returncode == 0

    assert not (orphan / "plate_2.png").exists()
    assert (orphan / "notes.txt").exists()
    assert orphan.is_dir()


def test_orphan_check_matches_extension_case_insensitively(root):
    src = root / "Mixed.3mF"
    make_3mf(src, {1: PNG1})
    # Pre-seed the thumb with an older mtime so the extract refreshes it.
    dest = root / ".thumbs" / "Mixed" / "plate_1.png"
    dest.parent.mkdir(parents=True)
    dest.write_bytes(b"old")
    old = src.stat().st_mtime - 500
    os.utime(dest, (old, old))

    assert run_helper(root).returncode == 0
    # The source exists (as .3mF), so the plate must survive the prune.
    assert dest.read_bytes() == PNG1


def test_missing_root_exits_nonzero(tmp_path):
    res = run_helper(tmp_path / "nowhere")
    assert res.returncode != 0
    assert res.stderr.strip()


def make_corrupt_member_3mf(path):
    """A zip whose central directory is intact but whose plate member's
    deflate stream has a flipped byte: it opens fine, and only reading the
    member raises."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("Metadata/plate_1.png", bytes(range(256)) * 16)
    raw = bytearray(buf.getvalue())
    header = raw.index(b"PK\x03\x04")
    name_len = raw[header + 26] | (raw[header + 27] << 8)
    extra_len = raw[header + 28] | (raw[header + 29] << 8)
    data_start = header + 30 + name_len + extra_len
    # Byte 0 is the deflate header (BFINAL/BTYPE); corrupting it raises
    # zlib.error from read(), where a mid-stream flip only fails the CRC.
    raw[data_start] ^= 0xFF
    path.write_bytes(bytes(raw))
    return path


def load_helper_module():
    spec = importlib.util.spec_from_file_location("qidi_3mf_thumbs", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_corrupt_deflate_member_does_not_stop_the_run(root):
    make_corrupt_member_3mf(root / "Broken.3mf")
    make_3mf(root / "Fine.3mf", {1: PNG1})

    res = run_helper(root)
    assert res.returncode == 0
    assert "Broken.3mf" in res.stderr
    assert not (root / ".thumbs" / "Broken").exists()
    assert (root / ".thumbs" / "Fine" / "plate_1.png").read_bytes() == PNG1


def test_current_destination_is_not_decompressed(root):
    # A current destination plus a member that cannot be read: silence (no
    # skip log, no rewrite) is the proof the member was never decompressed,
    # because reading it would raise and log.
    make_corrupt_member_3mf(root / "Fresh.3mf")
    dest = root / ".thumbs" / "Fresh" / "plate_1.png"
    dest.parent.mkdir(parents=True)
    dest.write_bytes(b"\x89PNG existing")
    os.utime(dest, None)  # newer than the archive: nothing to do

    res = run_helper(root)
    assert res.returncode == 0, res.stderr
    assert res.stderr == ""
    assert dest.read_bytes() == b"\x89PNG existing"


def test_oversized_plate_member_is_refused(root):
    make_3mf(root / "Huge.3mf", {1: b"\0" * (16 * 1024 * 1024 + 1)})

    res = run_helper(root)
    assert res.returncode == 0
    assert not (root / ".thumbs" / "Huge" / "plate_1.png").exists()
    assert "too large" in res.stderr


def test_unreadable_archive_is_retried_once(root, monkeypatch):
    # A copy still in flight reads as a bad zip on the first pass; the helper
    # pauses and retries exactly the unreadable files.
    module = load_helper_module()
    make_3mf(root / "Racing.3mf", {1: PNG1})

    real_zipfile = zipfile.ZipFile
    opens = {"n": 0}

    class FlakyZip:
        def __init__(self, path):
            opens["n"] += 1
            if opens["n"] == 1:
                raise zipfile.BadZipFile("upload still in flight")
            self._zf = real_zipfile(path)

        def __enter__(self):
            self._zf.__enter__()
            return self

        def __exit__(self, *exc):
            return self._zf.__exit__(*exc)

        def __getattr__(self, name):
            return getattr(self._zf, name)

    monkeypatch.setattr(module.zipfile, "ZipFile", FlakyZip)
    sleeps = []
    monkeypatch.setattr(module.time, "sleep", lambda s: sleeps.append(s))

    wrote = module.extract_thumbs(str(root))

    assert wrote == 1
    assert opens["n"] == 2
    assert sleeps == [2]
    assert (root / ".thumbs" / "Racing" / "plate_1.png").read_bytes() == PNG1
