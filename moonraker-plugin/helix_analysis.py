# SPDX-License-Identifier: GPL-3.0-or-later
"""
File analysis on upload. How the app uses it: docs/devel/HELIXPRINT_PLUGIN.md,
File Analysis.

Every G-code file under gcodes gets one analysis record, written in the
background after it appears or changes and kept in the `helix_analysis`
database namespace, keyed by the gcodes-relative path. Files are read only
through helix_gcode_reader (one reader for the whole plugin): its reducer
supplies positions, tools and heater targets, and a ScanObserver collects the
rest in the same pass.

Record (schema 1); a field the file does not answer is null, never a guess:

    {
      "schema": 1,
      "filename": "sub/part.gcode",
      "file_size": 1234567, "file_mtime": 1760000000.5,  # the version analyzed
      "analyzed_at": 1760000100.0,
      "slicer": "OrcaSlicer 2.3.1" | null,
      "tools": [                     # one per tool the file prints with, by tool number
        {"tool": 0, "mm": 1234.5, "grams": 3.71, "color": "#FF8800", "material": "PLA",
         "sources": {"mm": "footer", "grams": "footer", "color": "metadata",
                     "material": "footer"}}  # footer | metadata | scan, per field
      ],
      "bounds": {
        "print":  {"min": [x, y, z], "max": [x, y, z]} | null,  # extruding moves
        "travel": {"min": [x, y, z], "max": [x, y, z]} | null,  # every move
        "max_hotend_temp": 250.0 | null, "max_bed_temp": 60.0 | null
      },
      "header": {
        "printer_model": "Voron 2.4 350" | null,
        "nozzle_diameters": [0.4, 0.4],      # [] when the file declares none
        "nozzle_volume_types": ["Standard", "High Flow"],  # as written, by extruder; [] when none
        "bed": {"min": [0, 0], "max": [350, 350], "max_height": 340 | null} | null,
        "filament_types": ["PLA", "PETG"]    # [] when the file declares none
      },
      "objects": {
        "source": "define" | "label" | "none",
        "list": [{"name": "cube_id_0_copy_0", "center": [x, y] | null,
                  "polygon": [[x, y], ...]}]
      },
      "layers": {"count": 412, "index": ".helix_index/sub/part.gcode.layers.jsonl" | null},
      "pauses": 0,                  # M600 / M601 / PAUSE / M0 lines
      "sets_pressure_advance": false,  # a SET_PRESSURE_ADVANCE with ADVANCE=
      "thumbnail": {"relative_path": "sub/.thumbs/part-300x300.png", "width": 300,
                    "height": 300, "generated": true} | null
    }

Positions are the toolhead frame (what the machine moves to, gcode offsets
applied). `mm` is filament length; `grams` comes only from a source that
states grams. `layers.index` is gcodes-relative; the sidecar's format is
helix_gcode_reader's LAYER_INDEX_* comment. `thumbnail` is set only when the
analyzer published one (Moonraker's metadata had none).

Endpoint `server.helix.analysis {filename}` answers
{"status": "ready", "analysis": record}, {"status": "pending"} (queued at the
front), {"status": "failed"} (this version of the file failed twice; a change
to the file tries again) or {"status": "disabled"}. Notification
`helix:analysis_ready {filename}` follows each record written.

Cost: the file pass runs in a worker process, this module run as a script
(`main()`), one file at a time, at nice 19 and idle I/O priority where the host
has them, so it never holds the GIL Moonraker's event loop needs. The queue,
backfill and print policy stay in Moonraker: while a print runs, analysis runs
only on a multi-core host (helix_gcode_reader.whole_file_analysis_allowed), and
a print starting on a one-core host kills the worker and queues its file again.
"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import math
import os
import struct
import sys
import threading
import time
import zlib
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple

import helix_file_queue as file_queue
import helix_gcode_reader as reader
from helix_gcode_reader import ModalState, ScanObserver

ANALYSIS_FEATURE = "file_analysis"
ANALYSIS_FEATURE_VERSION = 1
RECORD_SCHEMA = 1
DB_NAMESPACE = "helix_analysis"
# A file version that failed MAX_ATTEMPTS times, by path: [size, mtime, attempts].
# Kept across restarts, so a file that crashes the worker is not retried at
# every Moonraker start.
DB_FAILURES_NAMESPACE = "helix_analysis_failures"
READY_EVENT = "helix:analysis_ready"

GCODE_EXTS = (".gcode", ".g", ".gco")

# Extrusion below this (mm of filament) is noise, not a printing move.
EXTRUSION_EPSILON = 1e-5

# A file version is tried this many times before it reads `failed`.
MAX_ATTEMPTS = 2

THUMB_SIZE = 300
# Embedded thumbnails bigger than this are not kept.
MAX_EMBEDDED_THUMB_B64 = 1024 * 1024

# Footprint raster for a generated thumbnail: cells per side and cell size.
FOOTPRINT_CELLS = 1024
FOOTPRINT_CELL_MM = 0.5


# Lines a progress bar places markers from; helix_gcode_reader stores them
# in the layer index for the screen to classify.
PROGRESS_COMMANDS = frozenset((b"M600", b"M601", b"PAUSE", b"M0", b"M73",
                               b"SET_PRINT_STATS_INFO"))
PAUSE_COMMANDS = frozenset((b"M600", b"M601", b"PAUSE", b"M0"))

# `;TYPE:` sections that are not the part (start gcode purge, wipe tower):
# a generated thumbnail leaves them out, as the screen's preview does.
AUXILIARY_FEATURES = frozenset(("Custom", "CUSTOM", "Wipe tower", "Prime tower", "PRIME-TOWER"))


def is_analyzable(path: str) -> bool:
    """A visible G-code file: no dot directory or dot file on its path."""
    if not path or any(part.startswith(".") for part in path.split("/")):
        return False
    return path.lower().endswith(GCODE_EXTS)


def record_current(record: Any, size: int, mtime: float) -> bool:
    """A record answers for the file version (size, mtime) it was made from."""
    return (isinstance(record, dict) and record.get("schema") == RECORD_SCHEMA
            and record.get("file_size") == size and record.get("file_mtime") == mtime)


# ============================================================================
# Slicer config comments
# ============================================================================

# `; key = value` keys the record reads (Orca, PrusaSlicer, SuperSlicer, Bambu).
CONFIG_KEYS = frozenset((
    "filament used [mm]", "filament used [g]", "filament_colour", "filament_color",
    "extruder_colour", "extruder_color", "filament_type", "nozzle_diameter",
    "nozzle_volume_type", "printer_model", "bed_shape", "printable_area",
    "max_print_height", "printable_height",
))

# Cura's `;KEY:value` header keys the record reads.
CURA_KEYS = frozenset(("Filament used", "TARGET_MACHINE.NAME", "MACHINE_NAME"))
_CURA_NOZZLE = "EXTRUDER_TRAIN."
_CURA_NOZZLE_SUFFIX = ".NOZZLE.DIAMETER"


def parse_config_comment(line: bytes) -> Optional[Tuple[str, str]]:
    """(key, value) for a config comment the record reads, else None."""
    text = line[1:].strip()
    if not text:
        return None
    eq = text.find(b" = ")
    if eq > 0:
        key = text[:eq].decode("utf-8", "replace").strip()
        if key in CONFIG_KEYS:
            return key, text[eq + 3:].decode("utf-8", "replace").strip()
        return None
    colon = text.find(b":")
    if colon > 0:
        key = text[:colon].decode("utf-8", "replace").strip()
        if key in CURA_KEYS or (key.startswith(_CURA_NOZZLE) and key.endswith(_CURA_NOZZLE_SUFFIX)):
            return key, text[colon + 1:].decode("utf-8", "replace").strip()
    return None


def slicer_of(line: bytes) -> Optional[str]:
    """The slicer and version from a `generated by` / `Generated with` line."""
    text = line[1:].strip().decode("utf-8", "replace")
    low = text.lower()
    if low.startswith("bambustudio "):
        return text
    for prefix in ("generated by ", "generated with "):
        if low.startswith(prefix):
            rest = text[len(prefix):]
            # Orca and Prusa append " on <date> at <time>"
            cut = rest.find(" on ")
            name = (rest[:cut] if cut > 0 else rest).strip()
            return name.replace("_SteamEngine", "") or None
    return None


def _split_list(value: Optional[str], seps: str = ",;") -> List[str]:
    if value is None:
        return []
    for sep in seps[1:]:
        value = value.replace(sep, seps[0])
    return [v.strip() for v in value.split(seps[0])]


def _floats(value: Optional[str]) -> List[Optional[float]]:
    out = []  # type: List[Optional[float]]
    for token in _split_list(value):
        try:
            f = float(token.rstrip("m") if token.endswith("m") else token)
        except ValueError:
            out.append(None)
            continue
        out.append(f if math.isfinite(f) else None)
    return out


def _color(value: str) -> Optional[str]:
    v = value.strip().strip('"')
    if len(v) in (7, 9) and v.startswith("#"):
        try:
            int(v[1:], 16)
        except ValueError:
            return None
        return v.upper()
    return None


def footer_palette(config: Dict[str, str]) -> List[Optional[str]]:
    """Per-tool colors: extruder_colour when it names any, else filament_colour."""
    for key in ("extruder_colour", "extruder_color", "filament_colour", "filament_color"):
        colors = [_color(v) for v in _split_list(config.get(key), ";,")] \
            if config.get(key) else []
        if any(colors):
            return colors
    return []


def footer_tools(config: Dict[str, str]) -> Dict[str, List[Any]]:
    """What the file's own config comments say per tool: mm, grams, color, material."""
    mm = _floats(config.get("filament used [mm]"))
    if not mm and config.get("Filament used"):
        # Cura: meters
        mm = [None if v is None else v * 1000.0 for v in _floats(config["Filament used"])]
    return {
        "mm": mm,
        "grams": _floats(config.get("filament used [g]")),
        "color": footer_palette(config),
        "material": [v or None for v in _split_list(config.get("filament_type"), ";")],
    }


