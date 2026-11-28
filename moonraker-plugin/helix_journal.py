# SPDX-License-Identifier: GPL-3.0-or-later
"""
Printer event journal: the HelixPrint plugin's record of what happened to the
printer, kept whichever client was connected at the time.

App side and fallback: docs/devel/HELIXPRINT_PLUGIN.md, Printer Event Journal.

Each entry carries a wall-clock time and the job that was active: klippy
shutdown (with its state_message), disconnect and ready, MCU disconnect,
firmware restart, pause with its cause, runout sensor trips, `!!` lines
(classified) and the end of each job with its status. The store is bounded by
count and by age and lives in its own Moonraker namespace, one key per entry,
so it survives a restart. `server.helix.journal {since, limit}` reads it and
every append is broadcast as `notify_helix_journal_appended`.

Everything above HelixJournal is pure and touches no Moonraker object. The app
classifies `!!` lines with its own copy of classify_error(); both are pinned by
tests/fixtures/journal/error_line_classes.json. Python 3.8, stdlib only.
"""

from __future__ import annotations

import json
import logging
import time
from collections import deque
from typing import Any, Deque, Dict, List, Optional, Tuple

JOURNAL_FEATURE = "event_journal"
JOURNAL_FEATURE_VERSION = 1
JOURNAL_NAMESPACE = "helix_journal"
# Broadcast as notify_helix_journal_appended, params [entry]
JOURNAL_EVENT = "helix_print:helix_journal_appended"

MAX_ENTRIES = 500
MAX_AGE_S = 30 * 86400
DEFAULT_LIMIT = 100

# A `!!` line repeating one already recorded within this long of its last
# occurrence counts on that entry ("count", "last_time") instead of adding one,
# so a macro erroring in a loop cannot push real events out of the store.
REPEAT_WINDOW_S = 60.0
# How many of the newest entries a repeat is matched against
REPEAT_LOOKBACK = 16
# A counting entry is written back to the database at most this often; the
# next new entry writes its final count.
REPEAT_PERSIST_S = 10.0

KIND_SHUTDOWN = "klippy_shutdown"
KIND_DISCONNECT = "klippy_disconnect"
KIND_READY = "klippy_ready"
KIND_MCU_DISCONNECT = "mcu_disconnect"
KIND_FIRMWARE_RESTART = "firmware_restart"
KIND_PAUSE = "pause"
KIND_RUNOUT = "runout"
KIND_GCODE_ERROR = "gcode_error"
KIND_JOB_END = "job_end"

KINDS = (
    KIND_SHUTDOWN, KIND_DISCONNECT, KIND_READY, KIND_MCU_DISCONNECT,
    KIND_FIRMWARE_RESTART, KIND_PAUSE, KIND_RUNOUT, KIND_GCODE_ERROR, KIND_JOB_END,
)

# A pause is put down to an event this recent before it
PAUSE_CAUSE_WINDOW_S = 10.0

RUNOUT_SENSOR_PREFIXES = ("filament_switch_sensor ", "filament_motion_sensor ")


# ============================================================================
# Classification
# ============================================================================

_MCU_DISCONNECT_MARKERS = ("lost communication with mcu", "unable to connect")

# Ordered: the first class with a matching marker wins. Heater faults come
# before MCU ones because Klipper reports an ADC fault as an MCU shutdown.
_CLASS_MARKERS = (
    ("heater", ("heater", "adc out of range", "thermocouple", "thermistor",
                "not heating at expected rate", "thermal runaway")),
    ("mcu", ("mcu", "timer too close", "missed scheduling", "rescheduled timer")),
    ("homing", ("must home", "no trigger", "endstop", "probe triggered", "homing")),
    ("move", ("move out of range", "move exceeds", "extrude below minimum",
              "extrude only move too long")),
    ("filament", ("filament", "runout")),
    ("command", ("unknown command", "error evaluating", "malformed command")),
)


def error_text(line: str) -> str:
    """`line` without its `!!` prefix and surrounding whitespace."""
    text = line.strip()
    if text.startswith("!!"):
        text = text[2:]
    return text.strip()


