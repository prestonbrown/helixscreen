#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-file facts the source gates share, cached across runs by file CONTENT.

The ESP32 checks and the registration checks (check_esp32_app_srcs.py --link,
check_esp32_xml_bindings.py, check_orphan_callbacks.py, check_orphan_subjects.py)
each parse every source in src/ and include/ for what one file says on its own: the
names it registers, the headers it includes, the identifiers it spells in the
branches the firmware compiles. A commit or a landing changes a handful of files,
so after the first run nearly every one of those answers is already known.

A fact is keyed on the file's bytes, a VARIANT naming how the caller reads the file
(raw, or with the firmware's #if branches applied under a given define set), and a
SALT: the hashes of the scripts whose code computes the fact. Editing a file, the
define set or a checker therefore misses the cache; nothing is keyed on a path or a
modification time, so a checkout, a rebase or a copied tree that holds the same bytes
reuses the answer, and a byte that differs never does.

Stored as JSON under build/.qc-cache/ (HELIX_QC_CACHE_DIR overrides it;
HELIX_QC_FILE_CACHE=0 turns it off). A save rereads the file and merges, then writes a
temporary file and renames it over the old one, so a reader never sees half a cache
and two gates saving at once each keep the other's entries, up to the rename race,
which only costs a later miss. Entries unused for STALE_DAYS are dropped.

    from qc_file_cache import FileCache

    cache = FileCache("callback_regs", salt_files=[Path(__file__)])
    names = cache.get(path, "raw", lambda text: sorted(parse(text)))
    cache.save()
"""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
import time
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parent.parent
STALE_DAYS = 3
FORMAT = 1


def cache_dir() -> Path:
    return Path(os.environ.get("HELIX_QC_CACHE_DIR") or ROOT / "build" / ".qc-cache")


def enabled() -> bool:
    return os.environ.get("HELIX_QC_FILE_CACHE", "1") != "0"


def digest(*parts: bytes | str) -> str:
    h = hashlib.sha256()
    for p in parts:
        h.update(p.encode() if isinstance(p, str) else p)
        h.update(b"\0")
    return h.hexdigest()


def files_digest(paths) -> str:
    """One hash over the named files' bytes, for a salt or a variant."""
    h = hashlib.sha256()
    for p in sorted(Path(x) for x in paths):
        h.update(str(p.name).encode() + b"\0")
        try:
            h.update(p.read_bytes())
        except OSError:
            h.update(b"<missing>")
        h.update(b"\0")
    return h.hexdigest()


class FileCache:
    """Facts about single files, keyed on (salt, variant, bytes)."""

    def __init__(self, name: str, salt_files=(), salt: str = ""):
        self.path = cache_dir() / f"{name}.json"
        self.salt = digest(str(FORMAT), salt, files_digest(salt_files))
        self.on = enabled()
        self.entries: dict[str, list] = {}
        self.touched: dict[str, list] = {}
        self.misses = 0
        self.hits = 0
        if self.on:
            self.entries = self._load()

    def _load(self) -> dict[str, list]:
        try:
            data = json.loads(self.path.read_text())
        except (OSError, ValueError):
            return {}
        if not isinstance(data, dict) or data.get("salt") != self.salt:
            return {}
        entries = data.get("entries")
        return entries if isinstance(entries, dict) else {}

    def get(self, path: Path, variant: str, compute: Callable[[str], Any],
            raw: bytes | None = None, errors: str = "replace") -> Any:
        """compute(text) for the file at PATH read as VARIANT, from the cache when known.

        compute must return JSON data (lists, not sets) and depend only on the text, the
        variant and the salted scripts. RAW is the file's bytes when the caller has them;
        ERRORS is how undecodable bytes are read, as for bytes.decode.
        """
        if raw is None:
            raw = path.read_bytes()
        if not self.on:
            return compute(raw.decode("utf-8", errors=errors))
        key = digest(variant, errors, raw)
        now = int(time.time())
        hit = self.entries.get(key)
        if hit is not None:
            self.hits += 1
            self.touched[key] = [now, hit[1]]
            return hit[1]
        self.misses += 1
        value = compute(raw.decode("utf-8", errors=errors))
        self.touched[key] = [now, value]
        return value

    def save(self) -> None:
        """Merge this run's entries into the stored cache, atomically."""
        if not self.on or not self.misses:
            return
        merged = self._load()
        cutoff = int(time.time()) - STALE_DAYS * 86400
        merged = {k: v for k, v in merged.items() if v[0] >= cutoff}
        merged.update(self.touched)
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            fd, tmp = tempfile.mkstemp(prefix=f".{self.path.name}.", dir=self.path.parent)
            with os.fdopen(fd, "w") as f:
                json.dump({"salt": self.salt, "entries": merged}, f, separators=(",", ":"))
            os.replace(tmp, self.path)
        except OSError:
            pass  # a cache that cannot be written only costs the next run its hits