def _shape_bounds(value: Optional[str]) -> Optional[Tuple[List[float], List[float]]]:
    """`0x0,350x0,350x350,0x350` -> ([0, 0], [350, 350])."""
    xs, ys = [], []
    for point in _split_list(value, ","):
        x, sep, y = point.partition("x")
        if not sep:
            return None
        try:
            xs.append(float(x))
            ys.append(float(y))
        except ValueError:
            return None
    if not xs:
        return None
    return [min(xs), min(ys)], [max(xs), max(ys)]


def header_from_config(config: Dict[str, str]) -> Dict[str, Any]:
    # The slicer's printer model only: a preset name is the user's label for a
    # profile and says nothing about which machine it targets.
    model = config.get("printer_model") \
        or config.get("TARGET_MACHINE.NAME") or config.get("MACHINE_NAME") or None
    nozzles = [v for v in _floats(config.get("nozzle_diameter")) if v is not None]
    if not nozzles:
        cura = sorted((k, v) for k, v in config.items()
                      if k.startswith(_CURA_NOZZLE) and k.endswith(_CURA_NOZZLE_SUFFIX))
        nozzles = [f for f in (_floats(v)[0] if _floats(v) else None for _k, v in cura)
                   if f is not None]
    bed = None
    shape = _shape_bounds(config.get("printable_area") or config.get("bed_shape"))
    if shape is not None:
        height = _floats(config.get("printable_height") or config.get("max_print_height"))
        bed = {"min": shape[0], "max": shape[1],
               "max_height": height[0] if height and height[0] else None}
    types = [t for t in _split_list(config.get("filament_type"), ";") if t]
    # Orca: `; nozzle_volume_type = Standard,High Flow`, by extruder.
    volume_types = _split_list(config.get("nozzle_volume_type")) \
        if config.get("nozzle_volume_type") else []
    return {"printer_model": model, "nozzle_diameters": nozzles,
            "nozzle_volume_types": volume_types, "bed": bed, "filament_types": types}


# ============================================================================
# Per-tool merge
# ============================================================================


def _metadata_tools(metadata: Optional[Dict[str, Any]]) -> Dict[str, List[Any]]:
    """Moonraker's metadata per tool. Its filament_weights holds grams only
    when they add up to filament_weight_total; its fallback fills the same
    list from millimetres."""
    md = metadata or {}
    grams = []  # type: List[Any]
    weights = md.get("filament_weights")
    total = md.get("filament_weight_total")
    if isinstance(weights, list) and isinstance(total, (int, float)) and total > 0:
        nums = [w if isinstance(w, (int, float)) else None for w in weights]
        if abs(sum(w or 0.0 for w in nums) - total) <= max(0.01 * total, 0.01):
            grams = nums
    colors = md.get("filament_colors")
    material = md.get("filament_type")
    mm = []  # type: List[Any]
    if isinstance(md.get("filament_total"), (int, float)):
        mm = [float(md["filament_total"])]
    return {
        "mm": mm,
        "grams": grams,
        "color": [_color(c) if isinstance(c, str) else None for c in colors]
        if isinstance(colors, list) else [],
        "material": [m or None for m in _split_list(material, ";")]
        if isinstance(material, str) else [],
    }


def merge_tools(used: Iterable[int], footer: Dict[str, List[Any]],
                metadata: Dict[str, List[Any]], scan_mm: Dict[int, float]) -> List[Dict[str, Any]]:
    """One entry per used tool; each field from the footer, else Moonraker's
    metadata, else the scan, with the source it came from. Metadata's single
    filament_total is a per-tool figure only for a one-tool file."""
    tools = sorted(set(used))
    single = len(tools) == 1
    out = []
    for t in tools:
        entry = {"tool": t}  # type: Dict[str, Any]
        sources = {}  # type: Dict[str, Optional[str]]
        for field in ("mm", "grams", "color", "material"):
            value = None
            source = None
            for name, table in (("footer", footer), ("metadata", metadata)):
                values = table.get(field) or []
                if field == "mm" and name == "metadata":
                    candidate = values[0] if single and values else None
                else:
                    candidate = values[t] if t < len(values) else None
                if candidate is not None and (field not in ("mm", "grams") or candidate > 0):
                    value, source = candidate, name
                    break
            if value is None and field == "mm" and scan_mm.get(t, 0.0) > 0:
                value, source = round(scan_mm[t], 3), "scan"
            entry[field] = value
            sources[field] = source
        entry["sources"] = sources
        out.append(entry)
    return out


