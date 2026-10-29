# SPDX-License-Identifier: GPL-3.0-or-later
"""
Power-loss recovery for any Klipper printer: the HelixPrint plugin's PLR module.

Design and protocol: docs/devel/POWER_LOSS_RECOVERY.md, "Inside the plugin".

This module holds the save side (storage-safety verdict, PLR profile and
switch, the two-slot snapshot store, the save policy) and the offer rules.
helix_plr_scan.py finds the resume point, helix_plr_render.py renders the
resume scripts, and HelixPlr at the bottom is the Moonraker-facing component
helix_print.py creates. Everything above HelixPlr is pure and touches no
Moonraker object. Python 3.8, stdlib only.
"""

from __future__ import annotations

import json
import logging
import os
import shutil
import struct
import time
import zlib
from pathlib import Path
from typing import Any, Callable, Dict, List, NamedTuple, Optional, Tuple

# helix_print.py loads its siblings into sys.modules in dependency order.
import helix_gcode_reader
import helix_printer_cfg
import helix_plr_render
import helix_plr_scan

try:
    import helix_plr_qidi
except ImportError:  # the QIDI adapter is optional; generic PLR runs without it
    helix_plr_qidi = None

# ============================================================================
# Storage safety
# ============================================================================

STORAGE_CLASSES = ("emmc", "sd", "nand", "ssd", "unknown")

# Runtime evidence that outranks the printer DB hint.
_DEFINITE_RUNTIME = ("emmc", "sd", "ssd")

# Wear-tolerant whatever the source; raw NAND counts only on the DB's word.
_WEAR_TOLERANT = ("emmc", "ssd")


class StorageVerdict(NamedTuple):
    storage_class: str  # "emmc" | "ssd" | "nand" | "sd" | "unknown"
    wear_tolerant: bool
    source: str  # "runtime" | "printer_db" | "none"
    device: Optional[str]  # e.g. "mmcblk0"

    def to_dict(self) -> Dict[str, Any]:
        return dict(self._asdict())


def _read_text(path: Path) -> Optional[str]:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def classify_block_device(sys_root: Path, st_dev: int) -> Tuple[str, Optional[str]]:
    """(storage class, disk name) of the block device behind `st_dev`.

    `sys_root` is "/sys" on a host and a fake tree in tests. A partition
    resolves to its disk. A filesystem with no block device behind it
    (overlay, tmpfs, NFS) is "unknown".
    """
    node = sys_root / "dev" / "block" / ("%d:%d" % (os.major(st_dev), os.minor(st_dev)))
    try:
        disk = node.resolve(strict=True)
    except (OSError, RuntimeError):
        return "unknown", None
    if (disk / "partition").exists():
        disk = disk.parent
    name = disk.name
    if name.startswith("mmcblk"):
        kind = _read_text(disk / "device" / "type")
        if kind == "MMC":
            return "emmc", name
        if kind == "SD":
            return "sd", name
        return "unknown", name
    if name.startswith("nvme"):
        return "ssd", name
    if name.startswith("sd"):
        removable = _read_text(disk / "removable")
        rotational = _read_text(disk / "queue" / "rotational")
        if removable == "0" and rotational == "0":
            return "ssd", name
        return "unknown", name
    if name.startswith("ubi") or name.startswith("mtdblock"):
        return "nand", name
    return "unknown", name


def storage_verdict(
    runtime_class: str, runtime_device: Optional[str], db_hint: str
) -> StorageVerdict:
    """Runtime evidence wins when it is definite (emmc, sd, ssd); otherwise
    the printer DB's hint decides. Raw NAND is wear-tolerant only when the DB
    says the model has it."""
    if runtime_class in _DEFINITE_RUNTIME:
        return StorageVerdict(
            runtime_class, runtime_class in _WEAR_TOLERANT, "runtime", runtime_device
        )
    if db_hint in STORAGE_CLASSES and db_hint != "unknown":
        return StorageVerdict(
            db_hint, db_hint in _WEAR_TOLERANT or db_hint == "nand", "printer_db",
            runtime_device,
        )
    if runtime_class == "nand":
        return StorageVerdict("nand", False, "runtime", runtime_device)
    return StorageVerdict("unknown", False, "none", runtime_device)


def saving_enabled(verdict: StorageVerdict, tier: int, user_enabled: Optional[bool]) -> bool:
    """The user's switch when set; otherwise on only for wear-tolerant
    storage on a printer listed at tier 1 or above."""
    if user_enabled is not None:
        return user_enabled
    return verdict.wear_tolerant and tier >= 1


# ============================================================================
# PLR profile and switch
# ============================================================================

# helix-screen namespace keys: the app writes the resolved profile, and the
# user's on/off switch is PLR's only opt-in store.
PLR_PROFILE_KEY = "plr_profile"
PLR_USER_KEY = "plr_user"

BACKENDS = ("generic", "qidi_print_state", "vendor")

DEFAULT_XY_HOME_GCODE = "G28 X Y"
DEFAULT_Z_LIFT_MM = 5.0
DEFAULT_NOZZLE_STANDBY_DROP_C = 70.0
RESUME_SKIP_FRACTION_DEFAULT = 0.0


class PlrProfile(NamedTuple):
    tier: int = 0
    backend: str = "generic"
    storage_hint: str = "unknown"
    z_holds_unpowered: bool = False
    xy_home_gcode: str = DEFAULT_XY_HOME_GCODE
    z_lift_mm: float = DEFAULT_Z_LIFT_MM
    purge_gcode: Optional[str] = None
    resume_skip_fraction: float = RESUME_SKIP_FRACTION_DEFAULT
    macro_vars: Dict[str, Tuple[str, ...]] = {}
    # Once the nozzle has let go of the part, prepare drops it this far under
    # the print temperature until commit.
    nozzle_standby_drop_c: float = DEFAULT_NOZZLE_STANDBY_DROP_C


def _num(value: Any) -> Optional[float]:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if value != value or value in (float("inf"), float("-inf")):
        return None
    return float(value)


def parse_profile(record: Any) -> PlrProfile:
    """The app's plr_profile record. A missing record is tier 0, and a key
    that is missing or of the wrong type takes its default."""
    if not isinstance(record, dict):
        return PlrProfile()
    d = PlrProfile()
    tier = record.get("tier")
    tier = tier if isinstance(tier, int) and not isinstance(tier, bool) else d.tier
    backend = record.get("backend")
    storage = record.get("storage_hint")
    holds = record.get("z_holds_unpowered")
    xy_home = record.get("xy_home_gcode")
    lift = _num(record.get("z_lift_mm"))
    drop = _num(record.get("nozzle_standby_drop_c"))
    purge = record.get("purge_gcode")
    skip = _num(record.get("resume_skip_fraction"))
    macro_vars = {}  # type: Dict[str, Tuple[str, ...]]
    raw_vars = record.get("macro_vars")
    if isinstance(raw_vars, dict):
        for macro, names in raw_vars.items():
            if isinstance(macro, str) and isinstance(names, list):
                macro_vars[macro] = tuple(n for n in names if isinstance(n, str))
    return PlrProfile(
        tier=max(0, min(3, tier)),
        backend=backend if backend in BACKENDS else d.backend,
        storage_hint=storage if storage in STORAGE_CLASSES else d.storage_hint,
        z_holds_unpowered=holds if isinstance(holds, bool) else d.z_holds_unpowered,
        xy_home_gcode=xy_home if isinstance(xy_home, str) and xy_home.strip() else d.xy_home_gcode,
        z_lift_mm=(max(lift, helix_plr_render.MIN_LIFT_MM) if lift is not None and lift > 0.0
                   else d.z_lift_mm),
        nozzle_standby_drop_c=drop if drop is not None and drop >= 0.0
        else d.nozzle_standby_drop_c,
        purge_gcode=purge if isinstance(purge, str) and purge.strip() else None,
        resume_skip_fraction=(
            skip if skip is not None and 0.0 <= skip <= 1.0 else d.resume_skip_fraction
        ),
        macro_vars=macro_vars,
    )


def user_enabled(record: Any) -> Optional[bool]:
    """plr_user.enabled, or None when the user has not chosen."""
    if isinstance(record, dict) and isinstance(record.get("enabled"), bool):
        return record["enabled"]
    return None


# ============================================================================
# XY-homing safety
# ============================================================================


class HomingCheck(NamedTuple):
    xy_home_safe: bool
    # "override_homes_z" | "override_sets_z" | "override_unknown" | "delta_kinematics" | None
    reason: Optional[str]
    # A [homing_override] runs for G28 X Y (it may home only unhomed axes).
    has_override: bool = False


# Commands a homing_override may run without making `G28 X Y` unsafe. Anything
# else (a macro, SET_KINEMATIC_POSITION, a probe) may home or move Z, and the
# override cannot be read further.
_OVERRIDE_SAFE_COMMANDS = frozenset((
    "G0", "G1", "G4", "G90", "G91", "M400", "M204", "M220", "M221", "M114", "M117", "M118",
    "RESPOND", "SET_VELOCITY_LIMIT", "SET_TMC_CURRENT", "SAVE_GCODE_STATE",
    "RESTORE_GCODE_STATE",
))


def _override_problem(gcode: str) -> Optional[str]:
    """Why a homing_override's gcode makes `G28 X Y` unsafe, or None: a G28
    that homes Z (or a bare G28), or any command not known to be harmless.
    Template tags ({% ... %}, {{ ... }}) are dropped and the commands
    around them read, so a one-line `{% if %}_HOME_Z{% endif %}` counts."""
    import re
    for line in gcode.splitlines():
        line = re.sub(r"\{%.*?%\}|\{\{.*?\}\}|\{.*?\}", " ", line.split(";", 1)[0])
        words = line.upper().split()
        if not words:
            continue
        if words[0] == "G28":
            axes = [w for w in words[1:] if w[:1] in ("X", "Y", "Z")]
            if not axes or any(w[:1] == "Z" for w in axes):
                return "override_homes_z"
            continue
        if words[0] not in _OVERRIDE_SAFE_COMMANDS:
            return "override_unknown"
    return None


def check_xy_homing(configfile_settings: Any, kinematics: str) -> HomingCheck:
    """Whether `G28 X Y` homes only X and Y.

    A delta cannot home XY alone. A [homing_override] that runs for X or Y
    and whose gcode homes Z would bring the nozzle down onto the part.
    """
    if kinematics in ("delta", "rotary_delta", "deltesian"):
        return HomingCheck(False, "delta_kinematics")
    override = None
    if isinstance(configfile_settings, dict):
        override = configfile_settings.get("homing_override")
    if not isinstance(override, dict):
        return HomingCheck(True, None)
    axes = override.get("axes", "xyz")
    if isinstance(axes, list):
        axes = "".join(str(a) for a in axes)
    axes = str(axes).lower()
    if "x" not in axes and "y" not in axes:
        return HomingCheck(True, None)
    if override.get("set_position_z") is not None:
        # Klipper declares Z after the override runs, replacing the frame
        # prepare declared from the snapshot.
        return HomingCheck(False, "override_sets_z", True)
    gcode = override.get("gcode", "")
    if isinstance(gcode, list):
        gcode = "\n".join(str(g) for g in gcode)
    problem = _override_problem(str(gcode))
    if problem is not None:
        return HomingCheck(False, problem, True)
    return HomingCheck(True, None, True)


def effective_tier(profile: PlrProfile, homing: HomingCheck) -> Tuple[int, Optional[str]]:
    """The tier PLR acts at: 0 when `G28 X Y` is unsafe and the profile names
    no XY homing command of its own."""
    if not homing.xy_home_safe and profile.xy_home_gcode == DEFAULT_XY_HOME_GCODE:
        return 0, homing.reason
    return profile.tier, None


# ============================================================================
# Snapshot record
# ============================================================================

SNAPSHOT_SCHEMA = 1

# Persist at most this often while file_position moves.
SAVE_INTERVAL_S = 1.0

# Klipper objects a snapshot reads, besides the per-name families below.
SNAPSHOT_OBJECTS = (
    "print_stats", "virtual_sdcard", "toolhead", "motion_report", "gcode_move",
    "heater_bed", "fan", "firmware_retraction", "exclude_object", "bed_mesh",
)
_HEATER_PREFIXES = ("heater_generic ", "temperature_fan ")
_FAN_PREFIXES = ("fan_generic ",)


def _is_extruder(name: str) -> bool:
    """extruder, extruder1, ...; not extruder_stepper."""
    return name.startswith("extruder") and name[len("extruder"):].isdigit() \
        or name == "extruder"


def _is_heater(name: str) -> bool:
    return _is_extruder(name) or name == "heater_bed" or name.startswith(_HEATER_PREFIXES)


