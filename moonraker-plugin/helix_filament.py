# SPDX-License-Identifier: GPL-3.0-or-later
"""
Filament accounting per lane and tool: the HelixPrint plugin's
`filament_accounting` feature.

Moonraker's spoolman component is the only thing that deducts filament. It
charges ONE active spool the rise of `toolhead.position[3]` above a high-water
mark, so usage is right exactly when the active spool is switched at every
tool or lane change. This module never deducts: it switches that active spool,
for the printers whose filament system does not, and keeps a per-job record of
what was charged to which spool from which tool and lane.

The screen keeps the map in Moonraker's database, `helix-screen` /
`filament_spools` (SPOOL_MAP_KEY): who already switches the active spool, and
the status fields that say what is feeding now, each value mapped to its tool,
lane and spool. This module reads that record and learns no backend.

Everything above FilamentAccounting is pure and touches no Moonraker object.
The rules shared with the app live in tests/fixtures/filament_accounting/
cases.json, which both suites read. Python 3.8, stdlib only.
"""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

FILAMENT_FEATURE = "filament_accounting"
FILAMENT_FEATURE_VERSION = 1

# Where the screen keeps the map: namespace helix-screen, this key.
SPOOL_MAP_KEY = "filament_spools"
SPOOL_MAP_VERSION = 1

# This module's own records: one per job id, the open job's ledger and the
# last job closed.
FILAMENT_NAMESPACE = "helix_filament"
ACTIVE_KEY = "active"
# The last job closed, which a power-loss resume continues.
LAST_KEY = "last"
# The id of the job history added last, counted or not: a power-loss resume
# continues only the job added right before it, the one that was interrupted.
LAST_ADDED_KEY = "last_added"
JOB_KEY_PREFIX = "job_"
RECORD_SCHEMA = 1

# Records kept, newest first by end time.
MAX_JOB_RECORDS = 500

# How often the open job's ledger is written while filament moves. A power cut
# loses at most this much of the record.
PERSIST_INTERVAL_S = 30.0

# The finished job's history auxiliary_data entry, where history can rewrite a
# job (get_job/save_job, Moonraker 0.9 and later).
AUX_NAME = "filament_spools"

# Who switches the active spool (active_spool_owner).
OWNER_NO_SPOOLMAN = "no_spoolman"
OWNER_BACKEND = "backend"
OWNER_PRINTER = "printer"
OWNER_MACROS = "macros"
OWNER_HELIX = "helix"

ACTION_SET = "set"
ACTION_CLEAR = "clear"

_RUNNING_STATES = ("printing", "paused")


def active_spool_owner(
    spoolman: bool, manages_active_spool: bool, printer_switches_active_spool: bool,
    macros_set_active_spool: bool
) -> str:
    """Who switches Moonraker's active spool on this printer. Only OWNER_HELIX
    lets this module (or the app's fallback) act; anything else would race the
    switcher that is already there: the filament system, the printer's
    firmware (the U1 extended firmware's SpoolLink switches it at every
    extruder change), or T<n> macros."""
    if not spoolman:
        return OWNER_NO_SPOOLMAN
    if manages_active_spool:
        return OWNER_BACKEND
    if printer_switches_active_spool:
        return OWNER_PRINTER
    if macros_set_active_spool:
        return OWNER_MACROS
    return OWNER_HELIX


