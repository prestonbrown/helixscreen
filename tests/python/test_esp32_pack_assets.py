#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Host-side round-trip test for the ESP32 packed asset container (Plan 4
Task 3): pack a small synthetic tree with the real mkfrogfs.py packer, then
read every file back through scripts/frogfs_reader.py and assert byte-exact
recovery. Also exercises the compressed vs. raw-stored code paths (the
frogfs.yaml filter deflates text but stores .png raw) and the djb2 hash
collision-safe path lookup.

The packing tests require the vendored jkent__frogfs component (fetched by
ESP-IDF's component manager into a gitignored managed_components/ tree) and
pyyaml. They skip on a dev box that has never run `idf.py reconfigure`, but
they FAIL under CI ($CI set) -- see _skip_or_fail(). The djb2 hash test needs
neither and runs everywhere.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from frogfs_reader import djb2_hash, load  # noqa: E402

MKFROGFS = (REPO_ROOT / "firmware" / "helixscreen-esp32" / "managed_components" /
           "jkent__frogfs" / "tools" / "mkfrogfs.py")

try:
    import yaml  # noqa: F401
    HAVE_YAML = True
except ImportError:
    HAVE_YAML = False


def _skip_or_fail(reason: str) -> None:
    """Gate the packer-dependent tests: a legitimate skip on a dev box, a hard
    failure in CI.

    managed_components/ is gitignored (firmware/helixscreen-esp32/.gitignore)
    and holds zero tracked files -- it exists only after `idf.py reconfigure`.
    The nightly test-python job does a plain checkout with no ESP-IDF setup, so
    a plain skipif here reports green forever while every assertion below is
    silently discarded. Make the gap loud in CI so it is a decision someone
    takes, not a hole nobody can see."""
    if os.environ.get("CI"):
        pytest.fail(
            f"{reason}\n"
            "In CI this is not a skip: the packer round-trip is unverifiable "
            "here, so this module tests nothing. Either provide the dependency "
            "or delete the module -- do not let it report green."
        )
    pytest.skip(reason)


def _pack(tmp_path: Path, source_tree: Path) -> Path:
    if not MKFROGFS.is_file():
        _skip_or_fail("jkent__frogfs not vendored -- run 'idf.py reconfigure' "
                      "in firmware/helixscreen-esp32 first")
    # PyYAML is pinned in requirements.txt, which CI installs, so unlike
    # mkfrogfs.py this one should never be missing there -- route it through the
    # same gate so a broken requirements.txt is loud rather than green.
    if not HAVE_YAML:
        _skip_or_fail("pyyaml not installed")

    tmp_path.mkdir(parents=True, exist_ok=True)
    config = tmp_path / "frogfs.yaml"
    config.write_text(
        'define:\n'
        '  staging: ${ENV:FROGFS_TEST_STAGING_DIR}\n'
        'collect:\n'
        '  $staging/: ""\n'
        'filter:\n'
        '  "*":\n'
        '    - compress zlib:\n'
        '        level: 9\n'
        '  "*.png":\n'
        '    - no compress\n',
        encoding="utf-8",
    )
    cache_dir = tmp_path / "cache"
    cache_dir.mkdir(parents=True, exist_ok=True)
    output = tmp_path / "packed.bin"

    env = dict(os.environ)
    env["FROGFS_TEST_STAGING_DIR"] = str(source_tree.resolve())

    result = subprocess.run(
        [sys.executable, str(MKFROGFS), "-C", str(REPO_ROOT), str(config), str(cache_dir),
         str(output)],
        env=env, capture_output=True, text=True,
    )
    assert result.returncode == 0, f"mkfrogfs.py failed:\n{result.stdout}\n{result.stderr}"
    return output


def _build_source_tree(root: Path) -> dict[str, bytes]:
    """A small tree covering: nested dirs, plain text (compresses), a
    deliberately incompressible binary blob, a fake .png (must be stored raw
    per the filter, verified by checking its comp_algo), and filename edge
    cases (a space and non-ASCII characters in the name) that real
    ui_xml/translations/*.xml and assets/images/printers/*.png filenames
    never exercise but that a robust packer/reader round-trip should still
    survive."""
    files = {
        "ui_xml/globals.xml": b"<globals>" + b"<entry/>" * 200 + b"</globals>",
        "ui_xml/translations/en.xml": "<translations><entry>Hello, world! 你好</entry></translations>".encode("utf-8"),
        "assets/config/printer_database.json": b'{"printers": [' + b'{"id": 1},' * 50 + b'{"id": 51}]}',
        "assets/images/printers/voron-v2.png": os.urandom(4096),  # incompressible stand-in for real PNG bytes
        "assets/filaments.json": b'{"filaments": []}',
        "empty.xml": b"",
        "assets/images/printers/creality k1 max.png": os.urandom(512),  # space in filename
        "ui_xml/translations/ünïcode name.xml": "<translations/>".encode("utf-8"),  # non-ASCII filename
    }
    for rel, content in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    return files


def test_round_trip_byte_exact(tmp_path):
    source_tree = tmp_path / "src"
    files = _build_source_tree(source_tree)

    packed = _pack(tmp_path / "pack", source_tree)
    img = load(str(packed))

    assert img.verify_footer_crc32()

    for rel, expected in files.items():
        actual = img.read_path(rel)
        assert actual == expected, f"{rel}: round-trip mismatch"


def test_png_stored_raw_text_compressed(tmp_path):
    source_tree = tmp_path / "src"
    _build_source_tree(source_tree)

    packed = _pack(tmp_path / "pack", source_tree)
    img = load(str(packed))

    png_entry = img.find("assets/images/printers/voron-v2.png")
    assert png_entry is not None
    assert png_entry.comp_algo == 0, "filter says '*.png': no compress"

    xml_entry = img.find("ui_xml/globals.xml")
    assert xml_entry is not None
    assert xml_entry.comp_algo == 1, "filter default is 'compress zlib' (algo id 1)"


def test_missing_path_returns_none(tmp_path):
    source_tree = tmp_path / "src"
    _build_source_tree(source_tree)

    packed = _pack(tmp_path / "pack", source_tree)
    img = load(str(packed))

    assert img.find("does/not/exist.xml") is None
    with pytest.raises(KeyError):
        img.read_path("does/not/exist.xml")


def test_djb2_hash_matches_reference_values():
    # Literal expected values, NOT recomputed from the same expression -- a
    # reimplementation checked against itself proves nothing. These were taken
    # from the packer side (mkfrogfs's tools/frogfs.py) for real asset paths;
    # the frogfs image format has no self-describing path index other than this
    # hash, so a wrong hash function reads back nothing and every lookup in
    # frogfs_reader.py silently returns None.
    assert djb2_hash("") == 0x1505  # the 5381 seed, unmodified
    assert djb2_hash("a") == 0x0002B5C4
    assert djb2_hash("ui_xml/globals.xml") == 0x6E5D0D5F
    assert djb2_hash("assets/config/printer_database.json") == 0x45EE4706
    assert djb2_hash("ui_xml/translations/en.xml") == 0x49B49B7B
    assert djb2_hash("assets/images/printers/voron-v2.png") == 0xAD2EC63C


def test_empty_file_round_trips(tmp_path):
    source_tree = tmp_path / "src"
    files = _build_source_tree(source_tree)
    packed = _pack(tmp_path / "pack", source_tree)
    img = load(str(packed))

    assert img.read_path("empty.xml") == b""
    assert files["empty.xml"] == b""