def tools_used(footer: Dict[str, List[Any]], scan_mm: Dict[int, float],
               tools_seen: Set[int]) -> Set[int]:
    """The tools that print: a positive footer amount, else extrusion seen
    under that tool in the scan."""
    used = set()
    for field in ("grams", "mm"):
        for i, v in enumerate(footer.get(field) or []):
            if v is not None and v > 0:
                used.add(i)
        if used:
            return used
    used = {t for t, mm in scan_mm.items() if mm > EXTRUSION_EPSILON}
    if not used and tools_seen:
        used = set(tools_seen)
    return used


# ============================================================================
# Objects
# ============================================================================


def convex_hull(points: Iterable[Tuple[float, float]]) -> List[List[float]]:
    """Counter-clockwise hull (Andrew's monotone chain), no repeated end point."""
    pts = sorted(set(points))
    if len(pts) <= 2:
        return [list(p) for p in pts]

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower = []  # type: List[Tuple[float, float]]
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper = []  # type: List[Tuple[float, float]]
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return [list(p) for p in lower[:-1] + upper[:-1]]


class _Hull:
    """A running convex hull: points buffer up and fold into the hull in
    batches, so memory stays small whatever the object's move count."""

    BATCH = 4096

    def __init__(self) -> None:
        self.hull = []  # type: List[List[float]]
        self._buf = []  # type: List[Tuple[float, float]]

    def add(self, x: float, y: float) -> None:
        self._buf.append((round(x, 3), round(y, 3)))
        if len(self._buf) >= self.BATCH:
            self._fold()

    def _fold(self) -> None:
        self.hull = convex_hull([tuple(p) for p in self.hull] + self._buf)
        self._buf = []

    def result(self) -> List[List[float]]:
        if self._buf:
            self._fold()
        return self.hull


def parse_object_define(line: str) -> Optional[Dict[str, Any]]:
    """EXCLUDE_OBJECT_DEFINE NAME=.. [CENTER=x,y] [POLYGON=[[x,y],..]]."""
    parts = line.split()
    params = {}
    for word in parts[1:]:
        k, sep, v = word.partition("=")
        if sep:
            params[k.upper()] = v.strip('"')
    name = params.get("NAME")
    if not name:
        return None
    center = None
    c = _floats(params.get("CENTER"))
    if len(c) == 2 and None not in c:
        center = [c[0], c[1]]
    polygon = []
    try:
        raw = json.loads(params.get("POLYGON", "[]"))
        polygon = [[float(p[0]), float(p[1])] for p in raw
                   if isinstance(p, (list, tuple)) and len(p) >= 2]
    except (ValueError, TypeError):
        polygon = []
    return {"name": name, "center": center, "polygon": polygon}


def _m486_params(body: bytes) -> Tuple[Optional[int], Optional[str]]:
    """M486's S (object index) and A (name, quoted or not) from the raw line."""
    text = body.decode("utf-8", "replace").strip()
    rest = text[4:]
    s_value = None
    name = None
    i = 0
    while i < len(rest):
        ch = rest[i]
        if ch in " \t":
            i += 1
            continue
        letter = ch.upper()
        j = i + 1
        if letter == "A":
            if j < len(rest) and rest[j] == '"':
                end = rest.find('"', j + 1)
                end = len(rest) if end < 0 else end
                name = rest[j + 1:end]
                i = end + 1
            else:
                name = rest[j:].strip()
                i = len(rest)
            continue
        end = j
        while end < len(rest) and rest[end] not in " \t":
            end += 1
        if letter == "S":
            try:
                s_value = int(float(rest[j:end]))
            except ValueError:
                s_value = None
        i = end
    return s_value, name


def _center_of(polygon: Sequence[Sequence[float]]) -> Optional[List[float]]:
    if not polygon:
        return None
    xs = [p[0] for p in polygon]
    ys = [p[1] for p in polygon]
    return [round((min(xs) + max(xs)) / 2.0, 3), round((min(ys) + max(ys)) / 2.0, 3)]


# ============================================================================
# Thumbnails
# ============================================================================


