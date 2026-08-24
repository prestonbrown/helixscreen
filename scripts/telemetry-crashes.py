#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
HelixScreen Crash Debugger

Resolves ASLR-randomized backtrace addresses from crash telemetry events
into human-readable function names, groups crashes by stack signature,
and provides summary/detail views.

Usage:
    telemetry-crashes.py --since 2026-02-09           # Summary of recent crashes
    telemetry-crashes.py --since 2026-02-09 --detail   # Full resolved backtraces
    telemetry-crashes.py --version 0.9.12 --detail     # Filter by version
    telemetry-crashes.py --sig a3f82b1c                # Zoom into one signature
    telemetry-crashes.py --json                        # Machine-readable output
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import urllib.error
from bisect import bisect_right
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Symbol table
# ---------------------------------------------------------------------------

class SymbolTable:
    """Parsed nm -nC output with binary-search lookup."""

    # Linker/runtime boundary symbols that aren't real functions.
    # Resolving to these means the unwind landed in a gap.
    GARBAGE_SYMBOLS = frozenset({
        "data_start", "_edata", "_end", "__bss_start", "__bss_start__",
        "__bss_end__", "__data_start", "__dso_handle", "__libc_csu_init",
        "__libc_csu_fini", "_fini", "_init", "_fp_hw", "_IO_stdin_used",
        "__init_array_start", "__init_array_end", "__fini_array_start",
        "__fini_array_end", "__FRAME_END__", "__GNU_EH_FRAME_HDR",
        "__TMC_END__", "__ehdr_start", "__exidx_start", "__exidx_end",
        "_GLOBAL_OFFSET_TABLE_", "_DYNAMIC", "_PROCEDURE_LINKAGE_TABLE_",
        "completed.0",
    })

    def __init__(self, entries: list[tuple[int, str]]):
        # entries: sorted list of (address, demangled_name)
        self.addrs = [a for a, _ in entries]
        self.names = [n for _, n in entries]
        self.crash_handler_offset: Optional[int] = None
        self._find_crash_handler()

    @classmethod
    def from_file(cls, path: Path) -> "SymbolTable":
        import re
        lto_pattern = re.compile(r'\.\w+\.[0-9a-f]{6,}$')
        entries: list[tuple[int, str]] = []
        with open(path, "r") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line:
                    continue
                parts = line.split(None, 2)
                if len(parts) < 3:
                    continue
                addr_str, sym_type, name = parts[0], parts[1], parts[2]
                if sym_type not in ("T", "t", "W", "w"):
                    continue
                try:
                    addr = int(addr_str, 16)
                except ValueError:
                    continue
                if addr == 0:
                    continue
                # Filter LTO compilation-unit section markers (e.g., "foo.c.35cb4f60")
                if lto_pattern.search(name):
                    continue
                entries.append((addr, name))
        entries.sort(key=lambda x: x[0])
        return cls(entries)

    def _find_crash_handler(self) -> None:
        for addr, name in zip(self.addrs, self.names):
            if "crash_signal_handler" in name:
                self.crash_handler_offset = addr
                return

    def find_offset(self, symbol_name: str) -> Optional[int]:
        """Return the file offset of the symbol matching `symbol_name`.

        Prefers an exact match (e.g. "helix_lvgl_anomaly" → the function,
        not "_GLOBAL__sub_I_helix_lvgl_anomaly"), then `name + "+"` (matching
        when the symbol table has trailing offsets), then substring as a
        last resort.
        """
        substr_hit: Optional[int] = None
        for addr, name in zip(self.addrs, self.names):
            if name == symbol_name:
                return addr
            if substr_hit is None and symbol_name in name:
                substr_hit = addr
        return substr_hit

    def lookup(self, file_offset: int) -> str:
        """Resolve a file offset to 'func_name+0xNN'."""
        if not self.addrs:
            return f"0x{file_offset:x}"
        idx = bisect_right(self.addrs, file_offset) - 1
        if idx < 0:
            return f"0x{file_offset:x}"
        base = self.addrs[idx]
        name = self.names[idx]
        # Filter garbage linker boundary symbols (data_start, _edata, etc.)
        if name in self.GARBAGE_SYMBOLS:
            return f"(unknown @ 0x{file_offset:x})"
        offset = file_offset - base
        if offset == 0:
            return name
        return f"{name}+0x{offset:x}"


# ---------------------------------------------------------------------------
# Symbol cache
# ---------------------------------------------------------------------------

CACHE_DIR = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "helixscreen" / "symbols"
R2_BASE_URL = os.environ.get("HELIX_R2_URL", "https://releases.helixscreen.org") + "/symbols"
GH_RELEASE_BASE = (
    "https://github.com/" + os.environ.get("HELIX_REPO", "prestonbrown/helixscreen") + "/releases/download"
)
# 32-bit ARM targets built in Thumb mode. Their return addresses have bit 0 set
# as the Thumb marker; nm addresses never do. Everything else (aarch64 pi, MIPS
# k1/ad5x) is unaffected by the mask, but listing only the ARM32 targets keeps
# the intent explicit.
THUMB_PLATFORMS = frozenset({"ad5m", "cc1", "pi32"})