def signal_key(value: Any) -> Optional[str]:
    """The lookup key of a status value: an integer (or an integral float)
    as its decimal digits, a non-empty string as itself. Anything else (a
    bool, a fraction, a list, None) is no key."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")):
            return None
        return str(int(value)) if value == int(value) else None
    if isinstance(value, str) and value:
        return value
    return None


def _int_or(value: Any, default: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        return default
    return value


def _spool_id(value: Any) -> Optional[int]:
    v = _int_or(value, 0)
    return v if v > 0 else None


class Feed:
    """What one signal value means: the tool and lane feeding, the spool
    linked to it (None when unlinked) and its grams per millimetre when the
    screen knows the filament."""

    __slots__ = ("tool", "lane", "spool_id", "grams_per_mm")

    def __init__(self, tool: int = -1, lane: int = -1, spool_id: Optional[int] = None,
                 grams_per_mm: Optional[float] = None) -> None:
        self.tool = tool
        self.lane = lane
        self.spool_id = spool_id
        self.grams_per_mm = grams_per_mm

    @classmethod
    def parse(cls, value: Any) -> Optional["Feed"]:
        if not isinstance(value, dict):
            return None
        gpm = value.get("grams_per_mm")
        if isinstance(gpm, bool) or not isinstance(gpm, (int, float)) or gpm <= 0:
            gpm = None
        return cls(_int_or(value.get("tool"), -1), _int_or(value.get("lane"), -1),
                   _spool_id(value.get("spool_id")), float(gpm) if gpm else None)

    def label(self) -> Tuple[int, int, Optional[int]]:
        return (self.tool, self.lane, self.spool_id)


class Signal:
    """A status field that says what is feeding: Klipper object, the path of
    keys inside it, and the feed each value stands for."""

    __slots__ = ("obj", "path", "values")

    def __init__(self, obj: str, path: Tuple[str, ...], values: Dict[str, Feed]) -> None:
        self.obj = obj
        self.path = path
        self.values = values

    def read(self, status: Dict[str, Any]) -> Optional[str]:
        node: Any = status.get(self.obj)
        for part in self.path:
            if not isinstance(node, dict):
                return None
            node = node.get(part)
        return signal_key(node)


class SpoolMap:
    """The screen's filament_spools record, parsed."""

    __slots__ = ("manages_active_spool", "printer_switches_active_spool",
                 "macros_set_active_spool", "signals")

    def __init__(self, manages: bool, printer: bool, macros: bool,
                 signals: List[Signal]) -> None:
        self.manages_active_spool = manages
        self.printer_switches_active_spool = printer
        self.macros_set_active_spool = macros
        self.signals = signals

    def objects(self) -> Dict[str, List[str]]:
        """What to subscribe for the signals: each object's top-level field."""
        objs: Dict[str, List[str]] = {}
        for sig in self.signals:
            fields = objs.setdefault(sig.obj, [])
            if sig.path[0] not in fields:
                fields.append(sig.path[0])
        return objs

    def mapped_spools(self) -> List[int]:
        seen: List[int] = []
        for sig in self.signals:
            for feed in sig.values.values():
                if feed.spool_id is not None and feed.spool_id not in seen:
                    seen.append(feed.spool_id)
        return seen


def parse_spool_map(record: Any) -> Optional[SpoolMap]:
    """The record, or None for an absent one, one of another version, or one
    that is malformed. A signal that is malformed is dropped on its own."""
    if not isinstance(record, dict) or record.get("version") != SPOOL_MAP_VERSION:
        return None
    signals: List[Signal] = []
    raw = record.get("signals")
    for item in raw if isinstance(raw, list) else []:
        if not isinstance(item, dict):
            continue
        obj = item.get("object")
        path = item.get("path")
        values = item.get("values")
        if not isinstance(obj, str) or not obj or not isinstance(values, dict):
            continue
        if not isinstance(path, list) or not path \
                or not all(isinstance(p, str) and p for p in path):
            continue
        feeds: Dict[str, Feed] = {}
        for key, value in values.items():
            feed = Feed.parse(value)
            if feed is not None:
                feeds[str(key)] = feed
        signals.append(Signal(obj, tuple(path), feeds))
    return SpoolMap(record.get("manages_active_spool") is True,
                    record.get("printer_switches_active_spool") is True,
                    record.get("macros_set_active_spool") is True, signals)