def encode_png(width: int, height: int, rgb_rows: Sequence[bytes]) -> bytes:
    """An 8-bit RGB PNG from `height` rows of width * 3 bytes."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def _hex_rgb(color: Optional[str]) -> Tuple[int, int, int]:
    if color and len(color) >= 7:
        try:
            return int(color[1:3], 16), int(color[3:5], 16), int(color[5:7], 16)
        except ValueError:
            pass
    return 0x4C, 0xAF, 0x50


def clip_segment(x0: float, y0: float, x1: float, y1: float, xmin: float, ymin: float,
                 xmax: float, ymax: float) -> Optional[Tuple[float, float, float, float]]:
    """The part of a segment inside a box (Liang-Barsky), or None when no part
    of it is."""
    t0, t1 = 0.0, 1.0
    dx, dy = x1 - x0, y1 - y0
    for p, q in ((-dx, x0 - xmin), (dx, xmax - x0), (-dy, y0 - ymin), (dy, ymax - y0)):
        if p == 0:
            if q < 0:
                return None
            continue
        t = q / p
        if p < 0:
            if t > t1:
                return None
            t0 = max(t0, t)
        else:
            if t < t0:
                return None
            t1 = min(t1, t)
    return x0 + t0 * dx, y0 + t0 * dy, x0 + t1 * dx, y0 + t1 * dy


class Footprint:
    """Top-down height map of the extruding moves, for a thumbnail when the
    file has none. The grid is centred on the first extruding point."""

    def __init__(self) -> None:
        self._origin = None  # type: Optional[Tuple[float, float]]
        self._height = None  # type: Optional[bytearray]
        self._tool = None  # type: Optional[bytearray]
        self._max_z = 0.0
        self._z = []  # type: List[float]

    def segment(self, x0: float, y0: float, x1: float, y1: float, z: float, tool: int) -> None:
        if self._origin is None:
            half = FOOTPRINT_CELLS * FOOTPRINT_CELL_MM / 2.0
            self._origin = (x1 - half, y1 - half)
            self._height = bytearray(FOOTPRINT_CELLS * FOOTPRINT_CELLS)
            self._tool = bytearray(FOOTPRINT_CELLS * FOOTPRINT_CELLS)
        self._max_z = max(self._max_z, z)
        level = max(1, min(255, int(z) + 1))
        ox, oy = self._origin
        n = FOOTPRINT_CELLS
        span = n * FOOTPRINT_CELL_MM
        clipped = clip_segment(x0, y0, x1, y1, ox, oy, ox + span, oy + span)
        if clipped is None:
            return
        x0, y0, x1, y1 = clipped
        steps = int(max(abs(x1 - x0), abs(y1 - y0)) / FOOTPRINT_CELL_MM) + 1
        for i in range(steps + 1):
            f = i / steps
            cx = int((x0 + (x1 - x0) * f - ox) / FOOTPRINT_CELL_MM)
            cy = int((y0 + (y1 - y0) * f - oy) / FOOTPRINT_CELL_MM)
            if 0 <= cx < n and 0 <= cy < n:
                k = cy * n + cx
                if level >= self._height[k]:
                    self._height[k] = level
                    self._tool[k] = min(255, tool + 1)

    def render(self, colors: Dict[int, Optional[str]], size: int = THUMB_SIZE) -> Optional[bytes]:
        if self._height is None:
            return None
        n = FOOTPRINT_CELLS
        h = self._height
        min_x = min_y = n
        max_x = max_y = -1
        for r in range(n):
            row = h[r * n:(r + 1) * n]
            if not any(row):
                continue
            min_y = min(min_y, r)
            max_y = r
            first = next(i for i, v in enumerate(row) if v)
            last = n - 1 - next(i for i, v in enumerate(reversed(row)) if v)
            min_x = min(min_x, first)
            max_x = max(max_x, last)
        if max_x < 0:
            return None
        span = max(max_x - min_x, max_y - min_y) + 1
        margin = max(2, span // 12)
        span += 2 * margin
        cx0 = (min_x + max_x) // 2 - span // 2
        cy0 = (min_y + max_y) // 2 - span // 2
        top = max(1, int(self._max_z) + 1)
        rgb = {t: _hex_rgb(colors.get(t)) for t in range(256)}
        background = (0x20, 0x20, 0x20)
        rows = []
        for py in range(size):
            # Image rows go top-down; bed Y grows upward.
            gy = cy0 + (size - 1 - py) * span // size
            row = bytearray(size * 3)
            for px in range(size):
                gx = cx0 + px * span // size
                if 0 <= gx < n and 0 <= gy < n and h[gy * n + gx]:
                    k = gy * n + gx
                    r, g, b = rgb[self._tool[k] - 1]
                    shade = 0.35 + 0.65 * min(1.0, h[k] / top)
                    row[px * 3:px * 3 + 3] = bytes((int(r * shade), int(g * shade),
                                                    int(b * shade)))
                else:
                    row[px * 3:px * 3 + 3] = bytes(background)
            rows.append(bytes(row))
        return encode_png(size, size, rows)


_BASE64 = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="

_THUMB_BEGIN = (b"thumbnail begin", b"thumbnail_PNG begin", b"thumbnail_JPG begin",
                b"thumbnail_QOI begin")


def _thumb_kind(text: bytes) -> Optional[str]:
    for marker in _THUMB_BEGIN:
        if text.startswith(marker):
            return "JPG" if b"JPG" in marker else ("QOI" if b"QOI" in marker else "PNG")
    return None


# ============================================================================
# The collector
# ============================================================================


def _box_add(box: List[List[float]], x: float, y: float, z: Optional[float]) -> None:
    lo, hi = box
    if x < lo[0]:
        lo[0] = x
    if x > hi[0]:
        hi[0] = x
    if y < lo[1]:
        lo[1] = y
    if y > hi[1]:
        hi[1] = y
    if z is not None:
        if z < lo[2]:
            lo[2] = z
        if z > hi[2]:
            hi[2] = z


def _new_box() -> List[List[float]]:
    return [[math.inf] * 3, [-math.inf] * 3]


def _box_out(box: List[List[float]]) -> Optional[Dict[str, Any]]:
    """{"min": [x, y, z], "max": [x, y, z]}; z is null when no move named Z."""
    if box[0][0] > box[1][0]:
        return None

    def axis(v: float) -> Optional[float]:
        return round(v, 4) if math.isfinite(v) else None

    return {"min": [axis(v) for v in box[0]], "max": [axis(v) for v in box[1]]}


class AnalysisCollector(ScanObserver):
    """Everything a record holds, gathered in the reader's one pass."""

    def __init__(self, want_thumbnail: bool = False) -> None:
        self.print_box = _new_box()
        # G-code frame, outside AUXILIARY_FEATURES: what a preview frames
        self.model_box = _new_box()
        self.initial_tool = None  # type: Optional[int]
        self.travel_box = _new_box()
        self._seen_axes = 0
        self._known_xy = False
        self._known_z = False
        self.tool_mm = {}  # type: Dict[int, float]
        self.tools_seen = set()  # type: Set[int]
        self.max_hotend = None  # type: Optional[float]
        self.max_bed = None  # type: Optional[float]
        self.config = {}  # type: Dict[str, str]
        self.slicer = None  # type: Optional[str]
        self.defines = []  # type: List[Dict[str, Any]]
        self.labels = {}  # type: Dict[str, _Hull]
        self._label = None  # type: Optional[str]
        self._m486_names = {}  # type: Dict[int, str]
        self.feature = None  # type: Optional[str]
        self.progress_lines = []  # type: List[Tuple[int, str]]
        self.pauses = 0
        # [layer, offset, state, extra] per layer start, file order
        self.layer_starts = []  # type: List[list]
        self._layer_tools = set()  # type: Set[int]
        self._layer_print_z = None  # type: Optional[float]
        self.want_thumbnail = want_thumbnail
        self.footprint = Footprint() if want_thumbnail else None
        self.thumbnails = []  # type: List[Tuple[str, int, int, str]]
        self._thumb = None  # type: Optional[list]

    # -- layers --------------------------------------------------------------

    def _close_layer(self) -> None:
        if self.layer_starts:
            extra = self.layer_starts[-1][3]
            extra["tools"] = sorted(self._layer_tools)
            extra["print_z"] = None if self._layer_print_z is None \
                else round(self._layer_print_z, 4)
        self._layer_tools = set()
        self._layer_print_z = None

    def layer_start(self, layer: int, offset: int, state: ModalState) -> None:
        if self.layer_starts and self.layer_starts[-1][0] == layer:
            # A later start for the same layer replaces the earlier one.
            tools = self._layer_tools
            z = self._layer_print_z
            self.layer_starts[-1] = [layer, offset, state, {"feature": self.feature}]
            self._layer_tools = tools
            self._layer_print_z = z
            return
        self._close_layer()
        self.layer_starts.append([layer, offset, state, {"feature": self.feature}])

    def finish(self) -> None:
        self._close_layer()
        self._thumb = None

    # -- observer ------------------------------------------------------------

    def move(self, state: ModalState, start: List[float], words: List[bytes],
             offset: int) -> None:
        lp = state.last_position
        de = lp[3] - start[3]
        tool = state.active_tool if state.active_tool is not None else 0
        if de:
            self.tool_mm[tool] = self.tool_mm.get(tool, 0.0) + de / state.extrude_factor
        # An axis the file has never moved sits at a position nobody knows;
        # it enters no bounds until a move names it.
        was_known = self._known_xy
        if not (self._known_xy and self._known_z):
            for w in words[1:]:
                axis = w[:1]
                if axis in (b"X", b"Y"):
                    self._seen_axes |= 1 if axis == b"X" else 2
                elif axis == b"Z":
                    self._known_z = True
            self._known_xy = self._seen_axes == 3
        if self._known_xy:
            if was_known:
                _box_add(self.travel_box, start[0], start[1], start[2] if self._known_z else None)
            _box_add(self.travel_box, lp[0], lp[1], lp[2] if self._known_z else None)
        if de > EXTRUSION_EPSILON and (lp[0] != start[0] or lp[1] != start[1]) \
                and self._known_xy:
            z = lp[2] if self._known_z else None
            if was_known:
                _box_add(self.print_box, start[0], start[1], z)
            _box_add(self.print_box, lp[0], lp[1], z)
            if self.feature not in AUXILIARY_FEATURES:
                bp = state.base_position
                gz = None if z is None else z - bp[2]
                if was_known:
                    _box_add(self.model_box, start[0] - bp[0], start[1] - bp[1], gz)
                _box_add(self.model_box, lp[0] - bp[0], lp[1] - bp[1], gz)
            self._layer_tools.add(tool)
            if self._layer_print_z is None and self._known_z:
                self._layer_print_z = lp[2]
            label = self._label if self._label is not None else state.current_object
            if label is not None:
                hull = self.labels.get(label)
                if hull is None:
                    hull = self.labels[label] = _Hull()
                if was_known:
                    hull.add(start[0], start[1])
                hull.add(lp[0], lp[1])
            if self.footprint is not None and was_known \
                    and self.feature not in AUXILIARY_FEATURES:
                self.footprint.segment(start[0], start[1], lp[0], lp[1], lp[2], tool)

    def command(self, state: ModalState, cmd: bytes, words: List[bytes],
                body: bytes, offset: int) -> None:
        if cmd[:1] == b"T" and cmd[1:].isdigit():
            self.tools_seen.add(int(cmd[1:]))
            if self.initial_tool is None:
                self.initial_tool = int(cmd[1:])
            return
        if cmd in (b"M104", b"M109", b"M140", b"M190", b"SET_HEATER_TEMPERATURE"):
            for name, target in state.heater_targets.items():
                if name == "heater_bed":
                    self.max_bed = target if self.max_bed is None else max(self.max_bed, target)
                elif name.startswith("extruder"):
                    self.max_hotend = target if self.max_hotend is None \
                        else max(self.max_hotend, target)
            return
        if cmd == b"EXCLUDE_OBJECT_DEFINE":
            parsed = parse_object_define(body.strip().decode("utf-8", "replace"))
            if parsed is not None:
                self.defines.append(parsed)
            return
        if cmd == b"M486":
            index, name = _m486_params(body)
            if index is not None and index < 0:
                self._label = None
            elif index is not None:
                if name:
                    self._m486_names[index] = name
                self._label = self._m486_names.get(index, str(index))
            return
        if cmd in PROGRESS_COMMANDS:
            self.progress_lines.append((offset, body.strip().decode("utf-8", "replace")))
            if cmd in PAUSE_COMMANDS:
                self.pauses += 1

    def comment(self, state: ModalState, line: bytes, offset: int) -> None:
        if self._thumb is not None:
            text = line[1:].strip()
            if text.startswith(b"thumbnail") and text.endswith(b"end"):
                kind, w, h, parts, _size = self._thumb
                self._thumb = None
                self.thumbnails.append((kind, w, h, "".join(parts)))
                return
            fits = self._thumb[4] + len(text) <= MAX_EMBEDDED_THUMB_B64
            if fits and text.rstrip(_BASE64) == b"":
                self._thumb[3].append(text.decode("ascii"))
                self._thumb[4] += len(text)
                return
            # Past the cap, or a line no thumbnail holds: the block ended
            # unterminated, and this line is read as any other comment.
            self._thumb = None
        text = line[1:].lstrip()
        if text.startswith(b"TYPE:"):
            self.feature = text[5:].strip().decode("utf-8", "replace") or None
            return
        if text.startswith(b"printing object "):
            self._label = text[16:].strip().decode("utf-8", "replace") or None
            return
        if text.startswith(b"stop printing object"):
            self._label = None
            return
        if line.startswith(b";MESH:"):
            name = line[6:].strip().decode("utf-8", "replace")
            self._label = None if not name or name == "NONMESH" else name
            return
        if self.want_thumbnail and len(self.thumbnails) < 4:
            kind = _thumb_kind(text)
            if kind is not None:
                size = text.split()[2:3]
                w = h = 0
                if size:
                    a, _, b = size[0].partition(b"x")
                    try:
                        w, h = int(a), int(b)
                    except ValueError:
                        w = h = 0
                self._thumb = [kind, w, h, [], 0]
                return
        if self.slicer is None and (text[:9].lower() == b"generated"
                                    or text.startswith(b"BambuStudio ")):
            self.slicer = slicer_of(line)
            return
        kv = parse_config_comment(line)
        if kv is not None:
            self.config[kv[0]] = kv[1]

    # -- results -------------------------------------------------------------

    def objects(self) -> Dict[str, Any]:
        if self.defines:
            out = []
            for d in self.defines:
                polygon = d["polygon"]
                if not polygon and d["name"] in self.labels:
                    polygon = self.labels[d["name"]].result()
                out.append({"name": d["name"], "center": d["center"] or _center_of(polygon),
                            "polygon": polygon})
            return {"source": "define", "list": out}
        if self.labels:
            out = []
            for name, hull in self.labels.items():
                polygon = hull.result()
                out.append({"name": name, "center": _center_of(polygon), "polygon": polygon})
            return {"source": "label", "list": out}
        return {"source": "none", "list": []}