def _is_firmware_envelope(text: str) -> bool:
    if not text.startswith("{"):
        return False
    try:
        parsed = json.loads(text)
    except ValueError:
        return False
    return isinstance(parsed, dict) and "code" in parsed


def is_mcu_disconnect(line: str) -> bool:
    """Whether an error line or shutdown reason reports a lost MCU."""
    lowered = error_text(line).lower()
    return any(marker in lowered for marker in _MCU_DISCONNECT_MARKERS)


def classify_error(line: str) -> str:
    """The class of a `!!` line or shutdown reason: firmware, mcu, heater,
    homing, move, filament, command or other."""
    text = error_text(line)
    if _is_firmware_envelope(text):
        return "firmware"
    if is_mcu_disconnect(text):
        return "mcu"
    lowered = text.lower()
    for cls, markers in _CLASS_MARKERS:
        if any(marker in lowered for marker in markers):
            return cls
    return "other"


def pause_cause(
    entries: List[Dict[str, Any]], commands: List[Tuple[float, str]], now: float
) -> str:
    """Why a print paused at `now`: "runout" or "error" when the journal
    recorded one within PAUSE_CAUSE_WINDOW_S, "command" when a client sent a
    pause in that window, else "unknown" (a pause in the G-code itself, or a
    macro nobody reported)."""
    cutoff = now - PAUSE_CAUSE_WINDOW_S
    recent = [e for e in entries if e.get("time", 0.0) >= cutoff]
    if any(e.get("kind") == KIND_RUNOUT for e in recent):
        return "runout"
    if any(e.get("kind") in (KIND_GCODE_ERROR, KIND_MCU_DISCONNECT) for e in recent):
        return "error"
    for when, script in commands:
        if when < cutoff:
            continue
        words = script.upper().split()
        if any(w in ("PAUSE", "M600", "M601") for w in words):
            return "command"
    return "unknown"


def is_firmware_restart(script: str) -> bool:
    return "FIRMWARE_RESTART" in script.upper().split()


def runout_trips(
    sensors: Dict[str, Dict[str, bool]], status: Dict[str, Any]
) -> List[str]:
    """Update `sensors` ({object: {"detected", "enabled"}}) from one status
    update and return the objects whose filament went from present to absent
    while the sensor was enabled."""
    tripped: List[str] = []
    for name, fields in status.items():
        if not name.startswith(RUNOUT_SENSOR_PREFIXES) or not isinstance(fields, dict):
            continue
        state = sensors.setdefault(name, {"detected": True, "enabled": True})
        if "enabled" in fields:
            state["enabled"] = bool(fields["enabled"])
        if "filament_detected" in fields:
            detected = bool(fields["filament_detected"])
            if state["detected"] and not detected and state["enabled"]:
                tripped.append(name)
            state["detected"] = detected
    return tripped


def runout_sensors(objects: List[str], settings: Any) -> List[str]:
    """The sensor objects whose trips are runouts: those the printer's config
    file defines with pause_on_runout on (Klipper's default). A filament
    system's own switches (Happy Hare's gates, its sync-feedback tension
    switch, AFC lanes) are created at runtime, never appear in the config
    file, and flip as part of normal feeding; that system reports its own
    runouts."""
    if not isinstance(settings, dict):
        return []
    picked: List[str] = []
    for name in objects:
        if not isinstance(name, str) or not name.startswith(RUNOUT_SENSOR_PREFIXES):
            continue
        section = settings.get(name.lower())
        if isinstance(section, dict) and section.get("pause_on_runout", True) is not False:
            picked.append(name)
    return picked


def sensor_label(name: str) -> str:
    """The configured name of a sensor object ("filament_switch_sensor runout"
    -> "runout")."""
    return name.split(" ", 1)[1] if " " in name else name


# ============================================================================
# Store
# ============================================================================


def entry_key(entry_id: int) -> str:
    """Database key of an entry: zero-padded so keys sort in id order."""
    return "%010d" % entry_id


def _valid_entry(record: Any) -> bool:
    return (
        isinstance(record, dict)
        and isinstance(record.get("id"), int)
        and isinstance(record.get("time"), (int, float))
        and record.get("kind") in KINDS
    )