def feeding(spool_map: SpoolMap, status: Dict[str, Any]) -> Tuple[Optional[str], Optional[Feed]]:
    """The first signal whose value maps to a feed: (signal value, feed). With
    no mapped value, the first signal's value and None."""
    first: Optional[str] = None
    for i, sig in enumerate(spool_map.signals):
        key = sig.read(status)
        if i == 0:
            first = key
        if key is not None and key in sig.values:
            return "%d:%s" % (i, key), sig.values[key]
    return (None if first is None else "0:%s" % first), None


def spool_switch(
    feed: Optional[Feed], active: Optional[int], mapped: List[int]
) -> Optional[Tuple[str, Optional[int]]]:
    """What to do with the active spool for the feed just read: (ACTION_SET,
    id), (ACTION_CLEAR, None) or None. An unlinked feed clears only a spool
    the map links to another feed: one chosen outside the map is the user's."""
    if feed is None:
        return None
    if feed.spool_id is not None:
        return None if feed.spool_id == active else (ACTION_SET, feed.spool_id)
    if active is not None and active in mapped:
        return (ACTION_CLEAR, None)
    return None


class UsageLedger:
    """Millimetres charged per (tool, lane, spool), from the same position
    deltas Moonraker's spoolman component charges: the rise of the E
    position above its high-water mark, re-based without a charge when the
    extruder changes or the active spool is switched."""

    def __init__(self) -> None:
        # [tool, lane, spool_id, mm, grams_per_mm] per entry
        self.entries: List[List[Any]] = []
        self.highest_e: Optional[float] = None
        self.last_e: Optional[float] = None
        self.extruder: Optional[str] = None
        self.label: Tuple[int, int, Optional[int]] = (-1, -1, None)
        self.grams_per_mm: Optional[float] = None

    def baseline(self, e: Optional[float], extruder: Optional[str]) -> None:
        """A fresh reading with nothing to charge: a job's start, or the
        first status after Klippy (re)connects."""
        self.highest_e = e
        self.last_e = e
        if extruder is not None:
            self.extruder = extruder

    def attribute(self, tool: int, lane: int, spool_id: Optional[int],
                  grams_per_mm: Optional[float]) -> None:
        """Charge what follows to this tool, lane and spool. A change of
        spool re-bases at the last position, as Moonraker does."""
        if spool_id != self.label[2]:
            self.highest_e = self.last_e
        self.label = (tool, lane, spool_id)
        self.grams_per_mm = grams_per_mm

    def position(self, e: Optional[float], extruder: Optional[str]) -> float:
        """A status update; returns the millimetres charged."""
        if extruder is not None and extruder != self.extruder:
            self.extruder = extruder
            self.highest_e = e
            self.last_e = e
            return 0.0
        if e is None:
            return 0.0
        self.last_e = e
        if self.highest_e is None:
            self.highest_e = e
            return 0.0
        if e <= self.highest_e:
            return 0.0
        used = e - self.highest_e
        self.highest_e = e
        self._charge(used)
        return used

    def _charge(self, mm: float) -> None:
        tool, lane, spool = self.label
        for entry in self.entries:
            if entry[0] == tool and entry[1] == lane and entry[2] == spool:
                entry[3] += mm
                if entry[4] is None:
                    entry[4] = self.grams_per_mm
                return
        self.entries.append([tool, lane, spool, mm, self.grams_per_mm])

    def spools(self) -> List[Dict[str, Any]]:
        """The record's per-spool lines, in first-use order."""
        out: List[Dict[str, Any]] = []
        for tool, lane, spool, mm, gpm in self.entries:
            out.append({
                "tool": tool if tool >= 0 else None,
                "lane": lane if lane >= 0 else None,
                "spool_id": spool,
                "mm": round(mm, 1),
                "grams": round(mm * gpm, 2) if gpm else None,
            })
        return out

    def to_state(self) -> Dict[str, Any]:
        return {"entries": [list(e) for e in self.entries],
                "label": list(self.label), "grams_per_mm": self.grams_per_mm}

    @classmethod
    def from_state(cls, state: Any) -> "UsageLedger":
        ledger = cls()
        if not isinstance(state, dict):
            return ledger
        for e in state.get("entries") or []:
            if not isinstance(e, list) or len(e) != 5:
                continue
            tool, lane, spool, mm, gpm = e
            if isinstance(mm, bool) or not isinstance(mm, (int, float)):
                continue
            ledger.entries.append([_int_or(tool, -1), _int_or(lane, -1), _spool_id(spool),
                                   float(mm), float(gpm) if isinstance(gpm, (int, float))
                                   and not isinstance(gpm, bool) and gpm > 0 else None])
        label = state.get("label")
        if isinstance(label, list) and len(label) == 3:
            ledger.label = (_int_or(label[0], -1), _int_or(label[1], -1), _spool_id(label[2]))
        gpm = state.get("grams_per_mm")
        if isinstance(gpm, (int, float)) and not isinstance(gpm, bool) and gpm > 0:
            ledger.grams_per_mm = float(gpm)
        return ledger