def snapshot_objects(object_list: Any) -> Dict[str, Any]:
    """The subscription for a printer's object list: every object a snapshot
    reads, with all fields (None)."""
    names = set(SNAPSHOT_OBJECTS)
    for name in object_list or ():
        if not isinstance(name, str):
            continue
        if _is_heater(name) or name.startswith(_FAN_PREFIXES) \
                or name.startswith("gcode_macro "):
            names.add(name)
    available = set(object_list or ())
    return {n: None for n in sorted(names) if n in available or not available}


# The identity a snapshot and a layer index are checked against; one rule for
# the whole plugin.
file_hash = helix_gcode_reader.file_hash
file_identity = helix_gcode_reader.file_identity


def _vec(value: Any, n: int = 4) -> Optional[list]:
    if isinstance(value, (list, tuple)) and len(value) >= n:
        out = [_num(v) for v in value[:n]]
        if all(v is not None for v in out):
            return out
    return None


def _obj(status: Dict[str, Any], name: str) -> Dict[str, Any]:
    value = status.get(name)
    return value if isinstance(value, dict) else {}


def build_record(status: Dict[str, Any], now: float,
                 macro_vars: Optional[Dict[str, Tuple[str, ...]]] = None) -> Dict[str, Any]:
    """The fast and slow blocks of a snapshot from Klipper's merged status.
    `seq` is the store's to add; identity and mesh live in the job record."""
    ps = _obj(status, "print_stats")
    info = ps.get("info") if isinstance(ps.get("info"), dict) else {}
    vsd = _obj(status, "virtual_sdcard")
    th = _obj(status, "toolhead")
    gm = _obj(status, "gcode_move")
    heaters = {}
    fans = {}
    pa = {}
    macros = {}
    for name in sorted(status):
        obj = _obj(status, name)
        if _is_heater(name):
            heaters[name] = {"target": _num(obj.get("target")), "temp": _num(obj.get("temperature"))}
        if _is_extruder(name):
            pa[name] = {"advance": _num(obj.get("pressure_advance")),
                        "smooth_time": _num(obj.get("smooth_time"))}
        if name == "fan" or name.startswith(_FAN_PREFIXES):
            fans[name] = _num(obj.get("speed"))
        if name.startswith("gcode_macro ") and macro_vars:
            macro = name[len("gcode_macro "):]
            wanted = macro_vars.get(macro)
            if wanted:
                macros[macro] = {v: obj[v.lower()] for v in wanted if v.lower() in obj}
    fr = status.get("firmware_retraction")
    retraction = None
    if isinstance(fr, dict) and fr:
        retraction = {"length": _num(fr.get("retract_length")),
                      "speed": _num(fr.get("retract_speed")),
                      "extra": _num(fr.get("unretract_extra_length")),
                      "unretract_speed": _num(fr.get("unretract_speed"))}
    eo = _obj(status, "exclude_object")
    limits = {"velocity": _num(th.get("max_velocity")), "accel": _num(th.get("max_accel")),
              "scv": _num(th.get("square_corner_velocity")),
              "min_cruise_ratio": _num(th.get("minimum_cruise_ratio"))}
    layer = info.get("current_layer")
    total = info.get("total_layer")
    return {
        "schema": SNAPSHOT_SCHEMA,
        "saved_at": now,
        "file_position": vsd.get("file_position") if isinstance(vsd.get("file_position"), int) else None,
        "toolhead_pos": _vec(th.get("position")),
        "live_pos": _vec(_obj(status, "motion_report").get("live_position")),
        "gcode_pos": _vec(gm.get("gcode_position")),
        "layer": layer if isinstance(layer, int) else None,
        "total_layers": total if isinstance(total, int) else None,
        "print_duration": _num(ps.get("print_duration")),
        "filament_used": _num(ps.get("filament_used")),
        "gcode_offset": _vec(gm.get("homing_origin")),
        "abs_coord": gm.get("absolute_coordinates"),
        "abs_extrude": gm.get("absolute_extrude"),
        "speed": _num(gm.get("speed")),
        "speed_factor": _num(gm.get("speed_factor")),
        "extrude_factor": _num(gm.get("extrude_factor")),
        "heaters": heaters,
        "fans": fans,
        "active_extruder": th.get("extruder") if isinstance(th.get("extruder"), str) else None,
        "limits": limits,
        "pa": pa,
        "retraction": retraction,
        "objects": eo.get("objects") if isinstance(eo.get("objects"), list) else [],
        "excluded": eo.get("excluded_objects") if isinstance(eo.get("excluded_objects"), list) else [],
        "macro_vars": macros,
        "power_fail_seen": False,
    }