class JournalStore:
    """Entries oldest first, bounded by count and age. Ids only grow, so a
    reader's `since` stays meaningful across evictions and restarts."""

    def __init__(self, max_entries: int = MAX_ENTRIES, max_age_s: float = MAX_AGE_S) -> None:
        self.max_entries = max_entries
        self.max_age_s = max_age_s
        self.entries: List[Dict[str, Any]] = []
        self.next_id = 1

    def load(self, records: List[Any], now: float) -> List[int]:
        """Take entries read back from the database and return the ids that
        aged out. Malformed records are skipped; their keys are the caller's
        to delete."""
        self.entries = sorted((r for r in records if _valid_entry(r)), key=lambda e: e["id"])
        if self.entries:
            self.next_id = max(self.next_id, self.entries[-1]["id"] + 1)
        return self.prune(now)

    def prune(self, now: float) -> List[int]:
        """Evict entries past the age limit, then the oldest past the count
        limit. Returns the evicted ids."""
        cutoff = now - self.max_age_s
        evicted = [e["id"] for e in self.entries if e["time"] < cutoff]
        kept = [e for e in self.entries if e["time"] >= cutoff]
        overflow = len(kept) - self.max_entries
        if overflow > 0:
            evicted.extend(e["id"] for e in kept[:overflow])
            kept = kept[overflow:]
        self.entries = kept
        return evicted

    def append(
        self,
        kind: str,
        now: float,
        text: str = "",
        job: Optional[Dict[str, Any]] = None,
        cls: Optional[str] = None,
        detail: Optional[str] = None,
    ) -> Tuple[Dict[str, Any], List[int]]:
        """Add an entry; (it, evicted ids). A repeated `!!` line returns the
        entry it counts on, with nothing evicted."""
        repeated = self._repeat_of(kind, now, text, job, cls, detail)
        if repeated is not None:
            repeated["count"] = repeated.get("count", 1) + 1
            repeated["last_time"] = now
            return repeated, []
        entry = {
            "id": self.next_id,
            "time": now,
            "kind": kind,
            "text": text,
            "class": cls,
            "detail": detail,
            "job": dict(job) if job else None,
        }
        self.next_id += 1
        self.entries.append(entry)
        return entry, self.prune(now)

    def _repeat_of(self, kind: str, now: float, text: str, job: Optional[Dict[str, Any]],
                   cls: Optional[str], detail: Optional[str]) -> Optional[Dict[str, Any]]:
        if kind != KIND_GCODE_ERROR:
            return None
        job = dict(job) if job else None
        for entry in reversed(self.entries[-REPEAT_LOOKBACK:]):
            if (entry["kind"] == kind and entry["text"] == text and entry["job"] == job
                    and entry["class"] == cls and entry["detail"] == detail
                    and now - entry.get("last_time", entry["time"]) <= REPEAT_WINDOW_S):
                return entry
        return None

    def query(self, since: int = 0, limit: int = DEFAULT_LIMIT) -> Dict[str, Any]:
        """The newest `limit` entries with an id above `since`, oldest first.
        `truncated` says older entries above `since` were left out."""
        newer = [e for e in self.entries if e["id"] > since]
        limit = max(1, limit)
        return {
            "entries": newer[-limit:],
            "truncated": len(newer) > limit,
            "latest_id": self.next_id - 1,
        }


# ============================================================================
# Moonraker component
# ============================================================================


