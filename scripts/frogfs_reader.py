#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Minimal, dependency-free reader for frogfs (jkent/frogfs) packed container
images, implementing the on-disk binary format documented in
firmware/helixscreen-esp32/managed_components/jkent__frogfs/src/frogfs_format.h
(struct layouts confirmed against that component's tools/format.py, which
mkfrogfs.py uses to write the same structs).

frogfs ships no standalone Python reader (only the C library, meant for
firmware); this module exists purely for host-side verification of packed
images (round-trip tests, size/breakdown reporting) without needing to build
or link the C library on the host.

Format summary (little-endian throughout):
    head:  magic(u32) ver_major(u8) ver_minor(u8) num_entries(u16) bin_sz(u32)
    hash:  hash(u32) header_offs(u32)              -- one per entry, sorted by hash
    entry: parent_offs(u32) child_count_or_comp(u16) seg_sz(u8) opts(u8)
           -- shared 8-byte prefix of every dir/file/comp header
    dir:   entry ++ child_offs(u32)[child_count] ++ name(seg_sz bytes)
    file:  entry ++ data_offs(u32) ++ data_sz(u32) ++ name(seg_sz bytes)
    comp:  entry ++ data_offs(u32) ++ data_sz(u32) ++ real_sz(u32) ++ name(...)
    foot:  crc32(u32) of the whole image excluding this field

child_count_or_comp < 0xFF00       -> directory, value is child count
child_count_or_comp == 0xFF00      -> uncompressed ("raw") file
child_count_or_comp > 0xFF00       -> compressed file, algo = value - 0xFF00
                                       (1=zlib/deflate, 2=heatshrink, 3=gzip)
"""

from __future__ import annotations

import gzip
import struct
import zlib
from dataclasses import dataclass

FROGFS_MAGIC = 0x474F5246

COMP_NONE = 0
COMP_ZLIB = 1
COMP_HEATSHRINK = 2
COMP_GZIP = 3

_HEAD = struct.Struct("<IBBHI")
_HASH = struct.Struct("<II")
_ENTRY = struct.Struct("<IHBB")


def djb2_hash(path: str) -> int:
    """Same hash mkfrogfs.py uses to key the hash table (tools/frogfs.py)."""
    h = 5381
    for b in path.encode("utf-8"):
        h = ((h << 5) + h ^ b) & 0xFFFFFFFF
    return h


@dataclass
class FrogfsEntry:
    offs: int
    parent_offs: int
    is_dir: bool
    comp_algo: int  # COMP_NONE for dirs and uncompressed files
    seg_sz: int
    name: str


class FrogfsImage:
    """Read-only parsed view of a packed frogfs image held in memory."""

    def __init__(self, data: bytes):
        self.data = data
        magic, ver_major, ver_minor, num_entries, bin_sz = _HEAD.unpack_from(data, 0)
        if magic != FROGFS_MAGIC:
            raise ValueError(f"bad frogfs magic: {magic:#x}")
        if bin_sz != len(data):
            raise ValueError(f"header bin_sz {bin_sz} != actual file size {len(data)}")
        self.ver_major = ver_major
        self.ver_minor = ver_minor
        self.num_entries = num_entries

        hash_table_offs = _align4(_HEAD.size)
        self._hashes = []
        for i in range(num_entries):
            h, offs = _HASH.unpack_from(data, hash_table_offs + i * _HASH.size)
            self._hashes.append((h, offs))
        self._hashes.sort()

    def verify_footer_crc32(self) -> bool:
        (stored,) = struct.unpack_from("<I", self.data, len(self.data) - 4)
        computed = zlib.crc32(self.data[:-4]) & 0xFFFFFFFF
        return stored == computed

    def _read_entry(self, offs: int) -> FrogfsEntry:
        parent_offs, child_or_comp, seg_sz, opts = _ENTRY.unpack_from(self.data, offs)
        if child_or_comp < 0xFF00:
            name_offs = offs + _ENTRY.size + 4 * child_or_comp
            is_dir, comp_algo = True, COMP_NONE
        elif child_or_comp == 0xFF00:
            name_offs = offs + _ENTRY.size + 8  # data_offs + data_sz
            is_dir, comp_algo = False, COMP_NONE
        else:
            name_offs = offs + _ENTRY.size + 12  # data_offs + data_sz + real_sz
            is_dir, comp_algo = False, child_or_comp - 0xFF00
        name = self.data[name_offs:name_offs + seg_sz].decode("utf-8") if seg_sz else ""
        return FrogfsEntry(offs, parent_offs, is_dir, comp_algo, seg_sz, name)

    def _full_path(self, offs: int) -> str:
        segments = []
        cur = offs
        while cur != 0:
            ent = self._read_entry(cur)
            if ent.name:
                segments.append(ent.name)
            cur = ent.parent_offs
        return "/".join(reversed(segments))

    def find(self, path: str) -> FrogfsEntry | None:
        """Locate an entry by its full path (relative to the container root,
        no leading slash). Resolves djb2 hash collisions by walking the
        parent chain of every hash-matching candidate and comparing the
        reconstructed full path — never trusts the hash alone."""
        target_hash = djb2_hash(path)
        import bisect

        i = bisect.bisect_left(self._hashes, (target_hash, -1))
        while i < len(self._hashes) and self._hashes[i][0] == target_hash:
            _, offs = self._hashes[i]
            if self._full_path(offs) == path:
                return self._read_entry(offs)
            i += 1
        return None

    def read_file(self, entry: FrogfsEntry) -> bytes:
        if entry.is_dir:
            raise ValueError(f"'{entry.name}' is a directory, not a file")
        if entry.comp_algo == COMP_NONE:
            data_offs, data_sz = struct.unpack_from("<II", self.data, entry.offs + _ENTRY.size)
            return self.data[data_offs:data_offs + data_sz]

        data_offs, data_sz, real_sz = struct.unpack_from(
            "<III", self.data, entry.offs + _ENTRY.size)
        raw = self.data[data_offs:data_offs + data_sz]
        if entry.comp_algo == COMP_ZLIB:
            out = zlib.decompress(raw)
        elif entry.comp_algo == COMP_GZIP:
            out = gzip.decompress(raw)
        else:
            raise NotImplementedError(
                f"comp_algo {entry.comp_algo} (heatshrink) not supported by this reader "
                "-- the packer config never selects it, only zlib/none")
        if len(out) != real_sz:
            raise ValueError(f"decompressed size {len(out)} != real_sz {real_sz}")
        return out

    def read_path(self, path: str) -> bytes:
        entry = self.find(path)
        if entry is None:
            raise KeyError(f"not found in frogfs image: {path}")
        return self.read_file(entry)


def _align4(n: int) -> int:
    return (n + 3) // 4 * 4


def load(path: str) -> FrogfsImage:
    with open(path, "rb") as f:
        return FrogfsImage(f.read())