def _number(value: Any) -> Optional[float]:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return float(value)


# History stores start times as floats; one read back may differ in the last
# digits, never by a second.
START_TIME_TOLERANCE_S = 1.0


def same_job(record: Any, start_time: Optional[float]) -> bool:
    """Whether a stored record is the job that started at `start_time`:
    history reuses job ids, so the id alone names a deleted job too. With no
    start time to compare, the id decides."""
    if not isinstance(record, dict):
        return False
    if start_time is None:
        return True
    stored = _number(record.get("start_time"))
    return stored is not None and abs(stored - start_time) < START_TIME_TOLERANCE_S


def job_record(open_job: "OpenJob", status: Optional[str], end_time: Optional[float]
               ) -> Dict[str, Any]:
    """The per-job record kept in FILAMENT_NAMESPACE."""
    return {"schema": RECORD_SCHEMA, "job_id": open_job.job_id, "filename": open_job.filename,
            "start_time": open_job.start_time, "status": status, "end_time": end_time,
            "continues": open_job.continues, "spools": open_job.ledger.spools()}


def _e_and_extruder(status: Dict[str, Any]) -> Tuple[Optional[float], Optional[str]]:
    toolhead = status.get("toolhead")
    if not isinstance(toolhead, dict):
        return None, None
    pos = toolhead.get("position")
    e: Optional[float] = None
    if isinstance(pos, list) and len(pos) >= 4 and isinstance(pos[3], (int, float)) \
            and not isinstance(pos[3], bool):
        e = float(pos[3])
    extruder = toolhead.get("extruder")
    return e, extruder if isinstance(extruder, str) else None


def _merge(into: Dict[str, Any], update: Dict[str, Any]) -> None:
    for obj, fields in update.items():
        if isinstance(fields, dict):
            node = into.setdefault(obj, {})
            if isinstance(node, dict):
                node.update(fields)
            else:
                into[obj] = dict(fields)


class OpenJob:
    """The job the ledger is counting."""

    def __init__(self, job_id: str, filename: str, continues: Optional[str],
                 ledger: Optional[UsageLedger] = None, start_time: Any = None) -> None:
        self.job_id = job_id
        self.filename = filename
        self.continues = continues
        self.ledger = ledger or UsageLedger()
        self.start_time = _number(start_time)

    def state(self) -> Dict[str, Any]:
        return {"job_id": self.job_id, "filename": self.filename, "start_time": self.start_time,
                "continues": self.continues, "ledger": self.ledger.to_state()}

    @classmethod
    def from_state(cls, state: Any) -> Optional["OpenJob"]:
        if not isinstance(state, dict) or not isinstance(state.get("job_id"), str):
            return None
        filename = state.get("filename")
        continues = state.get("continues")
        return cls(state["job_id"], filename if isinstance(filename, str) else "",
                   continues if isinstance(continues, str) else None,
                   UsageLedger.from_state(state.get("ledger")), state.get("start_time"))