# ============================================================================
# Analysis of one file (blocking; runs on the analysis thread)
# ============================================================================


class AnalysisOutput:
    def __init__(self, record: Dict[str, Any], thumbnail_png: Optional[bytes]) -> None:
        self.record = record
        self.thumbnail_png = thumbnail_png


def analyze_file(
    gc_path: Path,
    relpath: str,
    metadata: Optional[Dict[str, Any]] = None,
    cancel: Optional[threading.Event] = None,
    throttle: Optional[reader.ThrottleHook] = None,
    now: Optional[float] = None,
) -> AnalysisOutput:
    """Read gc_path/relpath once, write its layer index sidecar, and return
    its record plus a thumbnail PNG when Moonraker's metadata has none.
    Raises ScanCancelled or OSError."""
    full = gc_path / relpath
    identity = reader.file_identity(full)
    md = metadata or {}
    want_thumbnail = not md.get("thumbnails")
    collector = AnalysisCollector(want_thumbnail=want_thumbnail)
    with open(str(full), "rb") as f:
        result = reader.scan_to_offset(
            f, identity["file_size"], cancel=cancel, throttle=throttle,
            on_layer_start=collector.layer_start, observer=collector,
        )
    collector.finish()

    index_rel = None
    if collector.layer_starts:
        index_path = reader.layer_index_path(gc_path, relpath)
        os.makedirs(os.path.dirname(index_path), exist_ok=True)
        reader.write_layer_index(
            index_path, identity,
            [(s[0], s[1], s[2], s[3]) for s in collector.layer_starts],
            collector.progress_lines, result.offset,
            {"model_box": _box_out(collector.model_box),
             "palette": [c or "" for c in footer_palette(collector.config)],
             "initial_tool": collector.initial_tool,
             "tools": sorted(collector.tools_seen)},
        )
        index_rel = os.path.relpath(index_path, str(gc_path))

    footer = footer_tools(collector.config)
    used = tools_used(footer, collector.tool_mm, collector.tools_seen)
    tools = merge_tools(used, footer, _metadata_tools(md), collector.tool_mm)

    png = None
    if want_thumbnail:
        png = embedded_png(collector.thumbnails)
        if png is None and collector.footprint is not None:
            colors = {t["tool"]: t["color"] for t in tools}
            png = collector.footprint.render(colors)

    record = {
        "schema": RECORD_SCHEMA,
        "filename": relpath,
        "file_size": identity["file_size"],
        "file_mtime": identity["file_mtime"],
        "analyzed_at": time.time() if now is None else now,
        "slicer": collector.slicer,
        "tools": tools,
        "bounds": {
            "print": _box_out(collector.print_box),
            "travel": _box_out(collector.travel_box),
            "max_hotend_temp": collector.max_hotend,
            "max_bed_temp": collector.max_bed,
        },
        "header": header_from_config(collector.config),
        "objects": collector.objects(),
        "layers": {"count": len(collector.layer_starts), "index": index_rel},
        "pauses": collector.pauses,
        # A slicer profile with pressure advance on sets it for the whole job,
        # over any value measured or saved before the print.
        "sets_pressure_advance": any(
            "advance" in entry for entry in result.state_at_head.pressure_advance.values()),
        "thumbnail": None,
    }
    return AnalysisOutput(record, png)