def mesh_record(status: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """The active bed mesh: its profile name, whether that names a
    stored profile, and what an unsaved mesh needs to be written back."""
    bm = _obj(status, "bed_mesh")
    name = bm.get("profile_name")
    matrix = bm.get("probed_matrix")
    if not isinstance(matrix, list) or not matrix or not isinstance(matrix[0], list) \
            or not matrix[0]:
        return None
    profiles = bm.get("profiles") if isinstance(bm.get("profiles"), dict) else {}
    saved = bool(name) and name in profiles
    params = None
    if saved and isinstance(profiles[name], dict):
        params = profiles[name].get("mesh_params")
    return {
        "profile": name if isinstance(name, str) else "",
        "saved": saved,
        "min": _vec(bm.get("mesh_min"), 2),
        "max": _vec(bm.get("mesh_max"), 2),
        "params": params,
        "points": matrix,
    }


def mesh_cfg(mesh: Dict[str, Any], section: str = "helix_plr") -> Optional[str]:
    """An unsaved mesh as a `[bed_mesh <section>]` block in Klipper's autosave
    profile format. Klipper does not publish the live mesh's algorithm, so it
    is the one Klipper accepts for the point count (lagrange up to 6 points
    per axis, bicubic above)."""
    points = mesh.get("points")
    lo = mesh.get("min")
    hi = mesh.get("max")
    if not points or lo is None or hi is None:
        return None
    y_count = len(points)
    x_count = len(points[0])
    algo = "lagrange" if max(x_count, y_count) <= 6 else "bicubic"
    rows = ["\t  " + ", ".join("%.6f" % v for v in row) for row in points]
    lines = [
        "[bed_mesh %s]" % section,
        "version = 1",
        "points =",
    ] + rows + [
        "x_count = %d" % x_count, "y_count = %d" % y_count,
        "mesh_x_pps = 2", "mesh_y_pps = 2", "algo = %s" % algo, "tension = 0.2",
        "min_x = %.6f" % lo[0], "max_x = %.6f" % hi[0],
        "min_y = %.6f" % lo[1], "max_y = %.6f" % hi[1],
    ]
    return "\n".join(lines) + "\n"


# Fields whose change is saved at once rather than at the next interval.
_EVENT_KEYS = ("layer", "excluded", "speed_factor", "extrude_factor", "gcode_offset")


def _event_view(record: Dict[str, Any]) -> tuple:
    targets = tuple(sorted((k, v.get("target")) for k, v in record.get("heaters", {}).items()))
    fans = tuple(sorted(record.get("fans", {}).items()))
    return tuple(repr(record.get(k)) for k in _EVENT_KEYS) + (targets, fans)


class SavePolicy:
    """When to persist: while printing, at most every interval while
    file_position moves, and at once on a layer change, an exclude, a change
    of factors, offset, heater targets or fans, and on entering pause."""

    def __init__(self, interval_s: float = SAVE_INTERVAL_S) -> None:
        self.interval_s = interval_s
        self.reset()

    def reset(self) -> None:
        self._last_at = None  # type: Optional[float]
        self._last_position = None  # type: Optional[int]
        self._last_view = None  # type: Optional[tuple]
        self._last_state = None  # type: Optional[str]

    def decide(self, now: float, state: str, record: Dict[str, Any]) -> Optional[str]:
        prev_state = self._last_state
        self._last_state = state
        if state == "paused":
            return "pause" if prev_state == "printing" else None
        if state != "printing":
            return None
        view = _event_view(record)
        if self._last_view is not None and view != self._last_view:
            return "event"
        if self._last_at is None:
            return "first"
        if record.get("file_position") != self._last_position \
                and now - self._last_at >= self.interval_s:
            return "interval"
        return None

    def saved(self, now: float, record: Dict[str, Any]) -> None:
        self._last_at = now
        self._last_position = record.get("file_position")
        self._last_view = _event_view(record)


# ============================================================================
# Two-slot snapshot store
# ============================================================================

SLOT_SIZE = 4096
SLOT_MAGIC = b"HPLR"
# magic, seq, payload length, crc32 of (seq, length, payload)
_SLOT_HEADER = struct.Struct("<4sQII")
SLOT_PAYLOAD_MAX = SLOT_SIZE - _SLOT_HEADER.size

# Moved to the job record when the slot cannot hold them.
OVERFLOW_KEYS = ("objects", "macro_vars")


def _slot_crc(seq: int, payload: bytes) -> int:
    return zlib.crc32(struct.pack("<QI", seq, len(payload)) + payload) & 0xFFFFFFFF


def encode_slot(seq: int, payload: bytes) -> bytes:
    if len(payload) > SLOT_PAYLOAD_MAX:
        raise ValueError("payload of %d bytes exceeds a slot" % len(payload))
    header = _SLOT_HEADER.pack(SLOT_MAGIC, seq, len(payload), _slot_crc(seq, payload))
    return (header + payload).ljust(SLOT_SIZE, b"\0")


def decode_slot(data: bytes) -> Optional[Tuple[int, bytes]]:
    """(seq, payload) of a valid slot, else None."""
    if len(data) < _SLOT_HEADER.size:
        return None
    magic, seq, length, crc = _SLOT_HEADER.unpack_from(data)
    if magic != SLOT_MAGIC:
        return None
    payload = bytes(data[_SLOT_HEADER.size:_SLOT_HEADER.size + length])
    if _slot_crc(seq, payload) != crc:
        return None
    return seq, payload


def fit_record(record: Dict[str, Any]) -> Tuple[bytes, Dict[str, Any]]:
    """The slot payload, and what moved out of it to fit (OVERFLOW_KEYS)."""
    payload = json.dumps(record, separators=(",", ":")).encode("utf-8")
    if len(payload) <= SLOT_PAYLOAD_MAX:
        return payload, {}
    slim = dict(record)
    moved = {k: slim.pop(k) for k in OVERFLOW_KEYS if k in slim}
    slim["overflow"] = sorted(moved)
    payload = json.dumps(slim, separators=(",", ":")).encode("utf-8")
    if len(payload) > SLOT_PAYLOAD_MAX:
        raise ValueError("snapshot of %d bytes does not fit a slot" % len(payload))
    return payload, moved


class SlotStore:
    """Two fixed SLOT_SIZE slots written alternately with pwrite + fdatasync.

    The file is preallocated and never resized, so a save writes one data
    block and no inode metadata. A cut mid-write can only tear the slot being
    written; the other holds the previous snapshot.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self._fd = None  # type: Optional[int]
        self.seq = 0

    def open(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(str(self.path), os.O_RDWR | os.O_CREAT, 0o644)
        try:
            if os.fstat(fd).st_size != 2 * SLOT_SIZE:
                os.ftruncate(fd, 0)
                os.pwrite(fd, b"\0" * (2 * SLOT_SIZE), 0)
                os.fsync(fd)
                _fsync_dir(self.path.parent)
        except OSError:
            os.close(fd)
            raise
        self._fd = fd
        latest = self._read_latest()
        self.seq = latest[0] if latest is not None else 0

    def close(self) -> None:
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def _read_latest(self) -> Optional[Tuple[int, bytes]]:
        best = None
        for slot in (0, 1):
            found = decode_slot(os.pread(self._fd, SLOT_SIZE, slot * SLOT_SIZE))
            if found is not None and (best is None or found[0] > best[0]):
                best = found
        return best

    def load(self) -> Optional[Dict[str, Any]]:
        """The newest valid snapshot, with its seq; None when neither slot holds one."""
        latest = self._read_latest()
        if latest is None:
            return None
        try:
            record = json.loads(latest[1].decode("utf-8"))
        except ValueError:
            return None
        if not isinstance(record, dict):
            return None
        record["seq"] = latest[0]
        return record

    def write(self, payload: bytes) -> int:
        seq = self.seq + 1
        os.pwrite(self._fd, encode_slot(seq, payload), (seq % 2) * SLOT_SIZE)
        _sync_data(self._fd)
        self.seq = seq
        return seq

    def clear(self) -> None:
        os.pwrite(self._fd, b"\0" * (2 * SLOT_SIZE), 0)
        _sync_data(self._fd)


def _sync_data(fd: int) -> None:
    """Flush a file's data blocks: fdatasync where the OS has it, else fsync.

    macOS has no fdatasync. fsync also flushes metadata, so it is the stronger
    guarantee and is correct anywhere fdatasync is.
    """
    getattr(os, "fdatasync", os.fsync)(fd)


_fsync_dir = helix_gcode_reader.fsync_dir


# ============================================================================
# Layer checkpoints
# ============================================================================

# The follower runs when the print has moved this far past it, or this long
# after its last run.
FOLLOW_MIN_BYTES = 64 * 1024
FOLLOW_INTERVAL_S = 10.0


class LayerFollower:
    """Builds the printing file's layer index (helix_gcode_reader's sidecar)
    up to file_position while the print runs, so a resume scan starts at a
    layer instead of byte 0. It reads only bytes Klipper has already read,
    under a DutyCycleLimiter. step() and finish() run on the follower thread.
    """

    def __init__(self, path: Path, index_path: str, identity: Dict[str, Any],
                 limiter: Optional[Any] = None) -> None:
        self.path = path
        self.index_path = index_path
        self.identity = identity
        self.limiter = limiter
        self.offset = 0
        self.state = None  # type: Optional[Any]
        self.complete = False
        self._loaded = False
        self._written = -1
        self._writer = None  # type: Optional[Any]

    def _load(self) -> None:
        self._loaded = True
        summary = helix_gcode_reader.read_layer_index(self.index_path, self.identity)
        if summary is not None:
            self.complete = summary.complete
            if summary.last is not None:
                self.offset = summary.last.offset
                self.state = summary.last.state
                self._written = summary.last.offset
        self._writer = helix_gcode_reader.LayerIndexWriter(self.index_path, self.identity)
        if self.limiter is None:
            self.limiter = helix_gcode_reader.DutyCycleLimiter()

    def _on_layer(self, layer: int, offset: int, state: Any) -> None:
        if offset > self._written:
            self._writer.append(layer, offset, state)
            self._written = offset

    def step(self, file_position: int) -> None:
        if not self._loaded:
            self._load()
        if self.complete or file_position <= self.offset:
            return
        with open(str(self.path), "rb") as f:
            result = helix_gcode_reader.scan_to_offset(
                f, file_position, self.offset, self.state,
                throttle=self.limiter.pace, on_layer_start=self._on_layer,
            )
        self.offset = result.offset
        self.state = result.state_at_head
        if self._writer.inert:
            # A complete index of this file appeared (file analysis wrote it
            # whole): there is nothing left to follow.
            self.complete = True

    def finish(self, file_size: Optional[int]) -> None:
        """Close the index. A print that completed (file_size given) and was
        followed to the end marks it complete."""
        if self._writer is None:
            return
        if not self.complete and file_size is not None and self.offset >= file_size:
            self._writer.finish(self.offset)
            self.complete = True
        self._writer.close()


# ============================================================================
# Offer rules
# ============================================================================

# With no layer count, the offer waits for this much print time instead of layer 2.
OFFER_MIN_PRINT_S = 120.0

# A bed this far under its target when the plan is made has cooled long enough
# that adhesion is in doubt: the plan asks for accept_cold_bed.
COLD_BED_DIFF_C = 10.0


def bed_warnings(warnings: List[str], bed_target: float,
                 bed_now: Optional[float]) -> Tuple[List[str], bool]:
    """`warnings` with cold_bed set from the bed's temperature now, and
    whether it is cold."""
    cold = bool(bed_target and bed_now is not None and bed_now < bed_target - COLD_BED_DIFF_C)
    out = [w for w in warnings if w != "cold_bed"]
    if cold:
        out.append("cold_bed")
    return out, cold


class OfferInputs(NamedTuple):
    newest_job_status: Optional[str]  # Moonraker history
    snapshot_job_id: Optional[str]
    newest_job_id: Optional[str]
    identity_ok: bool
    layer: Optional[int]
    tier: int
    klippy_ready: bool
    print_duration: Optional[float] = None
    resume_rows: bool = True  # QIDI backend: its print_state file_position row exists
    lift_room: bool = True  # MIN_LIFT_MM fits under the top of Z travel


class OfferVerdict(NamedTuple):
    available: bool
    blocked_by: List[str]

    def to_dict(self) -> Dict[str, Any]:
        return {"available": self.available, "blocked_by": list(self.blocked_by)}


def offer_verdict(i: OfferInputs) -> OfferVerdict:
    """Every offer rule; blocked_by lists each one that fails."""
    blocked = []
    if i.newest_job_status != "interrupted":
        blocked.append("not_interrupted")
    if i.snapshot_job_id is None or i.snapshot_job_id != i.newest_job_id or not i.resume_rows:
        blocked.append("no_snapshot")
    if not i.identity_ok:
        blocked.append("file_changed")
    if i.layer is not None:
        if i.layer < 2:
            blocked.append("too_early")
    elif (i.print_duration or 0.0) < OFFER_MIN_PRINT_S:
        blocked.append("too_early")
    if i.tier < 1:
        blocked.append("not_supported")
    if not i.lift_room:
        blocked.append("no_lift_room")
    if not i.klippy_ready:
        blocked.append("klippy_not_ready")
    return OfferVerdict(not blocked, blocked)


# ============================================================================
# The mesh-include line in printer.cfg
# ============================================================================

def mesh_include_present(text: str) -> bool:
    """True when printer.cfg's text carries the include line, whosever hand
    added it: a resume then reloads the saved mesh."""
    return helix_printer_cfg.include_present(text, MESH_INCLUDE_LINE)


def add_mesh_include(text: str) -> Optional[str]:
    """`text` with the marker and include appended, or None when the include
    is already there."""
    return helix_printer_cfg.add_include(text, MESH_INCLUDE_MARKER, MESH_INCLUDE_LINE)


def strip_mesh_include(text: str) -> Tuple[str, int]:
    """`text` without the marker and include lines, and how many lines went."""
    return helix_printer_cfg.strip_include(text, MESH_INCLUDE_MARKER, MESH_INCLUDE_LINE)


def write_mesh_include_change(path: Path, old_text: str, new_text: str,
                              replace: Callable[[str, str], None] = os.replace) -> None:
    helix_printer_cfg.write_change(path, old_text, new_text, replace=replace)


def uninstall_mesh_include(config_dir: Path) -> Optional[str]:
    """The uninstaller's whole mesh-include step: strip the two lines from
    printer.cfg (the same backup and atomic write the endpoint uses) and
    delete the helix-plr directory. None on success, or the problem as a
    string - the caller reports it and carries on. Running the plugin's own
    rule here, rather than a second copy in the uninstaller, is what keeps
    "which lines are ours" answered in one place."""
    cfg = config_dir / "printer.cfg"
    try:
        try:
            text = helix_gcode_reader.read_file_text(str(cfg))
        except (OSError, UnicodeDecodeError) as e:
            return "printer.cfg unreadable: %s" % e
        new_text, removed = strip_mesh_include(text)
        if removed:
            write_mesh_include_change(cfg, text, new_text)
        if (config_dir / "helix-plr").is_dir():
            shutil.rmtree(str(config_dir / "helix-plr"))
    except OSError as e:
        return str(e)
    return None


# ============================================================================
# The Moonraker-facing component
# ============================================================================

PLR_FEATURE = "plr"
PLR_FEATURE_VERSION = 2

# The app's settings namespace, where plr_profile and plr_user live.
HELIX_SCREEN_NAMESPACE = "helix-screen"

# PLR's own namespace: `job` (identity, mesh, overflow, finish status) and
# `newest_job` (the last job history added, whatever its client).
PLR_NAMESPACE = "helix_plr"
JOB_KEY = "job"
NEWEST_JOB_KEY = "newest_job"
# The resume about to start: {start_name, history_name} (the original name
# when it starts through RESUME_LINK), read when history adds its job.
RESUMING_KEY = "resuming"

SNAPSHOT_FILE = Path("helix-plr") / "snapshot.bin"
MESH_CFG_FILE = Path("helix-plr") / "mesh.cfg"

# The two lines the mesh carry-over needs in printer.cfg: a marker naming who
# added the include, and the include itself. Both are matched on their
# stripped text, so surrounding whitespace never defeats a removal, and a
# user's own include of the directory in any other spelling never matches.
MESH_INCLUDE_MARKER = (
    "# Added by HelixScreen: lets a power-loss resume reload the print's bed mesh")
MESH_INCLUDE_LINE = "[include helix-plr/*.cfg]"
# printer.cfg's single rolling backup, beside the file.
PRINTER_CFG_BACKUP_SUFFIX = helix_printer_cfg.BACKUP_SUFFIX

# How long the switch's answer is reused by the 4 Hz status path.
SWITCH_CACHE_S = 5.0

# History statuses after which the snapshot is of no further use.
_CLEARED_STATUSES = ("completed", "cancelled")


class HelixPlr:
    """PLR inside a running Moonraker. helix_print.py creates one in its
    component_init, after its database and Klipper APIs are resolved, and
    reaches PLR only through this class."""

    def __init__(self, helix: Any, data_dir: Path, sys_root: Path = Path("/sys"),
                 clock: Callable[[], float] = time.time) -> None:
        self.helix = helix
        self.server = helix.server
        self.database = helix.database
        self.data_dir = data_dir
        self.sys_root = sys_root
        self.clock = clock
        self.storage = StorageVerdict("unknown", False, "none", None)
        self.runtime_storage = ("unknown", None)  # type: Tuple[str, Optional[str]]
        self.has_virtual_sdcard = True
        self.store_error = None  # type: Optional[str]
        self.store = SlotStore(data_dir / SNAPSHOT_FILE)
        self.policy = SavePolicy()
        # Klipper's status, merged from the subscription's updates.
        self.status = {}  # type: Dict[str, Any]
        self.job = None  # type: Optional[Dict[str, Any]]
        self._switch = (False, float("-inf"))  # (value, read at)
        # The profile's tier as last read, for availability(): a sync probe
        # cannot read the database itself.
        self.listed_tier = 0
        self._writing = False
        self._macro_vars = {}  # type: Dict[str, Tuple[str, ...]]
        self.follower = None  # type: Optional[LayerFollower]
        self._follow_future = None  # type: Optional[Any]
        self._follow_at = float("-inf")
        self._follow_executor = None  # type: Optional[Any]
        self.qidi = None  # type: Optional[Any]
        self._active_backend = "none"
        self._skip_chamber = False
        self.klippy_ready = False
        # on_klippy_ready has finished for the current Klippy connection.
        self._ready_settled = False
        self._ready_token = None  # type: Optional[object]
        self.homing = HomingCheck(True, None)
        self.axis_min = None  # type: Optional[list]
        self.axis_max = None  # type: Optional[list]
        self.bed_min = None  # type: Optional[list]
        self.bed_max = None  # type: Optional[list]
        self.mesh_fade = None  # type: Optional[Dict[str, Any]]
        self.park_xy = None  # type: Optional[Tuple[float, float]]
        self._plan = None  # type: Optional[Any]
        self._plan_seq = None  # type: Optional[int]
        self._plan_executor = None  # type: Optional[Any]
        self._pending = None  # type: Optional[Any]
        self._pending_id = None  # type: Optional[str]
        self._sequence = None  # type: Optional[str]
        self._run = None  # type: Optional[_Run]
        self._bed_now = None  # type: Optional[float]
        self._plan_file_size = None  # type: Optional[int]
        self._klippy_generation = 0
        self._prepared_generation = -1
        self._plan_error = None  # type: Optional[Tuple[Any, str, str]]
        import asyncio
        self._sleep = asyncio.sleep

    def detect_storage(self) -> Tuple[str, Optional[str]]:
        try:
            st_dev = os.stat(str(self.data_dir)).st_dev
        except OSError:
            return "unknown", None
        return classify_block_device(self.sys_root, st_dev)

    async def _setting(self, key: str) -> Any:
        if self.database is None:
            return None
        try:
            return await self.database.get_item(HELIX_SCREEN_NAMESPACE, key, None)
        except Exception:
            return None

    async def profile(self) -> PlrProfile:
        """The app's plr_profile record, read afresh. Every read also updates
        the tier availability() answers with; the features status reads it
        through the switch before the probe, so a profile the app writes shows
        on the next status."""
        profile = parse_profile(await self._setting(PLR_PROFILE_KEY))
        self.listed_tier = profile.tier
        return profile

    async def verdict(self, profile: Optional[PlrProfile] = None) -> StorageVerdict:
        if profile is None:
            profile = await self.profile()
        cls, dev = self.runtime_storage
        return storage_verdict(cls, dev, profile.storage_hint)

    async def feature_enabled(self) -> bool:
        """The `plr` entry's `enabled`: saving is on, or a saved snapshot is
        still waiting to be resumed or discarded. Turning saving off stops new
        snapshots; it does not withdraw the offer for one already saved."""
        if await self.saving_enabled():
            return True
        return self.job is not None and self._load_snapshot() is not None

    async def feature_extra(self) -> Dict[str, Any]:
        """`saving`: whether snapshots are being saved (the switch as applied,
        and PLR able to run). The settings switch shows this, not `enabled`."""
        return {"saving": await self.saving_enabled() and self.availability()[0]}

    async def saving_enabled(self) -> bool:
        """The `plr` feature's switch: saving_enabled() over the storage
        verdict, the profile's tier and plr_user.enabled."""
        profile = await self.profile()
        verdict = await self.verdict(profile)
        self.storage = verdict
        return saving_enabled(verdict, profile.tier, user_enabled(await self._setting(PLR_USER_KEY)))

    def availability(self) -> Tuple[bool, Optional[str]]:
        import sys
        if sys.version_info < (3, 8):
            return False, "python_too_old"
        if not self.has_virtual_sdcard:
            return False, "no_virtual_sdcard"
        if self.store_error is not None:
            return False, "store_unwritable"
        if self.listed_tier < 1:
            # A printer the app's database does not list has no PLR profile.
            return False, "not_listed"
        return True, None

    def start(self) -> None:
        self.runtime_storage = self.detect_storage()
        try:
            self.store.open()
        except OSError as e:
            self.store_error = str(e)
            logging.warning("HelixPrint: PLR snapshot store unwritable: %s", e)
        self.server.register_event_handler("server:status_update", self.on_status_update)
        self.server.register_event_handler("server:klippy_ready", self.on_klippy_ready)
        self.server.register_event_handler("history:history_changed", self.on_history_changed)
        self.server.register_event_handler("server:klippy_shutdown", self.on_klippy_gone)
        self.server.register_event_handler("server:klippy_disconnect", self.on_klippy_gone)
        self.register_endpoints()

    async def load_job(self) -> None:
        """Read the job record, and the profile's tier, back after a restart."""
        await self.profile()
        record = await self._ns_get(JOB_KEY)
        self.job = record if isinstance(record, dict) else None

    # -- database ------------------------------------------------------------

    async def _ns_get(self, key: str) -> Any:
        if self.database is None:
            return None
        try:
            return await self.database.get_item(PLR_NAMESPACE, key, None)
        except Exception:
            return None

    async def _ns_put(self, key: str, value: Any) -> None:
        if self.database is None:
            return
        try:
            await self.database.insert_item(PLR_NAMESPACE, key, value)
        except Exception as e:
            logging.warning("HelixPrint: PLR could not store %s: %s", key, e)

    async def _ns_delete(self, key: str) -> None:
        if self.database is None:
            return
        try:
            await self.database.delete_item(PLR_NAMESPACE, key)
        except Exception:
            pass

    # -- Klipper -------------------------------------------------------------

    async def on_klippy_ready(self) -> None:
        """Read Klipper's objects, homing config and backend for this
        connection, then announce the status they decide (PLR_STATUS_EVENT)."""
        token = object()
        self._ready_token = token
        try:
            await self._klippy_ready()
        except Exception:
            # A status still answers, and is announced, with what was read.
            logging.exception("HelixPrint: PLR could not read Klipper at ready")
        finally:
            # Unless Klippy went away meanwhile and this is another connection.
            if self._ready_token is token:
                self._ready_settled = True
        if self._ready_token is token:
            try:
                self._notify(PLR_STATUS_EVENT, await self.status_reply())
            except Exception as e:
                logging.warning("HelixPrint: PLR could not announce its status: %s", e)

    async def _klippy_ready(self) -> None:
        apis = self.helix.klippy_apis
        if apis is None:
            return
        try:
            objects = await apis.get_object_list()
        except Exception as e:
            logging.warning("HelixPrint: PLR could not list Klipper objects: %s", e)
            return
        self.has_virtual_sdcard = "virtual_sdcard" in objects
        self.klippy_ready = True
        self._klippy_generation += 1
        try:
            await self._refresh_homing()
        except Exception as e:
            logging.warning("HelixPrint: PLR could not read the homing config: %s", e)
        try:
            status = await apis.subscribe_objects(snapshot_objects(objects))
        except Exception as e:
            logging.warning("HelixPrint: PLR could not subscribe: %s", e)
            return
        self.status = {}
        self._merge(status)
        await self._detect_qidi(apis)

    async def _detect_qidi(self, apis: Any) -> None:
        """Select the QIDI print_state adapter when Klipper is QIDI's fork AND
        this Moonraker carries QIDI's resume endpoint. snapshot_objects
        subscribes gcode_move and virtual_sdcard whole, so the adapter's
        EXTRA_STATUS_FIELDS arrive with every update."""
        self.qidi = None
        if helix_plr_qidi is None:
            return
        gcode_help = {}  # type: Any
        send = getattr(apis, "_send_klippy_request", None)
        if send is not None:
            try:
                gcode_help = await send("gcode/help", {}, {})
            except Exception:
                gcode_help = {}
        if helix_plr_qidi.qidi_print_state_present(gcode_help, self.status.get("gcode_move")) \
                and helix_plr_qidi.moonraker_resumes_print_state(apis, self.database):
            self.qidi = helix_plr_qidi.QidiPrintStateAdapter(
                self.database, apis, mark=self._mark_qidi,
                unmark=self.helix.unmark_helix_initiated)
            logging.info("HelixPrint: PLR resumes through QIDI's print_state")

    def _qidi_status(self) -> Dict[str, Any]:
        fields = helix_plr_qidi.EXTRA_STATUS_FIELDS if helix_plr_qidi is not None else {}
        out = {}
        for obj, names in fields.items():
            have = _obj(self.status, obj)
            out[obj] = {n: have[n] for n in names if n in have}
        return out

    def _merge(self, status: Any) -> None:
        if not isinstance(status, dict):
            return
        for name, fields in status.items():
            if isinstance(fields, dict):
                self.status.setdefault(name, {}).update(fields)

    async def _saving(self) -> bool:
        value, read_at = self._switch
        now = self.clock()
        if now - read_at >= SWITCH_CACHE_S:
            value = await self.saving_enabled() and self.availability()[0]
            profile = await self.profile()
            self._macro_vars = profile.macro_vars
            self._active_backend = self.backend(profile)[0]
            self._switch = (value, now)
        return value

    async def on_status_update(self, status: Dict[str, Any], *args: Any) -> None:
        self._merge(status)
        if self.job is None or self._writing or self.store_error is not None:
            return
        state = _obj(self.status, "print_stats").get("state", "")
        if state not in ("printing", "paused"):
            self.policy.decide(self.clock(), state, {})
            return
        # One frame at a time from here: a frame arriving during this one's
        # awaits must not persist an older record after it.
        self._writing = True
        try:
            await self._save_frame(state)
        finally:
            self._writing = False

    async def _save_frame(self, state: str) -> None:
        if not await self._saving():
            return
        now = self.clock()
        await self._note_print_path()
        record = build_record(self.status, now, self._macro_vars)
        record["job_id"] = self.job.get("job_id")
        reason = self.policy.decide(now, state, record)
        if reason is None:
            return
        await self._persist(now, record)
        await self._maybe_save_mesh(record)
        if self.qidi is not None and self._active_backend == "qidi_print_state":
            try:
                await self.qidi.on_snapshot(record, self._qidi_status(),
                                            force=reason in ("event", "pause"))
            except Exception:
                # The generic snapshot and the follower carry on regardless.
                logging.exception("HelixPrint: PLR could not write QIDI's print_state")
        self._maybe_follow(now, record.get("file_position"))

    async def _note_print_path(self) -> None:
        """Record the file Klipper is reading (virtual_sdcard.file_path).
        file_position counts its bytes, and on some firmware it is a copy (a
        cache file, or a plate extracted from a .3mf), not the gcodes file."""
        job = self.job
        path = _obj(self.status, "virtual_sdcard").get("file_path")
        if job is None or "print_path" in job or not isinstance(path, str) or not path:
            return
        try:
            identity = file_identity(Path(path))
        except OSError:
            return
        job["print_path"] = path
        job["print_identity"] = identity
        await self._ns_put(JOB_KEY, job)

    def print_target(self) -> Optional[Tuple[Path, Optional[str], Dict[str, Any]]]:
        """(file to scan, its layer index, the identity that index is for):
        the file Klipper read when known, else the job's file under gcodes."""
        gc_path = getattr(self.helix, "gc_path", None)
        job = self.job
        if gc_path is None or job is None:
            return None
        printed = job.get("print_path")
        if printed and job.get("print_identity") and Path(printed).exists():
            path = Path(printed).resolve()
            identity = job["print_identity"]
        elif job.get("file_hash"):
            relpath = _resolve_link(gc_path, job.get("filename", ""))
            if relpath is None:
                return None
            path = gc_path / relpath
            identity = job
        else:
            return None
        try:
            rel = str(path.relative_to(gc_path.resolve()))
        except ValueError:
            rel = str(Path(EXTERNAL_INDEX_DIR) / path.name)
        return path, helix_gcode_reader.layer_index_path(gc_path, rel), identity

    # -- layer checkpoints ---------------------------------------------------

    def resume_unaccounted(self) -> bool:
        """A snapshot is on disk but no job record names its files (the
        record could not be read back): what a resume would read is unknown,
        so every print copy and link must be kept."""
        return self.job is None and self._load_snapshot() is not None

    def protected_paths(self) -> List[str]:
        """Every file no cleanup may remove, for a live print and for a
        snapshot or offer waiting to resume: the file Klipper read (it can lie
        outside gcodes), the job's file under gcodes, the layer indexes
        (protected_index_paths, from print_target), the snapshot itself, and
        on QIDI the file its own resume opens from its cache
        (qidi_resume_filename). Each is listed as given and resolved, so the
        job file's resolved form is what _resolve_link names and the read
        file's is print_target's scan target."""
        gc_path = getattr(self.helix, "gc_path", None)
        job = self.job
        paths = list(self.protected_index_paths())
        paths.append(str(self.store.path))
        if job is not None:
            printed = job.get("print_path")
            if isinstance(printed, str) and printed:
                paths.append(printed)
            name = job.get("filename")
            if gc_path is not None and isinstance(name, str) and name:
                paths.append(os.path.join(str(gc_path), name))
        if self.qidi is not None and helix_plr_qidi is not None:
            cached = helix_plr_qidi.qidi_resume_filename(self.qidi._cache_dir)
            if cached:
                paths.append(cached)
        return sorted({q for p in paths for q in (os.path.abspath(p), os.path.realpath(p))})

    def protected_index_paths(self) -> List[str]:
        """The layer indexes no prune may remove: the one the follower is
        writing during a print, and the one the job's snapshot resumes from."""
        paths = []
        if self.follower is not None:
            paths.append(self.follower.index_path)
        target = self.print_target()
        if target is not None and target[1]:
            paths.append(target[1])
        return paths

    def _prune_indexes(self) -> None:
        """Drop stale layer indexes (helix_gcode_reader.prune_layer_indexes)
        except the ones in use, on the follower thread."""
        gc_path = getattr(self.helix, "gc_path", None)
        if gc_path is None:
            return
        future = self._executor().submit(
            helix_gcode_reader.prune_layer_indexes, gc_path, self.protected_index_paths())
        future.add_done_callback(self._follow_done)

    def _make_follower(self) -> Optional[LayerFollower]:
        target = self.print_target()
        if target is None:
            return None
        path, index_path, identity = target
        return LayerFollower(path, index_path, identity)

    def _executor(self) -> Any:
        if self._follow_executor is None:
            from concurrent.futures import ThreadPoolExecutor
            self._follow_executor = ThreadPoolExecutor(
                max_workers=1, thread_name_prefix="helix-plr-follower",
                initializer=helix_gcode_reader.lower_thread_priority)
        return self._follow_executor

    def _maybe_follow(self, now: float, file_position: Any) -> None:
        """Start a follower step when the print has moved far enough or long
        enough past the last one. One step runs at a time."""
        if not isinstance(file_position, int):
            return
        if self._follow_future is not None and not self._follow_future.done():
            return
        if self.follower is None:
            self.follower = self._make_follower()
            if self.follower is None:
                return
        if self.follower.complete:
            return
        behind = file_position - self.follower.offset
        if behind <= 0 or (behind < FOLLOW_MIN_BYTES and now - self._follow_at < FOLLOW_INTERVAL_S):
            return
        self._follow_at = now
        future = self._executor().submit(self.follower.step, file_position)
        future.add_done_callback(self._follow_done)
        self._follow_future = future

    @staticmethod
    def _follow_done(future: Any) -> None:
        if not future.cancelled() and future.exception() is not None:
            logging.warning("HelixPrint: PLR layer follower failed: %s", future.exception())

    async def _stop_follower(self, file_size: Optional[int]) -> None:
        follower = self.follower
        self.follower = None
        if follower is None:
            return
        import asyncio
        if self._follow_future is not None:
            try:
                await asyncio.wrap_future(self._follow_future)
            except Exception:
                pass
        self._follow_future = None
        try:
            await asyncio.wrap_future(self._executor().submit(follower.finish, file_size))
        except Exception as e:
            logging.warning("HelixPrint: PLR could not close the layer index: %s", e)

    def close(self) -> None:
        for executor in (self._follow_executor, self._plan_executor):
            if executor is not None:
                executor.shutdown(wait=False)
        self.store.close()

    async def _persist(self, now: float, record: Dict[str, Any]) -> None:
        try:
            payload, moved = fit_record(record)
        except ValueError as e:
            logging.warning("HelixPrint: PLR snapshot skipped: %s", e)
            return
        try:
            await self.server.get_event_loop().run_in_thread(self.store.write, payload)
        except OSError as e:
            logging.warning("HelixPrint: PLR snapshot write failed: %s", e)
            return
        self.policy.saved(now, record)
        if moved and self.job is not None and self.job.get("overflow") != moved:
            self.job["overflow"] = moved
            await self._ns_put(JOB_KEY, self.job)

    async def _maybe_save_mesh(self, record: Dict[str, Any]) -> None:
        """Once per job, when layer 1 is done."""
        if self.job is None or "mesh" in self.job:
            return
        layer = record.get("layer")
        if not isinstance(layer, int) or layer < 2:
            return
        mesh = mesh_record(self.status)
        self.job["mesh"] = mesh
        if mesh is not None and not mesh["saved"]:
            text = mesh_cfg(mesh)
            if text is not None:
                await self._write_mesh_cfg(text)
        await self._ns_put(JOB_KEY, self.job)

    def _config_dir(self) -> Optional[Path]:
        fm = getattr(self.helix, "file_manager", None)
        if fm is None:
            return None
        path = fm.get_directory("config")
        return Path(path) if path else None

    async def _write_mesh_cfg(self, text: str) -> None:
        config = self._config_dir()
        if config is None:
            return
        path = config / MESH_CFG_FILE

        def write() -> None:
            path.parent.mkdir(parents=True, exist_ok=True)
            tmp = path.with_suffix(".tmp")
            tmp.write_text(text)
            os.replace(str(tmp), str(path))

        try:
            await self.server.get_event_loop().run_in_thread(write)
        except OSError as e:
            logging.warning("HelixPrint: PLR could not write %s: %s", path, e)

    def _remove_mesh_cfg(self) -> None:
        config = self._config_dir()
        if config is not None:
            try:
                (config / MESH_CFG_FILE).unlink()
            except OSError:
                pass

    # -- history -------------------------------------------------------------

    async def on_history_changed(self, payload: Dict[str, Any]) -> None:
        job = payload.get("job") if isinstance(payload, dict) else None
        if not isinstance(job, dict) or not job.get("job_id"):
            return
        action = payload.get("action")
        if action == "added":
            await self._job_added(job)
        elif action == "finished":
            await self._job_finished(job)

    async def _job_added(self, job: Dict[str, Any]) -> None:
        job_id = str(job["job_id"])
        filename = job.get("filename", "")
        await self._ns_put(NEWEST_JOB_KEY, {"job_id": job_id, "filename": filename})
        await self._stop_follower(None)
        self._follow_at = self.clock()
        self.policy.reset()
        self._switch = (False, float("-inf"))
        if self.store_error is None:
            try:
                self.store.clear()
            except OSError as e:
                logging.warning("HelixPrint: PLR could not clear the snapshot: %s", e)
        record = {"schema": SNAPSHOT_SCHEMA, "job_id": job_id, "filename": filename,
                  "started_at": self.clock()}
        resuming = await self._ns_get(RESUMING_KEY)
        if isinstance(resuming, dict) and resuming.get("start_name") == filename:
            # A resumed job keeps the include-file mesh it was resumed with,
            # so another outage during it can load the same mesh.
            if resuming.get("history_name"):
                record["history_name"] = resuming["history_name"]
            await self._ns_delete(RESUMING_KEY)
        else:
            self._remove_mesh_cfg()
        if self.qidi is not None:
            self.qidi.on_job_start()
        self._prune_indexes()
        gc_path = getattr(self.helix, "gc_path", None)
        if gc_path is not None and filename:
            try:
                record.update(file_identity(gc_path / filename))
            except OSError as e:
                logging.warning("HelixPrint: PLR could not read %s: %s", filename, e)
        self.job = record
        await self._ns_put(JOB_KEY, record)

    async def _job_finished(self, job: Dict[str, Any]) -> None:
        if self.job is None or str(job["job_id"]) != self.job.get("job_id"):
            return
        if self.job.get("filename") == RESUME_LINK and self.job.get("history_name"):
            await self._finish_resume_link(job)
        status = job.get("status")
        await self._stop_follower(
            self.job.get("file_size") if status == "completed" else None)
        if status in _CLEARED_STATUSES:
            if self.qidi is not None:
                self.qidi.on_job_end()
            await self.discard()
            return
        self.job["finished_status"] = status
        await self._ns_put(JOB_KEY, self.job)

    async def _finish_resume_link(self, job: Dict[str, Any]) -> None:
        """History names a resumed job after RESUME_LINK; name it after the
        file instead, and remove the link once the job is over."""
        import types
        info = types.SimpleNamespace(
            job_id=self.job["job_id"], original_filename=self.job["history_name"],
            temp_filename=RESUME_LINK, symlink_filename=RESUME_LINK,
            modifications=["plr_resume"])
        patch = getattr(self.helix, "_patch_history_entry", None)
        if patch is not None and info.original_filename:
            await patch(info, job.get("status", ""))
        if job.get("status") in _CLEARED_STATUSES:
            gc_path = getattr(self.helix, "gc_path", None)
            if gc_path is not None and (gc_path / RESUME_LINK).is_symlink():
                try:
                    (gc_path / RESUME_LINK).unlink()
                except OSError:
                    pass

    async def discard(self) -> None:
        """Delete the snapshot, the job record and the mesh file. The layer
        index stays: it describes the file, not the job."""
        await self._stop_follower(None)
        self.job = None
        self.policy.reset()
        if self.store_error is None:
            try:
                self.store.clear()
            except OSError as e:
                logging.warning("HelixPrint: PLR could not clear the snapshot: %s", e)
        self._remove_mesh_cfg()
        await self._ns_delete(JOB_KEY)

    # =========================================================================
    # Resume: offer, plan, prepare, commit
    # =========================================================================

    def register_endpoints(self) -> None:
        for path, methods, handler in (
            ("/server/helix/plr/status", ["GET"], self.handle_status),
            ("/server/helix/plr/plan", ["POST"], self.handle_plan),
            ("/server/helix/plr/prepare", ["POST"], self.handle_prepare),
            ("/server/helix/plr/commit", ["POST"], self.handle_commit),
            ("/server/helix/plr/abort", ["POST"], self.handle_abort),
            ("/server/helix/plr/discard", ["POST"], self.handle_discard),
            ("/server/helix/plr/mesh_include", ["GET", "POST"], self.handle_mesh_include),
        ):
            self.server.register_endpoint(path, methods, handler)
        register = getattr(self.server, "register_notification", None)
        if register is not None:
            register(PROGRESS_EVENT)
            register(PLAN_READY_EVENT)
            register(PLR_STATUS_EVENT)

    def _error(self, code: str, message: str, status: int = 400) -> Exception:
        error = self.server.error("%s: %s" % (code, message), status)
        error.plr_code = code
        return error

    def _notify(self, event: str, data: Dict[str, Any]) -> None:
        send = getattr(self.server, "send_event", None)
        if send is not None:
            send(event, data)

    def on_klippy_gone(self, *args: Any) -> None:
        self.klippy_ready = False
        self._ready_settled = False
        self._ready_token = None
        self._klippy_generation += 1

    def _klippy_connection_ready(self) -> bool:
        conn = self.server.lookup_component("klippy_connection", None)
        is_ready = getattr(conn, "is_ready", None)
        try:
            return bool(is_ready()) if callable(is_ready) else False
        except Exception:
            return False

    def _klippy_coming_up(self) -> bool:
        """Klippy is ready, or connected and still starting up: either way
        on_klippy_ready is about to run or running."""
        if self._klippy_connection_ready():
            return True
        conn = self.server.lookup_component("klippy_connection", None)
        state = getattr(conn, "state", None)
        return str(getattr(state, "name", state)).lower() == "startup"

    async def _wait_ready_settled(self) -> None:
        """on_klippy_ready (objects, homing, the backend detection) has not
        finished for a Klippy that is up or coming up: an answer now would
        carry the backend and the offer of no connection at all. Wait for its
        result, success or failure, bounded under the app's request timeout.
        A Klippy that is down answers at once; its ready announces the status."""
        started = self.clock()
        while not self._ready_settled and self._klippy_coming_up() \
                and self.clock() - started < READY_WAIT_S:
            await self._sleep(READY_POLL_S)

    async def _query(self, objects: Dict[str, Any]) -> Dict[str, Any]:
        apis = self.helix.klippy_apis
        if apis is None:
            return {}
        try:
            result = await apis.query_objects(objects)
        except Exception as e:
            raise self._error("plr_klippy_error", str(e), 503)
        return result if isinstance(result, dict) else {}

    async def _refresh_homing(self) -> None:
        status = await self._query({"configfile": ["settings"],
                                    "toolhead": ["axis_minimum", "axis_maximum"]})
        settings = _obj(status, "configfile").get("settings")
        kinematics = ""
        if isinstance(settings, dict) and isinstance(settings.get("printer"), dict):
            kinematics = str(settings["printer"].get("kinematics", ""))
        self.homing = check_xy_homing(settings, kinematics)
        th = _obj(status, "toolhead")
        self.axis_min = _vec(th.get("axis_minimum"), 3)
        self.axis_max = _vec(th.get("axis_maximum"), 3)
        settings = settings if isinstance(settings, dict) else {}
        bm = settings.get("bed_mesh") if isinstance(settings.get("bed_mesh"), dict) else {}
        self.mesh_fade = {k: bm[k] for k in ("fade_start", "fade_end", "fade_target")
                          if isinstance(bm.get(k), (int, float))} or None
        self.bed_min = _vec(bm.get("mesh_min"), 2)
        self.bed_max = _vec(bm.get("mesh_max"), 2)
        sx = settings.get("stepper_x") if isinstance(settings.get("stepper_x"), dict) else {}
        sy = settings.get("stepper_y") if isinstance(settings.get("stepper_y"), dict) else {}
        px, py = _num(sx.get("position_endstop")), _num(sy.get("position_endstop"))
        self.park_xy = (px, py) if px is not None and py is not None else None

    async def _newest_job_status(self, newest: Optional[Dict[str, Any]]) -> Optional[str]:
        if not newest:
            return None
        job_id = newest.get("job_id")
        history = getattr(self.helix, "history", None)
        if history is not None and hasattr(history, "get_job"):
            try:
                job = await history.get_job(job_id)
            except Exception:
                job = None
            if isinstance(job, dict) and job.get("status"):
                return job["status"]
        if self.job is not None and self.job.get("job_id") == job_id \
                and self.job.get("finished_status"):
            return self.job["finished_status"]
        state = _obj(self.status, "print_stats").get("state")
        return "in_progress" if state in ("printing", "paused") else "interrupted"

    def _identity_ok(self) -> bool:
        job = self.job
        gc_path = getattr(self.helix, "gc_path", None)
        if job is None or gc_path is None or not job.get("file_hash"):
            return False
        try:
            now = file_identity(gc_path / job.get("filename", ""))
        except OSError:
            return False
        return all(now[k] == job.get(k) for k in ("file_size", "file_mtime", "file_hash"))

    def backend(self, profile: PlrProfile) -> Tuple[str, int]:
        """(backend, tier) PLR acts with; ("none", 0) when it does not act.

        - tier 0 (an unlisted printer) never acts;
        - tier 2 (trusted Z) needs z_holds_unpowered, else it acts at tier 1;
        - qidi_print_state needs QIDI's firmware and endpoint (self.qidi), and
          tier 2: QIDI's sequence declares the snapshot's Z itself, so there
          is no tier-1 jog on that path;
        - generic needs safe XY homing (effective_tier); tier 3 is not in v1
          and acts at tier 2.
        """
        tier = min(profile.tier, 2)
        if tier >= 2 and not profile.z_holds_unpowered:
            tier = 1
        if profile.backend == "qidi_print_state":
            if self.qidi is None or tier < 2:
                return "none", 0
            return "qidi_print_state", 2
        if profile.backend != "generic":
            return "none", 0
        homed_tier, _ = effective_tier(profile._replace(tier=tier), self.homing)
        return ("generic" if homed_tier >= 1 else "none"), homed_tier

    def _lift_room(self, profile: PlrProfile, snapshot: Optional[Dict[str, Any]]) -> bool:
        live = (snapshot or {}).get("live_pos")
        if self.backend(profile)[0] != "generic" or not live or not self.axis_max:
            return True
        return helix_plr_render.lift_height(profile.z_lift_mm, live[2], self.axis_max[2]) \
            >= helix_plr_render.MIN_LIFT_MM

    async def _offer(self, profile: PlrProfile,
                     snapshot: Optional[Dict[str, Any]]) -> Tuple[OfferVerdict, int]:
        backend, tier = self.backend(profile)
        newest = await self._ns_get(NEWEST_JOB_KEY)
        snapshot_job = None
        if self.job and snapshot and snapshot.get("job_id", self.job.get("job_id")) \
                == self.job.get("job_id"):
            snapshot_job = self.job.get("job_id")
        rows = True
        if backend == "qidi_print_state":
            rows = await self.qidi.has_resume_rows()
        inputs = OfferInputs(
            newest_job_status=await self._newest_job_status(newest),
            snapshot_job_id=snapshot_job,
            newest_job_id=newest.get("job_id") if isinstance(newest, dict) else None,
            identity_ok=self._identity_ok(),
            layer=snapshot.get("layer") if snapshot else None,
            tier=tier,
            klippy_ready=self.klippy_ready,
            print_duration=snapshot.get("print_duration") if snapshot else None,
            resume_rows=rows,
            lift_room=self._lift_room(profile, snapshot),
        )
        return offer_verdict(inputs), tier

    def _load_snapshot(self) -> Optional[Dict[str, Any]]:
        if self.store_error is not None:
            return None
        try:
            return self.store.load()
        except OSError:
            return None

    async def handle_status(self, web_request: Any) -> Dict[str, Any]:
        await self._wait_ready_settled()
        return await self.status_reply()

    async def status_reply(self) -> Dict[str, Any]:
        """The plr.status reply, also sent as PLR_STATUS_EVENT."""
        profile = await self.profile()
        snapshot = self._load_snapshot()
        verdict, tier = await self._offer(profile, snapshot)
        snap = None
        if snapshot is not None and self.job is not None:
            live = snapshot.get("live_pos") or [None, None, None]
            saved_at = snapshot.get("saved_at")
            snap = {
                "job_id": self.job.get("job_id"),
                "filename": self.job.get("history_name") or self.job.get("filename"),
                "layer": snapshot.get("layer"),
                "z": live[2],
                "saved_at": saved_at,
                "age_s": (self.clock() - saved_at) if isinstance(saved_at, (int, float)) else None,
            }
        backend, _ = self.backend(profile)
        return {
            "saving": await self.saving_enabled() and self.availability()[0],
            "storage": (await self.verdict(profile)).to_dict(),
            "tier": tier,
            "backend": backend,
            "snapshot": snap,
            "offer": verdict.to_dict(),
        }

    # -- plan ----------------------------------------------------------------

    def _plan_submit(self, fn: Callable[..., Any], *args: Any) -> Any:
        if self._plan_executor is None:
            from concurrent.futures import ThreadPoolExecutor
            self._plan_executor = ThreadPoolExecutor(
                max_workers=1, thread_name_prefix="helix-plr-plan")
        return self._plan_executor.submit(fn, *args)

    async def handle_plan(self, web_request: Any) -> Dict[str, Any]:
        if self._busy():
            raise self._error("plr_busy", "a resume is running", 409)
        snapshot = self._load_snapshot()
        profile = await self.profile()
        if self._plan is not None and snapshot is not None \
                and self._plan_seq == snapshot.get("seq") and self._sequence is None:
            return self._plan_reply(await self._with_live_bed(self._plan))
        if self._pending is not None and not self._pending.done():
            return {"pending": True, "plan_id": self._pending_id}
        if self._plan_error is not None and snapshot is not None \
                and self._plan_error[0] == snapshot.get("seq"):
            # A scan that failed in the background fails the same way again:
            # answer with its error until the snapshot changes.
            raise self._plan_error[1]
        await self._refresh_homing()
        verdict, tier = await self._offer(profile, snapshot)
        if not verdict.available:
            raise self._error("plr_no_plan", ",".join(verdict.blocked_by))
        state = _obj(await self._query({"print_stats": ["state"]}), "print_stats").get("state")
        if state in ("printing", "paused"):
            raise self._error("plr_busy", "printer is %s" % state, 409)
        live = snapshot.get("live_pos")
        if self.backend(profile)[0] == "generic" and (not isinstance(live, list) or len(live) < 4
                                  or any(v is None for v in live[:4])):
            # The measured nozzle position is the Z prepare declares; never guess it.
            raise self._error("plr_scan_failed", "no_live_position", 400)
        self._plan = None
        self._sequence = None
        plan_id = "%s-%d-%d" % (self.job["job_id"], snapshot["seq"], int(self.clock() * 1000))
        target = self.print_target()
        if target is None:
            raise self._error("plr_scan_failed", "the printed file is gone", 400)
        path, index_path, identity = target
        import asyncio
        future = self._plan_submit(
            _scan_and_locate, path, snapshot["file_position"], index_path,
            identity, snapshot, profile.resume_skip_fraction)
        self._pending = future
        self._pending_id = plan_id
        try:
            scan, point = await asyncio.wait_for(asyncio.shield(asyncio.wrap_future(future)),
                                                 PLAN_SYNC_WAIT_S)
        except asyncio.TimeoutError:
            async def finish() -> None:
                event = {"plan_id": plan_id}
                try:
                    found = await asyncio.wrap_future(future)
                    await self._finish_plan(plan_id, snapshot, profile, tier, *found)
                except Exception as e:
                    logging.warning("HelixPrint: PLR plan %s failed: %s", plan_id, e)
                    if not getattr(e, "plr_code", None):
                        e = self._error("plr_scan_failed", str(e), 500)
                    self._plan_error = (snapshot.get("seq"), e)
                    event["error"] = str(e)
                self._notify(PLAN_READY_EVENT, event)
            asyncio.ensure_future(finish())
            return {"pending": True, "plan_id": plan_id}
        except Exception as e:
            self._pending = None
            if not getattr(e, "plr_code", None):
                e = self._error("plr_scan_failed", str(e), 500)
            self._plan_error = (snapshot.get("seq"), e)
            raise e
        self._pending = None
        return self._plan_reply(await self._finish_plan(plan_id, snapshot, profile, tier, scan, point))

    async def _finish_plan(self, plan_id: str, snapshot: Dict[str, Any], profile: PlrProfile,
                           tier: int, scan: Any, point: Any) -> Any:
        if point is None:
            raise self._error("plr_scan_failed", "no resume point and no layer start", 500)
        snap_tool = snapshot.get("active_extruder")
        if (snap_tool not in (None, "extruder")) or (point.state.active_tool not in (None, 0)):
            raise self._error("plr_scan_failed", "multi_material: active tool %s / %s"
                              % (snap_tool, point.state.active_tool), 400)
        status = await self._query({"heater_bed": ["temperature", "target"],
                                    "bed_mesh": ["profiles"]})
        job = self.job or {}
        warnings = list(point.warnings)
        backend = self.backend(profile)[0]
        qidi = backend == "qidi_print_state"
        if not qidi:
            self._check_printed_file()
        mesh = job.get("mesh")
        profiles = _obj(status, "bed_mesh").get("profiles")
        profiles = profiles if isinstance(profiles, dict) else {}
        mesh_loadable = None
        # QIDI's own sequence loads the mesh and purges; only the generic one does here.
        if isinstance(mesh, dict) and not qidi:
            if mesh.get("saved") and mesh.get("profile") in profiles:
                mesh_loadable = mesh["profile"]
            elif MESH_SECTION in profiles:
                mesh_loadable = MESH_SECTION
            else:
                warnings.append("mesh_unavailable")
        heaters = snapshot.get("heaters") or {}
        bed_target = (heaters.get("heater_bed") or {}).get("target") or 0.0
        # The snapshot's target, or the file's at the resume point when higher:
        # a snapshot taken with the heater down still resumes at the
        # temperature the resume line prints at.
        nozzle = (heaters.get("extruder") or {}).get("target") or 0.0
        nozzle = max(nozzle, point.state.heater_targets.get("extruder", 0.0))
        if nozzle <= 0.0:
            raise self._error("plr_scan_failed", NO_NOZZLE_TARGET, 400)
        chamber = None
        for name, heater in sorted(heaters.items()):
            if name.startswith("heater_generic ") and "chamber" in name \
                    and (heater or {}).get("target"):
                chamber = (name.split(" ", 1)[1], heater["target"])
                break
        objects = snapshot.get("objects")
        if objects is None or "objects" in (snapshot.get("overflow") or []):
            objects = (job.get("overflow") or {}).get("objects") or []
        macro_vars = snapshot.get("macro_vars")
        if "macro_vars" in (snapshot.get("overflow") or []):
            macro_vars = (job.get("overflow") or {}).get("macro_vars")
        purge = None
        if not profile.purge_gcode and not qidi:
            bed = self._bed_limits(job.get("mesh"))
            if bed is not None:
                purge = helix_plr_render.generic_purge_xy(objects, bed[0], bed[1])
            if purge is None:
                warnings.append("no_purge_location")
        excluded = list(snapshot.get("excluded") or [])
        self._check_open_object(point.state.current_object, excluded)
        if qidi:
            self._check_qidi_landing(snapshot, job.get("mesh"))
        if tier == 1 and not qidi and purge is None and self.park_xy is None:
            # Nowhere known to be off the part: the nozzle would heat and ooze
            # over the resume point the jog left it at.
            raise self._error("plr_scan_failed", "no_purge_location: no purge line and no XY "
                              "homing position to heat the nozzle at", 400)
        part_top = _part_top_z(scan, point)
        if tier == 1 and part_top is None and not qidi:
            raise self._error("plr_scan_failed", "no_part_top: no extrusion before the resume point",
                              400)
        live = snapshot.get("live_pos") or [0.0, 0.0, 0.0, 0.0]
        bed_now = _num(_obj(status, "heater_bed").get("temperature"))
        warnings, bed_cold = bed_warnings(warnings, bed_target, bed_now)
        filename = job.get("filename", "")
        start_name, history_name = self._start_name(filename)
        plan = helix_plr_render.ResumePlan(
            plan_id=plan_id, filename=job.get("history_name") or filename,
            start_name=start_name, history_name=history_name,
            resume_offset=point.offset, state=point.state,
            layer=_resume_layer(point.state, snapshot.get("layer")),
            total_layers=snapshot.get("total_layers"),
            tier=tier, z_physical=live[2],
            lift_mm=helix_plr_render.lift_height(
                profile.z_lift_mm, live[2], self.axis_max[2] if self.axis_max else None),
            snapshot=snapshot, object_defines=list(scan.object_defines),
            excluded=excluded, mesh=mesh,
            mesh_loadable=mesh_loadable, purge_xy=purge, nozzle_target=nozzle,
            bed_target=bed_target, chamber=chamber, macro_vars=macro_vars or {},
            method=point.method, retrace_estimate_mm=point.retrace_estimate_mm,
            skip_estimate_mm=point.skip_estimate_mm, warnings=warnings,
            requires=helix_plr_render.required_confirmations(tier, warnings, bed_cold),
            part_top_z=part_top, mesh_fade=self.mesh_fade, park_xy=self.park_xy,
            backend=backend,
        )
        self._check_renders(plan, profile)
        self._plan = plan
        self._plan_seq = snapshot.get("seq")
        target = self.print_target()
        self._plan_file_size = os.stat(str(target[0])).st_size if target else None
        self._bed_now = bed_now
        return plan

    async def _with_live_bed(self, plan: Any) -> Any:
        """The kept plan, made current with the bed as it is now: the plan
        can be hours old, and the bed has cooled since. A prepare that took
        the plan during the read keeps it unchanged, so commit never asks for
        a confirmation the app was not shown; a failed read keeps the last
        reading."""
        try:
            status = await self._query({"heater_bed": ["temperature"]})
        except Exception as e:
            logging.warning("HelixPrint: PLR kept the last bed reading: %s", e)
            status = {}
        if self._plan is not plan or self._sequence is not None:
            return self._plan if self._plan is not None else plan
        bed_now = _num(_obj(status, "heater_bed").get("temperature"))
        if bed_now is None:
            return plan
        warnings, cold = bed_warnings(plan.warnings, plan.bed_target, bed_now)
        self._bed_now = bed_now
        self._plan = plan._replace(warnings=warnings, requires=helix_plr_render
                                   .required_confirmations(plan.tier, warnings, cold))
        return self._plan

    def _check_qidi_landing(self, snapshot: Dict[str, Any], mesh: Any) -> None:
        """QIDI's landing move is off by the gcode offset plus the mesh at the
        cut (helix_plr_qidi.frame_z); refuse when that would press the hot
        nozzle into the plastic."""
        offset = snapshot.get("gcode_offset") or [0.0, 0.0, 0.0]
        live = snapshot.get("live_pos") or snapshot.get("toolhead_pos") or [0.0, 0.0, 0.0]
        mesh_z = helix_plr_render.mesh_adjustment(mesh, live[0], live[1], live[2],
                                                  self.mesh_fade) or 0.0
        landing = (offset[2] or 0.0) + mesh_z
        if landing < -QIDI_LANDING_TOL_MM:
            raise self._error("plr_scan_failed",
                              "qidi_landing_below_part: %.3f mm" % landing, 400)

    def _check_open_object(self, name: Optional[str], excluded: List[str]) -> None:
        """The resume must not start inside an excluded object's block:
        neither backend would skip it (helix_plr_scan.leave_excluded_object
        moves the point past it). Refuse a plan that still does."""
        keys = {helix_gcode_reader.object_key(e) for e in excluded}
        if name and helix_gcode_reader.object_key(name) in keys:
            raise self._error("plr_scan_failed", "excluded_object_open: %s" % name, 400)

    def _check_renders(self, plan: Any, profile: PlrProfile) -> None:
        """Render what commit would send, so a name or value no command can
        carry refuses the plan, before prepare heats or moves anything."""
        try:
            helix_plr_render.render_start(plan)
            if plan.backend != "qidi_print_state":
                helix_plr_render.render_commit_script(
                    plan, profile, "part_top" if plan.tier == 1 else "trusted")
        except helix_plr_render.UnsafeValue as e:
            raise self._error("plr_scan_failed", "unsafe_value: %s" % e, 400)

    def _check_printed_file(self) -> None:
        """The generic start selects the job's file; the offset was found in
        the file Klipper read. Refuse when that was a different file (a copy
        or a .3mf plate)."""
        job = self.job or {}
        printed = job.get("print_path")
        gc_path = getattr(self.helix, "gc_path", None)
        if not printed or gc_path is None:
            return
        try:
            same = Path(printed).resolve() == (gc_path / job.get("filename", "")).resolve()
        except OSError:
            same = False
        if not same:
            raise self._error("plr_scan_failed",
                              "printed_copy: Klipper read %s, not the job's file" % printed, 400)

    def _bed_limits(self, mesh: Any) -> Optional[Tuple[Tuple[float, float], Tuple[float, float]]]:
        """The bed, not the travel: [bed_mesh] mesh_min/mesh_max, else the
        job's mesh bounds, clamped to the axis limits. The travel envelope
        often reaches a purge bucket, brush or clips past the bed."""
        lo, hi = self.bed_min, self.bed_max
        if (lo is None or hi is None) and isinstance(mesh, dict):
            lo, hi = mesh.get("min"), mesh.get("max")
        if not lo or not hi:
            return None
        if self.axis_min and self.axis_max:
            lo = [max(lo[i], self.axis_min[i]) for i in (0, 1)]
            hi = [min(hi[i], self.axis_max[i]) for i in (0, 1)]
        return (lo[0], lo[1]), (hi[0], hi[1])

    def _start_name(self, filename: str) -> Tuple[str, Optional[str]]:
        """M23 finds files at the gcodes root only; a file elsewhere, or one
        whose name M23 would cut, starts through RESUME_LINK."""
        if "/" not in filename and _m23_safe(filename):
            return filename, (self.job or {}).get("history_name")
        return RESUME_LINK, (self.job or {}).get("history_name") or filename

    def _plan_reply(self, plan: Any) -> Dict[str, Any]:
        _, _, z_gcode, _ = plan.state.gcode_position()
        return {
            "plan_id": plan.plan_id,
            "tier": plan.tier,
            "filename": plan.filename,
            "method": plan.method,
            "resume_offset": plan.resume_offset,
            "layer": plan.layer,
            "z_gcode": z_gcode,
            "z_physical": plan.z_physical,
            "retrace_estimate_mm": plan.retrace_estimate_mm,
            "bed": {"target": plan.bed_target, "current": self._bed_now},
            "restores": helix_plr_render.restores_summary(plan),
            "warnings": list(plan.warnings),
            "requires": list(plan.requires),
        }

    def _current_plan(self, plan_id: str) -> Any:
        plan = self._plan
        if plan is None:
            raise self._error("plr_no_plan", "no plan; request one first")
        snapshot = self._load_snapshot()
        if plan_id != plan.plan_id or snapshot is None or snapshot.get("seq") != self._plan_seq:
            raise self._error("plr_stale_plan", "plan %s is not the latest" % plan_id, 409)
        return plan

    # -- prepare / commit ----------------------------------------------------

    def _busy(self) -> bool:
        """A prepare or commit has not exited yet, whatever its label says
        (an aborted one runs until its current step returns)."""
        return self._run is not None and not self._run.finished

    def _check_abort(self) -> None:
        if self._run is not None and self._run.aborted:
            raise self._error("plr_klippy_error", "aborted", 409)

    async def _send(self, plan_id: str, step: str, lines: List[str], done: bool = True) -> None:
        """One step's G-code, refused when an abort arrived before it.
        `done` False leaves the step running for the caller to finish."""
        self._check_abort()
        self._notify(PROGRESS_EVENT, {"plan_id": plan_id, "step": step, "state": "running"})
        try:
            if lines:
                await self.helix.klippy_apis.run_gcode("\n".join(lines))
        except Exception as e:
            self._notify(PROGRESS_EVENT, {"plan_id": plan_id, "step": step, "state": "failed"})
            raise self._error("plr_klippy_error", str(e), 500)
        if done:
            self._notify(PROGRESS_EVENT, {"plan_id": plan_id, "step": step, "state": "done"})

    async def _wait_temp(self, plan_id: str, step: str, sensor: str, minimum: float,
                         timeout: float) -> None:
        """Poll a heater to `minimum` in slices an abort can interrupt; the
        heater's set command was sent by the step itself."""
        started = self.clock()
        while True:
            self._check_abort()
            temp = _num(_obj(await self._query({sensor: ["temperature"]}), sensor)
                        .get("temperature"))
            if temp is not None and temp >= minimum:
                break
            if self.clock() - started >= timeout:
                self._notify(PROGRESS_EVENT, {"plan_id": plan_id, "step": step, "state": "failed"})
                raise self._error("plr_klippy_error", "%s timed out at %s" % (step, temp), 504)
            await self._sleep(HEAT_POLL_S)
        self._notify(PROGRESS_EVENT, {"plan_id": plan_id, "step": step, "state": "done"})

    async def _heat(self, plan: Any, skip_chamber: bool = False,
                    nozzle_in_place: bool = False) -> None:
        """The heat wait: set every heater at once, then poll each to
        temperature: nothing waits for the bed before the nozzle starts."""
        heats = helix_plr_render.render_heat_wait(plan, skip_chamber, nozzle_in_place)
        for heat in heats:
            await self._send(plan.plan_id, heat.step, heat.lines, done=not heat.wait)
        for heat in heats:
            if heat.wait:
                timeout = {"heater_bed": BED_WAIT_TIMEOUT_S, "extruder": NOZZLE_WAIT_TIMEOUT_S} \
                    .get(heat.sensor, CHAMBER_WAIT_TIMEOUT_S)
                await self._wait_temp(plan.plan_id, heat.step, heat.sensor, heat.minimum, timeout)

    async def _heaters_off(self, plan: Any) -> None:
        lines = ["M104 S0", "M140 S0"]
        heaters = (plan.snapshot.get("heaters") or {}) if plan is not None else {}
        for name in sorted(heaters):
            if name.startswith("heater_generic "):
                lines.append("SET_HEATER_TEMPERATURE HEATER=%s TARGET=0" % name.split(" ", 1)[1])
        lines.append("M107")
        try:
            await self.helix.klippy_apis.run_gcode("\n".join(lines))
        except Exception as e:
            logging.warning("HelixPrint: PLR could not turn the heaters off: %s", e)

    async def _check_ready(self) -> Tuple[str, Optional[float]]:
        """Klipper ready and idle; returns (toolhead.homed_axes, toolhead Z)."""
        if not self.klippy_ready:
            raise self._error("plr_klippy_error", "Klipper is not ready", 503)
        status = await self._query({"print_stats": ["state"],
                                    "toolhead": ["homed_axes", "position"]})
        state = _obj(status, "print_stats").get("state")
        if state in ("printing", "paused"):
            raise self._error("plr_busy", "printer is %s" % state, 409)
        th = _obj(status, "toolhead")
        homed = th.get("homed_axes")
        position = _vec(th.get("position"), 3)
        return (homed.lower() if isinstance(homed, str) else ""), \
            (position[2] if position else None)

    def _claim(self, phase: str) -> Any:
        """Single flight: a new run only once the previous one has exited, and
        taken before the first await. Each run has its own abort flag."""
        if self._busy():
            raise self._error("plr_busy", "a resume is running", 409)
        previous = self._sequence
        self._sequence = phase
        self._run = _Run()
        return previous

    async def handle_prepare(self, web_request: Any) -> Dict[str, Any]:
        plan_id = _param(web_request, "plan_id", "")
        skip_chamber = bool(_param(web_request, "skip_chamber_wait", False))
        plan = self._current_plan(plan_id)
        if plan.tier < 1:
            raise self._error("plr_no_plan", "not_supported")
        if not plan.nozzle_target or plan.nozzle_target <= 0.0:
            # The nozzle is heated in place before anything moves; without a
            # target it would be dragged out of the plastic cold.
            raise self._error("plr_scan_failed", NO_NOZZLE_TARGET, 400)
        previous = self._claim("preparing")
        run = self._run
        try:
            try:
                homed, z_now = await self._check_ready()
                profile = await self.profile()
                motion = self._prepare_motion(plan, homed, z_now) \
                    if plan.backend == "generic" else None
            except Exception:
                self._sequence = previous
                raise
            self._skip_chamber = skip_chamber
            try:
                if plan.backend == "qidi_print_state":
                    await self.qidi.prepare(self._heat_wait(plan, skip_chamber, nozzle_in_place=True))
                    # No lift: QIDI declares Z from its row and lands on it.
                    await self._send(plan_id, "nozzle_standby",
                                     helix_plr_render.nozzle_standby_lines(plan, profile))
                else:
                    await self._heat(plan, skip_chamber, nozzle_in_place=True)
                if motion is not None:
                    declare, lift = motion
                    for step, lines in helix_plr_render.render_prepare_script(
                            plan, profile, declare, lift):
                        if step == "home_xy":
                            await self._check_set_homed(homed, profile)
                        await self._send(plan_id, step, lines)
                        if step == "lift":
                            await self._note_lift((declare if declare is not None else z_now)
                                                  + lift)
                    if plan.tier == 1:
                        # The jog moves the nozzle down from here.
                        await self._forget_lift()
                self._check_abort()
            except Exception:
                if self._sequence == "preparing":
                    self._sequence = "failed"
                await self._heaters_off(plan)
                raise
            self._sequence = "prepared"
            self._prepared_generation = self._klippy_generation
            return {"ok": True}
        finally:
            run.finished = True

    def _prepare_motion(self, plan: Any, homed: str,
                        z_now: Optional[float]) -> Tuple[Optional[float], float]:
        """(Z to declare, lift) for the generic prepare, before anything moves.

        Z homed: Klipper knows where the nozzle is, so nothing is declared
        and the nozzle always lifts first (by what fits under the top of
        travel). Z unhomed: the snapshot's Z on the first prepare, the
        recorded lifted Z when nothing has moved the nozzle since that lift,
        and otherwise nothing is known: refuse.
        """
        if "z" in homed:
            room = plan.lift_mm
            if self.axis_max and z_now is not None:
                room = min(room, self.axis_max[2] - z_now - helix_plr_render.SAG_ALLOWANCE_MM)
            return None, max(0.0, room)
        lifted = (self.job or {}).get("lifted")
        if not isinstance(lifted, dict) or lifted.get("seq") != self._plan_seq:
            return plan.z_physical, plan.lift_mm
        if lifted.get("valid") and _num(lifted.get("z")) is not None:
            return _num(lifted.get("z")), 0.0
        raise self._error("plr_stale_plan", "position_unknown: the nozzle moved after its lift "
                          "and Klipper lost Z; home Z away from the part, then resume", 409)

    async def _note_lift(self, z: float) -> None:
        """The nozzle is lifted at kinematic `z`, valid until something moves it down."""
        if self.job is not None:
            self.job["lifted"] = {"seq": self._plan_seq, "z": z, "valid": True}
            await self._ns_put(JOB_KEY, self.job)

    async def _forget_lift(self) -> None:
        """After a tier-1 prepare (the jog follows) and once commit starts, the
        recorded lift no longer says where the nozzle is."""
        lifted = (self.job or {}).get("lifted")
        if isinstance(lifted, dict) and lifted.get("valid"):
            lifted["valid"] = False
            await self._ns_put(JOB_KEY, self.job)

    async def _check_set_homed(self, homed_before: str, profile: PlrProfile) -> None:
        """Klipper builds older than SET_KINEMATIC_POSITION's SET_HOMED mark
        X and Y homed too; an XY homing that homes only unhomed axes (a
        conditional macro, an override) would then skip them. Refuse before
        homing in that case unless the XY homing is a plain `G28 X Y` with no
        [homing_override], which homes whatever Klipper believes."""
        homed_now, _ = await self._check_ready()
        faked = any(a in homed_now and a not in homed_before for a in "xy")
        lines = [" ".join(l.split(";", 1)[0].upper().split())
                 for l in profile.xy_home_gcode.splitlines()]
        plain = [l for l in lines if l] == ["G28 X Y"] and not self.homing.has_override
        if faked and not plain:
            raise self._error("plr_klippy_error", "set_homed_unsupported: this Klipper marks X and "
                              "Y homed with Z, and the homing override may skip them", 409)

    async def handle_commit(self, web_request: Any) -> Dict[str, Any]:
        plan_id = _param(web_request, "plan_id", "")
        given = _param(web_request, "confirmations", {})
        z_reference = _param(web_request, "z_reference", "")
        plan = self._current_plan(plan_id)
        if self._sequence != "prepared":
            raise self._error("plr_busy", "prepare has not finished", 409)
        missing = [c for c in plan.requires if not (isinstance(given, dict) and given.get(c) is True)]
        if missing:
            raise self._error("plr_missing_confirmation", ",".join(missing))
        expected = "part_top" if plan.tier == 1 else "trusted"
        if z_reference != expected:
            raise self._error("plr_missing_confirmation",
                              "z_reference must be %s at tier %d" % (expected, plan.tier))
        self._claim("committing")
        run = self._run
        try:
            try:
                homed, _ = await self._check_ready()
                profile = await self.profile()
                self._check_machine_unchanged(plan, homed)
            except Exception as e:
                self._sequence = None if getattr(e, "plr_code", None) == "plr_stale_plan" \
                    else "prepared"
                raise
            try:
                await self._forget_lift()
                if plan.backend == "qidi_print_state":
                    await self._commit_qidi(plan)
                else:
                    # Rendered before anything moves: a name it cannot send
                    # refuses the commit here, not after the purge.
                    start = helix_plr_render.render_start(plan)
                    # Back to print temperature where the nozzle stands, before
                    # the tier-1 Z reference or any other move.
                    await self._heat(plan, self._skip_chamber, nozzle_in_place=True)
                    for step, lines in helix_plr_render.render_commit_script(
                            plan, profile, z_reference):
                        await self._send(plan_id, step, lines, done=step != "heat_nozzle")
                        if step == "heat_nozzle":
                            await self._wait_temp(
                                plan_id, step, "extruder",
                                plan.nozzle_target - helix_plr_render.HEAT_TOLERANCE_C,
                                NOZZLE_WAIT_TIMEOUT_S)
                    await self._start(plan, start)
            except Exception:
                if self._sequence == "committing":
                    self._sequence = "failed"
                await self._heaters_off(plan)
                raise
            self._sequence = "done"
            self._plan = None
            return {"ok": True, "job_started": True}
        finally:
            run.finished = True

    def _check_machine_unchanged(self, plan: Any, homed: str) -> None:
        """Commit trusts the frame prepare set up: refuse it when Klipper
        restarted since, or the axes prepare homed are no longer homed (an
        idle timeout's motors-off, a restart). The answer is a new prepare."""
        if plan.backend != "generic":
            return
        needed = "xyz" if plan.tier >= 2 else "xy"
        if self._klippy_generation != self._prepared_generation \
                or any(axis not in homed for axis in needed):
            raise self._error("plr_stale_plan",
                              "Klipper lost its position since prepare; prepare again", 409)

    def _heat_wait(self, plan: Any, skip_chamber: bool = False,
                   nozzle_in_place: bool = False) -> Callable[[], Any]:
        """The heat wait as the coroutine the QIDI adapter awaits."""
        async def wait() -> None:
            await self._heat(plan, skip_chamber, nozzle_in_place)
        return wait

    def _mark_qidi(self, name: str, kind: str) -> None:
        """The QIDI adapter marks the job immediately before QIDI's resume
        call; an abort that arrived during its wait stops it here."""
        self._check_abort()
        self.helix.mark_helix_initiated(name, kind)

    async def _commit_qidi(self, plan: Any) -> None:
        self._check_qidi_cache()
        self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "running"})
        try:
            await self.qidi.commit(plan.snapshot, plan.resume_offset, self._plan_file_size,
                                   self._heat_wait(plan, self._skip_chamber, nozzle_in_place=True),
                                   plan.state,
                                   self._qidi_status())
        except Exception as e:
            self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "failed"})
            if getattr(e, "plr_code", None):
                raise
            raise self._error("plr_klippy_error", str(e), 500)
        self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "done"})

    def _check_qidi_cache(self) -> None:
        """QIDI resumes whatever file sits in its .cache; it must be the
        file the snapshot and the resume offset are for."""
        name = helix_plr_qidi.qidi_resume_filename(self.qidi._cache_dir)
        job = self.job or {}
        printed = job.get("print_path")
        expected = job.get("print_identity") or job
        try:
            found = file_identity(Path(name)) if name else None
        except OSError:
            found = None
        if found is None or found["file_size"] != expected.get("file_size") \
                or found["file_hash"] != expected.get("file_hash") \
                or (printed and Path(printed).resolve() != Path(name).resolve()):
            raise self._error("plr_scan_failed", "qidi_cache_mismatch: %s" % name, 409)

    async def _start(self, plan: Any, start: str) -> None:
        """Mark the job helix-initiated, then the single start command."""
        self._check_abort()
        gc_path = self.helix.gc_path
        link = None
        if plan.start_name == RESUME_LINK:
            link = gc_path / RESUME_LINK
            target = (gc_path / (self.job or {}).get("filename", plan.filename)).resolve()
            try:
                if link.is_symlink() or link.exists():
                    link.unlink()
                link.symlink_to(target)
            except OSError as e:
                raise self._error("plr_klippy_error", "cannot link %s: %s" % (RESUME_LINK, e), 500)
        # Read back by the resumed job's history event, also after a restart.
        await self._ns_put(RESUMING_KEY, {"start_name": plan.start_name,
                                          "history_name": plan.history_name})
        self._check_abort()
        self.helix.mark_helix_initiated(plan.start_name, "plr_resume", plan.history_name)
        self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "running"})
        try:
            await self.helix.klippy_apis.run_gcode(start)
        except Exception as e:
            self.helix.unmark_helix_initiated(plan.start_name)
            if link is not None:
                try:
                    link.unlink()
                except OSError:
                    pass
            self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "failed"})
            raise self._error("plr_klippy_error", str(e), 500)
        self._notify(PROGRESS_EVENT, {"plan_id": plan.plan_id, "step": "start", "state": "done"})

    async def handle_abort(self, web_request: Any) -> Dict[str, Any]:
        plan_id = _param(web_request, "plan_id", "")
        plan = self._plan
        if plan is None or plan.plan_id != plan_id:
            raise self._error("plr_stale_plan", "plan %s is not the latest" % plan_id, 409)
        if self._run is not None:
            self._run.aborted = True
        self._sequence = "aborted"
        self._plan = None
        if plan.backend == "qidi_print_state":
            self.qidi.abort()
        await self._heaters_off(plan)
        return {"ok": True}

    async def handle_discard(self, web_request: Any) -> Dict[str, Any]:
        if self._busy():
            raise self._error("plr_busy", "a resume is running", 409)
        self._plan = None
        self._sequence = None
        if self.qidi is not None and self.backend(await self.profile())[0] == "qidi_print_state":
            try:
                await self.qidi.discard()
            except Exception as e:
                raise self._error("plr_klippy_error", "print_state not deleted: %s" % e, 500)
        await self.discard()
        return {"ok": True}

    async def handle_mesh_include(self, web_request: Any) -> Dict[str, Any]:
        """Whether printer.cfg carries the mesh include (no action), or add
        or remove it (action). The status is a read; an action backs the
        file up first and writes atomically, and is idempotent both ways.
        Klipper reads the change at its next restart, which the caller
        offers, not this endpoint."""
        config = self._config_dir()
        if config is None:
            raise self._error("plr_config_read", "the config directory is unknown", 503)
        path = config / "printer.cfg"
        loop = self.server.get_event_loop()
        try:
            text = await loop.run_in_thread(helix_gcode_reader.read_file_text, str(path))
        except (OSError, UnicodeDecodeError) as e:
            raise self._error("plr_config_read", "printer.cfg unreadable: %s" % e, 500)
        action = _param(web_request, "action", "")
        if action == "":
            return {"present": mesh_include_present(text)}
        if action not in ("add", "remove"):
            raise self._error("plr_bad_action", "unknown action '%s'" % action, 400)
        if action == "add":
            new_text = add_mesh_include(text)
        else:
            stripped, removed = strip_mesh_include(text)
            new_text = stripped if removed else None
        if new_text is None:
            return {"present": mesh_include_present(text), "changed": False}
        try:
            await loop.run_in_thread(write_mesh_include_change, path, text, new_text)
        except OSError as e:
            raise self._error("plr_config_write", "printer.cfg unwritable: %s" % e, 500)
        logging.info("HelixPrint: PLR %s the mesh include in printer.cfg", action)
        return {"present": mesh_include_present(new_text), "changed": True}