class HelixJournal:
    """Records printer events into a JournalStore persisted in
    JOURNAL_NAMESPACE. Created and started by helix_print.py."""

    def __init__(self, helix: Any) -> None:
        self.helix = helix
        self.server = helix.server
        self.store = JournalStore()
        self.job: Optional[Dict[str, Any]] = None
        self.sensors: Dict[str, Dict[str, bool]] = {}
        self.commands: Deque[Tuple[float, str]] = deque(maxlen=20)
        self.clock = time.time
        # A counting entry whose latest count is not in the database yet
        self._unsaved: Optional[Dict[str, Any]] = None
        self._saved_at: Dict[int, float] = {}

    def start(self) -> None:
        handlers = (
            ("server:klippy_shutdown", self.on_klippy_shutdown),
            ("server:klippy_disconnect", self.on_klippy_disconnect),
            ("server:klippy_ready", self.on_klippy_ready),
            ("server:klippy_started", self.on_klippy_started),
            ("server:gcode_response", self.on_gcode_response),
            ("server:status_update", self.on_status_update),
            ("klippy_connection:gcode_received", self.on_gcode_received),
            ("job_state:state_changed", self.on_job_state_changed),
            ("history:history_changed", self.on_history_changed),
        )
        for event, handler in handlers:
            self.server.register_event_handler(event, handler)
        self.server.register_endpoint("/server/helix/journal", ["GET"], self.handle_journal)
        register = getattr(self.server, "register_notification", None)
        if register is not None:
            register(JOURNAL_EVENT)

    @property
    def database(self) -> Any:
        return self.helix.database

    async def load(self) -> None:
        """Read the store back after a restart and drop what aged out."""
        if self.database is None:
            return
        items = dict(await self.helix._namespace_items(JOURNAL_NAMESPACE))
        malformed = [k for k, v in items.items() if not _valid_entry(v)]
        evicted = self.store.load(list(items.values()), self.clock())
        await self._delete([entry_key(i) for i in evicted] + malformed)
        await self._recover_job()

    async def _recover_job(self) -> None:
        """The job history has open, for a restart in the middle of a print."""
        history = self.helix.history
        job = getattr(history, "current_job", None) if history is not None else None
        job_id = getattr(history, "current_job_id", None) if history is not None else None
        filename = getattr(job, "filename", None)
        if job_id and filename and getattr(job, "status", "") == "in_progress":
            self.job = {"job_id": job_id, "filename": filename}

    async def _store(self, entry: Dict[str, Any], now: float) -> None:
        self._saved_at[entry["id"]] = now
        if len(self._saved_at) > 2 * MAX_ENTRIES:
            live = {e["id"] for e in self.store.entries}
            self._saved_at = {k: v for k, v in self._saved_at.items() if k in live}
        if self.database is None:
            return
        try:
            await self.database.insert_item(JOURNAL_NAMESPACE, [entry_key(entry["id"])], entry)
        except Exception as e:
            logging.warning("HelixPrint: journal could not store entry: %s", e)

    async def _delete(self, keys: List[str]) -> None:
        if self.database is None:
            return
        for key in keys:
            try:
                await self.database.delete_item(JOURNAL_NAMESPACE, [key])
            except Exception:
                pass

    async def record(
        self,
        kind: str,
        text: str = "",
        cls: Optional[str] = None,
        detail: Optional[str] = None,
        job: Optional[Dict[str, Any]] = None,
    ) -> Optional[Dict[str, Any]]:
        """Append an entry when the feature is on. `job` defaults to the
        active one; a caller that awaits before recording passes the job it
        captured first, since history may finish it in between."""
        if job is None:
            job = self.job
        if not await self.helix.feature_enabled(JOURNAL_FEATURE):
            return None
        now = self.clock()
        entry, evicted = self.store.append(kind, now, text, job, cls, detail)
        if entry.get("count", 1) > 1:
            # A repeat: written back now and then, never broadcast again.
            if now - self._saved_at.get(entry["id"], now) >= REPEAT_PERSIST_S:
                await self._store(entry, now)
            else:
                self._unsaved = entry
            return entry
        if self._unsaved is not None:
            unsaved, self._unsaved = self._unsaved, None
            if unsaved in self.store.entries:
                await self._store(unsaved, now)
        await self._store(entry, now)
        await self._delete([entry_key(i) for i in evicted])
        send = getattr(self.server, "send_event", None)
        if send is not None:
            send(JOURNAL_EVENT, entry)
        return entry

    # -- Klipper -------------------------------------------------------------

    async def _state_message(self) -> str:
        apis = self.helix.klippy_apis
        if apis is None:
            return ""
        try:
            result = await apis.query_objects({"webhooks": ["state_message"]})
        except Exception:
            return ""
        webhooks = result.get("webhooks") if isinstance(result, dict) else None
        message = webhooks.get("state_message") if isinstance(webhooks, dict) else None
        return message.strip() if isinstance(message, str) else ""

    async def _record_fault(self, message: str, job: Optional[Dict[str, Any]],
                            fallback_kind: str) -> None:
        kind = KIND_MCU_DISCONNECT if is_mcu_disconnect(message) else fallback_kind
        await self.record(kind, message, classify_error(message) if message else None, job=job)

    async def on_klippy_shutdown(self, *args: Any) -> None:
        job = self.job
        await self._record_fault(await self._state_message(), job, KIND_SHUTDOWN)

    async def on_klippy_started(self, state: Any = None, *args: Any) -> None:
        """A Klipper that failed to start (config error, missing MCU) never
        shuts down, so only an MCU it could not reach is recorded here."""
        name = str(getattr(state, "name", state) or "").lower()
        if name != "error":
            return
        message = getattr(state, "message", "") or await self._state_message()
        if is_mcu_disconnect(message):
            await self.record(KIND_MCU_DISCONNECT, message, classify_error(message))

    async def on_klippy_disconnect(self, *args: Any) -> None:
        await self.record(KIND_DISCONNECT)

    async def on_klippy_ready(self, *args: Any) -> None:
        await self.record(KIND_READY)
        await self._subscribe_sensors()

    async def _subscribe_sensors(self) -> None:
        apis = self.helix.klippy_apis
        if apis is None:
            return
        try:
            objects = await apis.get_object_list()
            status = await apis.query_objects({"configfile": ["settings"]})
        except Exception:
            return
        settings = ((status or {}).get("configfile") or {}).get("settings")
        names = runout_sensors(list(objects or []), settings)
        self.sensors = {}
        if not names:
            return
        try:
            status = await apis.subscribe_objects(
                {n: ["filament_detected", "enabled"] for n in names}
            )
        except Exception as e:
            logging.warning("HelixPrint: journal could not subscribe runout sensors: %s", e)
            return
        if isinstance(status, dict):
            runout_trips(self.sensors, status)

    async def on_status_update(self, status: Dict[str, Any], *args: Any) -> None:
        if not isinstance(status, dict):
            return
        for name in runout_trips(self.sensors, status):
            if self.job is not None:
                await self.record(KIND_RUNOUT, detail=sensor_label(name))

    async def on_gcode_response(self, response: str, *args: Any) -> None:
        if not isinstance(response, str) or not response.startswith("!!"):
            return
        await self.record(KIND_GCODE_ERROR, error_text(response), classify_error(response))

    async def on_gcode_received(self, script: str, *args: Any) -> None:
        if not isinstance(script, str):
            return
        self.commands.append((self.clock(), script))
        if is_firmware_restart(script):
            await self.record(KIND_FIRMWARE_RESTART)

    # -- jobs ----------------------------------------------------------------

    async def on_job_state_changed(
        self, job_event: Any, prev_stats: Dict[str, Any], new_stats: Dict[str, Any], *args: Any
    ) -> None:
        if new_stats.get("state") != "paused" or prev_stats.get("state") == "paused":
            return
        cause = pause_cause(self.store.entries, list(self.commands), self.clock())
        await self.record(KIND_PAUSE, detail=cause)

    async def on_history_changed(self, payload: Dict[str, Any], *args: Any) -> None:
        job = payload.get("job") if isinstance(payload, dict) else None
        if not isinstance(job, dict):
            return
        ref = {"job_id": job.get("job_id"), "filename": job.get("filename", "")}
        action = payload.get("action")
        if action == "added":
            self.job = ref
        elif action == "finished":
            self.job = None
            await self.record(KIND_JOB_END, detail=str(job.get("status", "")), job=ref)

    # -- endpoint ------------------------------------------------------------

    async def handle_journal(self, web_request: Any) -> Dict[str, Any]:
        since = _int_arg(web_request, "since", 0)
        limit = min(_int_arg(web_request, "limit", DEFAULT_LIMIT), self.store.max_entries)
        result = self.store.query(since, limit)
        result["enabled"] = await self.helix.feature_enabled(JOURNAL_FEATURE)
        return result


def _int_arg(web_request: Any, key: str, default: int) -> int:
    getter = getattr(web_request, "get_int", None)
    try:
        value = getter(key, default) if getter is not None else default
        return int(value)
    except Exception:
        return default