class FilamentAccounting:
    """The Moonraker-facing half, created by helix_print.py."""

    def __init__(self, helix: Any, namespace: str,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self.helix = helix
        self.server = helix.server
        # The app's settings namespace, where the map lives
        self.namespace = namespace
        self.clock = clock
        self.status: Dict[str, Any] = {}
        self.job: Optional[OpenJob] = None
        # The feed value last read on this Klippy connection; None until the
        # first status after ready, which only sets it.
        self._feed_key: Optional[str] = None
        self._baselined = False
        self._last_persist = float("-inf")
        self._dirty = False
        self._map: Optional[SpoolMap] = None
        self._last_added: Optional[str] = None
        # The objects and fields subscribed on this Klippy connection
        self._subscribed: Dict[str, set] = {}
        self._switch_lock = asyncio.Lock()

    # -- lifecycle -----------------------------------------------------------

    def start(self) -> None:
        self.server.register_endpoint(
            "/server/helix/filament_usage", ["GET"], self.handle_usage)
        self.server.register_event_handler("history:history_changed", self.on_history_changed)
        self.server.register_event_handler("server:status_update", self.on_status_update)
        self.server.register_event_handler("server:klippy_ready", self.on_klippy_ready)
        self.server.register_event_handler("server:klippy_disconnect", self.on_klippy_gone)
        self.server.register_event_handler("spoolman:active_spool_set", self.on_spool_set)

    async def load(self) -> None:
        """Read back the job a restart interrupted."""
        self.job = OpenJob.from_state(await self._ns_get(ACTIVE_KEY))
        last_added = await self._ns_get(LAST_ADDED_KEY)
        self._last_added = last_added if isinstance(last_added, str) else None
        await self._prune()

    # -- database ------------------------------------------------------------

    async def _ns_get(self, key: str) -> Any:
        db = self.helix.database
        if db is None:
            return None
        try:
            return await db.get_item(FILAMENT_NAMESPACE, key, None)
        except Exception:
            return None

    async def _ns_put(self, key: str, value: Any) -> None:
        db = self.helix.database
        if db is None:
            return
        try:
            await db.insert_item(FILAMENT_NAMESPACE, key, value)
        except Exception as e:
            logging.warning("HelixPrint: filament record %s not written: %s", key, e)

    async def _ns_delete(self, key: str) -> None:
        db = self.helix.database
        if db is None:
            return
        try:
            await db.delete_item(FILAMENT_NAMESPACE, key)
        except Exception:
            pass

    async def _read_map(self) -> Optional[SpoolMap]:
        db = self.helix.database
        if db is None:
            return None
        try:
            record = await db.get_item(self.namespace, SPOOL_MAP_KEY, None)
        except Exception:
            return None
        return parse_spool_map(record)

    async def _prune(self) -> None:
        items = await self.helix._namespace_items(FILAMENT_NAMESPACE)
        records = [(k, v) for k, v in items if k.startswith(JOB_KEY_PREFIX)]
        if len(records) <= MAX_JOB_RECORDS:
            return

        def end(item: Tuple[str, Any]) -> float:
            t = item[1].get("end_time") if isinstance(item[1], dict) else None
            return float(t) if isinstance(t, (int, float)) and not isinstance(t, bool) else 0.0

        records.sort(key=end, reverse=True)
        for key, _ in records[MAX_JOB_RECORDS:]:
            await self._ns_delete(key)

    async def _persist(self, force: bool = False) -> None:
        if self.job is None:
            return
        now = self.clock()
        if not force and (not self._dirty or now - self._last_persist < PERSIST_INTERVAL_S):
            return
        self._last_persist = now
        self._dirty = False
        await self._ns_put(ACTIVE_KEY, self.job.state())

    # -- the active spool ----------------------------------------------------

    def _spoolman(self) -> Optional[Any]:
        return self.server.lookup_component("spoolman", None)

    def availability(self) -> Tuple[bool, Optional[str]]:
        """Available only where Moonraker's spoolman component is loaded: with
        no deductor there is no spool to switch and no usage to record."""
        return (True, None) if self._spoolman() is not None else (False, OWNER_NO_SPOOLMAN)

    def _active_spool(self) -> Optional[int]:
        spoolman = self._spoolman()
        return _spool_id(getattr(spoolman, "spool_id", None)) if spoolman is not None else None

    async def _switch(self, why: str) -> None:
        """Switch the active spool for what feeds now, when this module owns
        it. The map is read here, so a spool linked or unlinked since the
        job started counts at the next change. Switches run one at a time,
        and each decides from the newest status only after its last await, so
        a quick T0 -> T1 -> T0 ends on T0's spool."""
        async with self._switch_lock:
            spool_map = await self._read_map()
            if spool_map is None:
                return
            self._map = spool_map
            await self._subscribe_signals(spool_map)
            spoolman = self._spoolman()
            owner = active_spool_owner(spoolman is not None, spool_map.manages_active_spool,
                                       spool_map.printer_switches_active_spool,
                                       spool_map.macros_set_active_spool)
            if owner != OWNER_HELIX:
                return
            if not await self.helix.feature_enabled(FILAMENT_FEATURE):
                return
            _key, feed = feeding(spool_map, self.status)
            action = spool_switch(feed, self._active_spool(), spool_map.mapped_spools())
            if action is None:
                return
            spool_id = action[1]
            logging.info("HelixPrint: active spool -> %s (%s, tool %s, lane %s)", spool_id, why,
                         feed.tool if feed else None, feed.lane if feed else None)
            try:
                spoolman.set_active_spool(spool_id)
            except Exception as e:
                logging.warning("HelixPrint: could not set the active spool: %s", e)

    async def _subscribe_signals(self, spool_map: SpoolMap) -> None:
        """Subscribe the map's signal fields Moonraker is not yet sending
        this module: a map written or rewritten after Klippy connected names
        objects the ready-time subscription could not."""
        apis = self.helix.klippy_apis
        if apis is None or not self._baselined:
            return
        missing: Dict[str, List[str]] = {}
        for obj, fields in spool_map.objects().items():
            have = self._subscribed.get(obj, set())
            if any(f not in have for f in fields):
                missing[obj] = sorted(set(fields) | have)
        if not missing:
            return
        try:
            status = await apis.subscribe_objects(missing)
        except Exception as e:
            logging.warning("HelixPrint: filament accounting could not subscribe %s: %s",
                            sorted(missing), e)
            return
        for obj, fields in missing.items():
            self._subscribed[obj] = set(fields)
        if isinstance(status, dict):
            _merge(self.status, status)

    def _attribute(self, feed: Optional[Feed]) -> None:
        if self.job is None:
            return
        active = self._active_spool()
        if feed is None:
            self.job.ledger.attribute(-1, -1, active, None)
            return
        gpm = feed.grams_per_mm if feed.spool_id == active else None
        self.job.ledger.attribute(feed.tool, feed.lane, active, gpm)

    # -- events --------------------------------------------------------------

    async def on_klippy_ready(self, *args: Any) -> None:
        """Subscribe what the ledger and the map need. The first reading of
        a connection only sets the baseline: a value carried over a
        reconnect says nothing about a change, so it never switches."""
        self._feed_key = None
        self._baselined = False
        apis = self.helix.klippy_apis
        if apis is None:
            return
        self._map = await self._read_map()
        objects: Dict[str, List[str]] = {"toolhead": ["position", "extruder"],
                                         "print_stats": ["state", "filename"]}
        if self._map is not None:
            for obj, fields in self._map.objects().items():
                objects[obj] = sorted(set(objects.get(obj, [])) | set(fields))
        try:
            status = await apis.subscribe_objects(objects)
        except Exception as e:
            logging.warning("HelixPrint: filament accounting could not subscribe: %s", e)
            return
        self._subscribed = {obj: set(fields) for obj, fields in objects.items()}
        self.status = {}
        if isinstance(status, dict):
            _merge(self.status, status)
        self._baseline()
        stats = self.status.get("print_stats") or {}
        if self.job is not None and (stats.get("state") not in _RUNNING_STATES
                                     or stats.get("filename") != self.job.filename):
            logging.info("HelixPrint: job %s is not running; its filament record waits for "
                         "history", self.job.job_id)

    def _baseline(self) -> None:
        e, extruder = _e_and_extruder(self.status)
        if self.job is not None:
            self.job.ledger.baseline(e, extruder)
        if self._map is not None:
            key, feed = feeding(self._map, self.status)
            self._feed_key = key
            self._attribute(feed)
        self._baselined = True

    async def on_klippy_gone(self, *args: Any) -> None:
        self._baselined = False
        self._feed_key = None
        self._subscribed = {}
        await self._persist(force=True)

    async def on_spool_set(self, payload: Any = None, *args: Any) -> None:
        """Whoever switched the active spool, what follows is charged to it."""
        if self.job is None or self._map is None:
            if self.job is not None:
                self._attribute(None)
            return
        _key, feed = feeding(self._map, self.status)
        self._attribute(feed)
        self._dirty = True

    async def on_status_update(self, status: Dict[str, Any], *args: Any) -> None:
        if not isinstance(status, dict):
            return
        _merge(self.status, status)
        if not self._baselined:
            return
        if self.job is not None and "toolhead" in status:
            e, extruder = _e_and_extruder(self.status)
            changed_extruder = isinstance(status.get("toolhead"), dict) \
                and "extruder" in status["toolhead"]
            if self.job.ledger.position(e, extruder if changed_extruder else None) > 0:
                self._dirty = True
        if self._map is not None and any(s.obj in status for s in self._map.signals):
            key, feed = feeding(self._map, self.status)
            if key != self._feed_key:
                self._feed_key = key
                if self._job_running():
                    await self._switch("feed changed")
                    _key, feed = feeding(self._map, self.status)
                self._attribute(feed)
                self._dirty = True
        if self._dirty:
            await self._persist()

    def _job_running(self) -> bool:
        stats = self.status.get("print_stats") or {}
        return self.job is not None and stats.get("state") in _RUNNING_STATES

    async def on_history_changed(self, payload: Dict[str, Any]) -> None:
        if not isinstance(payload, dict):
            return
        job = payload.get("job")
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
        # Read before the first await: helix_print binds its marker for this
        # job before it awaits.
        start_time = job.get("start_time")
        kind = self.helix.helix_initiated(job_id, filename, start_time)
        # The job a resume continues: the one PLR says it resumed, else (PLR
        # resumes only the newest job) the job history added before this one.
        # Either works whatever file name the jobs have (a QIDI resume prints
        # QIDI's .cache copy under its absolute path).
        resumed = getattr(self.helix, "helix_resumed_job", None)
        interrupted = (resumed(job_id) if resumed is not None else None) or self._last_added
        self._last_added = job_id
        previous = self.job
        if previous is not None and previous.job_id == job_id:
            return
        self.job = None
        await self._ns_put(LAST_ADDED_KEY, job_id)
        if previous is not None:
            # No finish was seen for it: a power cut, or Moonraker restarted.
            await self._close(previous, None, None)
        if not await self.helix.feature_enabled(FILAMENT_FEATURE):
            return
        base: Optional[OpenJob] = None
        if kind == "plr_resume":
            base = previous or OpenJob.from_state(await self._ns_get(LAST_KEY))
            # Only the job the resume continues: one that was not counted
            # leaves an older, unrelated job as the last one closed.
            if base is not None and base.job_id != interrupted:
                base = None
        if base is not None:
            # The resumed part continues the interrupted job's spools.
            self.job = OpenJob(job_id, filename, base.job_id,
                               UsageLedger.from_state(base.ledger.to_state()), start_time)
            logging.info("HelixPrint: job %s continues the filament record of job %s",
                         job_id, base.job_id)
        else:
            self.job = OpenJob(job_id, filename, None, start_time=start_time)
        self._map = await self._read_map()
        e, extruder = _e_and_extruder(self.status)
        self.job.ledger.baseline(e, extruder)
        if self._map is not None:
            await self._subscribe_signals(self._map)
            await self._query_signals(self._map)
            await self._switch("job start")
            key, feed = feeding(self._map, self.status)
            self._feed_key = key
            self._attribute(feed)
        else:
            self._attribute(None)
        await self._persist(force=True)

    async def _query_signals(self, spool_map: SpoolMap) -> Dict[str, Any]:
        """A fresh reading of the signals, for the start of a job."""
        apis = self.helix.klippy_apis
        if apis is None:
            return self.status
        try:
            status = await apis.query_objects(spool_map.objects())
        except Exception as e:
            logging.info("HelixPrint: could not read the feed signals: %s", e)
            return self.status
        if not isinstance(status, dict):
            return self.status
        _merge(self.status, status)
        return self.status

    async def _job_finished(self, job: Dict[str, Any]) -> None:
        if self.job is None or str(job["job_id"]) != self.job.job_id:
            return
        end_time = job.get("end_time")
        closing = self.job
        self.job = None
        record = await self._close(closing, job.get("status"),
                                   float(end_time) if isinstance(end_time, (int, float))
                                   and not isinstance(end_time, bool) else None)
        await self._link_in_history(job, record)

    async def _link_in_history(self, job: Dict[str, Any], record: Dict[str, Any]) -> None:
        """Put the spools in the finished job's auxiliary_data, through the
        one history rewrite every feature shares. History reuses job ids, so
        the row must still have this job's start time."""
        patch = getattr(self.helix, "patch_history_job", None)
        history = getattr(self.helix, "history", None)
        # A history that cannot rewrite a job (API 1.4.0 forks) keeps the
        # record in this module's namespace only; that is no fault to log.
        if patch is None or not (hasattr(history, "get_job") and hasattr(history, "save_job")):
            return
        start_time = job.get("start_time")

        def update(row: Dict[str, Any]) -> bool:
            if start_time is not None and row.get("start_time") != start_time:
                return False
            row["auxiliary_data"] = self.helix.set_history_aux(
                row.get("auxiliary_data"), AUX_NAME, record["spools"], "Filament used per spool")
            return True

        await patch(job["job_id"], update)

    async def _close(self, open_job: OpenJob, status: Optional[str],
                     end_time: Optional[float]) -> Dict[str, Any]:
        """Write the job's record and drop the open ledger. A job closed by
        the next job's start, with no finish seen, has no status."""
        record = job_record(open_job, status,
                            end_time if end_time is not None else time.time())
        await self._ns_put(JOB_KEY_PREFIX + open_job.job_id, record)
        await self._ns_put(LAST_KEY, open_job.state())
        await self._ns_delete(ACTIVE_KEY)
        logging.info("HelixPrint: filament record for job %s: %s", open_job.job_id,
                     record["spools"])
        await self._prune()
        return record

    # -- endpoint ------------------------------------------------------------

    async def handle_usage(self, web_request: Any) -> Dict[str, Any]:
        """The per-spool record of a job: the finished record, or the open
        ledger of the job still printing. `record` is None for a job this
        feature did not count. With `start_time` (history's, for that job),
        a record of an earlier job under a reused id is not answered."""
        job_id = web_request.get_str("job_id")
        if not job_id:
            raise self.server.error("job_id is required", 400)
        try:
            start_time: Optional[float] = float(web_request.get_str("start_time", ""))
        except ValueError:
            start_time = None
        enabled = await self.helix.feature_enabled(FILAMENT_FEATURE)
        if self.job is not None and self.job.job_id == job_id:
            record: Any = job_record(self.job, "in_progress", None)
        else:
            record = await self._ns_get(JOB_KEY_PREFIX + job_id)
        if not same_job(record, start_time):
            record = None
        return {"job_id": job_id, "enabled": enabled, "record": record}