# Notifications to clients: notify_plr_progress, notify_plr_plan_ready,
# notify_plr_status (the plr.status reply, once Klippy's connection is read).
PROGRESS_EVENT = "helix_print:plr_progress"
PLAN_READY_EVENT = "helix_print:plr_plan_ready"
PLR_STATUS_EVENT = "helix_print:plr_status"

# plr.status waits at most this long for on_klippy_ready to finish while Klippy
# is up or starting: on a Q2 Klippy's startup plus the QIDI check passes 15 s.
# The app's default request timeout is 60 s.
READY_WAIT_S = 45.0
READY_POLL_S = 0.25

# A plan whose scan takes longer replies {"pending": true} and announces itself.
PLAN_SYNC_WAIT_S = 10.0

# Neither the snapshot nor the file names a nozzle temperature to heat it to.
NO_NOZZLE_TARGET = "no_nozzle_target: no nozzle temperature to resume at"

# Heat waits poll the heater this often, and give up after these.
HEAT_POLL_S = 2.0
BED_WAIT_TIMEOUT_S = 20 * 60.0
NOZZLE_WAIT_TIMEOUT_S = 10 * 60.0
CHAMBER_WAIT_TIMEOUT_S = 45 * 60.0

# How far below the plastic QIDI's landing move may end before a QIDI resume
# is refused.
QIDI_LANDING_TOL_MM = 0.05