def embedded_png(thumbnails: Sequence[Tuple[str, int, int, str]]) -> Optional[bytes]:
    """The largest embedded PNG thumbnail, decoded; None when there is none."""
    best = None
    for kind, w, h, data in thumbnails:
        if kind != "PNG":
            continue
        try:
            png = base64.b64decode(data, validate=False)
        except (ValueError, TypeError):
            continue
        if not png.startswith(b"\x89PNG"):
            continue
        if best is None or w * h > best[0]:
            best = (w * h, png)
    return None if best is None else best[1]


def png_size(png: bytes) -> Tuple[int, int]:
    if len(png) >= 24 and png.startswith(b"\x89PNG"):
        return struct.unpack(">II", png[16:24])
    return 0, 0


def thumbnail_relpath(relpath: str, width: int, height: int) -> str:
    """Where Moonraker keeps a file's thumbnails: <dir>/.thumbs/<stem>-<w>x<h>.png."""
    directory, name = os.path.split(relpath)
    stem = os.path.splitext(name)[0]
    return os.path.join(directory, ".thumbs", "%s-%dx%d.png" % (stem, width, height))


# ============================================================================
# The component
# ============================================================================


WorkerFailed = reader.WorkerFailed


def worker_command(gc_path: Path, relpath: str) -> List[str]:
    """The worker's argv: this file as a script (main())."""
    return reader.worker_argv(__file__, [str(gc_path), relpath])


# A worker past this is killed, and the attempt counts as failed.
worker_timeout = reader.worker_timeout


def encode_output(out: AnalysisOutput) -> bytes:
    png = out.thumbnail_png
    return json.dumps({
        "record": out.record,
        "thumbnail_png": None if png is None else base64.b64encode(png).decode("ascii"),
    }).encode("utf-8")


def decode_output(data: bytes) -> AnalysisOutput:
    """The worker's stdout as an AnalysisOutput; WorkerFailed when it is not one."""
    try:
        doc = json.loads(data.decode("utf-8"))
        record = doc["record"]
        png = doc.get("thumbnail_png")
        if not isinstance(record, dict) or record.get("schema") != RECORD_SCHEMA:
            raise ValueError("not a record")
        return AnalysisOutput(record, None if png is None else base64.b64decode(png))
    except (ValueError, KeyError, TypeError, AttributeError) as e:
        raise WorkerFailed("unreadable worker output: %s" % e)


def main(argv: Optional[List[str]] = None) -> int:
    """Worker: analyze argv[1] (gcodes root) / argv[2] (relative path) with
    the Moonraker metadata JSON on stdin; the result JSON on stdout."""
    args = sys.argv[1:] if argv is None else argv
    logging.basicConfig(stream=sys.stderr, level=logging.WARNING)
    if len(args) != 2:
        sys.stderr.write("usage: helix_analysis.py GCODES_ROOT RELPATH < metadata.json\n")
        return 2
    reader.worker_nice()
    raw = sys.stdin.read()
    metadata = json.loads(raw) if raw.strip() else {}
    out = analyze_file(Path(args[0]), args[1], metadata if isinstance(metadata, dict) else {})
    out_stream = getattr(sys.stdout, "buffer", sys.stdout)
    out_stream.write(encode_output(out))
    out_stream.flush()
    return 0


def _log_task_failure(task: "asyncio.Future") -> None:
    if not task.cancelled() and task.exception() is not None:
        logging.error("HelixPrint: analysis backfill failed: %r", task.exception())