def _http_download(url: str, dest: Path) -> Optional[int]:
    """Download url → dest. Returns None on success, or the HTTP status code on failure."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "helixscreen-crashes/1.0"})
        with urllib.request.urlopen(req) as resp, open(dest, "wb") as out:
            out.write(resp.read())
        return None
    except urllib.error.HTTPError as e:
        return e.code
    except urllib.error.URLError:
        return -1


class SymbolCache:
    """Downloads and caches symbol files from R2."""

    def __init__(self):
        self._tables: dict[str, Optional[SymbolTable]] = {}
        self._warnings: list[str] = []

    @property
    def warnings(self) -> list[str]:
        return self._warnings

    def get(self, version: str, platform: str) -> Optional[SymbolTable]:
        key = f"{version}/{platform}"
        if key in self._tables:
            return self._tables[key]

        sym_path = CACHE_DIR / f"v{version}" / f"{platform}.sym"

        # Download if not cached
        if not sym_path.exists():
            sym_path.parent.mkdir(parents=True, exist_ok=True)
            if not self._fetch_symbols(version, platform, sym_path):
                self._tables[key] = None
                return None

        # Validate non-empty
        if sym_path.stat().st_size == 0:
            self._warnings.append(f"v{version}/{platform}: symbol file is empty (broken upload?)")
            self._tables[key] = None
            return None

        table = SymbolTable.from_file(sym_path)
        if not table.addrs:
            self._warnings.append(f"v{version}/{platform}: no text symbols found in .sym file")
            self._tables[key] = None
            return None

        if table.crash_handler_offset is None:
            self._warnings.append(f"v{version}/{platform}: crash_signal_handler not found in symbols")

        self._tables[key] = table
        return table

    def _fetch_symbols(self, version: str, platform: str, sym_path: Path) -> bool:
        """Fetch the symbol map into sym_path, returning True on success.

        Symbol maps are stored compressed (.sym.zst) since v0.99.73 — nm output
        compresses ~25:1. Try .sym.zst first (R2, then GitHub), then fall back to
        the legacy uncompressed .sym for older releases (v0.99.72 and earlier).
        Mirrors scripts/resolve-backtrace.sh.
        """
        print(f"  Downloading symbols for v{version}/{platform}...", file=sys.stderr)
        have_zstd = shutil.which("zstd") is not None
        zst_path = sym_path.with_suffix(".sym.zst")

        # (url, is_compressed) candidates in priority order
        candidates = []
        if have_zstd:
            candidates.append((f"{R2_BASE_URL}/v{version}/{platform}.sym.zst", True))
        candidates.append((f"{R2_BASE_URL}/v{version}/{platform}.sym", False))
        # GitHub release assets carry a `symbols-` prefix on newer releases and
        # a bare <platform> name on older ones; R2 uses the bare name only.
        # Trying just the bare name made every version whose R2 upload was
        # missing resolve to raw hex instead of symbols, silently.
        for asset in (f"symbols-{platform}", platform):
            if have_zstd:
                candidates.append((f"{GH_RELEASE_BASE}/v{version}/{asset}.sym.zst", True))
            candidates.append((f"{GH_RELEASE_BASE}/v{version}/{asset}.sym", False))

        last_code = None
        for url, compressed in candidates:
            dest = zst_path if compressed else sym_path
            code = _http_download(url, dest)
            if code is not None:
                last_code = code
                continue
            if compressed:
                try:
                    subprocess.run(
                        ["zstd", "-d", "--rm", "-q", "-f", str(zst_path), "-o", str(sym_path)],
                        check=True,
                    )
                except subprocess.CalledProcessError:
                    zst_path.unlink(missing_ok=True)
                    continue
            return True

        # Draft releases 404 on the public download URL but are reachable with
        # the authenticated gh CLI, so a version that never left draft is still
        # resolvable for whoever can see it.
        if self._fetch_symbols_via_gh(version, platform, sym_path, have_zstd):
            return True

        if not have_zstd:
            self._warnings.append(
                f"v{version}/{platform}: symbols not available (HTTP {last_code}); "
                "install 'zstd' to fetch compressed .sym.zst maps (v0.99.73+)"
            )
        else:
            self._warnings.append(f"v{version}/{platform}: symbols not available (HTTP {last_code})")
        return False

    def _fetch_symbols_via_gh(
        self, version: str, platform: str, sym_path: Path, have_zstd: bool
    ) -> bool:
        """Last-resort fetch through the gh CLI (handles draft/private releases)."""
        if shutil.which("gh") is None:
            return False
        repo = os.environ.get("HELIX_REPO", "prestonbrown/helixscreen")
        assets = [f"symbols-{platform}", platform]
        suffixes = [".sym.zst"] if have_zstd else []
        suffixes.append(".sym")
        for asset in assets:
            for suffix in suffixes:
                name = asset + suffix
                dest = sym_path.with_name(name)
                try:
                    subprocess.run(
                        ["gh", "release", "download", f"v{version}", "--repo", repo,
                         "--pattern", name, "--output", str(dest), "--clobber"],
                        check=True, capture_output=True,
                    )
                except (subprocess.CalledProcessError, OSError):
                    continue
                if suffix == ".sym":
                    if dest != sym_path:
                        dest.replace(sym_path)
                    return True
                try:
                    subprocess.run(
                        ["zstd", "-d", "--rm", "-q", "-f", str(dest), "-o", str(sym_path)],
                        check=True,
                    )
                except subprocess.CalledProcessError:
                    dest.unlink(missing_ok=True)
                    continue
                return True
        return False


# ---------------------------------------------------------------------------
# ASLR resolution
# ---------------------------------------------------------------------------

def is_static_platform(platform: str) -> bool:
    """Platforms built with -static that should have no shared libraries.

    Note: Even with -static, glibc's NSS subsystem may dlopen() shared
    libraries (libresolv, libnss_dns, libnss_files, and even libc itself)
    at runtime for DNS resolution.  So "static" platforms CAN have shared
    lib addresses in their backtraces — but only in the NSS/resolver area.
    We still return True here; the caller should use the precise range
    check (load_base + sym_max) when available, which handles this correctly.
    """
    return platform in ("ad5m", "ad5x", "cc1")


# ---------------------------------------------------------------------------
# memory_map parsing: handle binaries with multiple PT_LOAD r-xp segments
# ---------------------------------------------------------------------------
# A single `load_base` value can't resolve frames in binaries that mmap the
# .text section at a file offset other than 0, or that have more than one
# executable PT_LOAD segment (the snapmaker-u1 build, for example, maps
# 0x1382000 of r-xp starting at file offset 0x155000). Parse the memory_map
# lines from the crash bundle and translate each frame through the segment
# it actually falls in.

# /proc/self/maps line shape:
#   START-END PERMS FILE_OFFSET DEV INODE   /path/to/file
# we only care about r-xp lines that map the helix-screen executable.
_MAP_LINE_RE = re.compile(
    r"^([0-9a-f]+)-([0-9a-f]+)\s+r-xp\s+([0-9a-f]+)\s+\S+\s+\d+\s+(.+?)\s*$"
)


def parse_binary_segments(memory_map: list) -> list[tuple[int, int, int]]:
    """Extract (mem_start, mem_end, file_offset) tuples for helix-screen r-xp
    segments in /proc/self/maps lines. Returns [] if no map data or no matches.

    Telemetry's `memory_map` field is already filtered to r-xp .so / helix-screen
    lines (src/system/telemetry_manager.cpp:1217-1234), so we just match on path
    suffix here.
    """
    segments: list[tuple[int, int, int]] = []
    if not memory_map:
        return segments
    for line in memory_map:
        if not isinstance(line, str):
            continue
        # Only the binary itself, not shared libs — those resolve via different
        # symbol files (which we don't ship).
        if "helix-screen" not in line and "helix_screen" not in line:
            continue
        m = _MAP_LINE_RE.match(line)
        if not m:
            continue
        try:
            mem_start = int(m.group(1), 16)
            mem_end = int(m.group(2), 16)
            file_off = int(m.group(3), 16)
        except ValueError:
            continue
        segments.append((mem_start, mem_end, file_off))
    return segments


def addr_to_file_offset(addr: int, segments: list[tuple[int, int, int]]) -> Optional[int]:
    """If addr falls in a known helix-screen r-xp segment, return the
    corresponding file offset in the binary. Otherwise None (shared lib /
    unmapped / data segment).
    """
    for mem_start, mem_end, file_off in segments:
        if mem_start <= addr < mem_end:
            return (addr - mem_start) + file_off
    return None


def is_shared_lib_addr(addr: int, platform: str, load_base: int = 0,
                       sym_max: int = 0) -> bool:
    """Detect shared library addresses (not from our binary).

    Static platforms (ad5m, ad5x, cc1): built with -static, so there are NO
    shared libraries.  Every address is from our binary (or garbage from
    failed signal-frame unwinding).  Always returns False.

    aarch64 (pi): binary at 0x0000aaaa..., shared libs at 0x0000ffff...
    armhf (pi32): binary at low addresses, shared libs at 0xf0000000+

    When load_base and sym_max are provided, uses precise range check:
    if addr is outside [load_base, load_base + sym_max], it's a shared lib.
    """
    # Precise range check (non-static platforms only): if we know the binary
    # span, anything outside is a shared lib.
    # NOTE: Do NOT use sym_max for static platforms — their .sym files are
    # often from debug builds with different section layouts (e.g., 462MB sym
    # range vs 12MB actual binary), causing wrong classification.
    if not is_static_platform(platform) and load_base > 0 and sym_max > 0:
        binary_end = load_base + sym_max + 0x10000  # generous padding
        return addr < load_base or addr > binary_end

    if platform in ("pi", "rpi4_64bit"):
        # aarch64 PIE: binary at 0x0000_55XX_XXXX_XXXX (or 0x0000_aaXX)
        # Shared libs at 0x0000_7fXX_XXXX_XXXX
        # Kernel/vDSO at 0x0000_ffffXXXX_XXXX
        top_byte = (addr >> 40) & 0xFF
        # Binary ASLR range is 0x55-0x56 or 0xaa-0xab; everything 0x7f+ is lib
        return top_byte >= 0x7F

    # Static platforms (ad5m, cc1): built with -static, but glibc's NSS
    # subsystem dlopen()s libc.so, libresolv.so, libnss_dns.so at runtime.
    # Binary text is typically < 20MB starting at 0x10000.
    # NSS shared libs land at 0xa0000000+ on ARM32.
    if platform in ("ad5m", "cc1"):
        return addr >= 0x10000000

    # AD5X (MIPS): binary at load_base (~0x55640000), shared libs at 0x70000000+
    if platform == "ad5x":
        if load_base > 0 and sym_max > 0:
            binary_end = load_base + sym_max + 0x10000
            return addr < load_base or addr > binary_end
        return addr >= 0x70000000

    # Heuristic fallbacks for 32-bit platforms without load_base
    if platform == "pi32":
        return addr >= 0xF0000000
    return False


def detect_platform_from_addrs(backtrace: list[int]) -> str:
    """Heuristic: 64-bit pi addresses have 0xaaaa or 0xffff in upper bits.

    Only used as a last resort when session data doesn't provide the platform.
    """
    for addr in backtrace:
        if addr > 0xFFFFFFFF:
            return "pi"
    return "pi32"


def resolve_backtrace(
    backtrace: list[str],
    platform: str,
    symbols: Optional[SymbolTable],
    load_base: Optional[str] = None,
    anchor_names: Optional[list[str]] = None,
    segments: Optional[list[tuple[int, int, int]]] = None,
) -> list[dict]:
    """Resolve a crash backtrace to named frames.

    `anchor_names` is the list of symbol names used to auto-detect ASLR base
    when no explicit `load_base` is provided. Defaults to ["crash_signal_handler"]
    (correct for crash events). Anomaly events (no signal handler in trace)
    can pass e.g. ["helix_lvgl_anomaly", "report_bg_expired_check"].

    `segments` is the list of helix-screen r-xp memory mappings from
    parse_binary_segments(). When provided, each frame is translated to a
    file offset via the segment it falls in, which correctly handles binaries
    that mmap .text at a non-zero file offset or have multiple PT_LOAD r-xp
    segments (e.g. snapmaker-u1 maps r-xp at file_offset=0x155000). Frames
    outside any helix-screen segment are flagged shared-lib; frames inside one
    bypass the global `load_base` arithmetic entirely. Absent → fall back to
    the legacy single-base path.

    Returns list of {addr, resolved, is_shared_lib} dicts.
    """
    if anchor_names is None:
        anchor_names = ["crash_signal_handler"]
    if not backtrace:
        return []

    addrs = []
    for addr_str in backtrace:
        try:
            addr = int(addr_str, 16)
        except ValueError:
            addr = 0
        # On Thumb-mode ARM the return address carries the Thumb bit in bit 0,
        # while nm addresses do not. _derive_anomaly_base already masks the
        # anchor; masking here keeps the frames themselves consistent with it
        # (an unmasked LR resolves one byte high and lands in the previous
        # symbol whenever the callsite is a function's first instruction).
        if platform in THUMB_PLATFORMS:
            addr &= ~1
        addrs.append(addr)

    frames: list[dict] = []

    # Check whether ANY anchor is resolvable. crash_signal_handler is the
    # precomputed default; other anchors look up fresh. Anomaly mode passes
    # non-default anchors so we can't short-circuit on crash_handler_offset
    # alone — that field's None doesn't mean the file is unusable for anomalies.
    have_anchor = symbols is not None and (
        any((name == "crash_signal_handler" and symbols.crash_handler_offset is not None)
            or (name != "crash_signal_handler" and symbols.find_offset(name) is not None)
            for name in anchor_names)
    )

    # An anchor is only needed to *derive* a base. When the caller already
    # supplies one (anomaly mode always does — see _derive_anomaly_base) the
    # anchor is irrelevant, and gating on it silently downgraded a fully
    # resolvable trace to raw hex whenever the default crash_signal_handler
    # was absent from the symbol map.
    if symbols is None or (not have_anchor and load_base is None):
        # Can't resolve — return raw addresses with platform-aware lib detection
        lb = 0
        if load_base is not None:
            try:
                lb = int(load_base, 16)
            except ValueError:
                pass
        sym_max = symbols.addrs[-1] if symbols and symbols.addrs else 0
        for i, addr in enumerate(addrs):
            is_lib = is_shared_lib_addr(addr, platform, load_base=lb,
                                         sym_max=sym_max)
            frames.append({
                "addr": f"0x{addr:x}",
                "resolved": "<shared lib>" if is_lib else f"0x{addr:x}",
                "is_shared_lib": is_lib,
            })
        return frames

    # Determine ASLR base address
    have_explicit_base = False
    if load_base is not None:
        # Explicit load_base from crash event (preferred — no heuristic needed)
        try:
            base_address = int(load_base, 16)
            have_explicit_base = True
        except ValueError:
            base_address = 0
    else:
        base_address = 0

    # Auto-detect base from anchor symbol(s) when no explicit load_base
    # was provided. This handles legacy events that predate load_base AND
    # anomaly events (which never include it). We do NOT override an explicit
    # load_base — even if validation fails (e.g. SIGABRT in shared lib code),
    # the explicit base is more trustworthy than heuristic detection.
    if not have_explicit_base and addrs:
        # Resolve each anchor name to its file offset (crash_handler_offset
        # is precomputed for backwards compat; anomaly anchors look up fresh).
        anchor_offsets: list[tuple[str, int]] = []
        for name in anchor_names:
            if name == "crash_signal_handler" and symbols.crash_handler_offset is not None:
                anchor_offsets.append((name, symbols.crash_handler_offset))
            else:
                off = symbols.find_offset(name)
                if off is not None:
                    anchor_offsets.append((name, off))

        if anchor_offsets:
            # First check if base=0 already resolves any anchor correctly.
            handler_found = False
            for addr in addrs:
                file_offset = addr - base_address
                if file_offset >= 0:
                    resolved_test = symbols.lookup(file_offset)
                    if any(name in resolved_test for name, _ in anchor_offsets):
                        handler_found = True
                        break

            if not handler_found:
                # Try each (anchor, address) pair to derive a candidate base.
                for anchor_name, anchor_off in anchor_offsets:
                    matched = False
                    for addr in addrs:
                        candidate_base = addr - anchor_off
                        if candidate_base < 0:
                            continue
                        test_offset = addr - candidate_base
                        resolved_test = symbols.lookup(test_offset)
                        if anchor_name in resolved_test:
                            base_address = candidate_base
                            matched = True
                            break
                    if matched:
                        break

    # Determine max symbol address for precise shared-lib detection
    sym_max = symbols.addrs[-1] if symbols and symbols.addrs else 0

    for i, addr in enumerate(addrs):
        # Classification: prefer memory_map segments when present.
        # `is_shared_lib_addr` falls back to `load_base + sym_max` heuristics,
        # which misclassifies binary frames past `_etext` as shared lib on
        # builds with `-ffunction-sections + LTO` (snapmaker-u1). Segments
        # tell us definitively which addrs are in helix-screen vs elsewhere.
        if segments is not None:
            is_lib = addr_to_file_offset(addr, segments) is None
        else:
            is_lib = is_shared_lib_addr(addr, platform, load_base=base_address,
                                         sym_max=sym_max)

        if is_lib:
            frames.append({
                "addr": f"0x{addr:x}",
                "resolved": "<shared lib>",
                "is_shared_lib": True,
            })
        else:
            # Translation always goes through `addr - load_base`. For PIE the
            # base makes this == file_offset (= sym_addr, since the linker uses
            # vaddr 0). For non-PIE static binaries the base is 0 and `nm`
            # encodes runtime virtual addresses directly, so this is also
            # correct. The segment-based `(addr - mem_start) + file_off`
            # formula computes file_offset and is WRONG for non-PIE because
            # virtual_addr != file_offset there — see ad5m static link.
            file_offset = addr - base_address
            resolved = symbols.lookup(file_offset)
            frames.append({
                "addr": f"0x{addr:x}",
                "resolved": resolved,
                "is_shared_lib": False,
            })

    return frames


# ---------------------------------------------------------------------------
# Stack signature
# ---------------------------------------------------------------------------

def compute_signature(frames: list[dict]) -> str:
    """Hash resolved function names (no offsets) to group identical crashes.

    Skips frame 0 (crash_signal_handler) and shared lib frames.
    When frames are unresolved (raw hex), uses relative offsets from frame 0
    so that ASLR-randomized addresses still group correctly.
    """
    sig_parts = []
    # Check if we have resolved symbols (any frame has a non-hex name)
    has_symbols = any(
        not f["is_shared_lib"] and not f["resolved"].startswith("0x")
        for f in frames[1:]  # skip frame 0
    )

    if has_symbols:
        for i, frame in enumerate(frames):
            if i == 0:
                continue
            if frame["is_shared_lib"]:
                continue
            name = frame["resolved"]
            plus_idx = name.rfind("+0x")
            if plus_idx > 0:
                name = name[:plus_idx]
            sig_parts.append(name)
    else:
        # No symbols: compute relative offsets from frame 0 for grouping
        # This makes ASLR-randomized addresses produce the same signature
        base_addr = None
        for frame in frames:
            if not frame["is_shared_lib"]:
                try:
                    base_addr = int(frame["addr"], 16)
                except ValueError:
                    pass
                break
        if base_addr is not None:
            for i, frame in enumerate(frames):
                if i == 0:
                    continue
                if frame["is_shared_lib"]:
                    continue
                try:
                    addr = int(frame["addr"], 16)
                    rel = addr - base_addr
                    sig_parts.append(f"rel+{rel:#x}")
                except ValueError:
                    sig_parts.append(frame["resolved"])

    if not sig_parts:
        return "unknown"

    sig_str = "\n".join(sig_parts)
    return hashlib.sha256(sig_str.encode()).hexdigest()[:8]


# ---------------------------------------------------------------------------
# Event loading (reuses patterns from telemetry-analyze.py)
# ---------------------------------------------------------------------------

def find_project_root() -> Path:
    path = Path(__file__).resolve().parent
    for _ in range(10):
        if (path / ".git").exists() or (path / "Makefile").exists():
            return path
        path = path.parent
    return Path.cwd()


def load_events(
    data_dir: str,
    since: Optional[str] = None,
    until: Optional[str] = None,
    anomaly_codes: Optional[list[str]] = None,
) -> tuple[list[dict], list[dict], list[dict]]:
    """Load crash, session, and (optionally) anomaly events.

    `anomaly_codes`: when provided, also load `error_encountered` events whose
    `code` matches any entry in the list. Returns (crashes, sessions, anomalies).
    """
    from datetime import datetime, timezone

    data_path = Path(data_dir)
    if not data_path.exists():
        print(f"Data directory not found: {data_path}", file=sys.stderr)
        return [], [], []

    crashes: list[dict] = []
    sessions: list[dict] = []
    anomalies: list[dict] = []
    file_count = 0
    code_filter: Optional[set[str]] = set(anomaly_codes) if anomaly_codes else None

    # Parse date filters
    since_dt = None
    until_dt = None
    if since:
        since_dt = datetime.strptime(since, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    if until:
        until_dt = datetime.strptime(until, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        # Include the entire "until" day
        until_dt = until_dt.replace(hour=23, minute=59, second=59)

    seen_crashes: set[tuple] = set()  # (device_id, timestamp, backtrace_key)

    for fpath in sorted(data_path.rglob("*.json")):
        file_count += 1
        try:
            with open(fpath, "r") as f:
                data = json.load(f)
        except (json.JSONDecodeError, OSError):
            continue

        events = data if isinstance(data, list) else [data]
        for ev in events:
            # Date filter
            ts_str = ev.get("timestamp")
            if ts_str and (since_dt or until_dt):
                try:
                    ts = datetime.fromisoformat(ts_str.replace("Z", "+00:00"))
                    if since_dt and ts < since_dt:
                        continue
                    if until_dt and ts > until_dt:
                        continue
                except ValueError:
                    pass

            if ev.get("event") == "crash":
                # Deduplicate: same device + timestamp + backtrace = same crash
                dedup_key = (
                    ev.get("device_id", ""),
                    ev.get("timestamp", ""),
                    tuple(ev.get("backtrace", [])),
                )
                if dedup_key not in seen_crashes:
                    seen_crashes.add(dedup_key)
                    crashes.append(ev)
            elif ev.get("event") == "session":
                sessions.append(ev)
            elif (code_filter is not None
                  and ev.get("event") == "error_encountered"
                  and ev.get("code") in code_filter):
                anomalies.append(ev)

    msg = f"Loaded {len(crashes)} crashes, {len(sessions)} sessions"
    if code_filter is not None:
        msg += f", {len(anomalies)} anomalies"
    msg += f" from {file_count} files"
    print(msg, file=sys.stderr)
    return crashes, sessions, anomalies


# ---------------------------------------------------------------------------
# Platform resolution
# ---------------------------------------------------------------------------

def build_device_platform_map(sessions: list[dict]) -> dict[str, str]:
    """Map device_id → platform from session events."""
    device_map: dict[str, str] = {}
    for s in sessions:
        did = s.get("device_id")
        plat = (s.get("app") or {}).get("platform")
        if did and plat:
            # Normalize rpi4_64bit → pi
            if plat == "rpi4_64bit":
                plat = "pi"
            device_map[did] = plat
    return device_map


def get_platform(
    crash: dict,
    device_map: dict[str, str],
    override: Optional[str] = None,
) -> str:
    """Determine platform for a crash event."""
    if override:
        return override
    did = crash.get("device_id", "")
    if did in device_map:
        return device_map[did]
    # Fallback: heuristic from addresses
    bt = crash.get("backtrace", [])
    addrs = []
    for a in bt:
        try:
            addrs.append(int(a, 16))
        except ValueError:
            pass
    return detect_platform_from_addrs(addrs)


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------

def analyze_crashes(
    crashes: list[dict],
    sessions: list[dict],
    symbol_cache: SymbolCache,
    platform_override: Optional[str] = None,
    version_filter: Optional[str] = None,
    sig_filter: Optional[str] = None,
) -> dict:
    """Analyze crashes: resolve symbols, group by signature."""
    device_map = build_device_platform_map(sessions)

    # Filter by version if requested
    if version_filter:
        crashes = [c for c in crashes if c.get("app_version") == version_filter]

    signatures: dict[str, dict] = {}  # sig_hash → group info

    for crash in crashes:
        version = crash.get("app_version", "unknown")
        platform = get_platform(crash, device_map, platform_override)
        backtrace = crash.get("backtrace", [])
        device_id = crash.get("device_id", "")
        uptime = crash.get("uptime_sec", 0)
        signal_name = crash.get("signal_name", "?")
        timestamp = crash.get("timestamp", "")

        # Get symbols
        symbols = symbol_cache.get(version, platform)

        # Resolve backtrace
        load_base = crash.get("load_base")
        # Parse the bundle's /proc/self/maps r-xp lines into segments so
        # multi-PT_LOAD or non-zero-file-offset binaries resolve correctly.
        # Empty list ⇒ no map data; pass None so the legacy single-base path
        # is taken instead of misclassifying every frame as <shared lib>.
        seg_list = parse_binary_segments(crash.get("memory_map") or [])
        segments = seg_list if seg_list else None
        frames = resolve_backtrace(backtrace, platform, symbols,
                                   load_base=load_base, segments=segments)

        # If backtrace is shallow (only crash_handler + signal_restorer),
        # inject reg_pc and reg_lr as additional frames for resolution.
        # This recovers the actual crash location from ucontext registers
        # on platforms where backtrace() can't unwind past signal frames.
        non_lib = [f for f in frames if not f["is_shared_lib"]]
        if len(non_lib) <= 2:
            reg_addrs = []
            for reg_key in ("reg_pc", "reg_lr"):
                reg_val = crash.get(reg_key)
                if reg_val:
                    reg_addrs.append(reg_val)
            if reg_addrs:
                reg_frames = resolve_backtrace(reg_addrs, platform, symbols,
                                               load_base=load_base, segments=segments)
                useful_reg = [f for f in reg_frames if not f["is_shared_lib"]
                              and "crash_signal_handler" not in f["resolved"]]
                if useful_reg:
                    frames = useful_reg + frames

        # Compute signature
        sig = compute_signature(frames)

        # Check if we're filtering to a specific signature
        if sig_filter and not sig.startswith(sig_filter):
            continue

        # Warn about pi32 shallow backtraces
        non_lib_frames = [f for f in frames if not f["is_shared_lib"]]
        shallow = len(non_lib_frames) <= 2

        if sig not in signatures:
            signatures[sig] = {
                "sig": sig,
                "count": 0,
                "signal": signal_name,
                "versions": set(),
                "devices": set(),
                "platforms": set(),
                "uptimes": [],
                "timestamps": [],
                "frames": frames,  # representative backtrace
                "shallow": shallow,
                "instances": [],
            }

        group = signatures[sig]
        group["count"] += 1
        group["versions"].add(version)
        group["devices"].add(device_id[:8])
        group["platforms"].add(platform)
        group["uptimes"].append(uptime)
        group["timestamps"].append(timestamp)
        group["instances"].append({
            "version": version,
            "platform": platform,
            "device": device_id[:8],
            "uptime": uptime,
            "signal": signal_name,
            "timestamp": timestamp,
            "frames": frames,
        })

    # Sort by count descending
    sorted_sigs = sorted(signatures.values(), key=lambda g: -g["count"])

    return {
        "total_crashes": len(crashes),
        "total_signatures": len(sorted_sigs),
        "signatures": sorted_sigs,
        "warnings": symbol_cache.warnings,
    }


# ---------------------------------------------------------------------------
# Anomaly events (error_encountered with code=bg_tok_expired_check, etc.)
# ---------------------------------------------------------------------------

import re as _re
_LR_RE = _re.compile(r"\blr=0x([0-9a-fA-F]+)")
_BT_RE = _re.compile(r"\bbt=([0-9a-fA-Fx,]+)")
_ANCHOR_RE = _re.compile(r"\bruntime_anchor=0x([0-9a-fA-F]+)")


_ZERO_BASE_PLATFORMS = frozenset({"ad5m", "cc1"})


def _derive_anomaly_base(
    symbols: Optional[SymbolTable],
    platform: str,
    runtime_anchor_hex: Optional[str],
) -> Optional[int]:
    """Derive ASLR load base for anomaly events.

    Three cases, in order of precision:

    - **Explicit `runtime_anchor=` field (v0.99.60+)**: helix_lvgl_anomaly
      embeds its own runtime address in the context. Combined with the
      .sym file's offset for `helix_lvgl_anomaly`, this gives an exact
      base: `base = runtime_anchor - helix_lvgl_anomaly_file_offset`.
      Works on every platform.

    - **Zero-base platforms (ad5m, cc1) without `runtime_anchor=`**:
      statically linked at file offset 0; addresses ARE file offsets.
      Return 0.

    - **PIE platforms without `runtime_anchor=` (pre-v0.99.60 bundles)**:
      no reliable anchor in bt[] (capture_backtrace_hex skipped both
      itself and helix_lvgl_anomaly). Return None and let the caller
      fall back to raw-hex display. Groups still cluster per-LR within
      a single process even unresolved.
    """
    if symbols is None:
        return None
    if runtime_anchor_hex:
        try:
            anchor_runtime = int(runtime_anchor_hex, 16)
        except ValueError:
            anchor_runtime = 0
        # On Thumb-mode ARM builds, function pointer values have the LSB
        # set (1) to mark Thumb encoding, while .sym addresses do not.
        # Mask it on 32-bit ARM platforms so the derived base aligns.
        # Harmless on aarch64 / static (LSB is already 0 there).
        if platform in THUMB_PLATFORMS and (anchor_runtime & 1):
            anchor_runtime &= ~1
        anchor_off = symbols.find_offset("helix_lvgl_anomaly")
        if anchor_runtime > 0 and anchor_off is not None:
            return max(0, anchor_runtime - anchor_off)
    if platform in _ZERO_BASE_PLATFORMS:
        return 0
    return None


def parse_anomaly_context(ctx: str) -> tuple[Optional[str], list[str], Optional[str]]:
    """Extract (lr_hex, bt_hex_list, runtime_anchor_hex) from an anomaly's context.

    Format produced by helix_lvgl_anomaly (v0.99.60+):
      "lr=0xXXXX tid=0xYYYY (description) | runtime_anchor=0xZZZZ | bt=0xAAA,0xBBB,..."

    Pre-v0.99.60 bundles lack `runtime_anchor` — we return None for that
    field and the resolver falls back to per-platform heuristics
    (zero-base for static binaries, raw hex for PIE).

    Returns (None, [], None) when the context is missing/malformed.
    """
    if not ctx:
        return None, [], None
    lr_match = _LR_RE.search(ctx)
    bt_match = _BT_RE.search(ctx)
    anchor_match = _ANCHOR_RE.search(ctx)
    lr_hex = f"0x{lr_match.group(1).lower()}" if lr_match else None
    anchor_hex = f"0x{anchor_match.group(1).lower()}" if anchor_match else None
    bt_hex: list[str] = []
    if bt_match:
        for tok in bt_match.group(1).split(","):
            tok = tok.strip()
            if not tok:
                continue
            # Normalize "0x" prefix
            if not tok.lower().startswith("0x"):
                tok = "0x" + tok
            # Reject malformed tokens at parse time rather than letting
            # them silently become 0x0 in resolve_backtrace via the
            # `int(addr_str, 16)` ValueError swallow.
            try:
                int(tok, 16)
            except ValueError:
                continue
            bt_hex.append(tok)
    return lr_hex, bt_hex, anchor_hex


def build_device_session_history(sessions: list[dict]) -> dict[str, list[dict]]:
    """Map device_id → list of sessions, sorted by timestamp ascending.

    Used to find the closest-in-time session for an anomaly event so we can
    derive the app version (anomaly events don't carry app_version themselves).
    """
    history: dict[str, list[dict]] = {}
    for s in sessions:
        did = s.get("device_id")
        if not did:
            continue
        history.setdefault(did, []).append(s)
    for did in history:
        history[did].sort(key=lambda s: s.get("timestamp", ""))
    return history


def session_for_anomaly(anomaly: dict, history: dict[str, list[dict]]) -> Optional[dict]:
    """Pick the most recent session at-or-before the anomaly's timestamp.

    Falls back to the earliest session for the device if none precede the
    anomaly (newer-format export, older-format anomaly, etc.).
    """
    did = anomaly.get("device_id", "")
    sessions = history.get(did)
    if not sessions:
        return None
    ts = anomaly.get("timestamp", "")
    if not ts:
        return sessions[-1]
    pick: Optional[dict] = None
    for s in sessions:
        s_ts = s.get("timestamp", "")
        if s_ts <= ts:
            pick = s
        else:
            break
    return pick or sessions[0]


def analyze_anomalies(
    anomalies: list[dict],
    sessions: list[dict],
    symbol_cache: SymbolCache,
    platform_override: Optional[str] = None,
    version_filter: Optional[str] = None,
    sig_filter: Optional[str] = None,
) -> dict:
    """Analyze anomaly events: resolve LRs, group by callsite (LR symbol).

    Anomalies come from `helix_lvgl_anomaly()` and carry an `lr=0xXXXX` plus
    optional `bt=0xAAA,0xBBB,...` in their context string. The LR is the
    immediate callsite and is the natural grouping key (one entry per
    distinct source-level callsite). The full bt is shown for diagnosis.
    """
    device_session_map = build_device_session_history(sessions)
    device_platform_map = build_device_platform_map(sessions)

    groups: dict[str, dict] = {}  # group_key → group info

    for ev in anomalies:
        lr_hex, bt_hex, runtime_anchor_hex = parse_anomaly_context(ev.get("context", ""))
        if not lr_hex:
            continue  # malformed; skip silently

        # Derive version + platform from the anomaly's session.
        sess = session_for_anomaly(ev, device_session_map)
        version = "unknown"
        if sess:
            version = (sess.get("app") or {}).get("version") or "unknown"

        if version_filter and version != version_filter:
            continue

        platform = platform_override
        if not platform:
            did = ev.get("device_id", "")
            platform = device_platform_map.get(did, "")
            if not platform and bt_hex:
                # Heuristic from the LR/bt addresses themselves
                addrs = []
                for h in [lr_hex] + bt_hex:
                    try:
                        addrs.append(int(h, 16))
                    except ValueError:
                        pass
                platform = detect_platform_from_addrs(addrs) if addrs else "unknown"
            if not platform:
                platform = "unknown"

        symbols = symbol_cache.get(version, platform)

        # Derive ASLR base honestly (static→0, PIE w/o anchors→None). Never
        # let resolve_backtrace's auto-detect fire on anomaly addresses —
        # the tautological "addr - anchor_off resolves to anchor" check
        # produces false positives across different real callsites (the LR
        # is user code, not the anchor). When derivation fails, we keep
        # addresses raw rather than forcing a wrong base.
        derived_base = _derive_anomaly_base(symbols, platform, runtime_anchor_hex)
        if derived_base is not None:
            load_base_hex = f"0x{derived_base:x}"
            lr_frames = resolve_backtrace(
                [lr_hex], platform, symbols, load_base=load_base_hex,
            )
            bt_frames = resolve_backtrace(
                bt_hex, platform, symbols, load_base=load_base_hex,
            ) if bt_hex else []
        else:
            # No symbol resolution available — emit raw hex frames so the LR
            # still groups consistently across events for the same callsite.
            lr_frames = [{"addr": lr_hex, "resolved": lr_hex, "is_shared_lib": False}]
            bt_frames = [{"addr": h, "resolved": h, "is_shared_lib": False} for h in bt_hex]

        # Build a stable group key. Prefer the resolved LR symbol (with
        # offset stripped) so renames between versions don't fragment the
        # group. Without symbols, the raw LR is useless as a key on PIE
        # platforms — ASLR re-randomizes it every launch, so N events from
        # one callsite report as N distinct callsites. The LR's distance
        # from `runtime_anchor` is the same arithmetic minus the unknown
        # load base, which makes it ASLR-invariant and stable for as long
        # as the binary is (i.e. per version).
        lr_resolved = lr_frames[0]["resolved"] if lr_frames else lr_hex
        plus_idx = lr_resolved.rfind("+0x")
        if plus_idx > 0:
            group_key = lr_resolved[:plus_idx]
        elif runtime_anchor_hex:
            try:
                delta = int(lr_hex, 16) - int(runtime_anchor_hex, 16)
                group_key = f"<unresolved v{version}/{platform} anchor{delta:+#x}>"
            except ValueError:
                group_key = lr_resolved
        else:
            group_key = lr_resolved

        if sig_filter and sig_filter not in group_key:
            continue

        device_id = ev.get("device_id", "")
        timestamp = ev.get("timestamp", "")
        uptime = ev.get("uptime_sec", 0)

        if group_key not in groups:
            groups[group_key] = {
                "lr_symbol": group_key,
                "count": 0,
                "code": ev.get("code", "?"),
                "category": ev.get("category", "?"),
                "versions": set(),
                "devices": set(),
                "platforms": set(),
                "uptimes": [],
                "timestamps": [],
                "lr_frame": lr_frames[0] if lr_frames else {"addr": lr_hex, "resolved": lr_hex, "is_shared_lib": False},
                "frames": bt_frames,  # representative backtrace
                "instances": [],
            }

        g = groups[group_key]
        g["count"] += 1
        g["versions"].add(version)
        g["devices"].add(device_id[:8])
        g["platforms"].add(platform)
        g["uptimes"].append(uptime)
        g["timestamps"].append(timestamp)
        g["instances"].append({
            "version": version,
            "platform": platform,
            "device": device_id[:8],
            "uptime": uptime,
            "timestamp": timestamp,
            "lr_frame": lr_frames[0] if lr_frames else None,
            "frames": bt_frames,
        })

    sorted_groups = sorted(groups.values(), key=lambda g: -g["count"])

    return {
        "mode": "anomalies",
        "total_anomalies": len(anomalies),
        "total_groups": len(sorted_groups),
        "groups": sorted_groups,
        "warnings": symbol_cache.warnings,
    }


def format_anomalies_terminal(result: dict, detail: bool = False) -> str:
    lines: list[str] = []
    sep = "=" * 70

    lines.append(sep)
    lines.append("  HELIXSCREEN ANOMALY WATCH")
    lines.append(sep)
    lines.append(f"  Total anomalies: {result['total_anomalies']}")
    lines.append(f"  Unique callsites: {result['total_groups']}")

    if result["warnings"]:
        lines.append("")
        lines.append("  Warnings:")
        for w in result["warnings"]:
            lines.append(f"    ⚠ {w}")

    lines.append("")

    for g in result["groups"]:
        versions = sorted(g["versions"])
        platforms = sorted(g["platforms"])
        devices = sorted(g["devices"])
        uptimes = g["uptimes"]
        lr = g["lr_frame"]

        lines.append(f"  [{g['code']}] {g['count']}x — {g['lr_symbol']}")
        lines.append(f"    LR: {lr.get('addr','?')}  →  {lr.get('resolved','?')}")
        lines.append(f"    versions: {', '.join(f'v{v}' for v in versions)}  |  "
                     f"platforms: {', '.join(platforms)}  |  "
                     f"devices: {len(devices)}")
        if uptimes:
            min_up, max_up = min(uptimes), max(uptimes)
            if min_up == max_up:
                lines.append(f"    uptime at fire: {_fmt_duration(min_up)}")
            else:
                lines.append(f"    uptime at fire: {_fmt_duration(min_up)} — {_fmt_duration(max_up)}")

        if detail:
            for inst in g["instances"]:
                lines.append(f"    ── v{inst['version']} {inst['platform']} "
                             f"dev={inst['device']} uptime={inst['uptime']}s "
                             f"{inst['timestamp']}")
                if inst["frames"]:
                    for idx, frame in enumerate(inst["frames"]):
                        lines.append(format_frame(frame, idx))
                else:
                    lines.append("       (no bt captured)")
            lines.append("")
        else:
            # Show representative bt (condensed) so the grouping is auditable
            frames = g["frames"]
            if frames:
                for idx, frame in enumerate(frames):
                    if idx > 6:
                        lines.append(f"       ... +{len(frames) - 6} more frames")
                        break
                    lines.append(format_frame(frame, idx))
            lines.append("")

    if not result["groups"]:
        lines.append("  No anomalies grouped (LR could not be parsed from context).")

    lines.append(sep)
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Output formatting
# ---------------------------------------------------------------------------

def format_frame(frame: dict, index: int) -> str:
    """Format a single backtrace frame."""
    marker = "→" if index == 0 else " "
    return f"  {marker} #{index:<2} {frame['addr']:>20s}  {frame['resolved']}"


def format_terminal(result: dict, detail: bool = False) -> str:
    lines: list[str] = []
    sep = "=" * 70

    lines.append(sep)
    lines.append("  HELIXSCREEN CRASH DEBUGGER")
    lines.append(sep)
    lines.append(f"  Total crashes: {result['total_crashes']}")
    lines.append(f"  Unique signatures: {result['total_signatures']}")

    if result["warnings"]:
        lines.append("")
        lines.append("  Warnings:")
        for w in result["warnings"]:
            lines.append(f"    ⚠ {w}")

    lines.append("")

    for group in result["signatures"]:
        sig = group["sig"]
        count = group["count"]
        signal = group["signal"]
        versions = sorted(group["versions"])
        devices = sorted(group["devices"])
        platforms = sorted(group["platforms"])
        uptimes = group["uptimes"]

        # Top-of-stack preview (first non-handler, non-shared-lib frame)
        top_func = "?"
        for i, frame in enumerate(group["frames"]):
            if i == 0:
                continue
            if not frame["is_shared_lib"] and not frame["resolved"].startswith("0x"):
                top_func = frame["resolved"]
                # Strip offset for preview
                plus_idx = top_func.rfind("+0x")
                if plus_idx > 0:
                    top_func = top_func[:plus_idx]
                break

        lines.append(f"  [{sig}] {count}x {signal} — {top_func}")
        lines.append(f"    versions: {', '.join(f'v{v}' for v in versions)}  |  "
                      f"platforms: {', '.join(platforms)}  |  "
                      f"devices: {len(devices)}")

        if uptimes:
            min_up = min(uptimes)
            max_up = max(uptimes)
            if min_up == max_up:
                lines.append(f"    uptime: {_fmt_duration(min_up)}")
            else:
                lines.append(f"    uptime: {_fmt_duration(min_up)} — {_fmt_duration(max_up)}")

        if group["shallow"]:
            plat_hint = "/".join(sorted(group["platforms"])) if group["platforms"] else "pi32?"
            lines.append(f"    ⚠ shallow backtrace ({plat_hint}) — grouping may be unreliable")

        if detail:
            for inst in group["instances"]:
                lines.append(f"    ── v{inst['version']} {inst['platform']} "
                              f"dev={inst['device']} uptime={inst['uptime']}s "
                              f"{inst['timestamp']}")
                for idx, frame in enumerate(inst["frames"]):
                    lines.append(format_frame(frame, idx))
            lines.append("")
        else:
            # Show representative backtrace (condensed)
            frames = group["frames"]
            if frames:
                for idx, frame in enumerate(frames):
                    if idx > 8:
                        lines.append(f"       ... +{len(frames) - 8} more frames")
                        break
                    lines.append(format_frame(frame, idx))
            lines.append("")

    if not result["signatures"]:
        lines.append("  No crashes found matching filters.")

    lines.append(sep)
    return "\n".join(lines)


def format_json_output(result: dict) -> str:
    """JSON output with sets converted to lists."""
    def serialize(obj):
        if isinstance(obj, set):
            return sorted(obj)
        return str(obj)
    return json.dumps(result, indent=2, default=serialize)


def _fmt_duration(seconds) -> str:
    seconds = float(seconds)
    if seconds < 60:
        return f"{seconds:.0f}s"
    if seconds < 3600:
        return f"{seconds / 60:.1f}min"
    return f"{seconds / 3600:.1f}hr"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="HelixScreen Crash Debugger — resolve ASLR backtraces from telemetry",
    )
    parser.add_argument("--since", metavar="YYYY-MM-DD", help="Include crashes on or after this date")
    parser.add_argument("--until", metavar="YYYY-MM-DD", help="Include crashes on or before this date")
    parser.add_argument("--version", metavar="VER", help="Filter to specific app version (e.g. 0.9.12)")
    parser.add_argument("--platform", metavar="PLAT", help="Override platform detection (pi, pi32)")
    parser.add_argument("--sig", metavar="HASH", help="Show only crashes matching this signature prefix")
    parser.add_argument("--detail", action="store_true", help="Show full resolved backtraces per instance")
    parser.add_argument("--json", action="store_true", help="Machine-readable JSON output")
    parser.add_argument("--data-dir", metavar="PATH", help="Override telemetry data directory")
    parser.add_argument("--anomalies", action="store_true",
                        help="Watch error_encountered events instead of crashes (e.g. L081 cluster:pstat-async-delete)")
    parser.add_argument("--code", metavar="NAME",
                        help="Anomaly code filter (default: bg_tok_expired_check). Use with --anomalies.")
    args = parser.parse_args()

    # Resolve data dir
    if args.data_dir:
        data_dir = args.data_dir
    else:
        root = find_project_root()
        data_dir = str(root / ".telemetry-data" / "events")

    # Load events. In anomaly mode, we additionally load error_encountered
    # events whose code matches the user's filter (default bg_tok_expired_check).
    cache = SymbolCache()
    if args.anomalies:
        codes = [args.code] if args.code else ["bg_tok_expired_check"]
        crashes, sessions, anomalies = load_events(
            data_dir, since=args.since, until=args.until, anomaly_codes=codes,
        )
        if not anomalies:
            print(f"No anomalies found (codes={codes}).", file=sys.stderr)
            sys.exit(0)
        result = analyze_anomalies(
            anomalies,
            sessions,
            cache,
            platform_override=args.platform,
            version_filter=args.version,
            sig_filter=args.sig,
        )
        if args.json:
            print(format_json_output(result))
        else:
            print(format_anomalies_terminal(result, detail=args.detail))
        return

    crashes, sessions, _ = load_events(data_dir, since=args.since, until=args.until)
    if not crashes:
        print("No crashes found.", file=sys.stderr)
        sys.exit(0)

    result = analyze_crashes(
        crashes,
        sessions,
        cache,
        platform_override=args.platform,
        version_filter=args.version,
        sig_filter=args.sig,
    )

    if args.json:
        print(format_json_output(result))
    else:
        print(format_terminal(result, detail=args.detail))


if __name__ == "__main__":
    main()