# The include-file mesh section.
MESH_SECTION = "helix_plr"

# A root-level link M23 can select when the file is in a subfolder.
RESUME_LINK = "helix_plr_resume.gcode"

# Layer indexes of printed files outside the gcodes root (a firmware's cache
# copy), under <gcodes>/.helix_index/.
EXTERNAL_INDEX_DIR = ".external"


class _Run:
    """One prepare or commit: its abort flag, and whether it has exited.
    A new run is refused until the previous one has exited."""

    def __init__(self) -> None:
        self.aborted = False
        self.finished = False


def _param(web_request: Any, key: str, default: Any) -> Any:
    getter = getattr(web_request, "get", None)
    if getter is not None:
        try:
            return getter(key, default)
        except Exception:
            return default
    return default


def _m23_safe(name: str) -> bool:
    return bool(name) and name == name.strip() and not name.startswith(".") \
        and not any(c in name for c in "#*;\n\r")


def _resolve_link(gc_path: Path, filename: str) -> Optional[str]:
    """The gcodes-relative path a (possibly linked) file really is."""
    try:
        real = (gc_path / filename).resolve()
        return str(real.relative_to(gc_path.resolve()))
    except (OSError, ValueError):
        return None


def _part_top_z(scan: Any, point: Any) -> Optional[float]:
    """G-code Z of the last extruding move before the resume point: the top of
    the plastic. The resume line's own Z may be a Z-hop or the next layer."""
    base_z = point.state.last_position[2] - point.state.gcode_position()[2]
    for rec in reversed(scan.window):
        if rec.offset < point.offset and rec.toolhead_de > 0.0:
            return rec.z - base_z
    return None


def _resume_layer(state: Any, snapshot_layer: Optional[int]) -> Optional[int]:
    """The layer print_stats reports at the resume line: the file's own
    SET_PRINT_STATS_INFO count there, which a point moved past an excluded
    block can have advanced, else the snapshot's."""
    if state.layer is not None and state.layer_source == "print_stats":
        return state.layer
    return snapshot_layer


def _scan_and_locate(path: Path, file_position: int, index_path: Optional[str],
                     identity: Optional[Dict[str, Any]], snapshot: Dict[str, Any],
                     skip_fraction: float) -> Tuple[Any, Any]:
    """The resume scan, on the plan thread."""
    with open(str(path), "rb") as f:
        scan = helix_plr_scan.scan_for_resume(f, file_position, index_path, identity)
        point = helix_plr_scan.resume_point(f, scan, snapshot, skip_fraction, SAVE_INTERVAL_S)
        if point is not None:
            point = helix_plr_scan.leave_excluded_object(f, point, snapshot.get("excluded") or [])
    return scan, point