class HelixAnalysis(file_queue.FileTracker):
    """Analyzes G-code files one at a time and answers for their records."""

    def __init__(self, helix: Any, cpu_count: Optional[int] = None) -> None:
        self.helix = helix
        self.server = helix.server
        self.records = {}  # type: Dict[str, Dict[str, Any]]
        self.printing = False
        self.cpu_count = os.cpu_count() if cpu_count is None else cpu_count
        self._queue = file_queue.WorkQueue(self._analyze_queued, self._queue_ready, "analysis")
        database = lambda: getattr(self.helix, "database", None)  # noqa: E731
        self._store = file_queue.RecordStore(database, DB_NAMESPACE, "the analysis")
        self._failure_store = file_queue.RecordStore(
            database, DB_FAILURES_NAMESPACE, "the failed analysis")
        self._executor = None  # type: Optional[Any]
        self._backfill_task = None  # type: Optional[asyncio.Future]
        # The file the worker is reading now; prune spares its index
        self._analyzing = None  # type: Optional[str]
        self._runner = reader.WorkerProcess()
        # path -> (size, mtime, failed attempts) of the version last tried
        self._failures = {}  # type: Dict[str, Tuple[int, float, int]]
        self._resume = None  # type: Optional[asyncio.Event]
        self._stale = False
        self.started = False
        # path -> futures of record_for() callers waiting on its analysis
        self._waiters = {}  # type: Dict[str, List[asyncio.Future]]

    # -- lifecycle -----------------------------------------------------------

    @property
    def gc_path(self) -> Optional[Path]:
        return getattr(self.helix, "gc_path", None)

    def availability(self) -> Tuple[bool, Optional[str]]:
        if self.gc_path is None:
            return False, "no_gcodes_dir"
        return True, None

    def register(self) -> None:
        self.server.register_endpoint("/server/helix/analysis", ["GET"], self.handle_analysis)
        self.server.register_event_handler("file_manager:filelist_changed",
                                           self.on_filelist_changed)
        self.server.register_event_handler("job_state:state_changed", self.on_job_state_changed)
        register = getattr(self.server, "register_notification", None)
        if register is not None:
            register(READY_EVENT)

    async def start(self) -> None:
        """Load records, read whether a print runs, and backfill."""
        self._resume = asyncio.Event()
        self._resume.set()
        await self._load_records()
        await self._read_print_state()
        self.started = True
        await self.backfill()

    async def close(self) -> None:
        self._queue.cancel()
        await self._runner.stop()
        if self._executor is not None:
            self._executor.shutdown(wait=False)

    # -- print state ---------------------------------------------------------

    async def _read_print_state(self) -> None:
        apis = getattr(self.helix, "klippy_apis", None)
        if apis is None:
            return
        try:
            status = await apis.query_objects({"print_stats": None})
        except Exception:
            return
        state = ((status or {}).get("print_stats") or {}).get("state")
        self._set_printing(state in ("printing", "paused"))

    def _set_printing(self, printing: bool) -> None:
        self.printing = printing
        allowed = reader.whole_file_analysis_allowed(printing, self.cpu_count)
        if self._resume is None:
            return
        if allowed:
            self._resume.set()
        else:
            self._resume.clear()
            self._runner.kill()

    async def on_job_state_changed(self, job_event: Any, prev_stats: Dict[str, Any],
                                   new_stats: Dict[str, Any]) -> None:
        state = new_stats.get("state", "")
        if state in ("printing", "paused"):
            self._set_printing(True)
        elif state in file_queue.IDLE_JOB_STATES:
            self._set_printing(False)

    # -- events --------------------------------------------------------------

    def tracking(self) -> bool:
        return self.started

    async def file_changed(self, path: str) -> None:
        if is_analyzable(path):
            self.enqueue(path)

    async def file_deleted(self, path: str) -> None:
        await self.forget(path)

    async def file_moved(self, old: str, new: str) -> None:
        if old in self.records and is_analyzable(new):
            await self._rekey(old, new)
        else:
            await self.forget(old)
            if is_analyzable(new):
                self.enqueue(new)

    # -- endpoint ------------------------------------------------------------

    async def handle_analysis(self, web_request: Any) -> Dict[str, Any]:
        filename = (web_request.get_str("filename", "") or "").lstrip("/")
        if not is_analyzable(filename) or ".." in filename.split("/"):
            raise self.server.error("not an analyzable G-code file: %s" % filename, 400)
        if not await self.helix.feature_enabled(ANALYSIS_FEATURE):
            return {"filename": filename, "status": "disabled"}
        full = self.gc_path / filename
        try:
            st = os.stat(str(full))
        except OSError:
            raise self.server.error("file not found: %s" % filename, 404)
        record = self.records.get(filename)
        if record_current(record, st.st_size, st.st_mtime):
            return {"filename": filename, "status": "ready", "analysis": record}
        if self._gave_up(filename, st.st_size, st.st_mtime):
            return {"filename": filename, "status": "failed"}
        if self._stale:
            self._stale = False
            self._backfill_task = asyncio.ensure_future(self.backfill())
            self._backfill_task.add_done_callback(_log_task_failure)
        self.enqueue(filename, front=True)
        return {"filename": filename, "status": "pending"}

    async def record_for(self, path: str, timeout: float) -> Optional[Dict[str, Any]]:
        """The current record of `path`, analyzing it ahead of the queue when
        it has none. None when no record arrives within `timeout` seconds: the
        file is gone, its analysis failed, or the queue is held (switched off,
        or a print running on a one-core host)."""
        if self.gc_path is None or not is_analyzable(path):
            return None
        try:
            st = os.stat(str(self.gc_path / path))
        except OSError:
            return None
        record = self.records.get(path)
        if record_current(record, st.st_size, st.st_mtime):
            return record
        if self._gave_up(path, st.st_size, st.st_mtime):
            return None
        waiter = asyncio.get_running_loop().create_future()
        self._waiters.setdefault(path, []).append(waiter)
        self.enqueue(path, front=True)
        try:
            return await asyncio.wait_for(waiter, timeout)
        except asyncio.TimeoutError:
            return None
        finally:
            waiting = self._waiters.get(path, [])
            if waiter in waiting:
                waiting.remove(waiter)
            if not waiting:
                self._waiters.pop(path, None)

    def queue_held(self) -> bool:
        """Whether analysis waits now (a print running on a one-core host), so a
        record asked for cannot arrive until it ends."""
        return self._resume is not None and not self._resume.is_set()

    def _answer_waiters(self, path: str, record: Optional[Dict[str, Any]]) -> None:
        for waiter in self._waiters.pop(path, []):
            if not waiter.done():
                waiter.set_result(record)

    # -- queue ---------------------------------------------------------------

    def enqueue(self, path: str, front: bool = False) -> None:
        self._queue.put(path, front=front)

    async def wait_idle(self) -> None:
        await self._queue.wait_idle()

    async def _queue_ready(self) -> bool:
        if not await self.helix.feature_enabled(ANALYSIS_FEATURE):
            # Switched off: the queue drops; the next request while on backfills.
            self._stale = True
            return False
        if self._resume is not None:
            await self._resume.wait()
        return True

    async def _analyze_queued(self, path: str, _item: Any) -> None:
        try:
            await self.analyze(path)
        except reader.ScanCancelled:
            # A print started on a one-core host: back of the queue, run later.
            self.enqueue(path)
        except WorkerFailed as e:
            if await self._note_failure(path):
                logging.warning("HelixPrint: analysis of %s failed (%s), retrying", path, e)
                self.enqueue(path)
            else:
                logging.warning("HelixPrint: analysis of %s failed again (%s)", path, e)
                self._answer_waiters(path, None)

    async def backfill(self) -> None:
        """Queue every file without a current record, newest first; drop
        records of files that are gone."""
        if self.gc_path is None:
            return
        await self._run_in_thread(reader.prune_layer_indexes, self.gc_path,
                                  self.protected_indexes())
        found = await self._run_in_thread(
            file_queue.scan_files, self.gc_path, is_analyzable)
        for name in [n for n in set(self.records) | set(self._failures) if n not in found]:
            await self.forget(name)
        for name, (size, mtime) in sorted(found.items(), key=lambda kv: -kv[1][1]):
            if not record_current(self.records.get(name), size, mtime) \
                    and not self._gave_up(name, size, mtime):
                self.enqueue(name)

    def protected_indexes(self) -> List[str]:
        """Indexes power-loss recovery is using, which no prune may remove."""
        plr = getattr(self.helix, "plr", None)
        protected = getattr(plr, "protected_index_paths", None)
        paths = list(protected()) if protected is not None else []
        if self._analyzing is not None and self.gc_path is not None:
            paths.append(reader.layer_index_path(self.gc_path, self._analyzing))
        return paths

    # -- one file ------------------------------------------------------------

    def _metadata(self, path: str) -> Dict[str, Any]:
        fm = getattr(self.helix, "file_manager", None)
        get = getattr(fm, "get_file_metadata", None)
        if get is None:
            return {}
        try:
            md = get(path)
        except Exception:
            return {}
        return md if isinstance(md, dict) else {}

    async def _run_in_thread(self, fn: Any, *args: Any) -> Any:
        if self._executor is None:
            from concurrent.futures import ThreadPoolExecutor
            self._executor = ThreadPoolExecutor(
                max_workers=1, thread_name_prefix="helix-analysis",
                initializer=reader.lower_thread_priority)
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(self._executor, fn, *args)

    async def analyze(self, path: str) -> Optional[Dict[str, Any]]:
        if self.gc_path is None:
            return None
        full = self.gc_path / path
        try:
            st = os.stat(str(full))
        except OSError:
            await self.forget(path)
            return None
        if record_current(self.records.get(path), st.st_size, st.st_mtime):
            self._answer_waiters(path, self.records[path])
            return self.records[path]
        if self._gave_up(path, st.st_size, st.st_mtime):
            self._answer_waiters(path, None)
            return None
        self._analyzing = path
        try:
            out = await self._run_worker(path, self._metadata(path), st.st_size)
        finally:
            self._analyzing = None
        await self._clear_failure(path)
        if not await self._still_the_version(path, st):
            return None
        record = out.record
        if out.thumbnail_png is not None:
            record["thumbnail"] = await self._run_in_thread(
                self._publish_thumbnail, path, out.thumbnail_png)
            await self._add_metadata_thumbnail(path, record["thumbnail"])
        self.records[path] = record
        await self._save(path)
        self._notify(path)
        self._answer_waiters(path, record)
        return record

    async def _still_the_version(self, path: str, before: os.stat_result) -> bool:
        """After a pass: whether the file is still the version it read. A file
        deleted meanwhile is forgotten, sidecar included; one changed
        meanwhile loses the sidecar just written and is queued again."""
        try:
            st = os.stat(str(self.gc_path / path))
        except OSError:
            await self.forget(path)
            return False
        if (st.st_size, st.st_mtime) == (before.st_size, before.st_mtime):
            return True
        await self._run_in_thread(self._remove_files, path, None)
        self.enqueue(path)
        return False

    async def _run_worker(self, path: str, metadata: Dict[str, Any],
                          size: int) -> AnalysisOutput:
        """One file's pass in the worker process. Raises ScanCancelled when a
        print starting on a one-core host stopped it, else WorkerFailed."""
        try:
            out = await self._runner.run(
                worker_command(self.gc_path, path),
                json.dumps(metadata, default=str).encode("utf-8"), worker_timeout(size))
        except reader.WorkerStopped:
            raise reader.ScanCancelled()
        return decode_output(out)

    async def _note_failure(self, path: str) -> bool:
        """Count a failed attempt at the file's current version; True when it
        gets another. The count is stored once it gives up."""
        try:
            st = os.stat(str(self.gc_path / path))
        except (OSError, TypeError):
            return False
        size, mtime, count = self._failures.get(path, (st.st_size, st.st_mtime, 0))
        if (size, mtime) != (st.st_size, st.st_mtime):
            count = 0
        count += 1
        self._failures[path] = (st.st_size, st.st_mtime, count)
        if count >= MAX_ATTEMPTS:
            await self._failure_store.save(path, [st.st_size, st.st_mtime, count])
        return count < MAX_ATTEMPTS

    async def _clear_failure(self, path: str) -> None:
        if self._failures.pop(path, None) is not None:
            await self._failure_store.delete(path)

    def _gave_up(self, path: str, size: int, mtime: float) -> bool:
        failed = self._failures.get(path)
        return failed is not None and failed[:2] == (size, mtime) and failed[2] >= MAX_ATTEMPTS

    def _publish_thumbnail(self, path: str, png: bytes) -> Optional[Dict[str, Any]]:
        w, h = png_size(png)
        rel = thumbnail_relpath(path, w, h)
        dest = self.gc_path / rel
        try:
            dest.parent.mkdir(parents=True, exist_ok=True)
            reader.atomic_write_bytes(str(dest), png)
        except OSError as e:
            logging.warning("HelixPrint: could not write thumbnail for %s: %s", path, e)
            return None
        return {"relative_path": rel, "width": w, "height": h, "size": len(png),
                "generated": True}

    async def _add_metadata_thumbnail(self, path: str, thumb: Optional[Dict[str, Any]]) -> None:
        """List the published thumbnail in Moonraker's metadata for the file,
        as its own extraction would, so every client shows it. Best effort:
        a Moonraker without this storage shape keeps the file in .thumbs."""
        if thumb is None:
            return
        fm = getattr(self.helix, "file_manager", None)
        storage = getattr(fm, "gcode_metadata", None)
        insert = getattr(storage, "insert", None)
        get = getattr(storage, "get", None)
        if insert is None or get is None:
            return
        try:
            entry = get(path, None)
            if not isinstance(entry, dict) or entry.get("thumbnails"):
                return
            directory = os.path.dirname(path)
            entry = dict(entry)
            entry["thumbnails"] = [{
                "width": thumb["width"], "height": thumb["height"], "size": thumb["size"],
                # Moonraker's paths are relative to the file's own directory.
                "relative_path": os.path.relpath(thumb["relative_path"], directory or "."),
            }]
            result = insert(path, entry)
            if asyncio.iscoroutine(result):
                await result
        except Exception as e:
            logging.info("HelixPrint: could not add a thumbnail to %s's metadata: %s", path, e)

    def _notify(self, path: str) -> None:
        send = getattr(self.server, "send_event", None)
        if send is not None:
            send(READY_EVENT, {"filename": path})

    async def forget(self, path: str) -> None:
        """Drop a file's record, its layer index and a thumbnail we generated."""
        record = self.records.pop(path, None)
        self._queue.discard(path)
        self._answer_waiters(path, None)
        await self._clear_failure(path)
        if self.gc_path is not None:
            await self._run_in_thread(self._remove_files, path, record)
        await self._store.delete(path)

    def _remove_files(self, path: str, record: Optional[Dict[str, Any]]) -> None:
        targets = [reader.layer_index_path(self.gc_path, path)]
        thumb = (record or {}).get("thumbnail") or {}
        if thumb.get("generated") and thumb.get("relative_path"):
            targets.append(str(self.gc_path / thumb["relative_path"]))
        for target in targets:
            try:
                os.remove(target)
            except OSError:
                pass

    async def _rekey(self, old: str, new: str) -> None:
        record = self.records.pop(old, None)
        self._queue.discard(old)
        await self._clear_failure(old)
        if record is None:
            return
        if self.gc_path is not None:
            src = reader.layer_index_path(self.gc_path, old)
            dst = reader.layer_index_path(self.gc_path, new)
            try:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.replace(src, dst)
                record["layers"]["index"] = os.path.relpath(dst, str(self.gc_path))
            except OSError:
                record["layers"]["index"] = None
        # A thumbnail lives beside its file; the moved file gets a new one.
        record["thumbnail"] = None
        record["filename"] = new
        await self._store.delete(old)
        try:
            st = os.stat(str(self.gc_path / new)) if self.gc_path is not None else None
        except OSError:
            st = None
        if st is None or not record_current(record, st.st_size, st.st_mtime):
            self.enqueue(new)
            return
        self.records[new] = record
        await self._save(new)

    # -- persistence ---------------------------------------------------------

    async def _load_records(self) -> None:
        for name, record in (await self._store.load()).items():
            if isinstance(record, dict) and record.get("schema") == RECORD_SCHEMA:
                self.records[name] = record
        for name, failed in (await self._failure_store.load()).items():
            if isinstance(failed, list) and len(failed) == 3 \
                    and all(isinstance(v, (int, float)) for v in failed):
                self._failures[name] = (failed[0], failed[1], int(failed[2]))

    async def _save(self, path: str) -> None:
        if path in self.records:
            await self._store.save(path, self.records[path])


if __name__ == "__main__":
    sys.exit(main())
