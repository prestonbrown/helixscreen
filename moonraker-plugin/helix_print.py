# SPDX-License-Identifier: GPL-3.0-or-later
"""
HelixPrint - Moonraker component for handling modified G-code files.

This component provides a single API endpoint that handles the complete workflow
for printing modified G-code while preserving original file attribution in
Klipper's print_stats and Moonraker's history.

API v2.0: Path-based interface
- Client uploads modified file first via standard Moonraker file upload
- Then calls print_modified with path to the already-uploaded file
- This avoids memory-intensive JSON payloads for large G-code files

Key features:
- Single API endpoint: POST /server/helix/print_modified
- Path-based interface (receives file path, not content)
- Symlink-based filename preservation (Klipper sees original name)
- Automatic history patching to record original filename
- Configurable cleanup of temporary files

Moonraker versions:
- Symlink attribution, temp tracking and cleanup work from v0.8.x up; v0.8.x
  persists through the namespace key-value API instead of a SQL table.
- Rewriting the finished history entry to the original filename needs v0.9.0,
  where History grew save_job() and auxiliary_data became a list of provider
  entries. Those arrived together, so probing for save_job() is the same test
  as probing for the list shape. Older installs keep the symlink's name in
  history and log one warning per print.

Configuration (moonraker.conf):
    [helix_print]
    enabled: True
    temp_dir: .helix_temp
    symlink_dir: .helix_print
    cleanup_delay: 86400
    convert_3mf: True
    start_3mf_on_upload: False

The .3mf converter lives in helix_3mf.py and the capability probes in
helix_capabilities.py, both beside this file (README.md, ".3mf Conversion").
"""

from __future__ import annotations

import asyncio
import glob as glob_module
import importlib.util
import inspect
import json
import logging
import os
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any, Awaitable, Callable, Dict, List, Optional, Tuple

if TYPE_CHECKING:
    from moonraker.common import RequestType, WebRequest
    from moonraker.confighelper import ConfigHelper
    from moonraker.server import Server

# Database table name for tracking temp files
HELIX_TEMP_TABLE = "helix_temp_files"

# Maximum age for cleaned database records before deletion (30 days)
DB_RECORD_MAX_AGE = 30 * 86400

# Plugin version - used for API version detection by clients
PLUGIN_VERSION = "1.4.0"

# Shape of the status reply; 2 added the `features` map
STATUS_API = 2

# Namespace for the key-value storage fallback (database without execute_db_command)
HELIX_NAMESPACE = "helix_temp_files"

# Top-level gcodes directories the app stages a linked print's copy in, beside
# temp_dir: hidden where Klipper prints from dot-directories, else visible.
APP_STAGING_DIRS = (".helix_temp", "helix_staging")

def _ns_key(temp_filename: str) -> List[str]:
    """A namespace key naming one record. Moonraker's database splits a string
    key on "." into nested keys, and rejects one with an empty segment, which
    every ".helix_temp/..." name has; a one-element list is taken whole."""
    return [temp_filename]


def _legacy_records(value: Any, depth: int = 0) -> List[Dict[str, Any]]:
    """Print records nested inside a namespace value.

    A record stored under a dotted string key lands nested by the key's
    segments ("helix_temp/modified_1_x.gcode" -> {"gcode": record} under
    "helix_temp/modified_1_x"), so the value at a top-level key is either a
    record or dicts leading to one.
    """
    if not isinstance(value, dict) or depth > 8:
        return []
    if "temp_filename" in value:
        return [value]
    found: List[Dict[str, Any]] = []
    for child in value.values():
        found.extend(_legacy_records(child, depth + 1))
    return found


# Provider name stamped on our entry in a history job's auxiliary_data
HELIX_AUX_PROVIDER = "helix_print"


# Why each sibling module that did not load is missing, by module name. A
# feature built on one reports this as its `reason`.
SIBLING_LOAD_ERRORS: Dict[str, str] = {}


def _load_sibling(name: str) -> Optional[Any]:
    """Load <name>.py from beside this file, as module `name`.

    Moonraker imports this component through a symlink in its components
    directory, where no sibling module exists, so the import follows the link
    to the plugin's own directory. None when the file is missing or fails to
    load: the plugin then runs without what it provides, and
    SIBLING_LOAD_ERRORS says why.
    """
    SIBLING_LOAD_ERRORS.pop(name, None)
    path = Path(__file__).resolve().parent / f"{name}.py"
    loaded = sys.modules.get(name)
    if loaded is not None and Path(getattr(loaded, "__file__", "")).resolve() == path:
        return loaded
    if not path.is_file():
        SIBLING_LOAD_ERRORS[name] = "module_missing"
        return None
    spec = importlib.util.spec_from_file_location(name, str(path))
    if spec is None or spec.loader is None:
        SIBLING_LOAD_ERRORS[name] = "module_missing"
        return None
    module = importlib.util.module_from_spec(spec)
    # Registered before it runs, as an import would, so a sibling loaded
    # later can import this one by name.
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except Exception as e:
        del sys.modules[name]
        logging.exception(f"HelixPrint: {name}.py failed to load")
        SIBLING_LOAD_ERRORS[name] = f"load_failed: {type(e).__name__}: {e}"
        return None
    return module


helix_capabilities = _load_sibling("helix_capabilities")
# Dependencies first: helix_3mf and helix_analysis import it by name.
helix_file_queue = _load_sibling("helix_file_queue")
helix_3mf = _load_sibling("helix_3mf")
# Dependencies first: helix_plr imports the reader by name.
helix_gcode_reader = _load_sibling("helix_gcode_reader")
# Dependencies first: helix_plr and helix_companions import it by name.
helix_printer_cfg = _load_sibling("helix_printer_cfg")
helix_plr_scan = _load_sibling("helix_plr_scan")
helix_plr_render = _load_sibling("helix_plr_render")
helix_plr_qidi = _load_sibling("helix_plr_qidi")
helix_plr = _load_sibling("helix_plr")
helix_analysis = _load_sibling("helix_analysis")
helix_fitcheck = _load_sibling("helix_fitcheck")
helix_journal = _load_sibling("helix_journal")
helix_config = _load_sibling("helix_config")
helix_edits = _load_sibling("helix_edits")
helix_print_hooks = _load_sibling("helix_print_hooks")
helix_companions = _load_sibling("helix_companions")
# Dependencies first: helix_storage imports helix_photos by name.
helix_photos = _load_sibling("helix_photos")
helix_storage = _load_sibling("helix_storage")
helix_filament = _load_sibling("helix_filament")
# Dependencies first: each vendor module imports helix_vendor_push by name.
helix_vendor_push = _load_sibling("helix_vendor_push")
helix_vendor_ad5x = _load_sibling("helix_vendor_ad5x")
helix_vendor_ace = _load_sibling("helix_vendor_ace")


# ============================================================================
# Feature framework
# ============================================================================

# The app's settings namespace. Opt-ins live in its `plugin_features` record,
# {"<feature>": bool, ...}, written by the app's settings UI.
HELIX_SCREEN_NAMESPACE = "helix-screen"
PLUGIN_FEATURES_KEY = "plugin_features"

# Sent when a feature's availability changes after startup, so a screen
# re-reads the status (clients see notify_helix_features_changed).
FEATURES_CHANGED_EVENT = "helix_print:helix_features_changed"

# A feature's own switch: answers its `enabled` instead of plugin_features.
FeatureSwitch = Callable[[], Awaitable[bool]]


async def always_on() -> bool:
    """The switch of a feature with no user setting: one that only observes, or
    a transport the user drives from the screen. No plugin_features key turns
    it off."""
    return True

# A feature's live availability: (available, reason when not).
FeatureProbe = Callable[[], Tuple[bool, Optional[str]]]

# Fields a feature adds to its own entry beyond version/available/enabled/reason.
FeatureExtra = Callable[[], Awaitable[Dict[str, Any]]]


class Feature:
    """One entry of the status `features` map.

    `acts` is True for a feature that acts on a print or a spool: it is off
    until the user opts in. A feature that only observes is on unless the
    user opts out.
    """

    def __init__(
        self,
        name: str,
        version: int,
        acts: bool,
        available: bool = True,
        reason: Optional[str] = None,
        switch: Optional[FeatureSwitch] = None,
        probe: Optional[FeatureProbe] = None,
        extra: Optional[FeatureExtra] = None,
    ) -> None:
        self.name = name
        self.version = version
        self.acts = acts
        self.available = available
        self.reason = reason
        self.switch = switch
        self.probe = probe
        self.extra = extra

    def availability(self) -> Tuple[bool, Optional[str]]:
        """Whether the feature can run now, read from its probe when it has
        one, and why not."""
        if self.probe is not None:
            return self.probe()
        return self.available, self.reason


def opt_in_enabled(opt_ins: Any, name: str, acts: bool) -> bool:
    """A feature's opt-in from the plugin_features record.

    A key holding a bool is the user's choice; anything else, including no
    record at all, is the default: observers on, actors off.
    """
    if isinstance(opt_ins, dict):
        value = opt_ins.get(name)
        if isinstance(value, bool):
            return value
    return not acts


def feature_availability(feature: Feature, plugin_enabled: bool) -> Tuple[bool, Optional[str]]:
    """Whether a feature can run now, and why not. `[helix_print] enabled:
    False` stops every feature, whatever its own probe says."""
    if not plugin_enabled:
        return False, "plugin_disabled"
    return feature.availability()


def feature_entry(
    feature: Feature, enabled: bool, plugin_enabled: bool = True
) -> Dict[str, Any]:
    """The status map's entry for a feature. A feature that cannot run is
    never enabled, whatever its switch says."""
    available, reason = feature_availability(feature, plugin_enabled)
    return {
        "version": feature.version,
        "available": available,
        "enabled": bool(available and enabled),
        "reason": None if available else (reason or "unavailable"),
    }


# ============================================================================
# Helix-initiated job marker
# ============================================================================

# Plugin paths that start a job themselves, as recorded against its history id
HELIX_JOB_KINDS = ("plr_resume", "print_modified", "print_edited")

# Where a bound marker persists: key = history job id, value
# {"kind": ..., "filenames": [...], "start_time": ..., "bound_at": <unix time>}
HELIX_JOBS_NAMESPACE = "helix_jobs"

# How long a marker waits for history to add its job. A marker is set
# immediately before the single command that starts the print, so this spans
# only that command reaching Klipper and history adding the job.
HELIX_MARKER_TTL_S = 60.0

# How long a binding is kept once bound
HELIX_BINDING_MAX_AGE_S = 30 * 86400


def _job_key(job_id: Any) -> str:
    """History's job id as the hex string it reports; an int is rendered the
    same way so both spellings name one job."""
    if isinstance(job_id, int):
        return f"{job_id:06X}"
    return str(job_id)


class JobBinding:
    """A marker bound to one history job."""

    def __init__(
        self, kind: str, filenames: List[str], start_time: Any, bound_at: float,
        resumes: Optional[str] = None,
    ) -> None:
        self.kind = kind
        self.filenames = filenames
        self.start_time = start_time
        self.bound_at = bound_at
        # For a resume: the history job id of the job it continues
        self.resumes = resumes

    def record(self) -> Dict[str, Any]:
        extra = {"resumes": self.resumes} if self.resumes is not None else {}
        return {
            **extra,
            "kind": self.kind,
            "filenames": list(self.filenames),
            "start_time": self.start_time,
            "bound_at": self.bound_at,
        }


class HelixJobMarkers:
    """Markers a plugin path leaves before it starts a job, and the history
    job ids they were bound to.

    A marker is matched by filename to the next job history adds for that
    file, oldest marker first. Unmatched markers expire after `ttl`
    (HELIX_MARKER_TTL_S unless the owner sets its own). History reuses job ids (after a delete, or a
    delete-all and restart), so a binding answers only for its job's start
    time and one of its filenames, and an id history adds again without a
    marker loses its binding.
    """

    def __init__(
        self,
        clock: Callable[[], float] = time.monotonic,
        wall_clock: Callable[[], float] = time.time,
        kinds: Tuple[str, ...] = HELIX_JOB_KINDS,
        ttl: float = HELIX_MARKER_TTL_S,
    ) -> None:
        self._ttl = ttl
        self._clock = clock
        self._wall_clock = wall_clock
        # The kinds mark() accepts: a module keeping markers of its own passes
        # its private kinds, which helix_initiated() never answers.
        self._kinds = kinds
        # (start filename, kind, marked at, history filename or None)
        # (start filename, kind, marked at, history filename, resumed job id)
        self._pending: List[Tuple[str, str, float, Optional[str], Optional[str]]] = []
        self._bound: Dict[str, JobBinding] = {}

    def _expire(self) -> None:
        cutoff = self._clock() - self._ttl
        self._pending = [p for p in self._pending if p[2] >= cutoff]

    def mark(self, filename: str, kind: str, history_name: Optional[str] = None,
             resumes: Optional[str] = None) -> None:
        if kind not in self._kinds:
            raise ValueError(f"unknown helix job kind {kind!r}")
        self._expire()
        self._pending.append((filename, kind, self._clock(), history_name, resumes))

    def unmark(self, filename: str) -> bool:
        """Withdraw the newest pending marker for filename, for a start that
        failed. False when there was none."""
        for i in range(len(self._pending) - 1, -1, -1):
            if self._pending[i][0] == filename:
                del self._pending[i]
                return True
        return False

    def bind(self, filename: str, job_id: Any, start_time: Any) -> Optional[JobBinding]:
        """Bind the oldest live marker for filename to job_id; the binding, or
        None when no marker matches (a job some other client started)."""
        self._expire()
        for i, (name, kind, _t, history_name, resumes) in enumerate(self._pending):
            if name == filename:
                del self._pending[i]
                names = [name]
                if history_name and history_name != name:
                    names.append(history_name)
                binding = JobBinding(kind, names, start_time, self._wall_clock(), resumes)
                self._bound[_job_key(job_id)] = binding
                return binding
        return None

    def forget(self, job_id: Any) -> bool:
        """Drop the binding for job_id; True when there was one."""
        return self._bound.pop(_job_key(job_id), None) is not None

    def restore(self, job_id: Any, binding: JobBinding) -> None:
        """A binding read back from the database."""
        if binding.kind in HELIX_JOB_KINDS:
            self._bound[_job_key(job_id)] = binding

    def prune(self) -> List[str]:
        """Drop bindings older than HELIX_BINDING_MAX_AGE_S; their keys."""
        cutoff = self._wall_clock() - HELIX_BINDING_MAX_AGE_S
        old = [k for k, b in self._bound.items() if b.bound_at < cutoff]
        for key in old:
            del self._bound[key]
        return old

    def kind_of(self, job_id: Any, filename: str, start_time: Any) -> Optional[str]:
        bound = self._bound.get(_job_key(job_id))
        if bound is None or filename not in bound.filenames:
            return None
        if bound.start_time != start_time:
            return None
        return bound.kind

    def resumed_by(self, job_id: Any) -> Optional[str]:
        """The job id a bound resume continues, or None."""
        bound = self._bound.get(_job_key(job_id))
        return bound.resumes if bound is not None else None

    def pending_count(self) -> int:
        self._expire()
        return len(self._pending)


def _binding_from_record(record: Any) -> Optional[JobBinding]:
    """A stored binding, or None for a record that is not one."""
    if not isinstance(record, dict):
        return None
    kind = record.get("kind")
    filenames = record.get("filenames")
    start_time = record.get("start_time")
    bound_at = record.get("bound_at")
    if kind not in HELIX_JOB_KINDS:
        return None
    if not isinstance(filenames, list) or not filenames:
        return None
    if not all(isinstance(f, str) for f in filenames):
        return None
    if "start_time" not in record or isinstance(start_time, bool):
        return None
    if start_time is not None and not isinstance(start_time, (int, float)):
        return None
    if isinstance(bound_at, bool) or not isinstance(bound_at, (int, float)):
        return None
    resumes = record.get("resumes")
    return JobBinding(kind, filenames, start_time, float(bound_at),
                      resumes if isinstance(resumes, str) else None)


class PrintInfo:
    """Tracks information about an active modified print."""

    def __init__(
        self,
        original_filename: str,
        temp_filename: str,
        symlink_filename: str,
        modifications: List[str],
        start_time: float,
    ) -> None:
        self.original_filename = original_filename
        self.temp_filename = temp_filename
        self.symlink_filename = symlink_filename
        self.modifications = modifications
        self.start_time = start_time
        self.job_id: Optional[str] = None
        self.db_id: Optional[int] = None


class HelixPrint:
    """
    Moonraker component for handling modified G-code files.

    Provides:
    - Single API endpoint for modified print workflow
    - Symlink-based filename preservation for print_stats
    - History patching to record original filename
    - Automatic cleanup of temp files
    """

    def __init__(self, config: ConfigHelper) -> None:
        self.server: Server = config.get_server()
        self.eventloop = self.server.get_event_loop()

        # Configuration options
        self.temp_dir = config.get("temp_dir", ".helix_temp")
        self.symlink_dir = config.get("symlink_dir", ".helix_print")
        self.cleanup_delay = config.getint("cleanup_delay", 86400)  # 24 hours
        self.enabled = config.getboolean("enabled", True)
        self.convert_3mf = config.getboolean("convert_3mf", True)
        self.start_3mf_on_upload = config.getboolean("start_3mf_on_upload", False)
        self.converter: Optional[Any] = None
        self.plr: Optional[Any] = None
        self.analysis: Optional[Any] = None
        self.fit_check: Optional[Any] = None
        self.journal: Optional[Any] = None
        self.print_hooks: Optional[Any] = None
        self.companions: Optional[Any] = None
        self.storage: Optional[Any] = None
        self.filament: Optional[Any] = None
        self.photos: Optional[Any] = None
        self._history_rewrite_lock: Optional[asyncio.Lock] = None
        self.vendor_pushes: List[Any] = []
        self._link_lock: Optional[asyncio.Lock] = None

        # Validate directory names don't contain path separators
        if "/" in self.temp_dir:
            raise config.error("temp_dir cannot contain path separators")
        if "/" in self.symlink_dir:
            raise config.error("symlink_dir cannot contain path separators")

        # Component references (resolved after init)
        self.file_manager: Optional[Any] = None
        self.history: Optional[Any] = None
        self.klippy_apis: Optional[Any] = None
        self.database: Optional[Any] = None

        # State tracking
        self.active_prints: Dict[str, PrintInfo] = {}
        self.features: Dict[str, Feature] = {}
        self.job_markers = HelixJobMarkers()
        self.gc_path: Optional[Path] = None

        # Database backend flags (mutually exclusive, one will be True after init)
        self._use_sqlite = False      # database.execute_db_command
        self._use_namespace = False   # database.insert_item/get_item

        # Register API endpoints
        self.server.register_endpoint(
            "/server/helix/print_modified",
            ["POST"],
            self._handle_print_modified,
        )
        self.server.register_endpoint(
            "/server/helix/status",
            ["GET"],
            self._handle_status,
        )
        self.server.register_endpoint(
            "/server/helix/3mf_conversions",
            ["GET"],
            self._handle_3mf_conversions,
        )

        # Register event handlers
        self.server.register_event_handler(
            "job_state:state_changed", self._on_job_state_changed
        )
        self.server.register_event_handler(
            "history:history_changed", self._on_history_changed
        )
        self.server.register_event_handler(
            "server:klippy_ready", self._on_klippy_ready
        )
        register = getattr(self.server, "register_notification", None)
        if register is not None:
            register(FEATURES_CHANGED_EVENT)

        logging.info(
            f"HelixPrint v{PLUGIN_VERSION} initialized: temp={self.temp_dir}, "
            f"symlink={self.symlink_dir}, cleanup={self.cleanup_delay}s"
        )

    # =========================================================================
    # Input Validation
    # =========================================================================

    def _validate_filename(self, filename: str) -> None:
        """
        Validate filename for security issues.

        Raises server.error if validation fails.
        """
        if not filename:
            raise self.server.error("Filename cannot be empty", 400)

        # Check for null bytes and control characters
        if "\0" in filename or any(ord(c) < 32 for c in filename):
            raise self.server.error("Filename contains invalid characters", 400)

        # Check for absolute paths
        if filename.startswith("/"):
            raise self.server.error("Filename cannot be absolute path", 400)

        # Check for path traversal
        if ".." in filename:
            raise self.server.error("Filename cannot contain '..'", 400)

    def _validate_path_within_gcodes(self, path: Path) -> Path:
        """
        Resolve path and ensure it stays within gcodes directory.

        Returns resolved path if valid, raises server.error otherwise.
        """
        if self.gc_path is None:
            raise self.server.error("File manager not initialized", 500)

        # Resolve to absolute path (follows symlinks, resolves ..)
        try:
            resolved = path.resolve()
            gc_resolved = self.gc_path.resolve()
        except (OSError, RuntimeError) as e:
            raise self.server.error(f"Invalid path: {e}", 400)

        # Ensure resolved path is under gcodes directory
        try:
            resolved.relative_to(gc_resolved)
        except ValueError:
            raise self.server.error(
                "Path traversal detected: path escapes gcodes directory", 400
            )

        return resolved

    def _escape_gcode_string(self, s: str) -> str:
        """Escape a string for use in G-code commands."""
        # Remove any quotes that could break the command
        return s.replace('"', "").replace("'", "")

    # =========================================================================
    # Features
    # =========================================================================

    def register_feature(
        self,
        name: str,
        version: int,
        *,
        acts: bool,
        available: bool = True,
        reason: Optional[str] = None,
        switch: Optional[FeatureSwitch] = None,
        probe: Optional[FeatureProbe] = None,
        extra: Optional[FeatureExtra] = None,
    ) -> Feature:
        """Add a feature to the status `features` map.

        A feature with a `switch` owns its on/off: its plugin_features key is
        never read. One with a `probe` reads its availability live instead of
        from `available` / `reason`. Registering a name again replaces the
        entry.
        """
        feature = Feature(name, version, acts, available, reason, switch, probe, extra)
        self.features[name] = feature
        return feature

    def notify_features_changed(self) -> None:
        """Tell the screens a feature's availability changed: they act on the
        status they last read, so one that would take over for an unavailable
        feature, or hand back to an available one, must read it again."""
        send = getattr(self.server, "send_event", None)
        if send is not None:
            send(FEATURES_CHANGED_EVENT, {})

    async def _plugin_feature_opt_ins(self) -> Dict[str, Any]:
        if self.database is None:
            return {}
        try:
            record = await self.database.get_item(
                HELIX_SCREEN_NAMESPACE, PLUGIN_FEATURES_KEY, None
            )
        except Exception:
            return {}
        return record if isinstance(record, dict) else {}

    async def _feature_switch_value(
        self, feature: Feature, opt_ins: Optional[Dict[str, Any]]
    ) -> bool:
        if feature.switch is not None:
            try:
                return bool(await feature.switch())
            except Exception:
                logging.exception(f"HelixPrint: switch of feature {feature.name} failed")
                return False
        if opt_ins is None:
            opt_ins = await self._plugin_feature_opt_ins()
        return opt_in_enabled(opt_ins, feature.name, feature.acts)

    async def feature_enabled(self, name: str) -> bool:
        """Whether a feature may run now: registered, available and switched
        on. Read at the moment a feature acts, so a settings change applies
        without any notification."""
        feature = self.features.get(name)
        if feature is None or not feature_availability(feature, self.enabled)[0]:
            return False
        return await self._feature_switch_value(feature, None)

    async def _features_status(self) -> Dict[str, Any]:
        opt_ins = await self._plugin_feature_opt_ins()
        if self.companions is not None:
            self.companions.note_opt_ins(opt_ins)
        result: Dict[str, Any] = {}
        for name, feature in self.features.items():
            enabled = await self._feature_switch_value(feature, opt_ins)
            entry = feature_entry(feature, enabled, self.enabled)
            if feature.extra is not None:
                try:
                    entry.update({k: v for k, v in (await feature.extra()).items()
                                  if k not in entry})
                except Exception:
                    logging.exception(f"HelixPrint: extra fields of feature {name} failed")
            result[name] = entry
        return result

    # =========================================================================
    # Helix-initiated jobs
    # =========================================================================

    def mark_helix_initiated(
        self, filename: str, kind: str, history_name: Optional[str] = None
    ) -> None:
        """Record that a plugin path is starting `filename`, so history's job
        for that file carries `kind` (one of HELIX_JOB_KINDS). `history_name`
        is the name the finished history entry is rewritten to, when the start
        goes through a link (_patch_history_entry); the job answers to both.

        Call it immediately before the single command that starts the print
        (SDCARD_PRINT_FILE, or the M24 of an M23/M26/M24 resume), never before
        a script that homes, heats or purges first: the marker waits only
        HELIX_MARKER_TTL_S for history to add the job. A start that fails
        calls unmark_helix_initiated().
        """
        resumes = None
        if kind == "plr_resume":
            # A resume starts from PLR's snapshot of the interrupted job, which
            # is still PLR's job until history adds the resumed one.
            plr_job = getattr(self.plr, "job", None)
            if isinstance(plr_job, dict) and plr_job.get("job_id"):
                resumes = str(plr_job["job_id"])
        self.job_markers.mark(filename, kind, history_name, resumes)

    def unmark_helix_initiated(self, filename: str) -> bool:
        """Withdraw the marker of a start that failed."""
        return self.job_markers.unmark(filename)

    def helix_initiated(
        self, job_id: Any, filename: str, start_time: Any
    ) -> Optional[str]:
        """The kind of plugin path that started history job `job_id`, or None
        for a job some other client started. `filename` and `start_time` are
        the job's as history reports them, before or after the finish rename:
        history reuses ids, and a binding for a reused id names another file
        or start time."""
        return self.job_markers.kind_of(job_id, filename, start_time)

    def helix_resumed_job(self, job_id: Any) -> Optional[str]:
        """For a helix-initiated resume, the history job id it continues (PLR's
        interrupted job); None for any other job or when PLR named none."""
        return self.job_markers.resumed_by(job_id)

    async def _drop_job_bindings(self, keys: List[str], must_exist: bool) -> None:
        """Delete bindings from the database. A failure where the record must
        exist leaves a stale binding for the next restart, so it warns."""
        if self.database is None:
            return
        for key in keys:
            try:
                await self.database.delete_item(HELIX_JOBS_NAMESPACE, [key])
            except Exception as e:
                if must_exist:
                    logging.warning(f"HelixPrint: Failed to drop job marker {key}: {e}")
                else:
                    logging.debug(f"HelixPrint: No job marker {key} to drop: {e}")

    async def _load_job_markers(self) -> None:
        """Read bound markers back. Records that are malformed or older than
        HELIX_BINDING_MAX_AGE_S are deleted."""
        if self.database is None:
            return
        cutoff = time.time() - HELIX_BINDING_MAX_AGE_S
        drop: List[str] = []
        for key, record in await self._namespace_items(HELIX_JOBS_NAMESPACE):
            binding = _binding_from_record(record)
            if binding is None or binding.bound_at < cutoff:
                drop.append(key)
                continue
            self.job_markers.restore(key, binding)
        await self._drop_job_bindings(drop, must_exist=True)

    async def _bind_job_marker(self, filename: str, job_id: Any, start_time: Any) -> None:
        """Bind or clear the marker for a job history just added. The
        in-memory binding is made before the first await, so another
        module's handler of the same event already sees it."""
        binding = self.job_markers.bind(filename, job_id, start_time)
        pruned = self.job_markers.prune()
        forgotten: List[str] = []
        if binding is None and self.job_markers.forget(job_id):
            forgotten.append(_job_key(job_id))
        await self._drop_job_bindings(pruned, must_exist=False)
        await self._drop_job_bindings(forgotten, must_exist=True)
        if binding is None:
            return
        logging.info(f"HelixPrint: Job {job_id} is helix-initiated ({binding.kind})")
        if self.database is None:
            return
        try:
            await self.database.insert_item(
                HELIX_JOBS_NAMESPACE, [_job_key(job_id)], binding.record()
            )
        except Exception as e:
            logging.warning(f"HelixPrint: Failed to persist job marker: {e}")

    # =========================================================================
    # Component Lifecycle
    # =========================================================================

    async def component_init(self) -> None:
        """Called after all components are loaded."""
        self.file_manager = self.server.lookup_component("file_manager")
        self.history = self.server.lookup_component("history", None)
        self.klippy_apis = self.server.lookup_component("klippy_apis")
        self.database = self.server.lookup_component("database")

        # Get gcodes path
        self.gc_path = Path(self.file_manager.get_directory("gcodes"))

        # Ensure directories exist
        await self._ensure_directories()

        # Initialize database table
        await self._init_database()

        await self._start_3mf_converter()
        self._register_3mf_feature()
        await self._start_isolated("plr", True, self._start_plr)
        await self._start_isolated("file_analysis", False, self._start_analysis)
        await self._start_isolated("event_journal", False, self._start_journal)
        await self._start_isolated("config_edit", True, self._start_config)
        await self._start_isolated("server_edits", False, self._start_edits)
        await self._start_isolated("print_hooks", True, self._start_print_hooks)
        await self._start_isolated("storage", False, self._start_storage)
        await self._start_isolated("filament_accounting", True, self._start_filament)
        await self._start_isolated("photos", False, self._start_photos)
        await self._start_isolated("fit_check_blocks_autostart", True, self._start_fit_check)
        await self._start_isolated("vendor_ad5x", False, self._start_vendor_ad5x)
        await self._start_isolated("vendor_ace", False, self._start_vendor_ace)
        await self._start_isolated("klipper_companions", False, self._start_companions)

        await self._load_job_markers()
        if self.plr is not None:
            await self._start_isolated("plr", True, self.plr.load_job)
        # After recovery's job is loaded: the cleanup keeps what it names.
        self.eventloop.register_callback(self._startup_cleanup)
        # Analysis prunes stale layer indexes as it starts, and recovery's job,
        # loaded just above, names the ones it must keep.
        if self.analysis is not None and self.features["file_analysis"].available \
                and self.enabled:
            self.eventloop.register_callback(self.analysis.start)

    async def _start_isolated(
        self, name: str, acts: bool, start: Callable[[], Any]
    ) -> None:
        """Run one feature module's start. One that raises is registered
        unavailable with the reason, and the component and every other
        feature keep running. With the plugin disabled no module starts: no
        handler, endpoint or worker of one exists."""
        if not self.enabled:
            self.register_feature(name, 1, acts=acts, available=False, reason="plugin_disabled")
            return
        try:
            result = start()
            if inspect.isawaitable(result):
                await result
        except Exception as e:
            logging.exception(f"HelixPrint: feature {name} failed to start")
            previous = self.features.get(name)
            self.register_feature(
                name, previous.version if previous is not None else 1, acts=acts,
                available=False, reason=f"start_failed: {type(e).__name__}: {e}",
            )

    async def _start_3mf_converter(self) -> None:
        if not (self.enabled and self.convert_3mf):
            return
        if helix_3mf is None or helix_capabilities is None:
            logging.warning(
                "HelixPrint: helix_3mf.py or helix_capabilities.py not found, .3mf conversion off"
            )
            return
        self.converter = helix_3mf.Helix3mfConverter(
            self.server,
            self.file_manager,
            self.database,
            self.klippy_apis,
            self.gc_path,
            self.temp_dir,
            self.start_3mf_on_upload,
            helix_capabilities.prints_3mf_natively,
        )
        try:
            await self.converter.start()
        except Exception:
            logging.exception("HelixPrint: .3mf converter failed to start")
            await self.converter.close()
            self.converter.active = False

    def _converter_active(self) -> bool:
        return self.converter is not None and self.converter.active

    def _3mf_availability(self) -> Tuple[bool, Optional[str]]:
        """The converter's state now, from the same source as the status'
        top-level convert_3mf field."""
        if self._converter_active():
            return True, None
        if not self.convert_3mf:
            # Off by its own switch, not unavailable
            return True, None
        return False, (
            SIBLING_LOAD_ERRORS.get("helix_3mf")
            or SIBLING_LOAD_ERRORS.get("helix_capabilities")
            or ("stopped" if self.converter is not None else "start_failed")
        )

    def _register_3mf_feature(self) -> None:
        """.gcode.3mf conversion, switched by convert_3mf in moonraker.conf."""

        async def switch() -> bool:
            return self.convert_3mf

        self.register_feature(
            "convert_3mf", 1, acts=False, switch=switch, probe=self._3mf_availability,
        )

    def _data_dir(self) -> Path:
        """Moonraker's data directory (printer_data)."""
        get_args = getattr(self.server, "get_app_args", None)
        if get_args is not None:
            data_path = get_args().get("data_path")
            if data_path:
                return Path(data_path)
        return self.gc_path.parent if self.gc_path is not None else Path(".")

    def _start_plr(self) -> None:
        """Power-loss recovery (helix_plr.py), switched by its own plr_user
        record rather than plugin_features."""
        if helix_plr is None:
            self.register_feature(
                "plr", 1, acts=True, available=False,
                reason=SIBLING_LOAD_ERRORS.get("helix_plr", "module_missing"),
            )
            return
        self.plr = helix_plr.HelixPlr(self, self._data_dir())
        self.plr.start()
        self.register_feature(
            helix_plr.PLR_FEATURE, helix_plr.PLR_FEATURE_VERSION, acts=True,
            switch=self.plr.feature_enabled, probe=self.plr.availability,
            extra=self.plr.feature_extra,
        )

    def _start_analysis(self) -> None:
        """File analysis (helix_analysis.py). It only observes, so it has no
        switch."""
        if helix_analysis is None:
            self.register_feature(
                "file_analysis", 1, acts=False, available=False,
                reason=SIBLING_LOAD_ERRORS.get("helix_analysis", "module_missing"),
            )
            return
        self.analysis = helix_analysis.HelixAnalysis(self)
        self.analysis.register()
        self.register_feature(
            helix_analysis.ANALYSIS_FEATURE, helix_analysis.ANALYSIS_FEATURE_VERSION,
            acts=False, switch=always_on, probe=self.analysis.availability,
        )

    async def _start_journal(self) -> None:
        """Printer event journal (helix_journal.py). It has no switch: it owns
        one that is always on, so no plugin_features key turns it off."""
        if helix_journal is None:
            self.register_feature(
                "event_journal", 1, acts=False, available=False,
                reason=SIBLING_LOAD_ERRORS.get("helix_journal", "module_missing"),
            )
            return
        self.journal = helix_journal.HelixJournal(self)
        self.journal.start()
        self.register_feature(
            helix_journal.JOURNAL_FEATURE, helix_journal.JOURNAL_FEATURE_VERSION, acts=False,
            switch=always_on,
        )
        await self.journal.load()

    def _start_config(self) -> None:
        """Config read and edit (helix_config.py). It acts only when the user
        saves a change on the screen, which is its own consent, so it has no
        switch: always on while the module loads."""
        if helix_config is None:
            self.register_feature(
                "config_edit", 1, acts=True, available=False,
                reason=SIBLING_LOAD_ERRORS.get("helix_config", "module_missing"),
            )
            return
        helix_config.HelixConfig(self).start()
        self.register_feature(
            helix_config.CONFIG_EDIT_FEATURE, helix_config.CONFIG_EDIT_FEATURE_VERSION,
            acts=True, switch=always_on,
        )

    def _start_edits(self) -> None:
        """Server-side G-code edits (helix_edits.py): print_edited. A transport for
        a print the user started on the screen, so it has no switch."""
        if helix_edits is None:
            self.register_feature(
                "server_edits", 1, acts=False, available=False,
                reason=SIBLING_LOAD_ERRORS.get("helix_edits", "module_missing"),
            )
            return
        helix_edits.HelixEdits(self).start()
        self.register_feature(
            helix_edits.SERVER_EDITS_FEATURE, helix_edits.SERVER_EDITS_FEATURE_VERSION,
            acts=False, switch=always_on,
        )

    def _start_print_hooks(self) -> None:
        """Print-start hooks for prints started anywhere (helix_print_hooks.py),
        opted into through plugin_features."""
        if helix_print_hooks is None:
            self.register_feature(
                "print_hooks", 1, acts=True, available=False,
                reason=SIBLING_LOAD_ERRORS.get("helix_print_hooks", "module_missing"),
            )
            return
        markers = HelixJobMarkers(kinds=(helix_print_hooks.SCREEN_START_KIND,),
                                  ttl=helix_print_hooks.SCREEN_START_TTL_S)
        self.print_hooks = helix_print_hooks.PrintHooks(self, markers, HELIX_SCREEN_NAMESPACE)
        self.print_hooks.start()
        self.register_feature(
            helix_print_hooks.PRINT_HOOKS_FEATURE,
            helix_print_hooks.PRINT_HOOKS_FEATURE_VERSION,
            acts=True,
        )

    def _start_companions(self) -> None:
        """Klipper companions (helix_companions.py): Klipper-side modules
        installed for the features that need them. The installer itself
        always runs; `pa_gauge` heats and moves, so it is opted into."""
        if helix_companions is None:
            reason = SIBLING_LOAD_ERRORS.get("helix_companions", "module_missing")
            for name, acts in (("klipper_companions", False), ("pa_gauge", True)):
                self.register_feature(name, 1, acts=acts, available=False, reason=reason)
            return
        self.companions = helix_companions.CompanionManager(self)
        self.companions.start()

        async def companions_extra() -> Dict[str, Any]:
            return {"companions": self.companions.status()}

        def pa_gauge_availability() -> Tuple[bool, Optional[str]]:
            reason = self.companions.status().get("helix_pa_gauge", {}).get("reason")
            return reason is None, reason

        self.register_feature(
            helix_companions.COMPANIONS_FEATURE, helix_companions.COMPANIONS_FEATURE_VERSION,
            acts=False, switch=always_on, extra=companions_extra,
        )
        self.register_feature("pa_gauge", 1, acts=True, probe=pa_gauge_availability)

    def _start_storage(self) -> None:
        """Storage report, cleanup and free-space guard (helix_storage.py). The
        report only observes, so it has no switch; automatic cleanup acts, so
        it is its own feature, off until opted into through plugin_features."""
        if helix_storage is None:
            reason = SIBLING_LOAD_ERRORS.get("helix_storage", "module_missing")
            self.register_feature("storage", 1, acts=False, available=False, reason=reason)
            self.register_feature("storage_auto_cleanup", 1, acts=True, available=False,
                                  reason=reason)
            return
        self.storage = helix_storage.HelixStorage(self)
        self.storage.start()
        self.register_feature(
            helix_storage.STORAGE_FEATURE, helix_storage.STORAGE_FEATURE_VERSION,
            acts=False, switch=always_on, probe=self.storage.availability,
        )
        self.register_feature(
            helix_storage.AUTO_CLEANUP_FEATURE, helix_storage.AUTO_CLEANUP_FEATURE_VERSION,
            acts=True, probe=self.storage.availability,
        )

    def staging_dirs(self) -> Tuple[str, ...]:
        """Every top-level gcodes directory that holds print copies and their
        links rather than the user's files: temp_dir, symlink_dir and the
        app's staging directories."""
        return tuple(dict.fromkeys((self.temp_dir, self.symlink_dir) + APP_STAGING_DIRS))

    def link_lock(self) -> asyncio.Lock:
        """Held while a print link's directory is made and the link created,
        and while a storage cleanup removes files and empty directories."""
        if self._link_lock is None:
            self._link_lock = asyncio.Lock()
        return self._link_lock

    def check_free_space(self, incoming_bytes: int) -> None:
        """Refuse (507) a print whose copy would leave the gcodes volume below
        the storage floor. No guard without the storage module."""
        if self.storage is not None:
            self.storage.check_free_space(incoming_bytes)

    async def _start_filament(self) -> None:
        """Filament accounting per lane and tool (helix_filament.py), opted
        into through plugin_features."""
        if helix_filament is None:
            self.register_feature(
                "filament_accounting", 1, acts=True, available=False,
                reason=SIBLING_LOAD_ERRORS.get("helix_filament", "module_missing"),
            )
            return
        self.filament = helix_filament.FilamentAccounting(self, HELIX_SCREEN_NAMESPACE)
        self.filament.start()
        await self.filament.load()
        self.register_feature(
            helix_filament.FILAMENT_FEATURE, helix_filament.FILAMENT_FEATURE_VERSION, acts=True,
        )

    async def _start_photos(self) -> None:
        """Camera photos in history (helix_photos.py). It only observes and
        writes its own files, so it is on unless opted out in
        plugin_features."""
        if helix_photos is None or helix_gcode_reader is None:
            self.register_feature(
                "photos", 1, acts=False, available=False,
                reason=SIBLING_LOAD_ERRORS.get("helix_photos")
                or SIBLING_LOAD_ERRORS.get("helix_gcode_reader", "module_missing"),
            )
            return
        self.photos = helix_photos.HelixPhotos(self, helix_gcode_reader.atomic_write_bytes)
        self.photos.start()
        self.register_feature(
            helix_photos.PHOTOS_FEATURE, helix_photos.PHOTOS_FEATURE_VERSION, acts=False,
            probe=self.photos.availability,
        )
        await self.photos.load()
        self.eventloop.register_callback(self.photos.prune)

    def _start_fit_check(self) -> None:
        """File-fits-printer check (helix_fitcheck.py): holds a slicer's
        upload-and-print while the file breaks a printer limit, opted into
        through plugin_features. It reads file analysis' records, so it starts
        after that and after the .3mf converter whose starts it gates."""
        if helix_fitcheck is None:
            self.register_feature(
                "fit_check_blocks_autostart", 1, acts=True, available=False,
                reason=SIBLING_LOAD_ERRORS.get("helix_fitcheck", "module_missing"),
            )
            return
        self.fit_check = helix_fitcheck.FitCheck(self)
        self.fit_check.start()
        self.register_feature(
            helix_fitcheck.FIT_CHECK_FEATURE, helix_fitcheck.FIT_CHECK_FEATURE_VERSION,
            acts=True, probe=self.fit_check.availability,
        )

    def _start_vendor_push(
        self, name: str, module_name: str, version: int, factory: Optional[Callable[[Any], Any]]
    ) -> None:
        """A vendor push module: a transport that only observes, so it has no
        switch. Unavailable, with its reason, where its vendor is absent."""
        if factory is None:
            self.register_feature(
                name, version, acts=False, available=False,
                reason=SIBLING_LOAD_ERRORS.get(module_name)
                or SIBLING_LOAD_ERRORS.get("helix_vendor_push", "module_missing"),
            )
            return
        vendor = factory(self)
        vendor.start()
        self.vendor_pushes.append(vendor)
        self.register_feature(
            name, version, acts=False, switch=always_on, probe=vendor.availability,
        )

    def _start_vendor_ad5x(self) -> None:
        module = helix_vendor_ad5x
        self._start_vendor_push(
            "vendor_ad5x", "helix_vendor_ad5x",
            module.AD5X_FEATURE_VERSION if module is not None else 1,
            module.HelixVendorAd5x if module is not None else None,
        )

    def _start_vendor_ace(self) -> None:
        module = helix_vendor_ace
        self._start_vendor_push(
            "vendor_ace", "helix_vendor_ace",
            module.ACE_FEATURE_VERSION if module is not None else 1,
            module.HelixVendorAce if module is not None else None,
        )

    async def close(self) -> None:
        for vendor in self.vendor_pushes:
            vendor.close()
        if self.storage is not None:
            self.storage.close()
        if self.fit_check is not None:
            # Before the converter, whose upload hook this one wraps.
            self.fit_check.close()
        if self.analysis is not None:
            await self.analysis.close()
        if self.plr is not None:
            self.plr.close()
        if self.converter is not None:
            await self.converter.close()
        if self.photos is not None:
            await self.photos.close()

    async def _ensure_directories(self) -> None:
        """Ensure temp and symlink directories exist."""
        if self.gc_path is None:
            return

        temp_path = self.gc_path / self.temp_dir
        symlink_path = self.gc_path / self.symlink_dir

        temp_path.mkdir(parents=True, exist_ok=True)
        symlink_path.mkdir(parents=True, exist_ok=True)

        logging.debug(
            f"HelixPrint: Ensured directories exist: {temp_path}, {symlink_path}"
        )

    async def _init_database(self) -> None:
        """Initialize database for tracking temp files.

        Uses the database's execute_db_command when it has one, else its
        namespace key-value API (insert_item/get_item).
        """
        if self.database is None:
            logging.warning(
                "HelixPrint: Database not available, persistence disabled"
            )
            return

        # SQL table through execute_db_command, where the database offers it
        if hasattr(self.database, "execute_db_command"):
            try:
                await self.database.execute_db_command(
                    f"""
                    CREATE TABLE IF NOT EXISTS {HELIX_TEMP_TABLE} (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        original_filename TEXT NOT NULL,
                        temp_filename TEXT NOT NULL,
                        symlink_filename TEXT NOT NULL,
                        modifications TEXT,
                        job_id TEXT,
                        created_at REAL NOT NULL,
                        cleanup_scheduled_at REAL,
                        status TEXT DEFAULT 'active'
                    )
                    """
                )
                self._use_sqlite = True
                logging.info("HelixPrint: Using SQLite database for persistence")
                return
            except Exception as e:
                logging.warning(f"HelixPrint: SQLite init failed: {e}, trying namespace API")

        # Otherwise the namespace key-value API
        if hasattr(self.database, "insert_item"):
            self._use_namespace = True
            logging.info("HelixPrint: Using namespace API (insert_item/get_item) for persistence: "
                "the database has no execute_db_command")
            return

        logging.warning("HelixPrint: No compatible database API found, persistence disabled")

    # =========================================================================
    # API Handlers
    # =========================================================================

    async def _handle_status(self, web_request: WebRequest) -> Dict[str, Any]:
        """Handle status request - useful for plugin detection and version checking."""
        return {
            "enabled": self.enabled,
            "temp_dir": self.temp_dir,
            "symlink_dir": self.symlink_dir,
            "cleanup_delay": self.cleanup_delay,
            "active_prints": len(self.active_prints),
            "version": PLUGIN_VERSION,
            "convert_3mf": self._converter_active(),
            "start_3mf_on_upload": (
                self.converter is not None and self.converter.hook_installed
            ),
            "api": STATUS_API,
            "features": await self._features_status(),
        }

    async def _handle_3mf_conversions(self, web_request: WebRequest) -> Dict[str, Any]:
        """List .3mf conversions (README.md, "List .3mf Conversions")."""
        if self.converter is None:
            return {"conversions": []}
        return await self.converter.handle_conversions(web_request)

    async def _handle_print_modified(
        self, web_request: WebRequest
    ) -> Dict[str, Any]:
        """
        Handle the print_modified API request (v2.0 - path-based).

        This is the main entry point for printing modified G-code files.
        The client must upload the modified file first via standard Moonraker
        file upload, then call this endpoint with the path.

        Workflow:
        1. Client uploads modified file to .helix_temp/ via /server/files/upload
        2. Client calls this endpoint with temp_file_path
        3. Plugin validates paths, copies metadata, creates symlink
        4. Plugin starts print via symlink

        Parameters:
            original_filename: Path to the original G-code file (for history)
            temp_file_path: Path to the already-uploaded modified file
            modifications: List of modification identifiers for tracking
            copy_metadata: Whether to copy thumbnails from original (default: True)
        """
        if not self.enabled:
            raise self.server.error("HelixPrint component is disabled", 503)

        if self.gc_path is None:
            raise self.server.error("File manager not initialized", 500)

        original_filename = web_request.get_str("original_filename")
        temp_file_path = web_request.get_str("temp_file_path")
        modifications = web_request.get_list("modifications", [])
        copy_metadata = web_request.get_boolean("copy_metadata", True)
        return await self.start_linked_print(
            original_filename, temp_file_path, modifications, copy_metadata, "print_modified"
        )

    def resolve_original(self, original_filename: str) -> Path:
        """The original file of a linked print, resolved: a file inside the
        gcodes root that exists and is not itself a symlink."""
        self._validate_filename(original_filename)
        original_path = self.gc_path / original_filename
        original_resolved = self._validate_path_within_gcodes(original_path)

        if not original_resolved.exists():
            raise self.server.error(
                f"Original file not found: {original_filename}", 400
            )

        # Don't allow following symlinks for the original file
        if original_path.is_symlink():
            raise self.server.error(
                "Original file cannot be a symlink", 400
            )
        return original_resolved

    def resolve_temp(self, temp_file_path: str, original: Path) -> Path:
        """Where a linked print's copy lives, resolved: directly inside temp_dir
        or one of the app's staging directories, and never the original. A
        failed start deletes the copy, so any other path would let a request
        delete a user's file. The file need not exist yet."""
        self._validate_filename(temp_file_path)
        resolved = self._validate_path_within_gcodes(self.gc_path / temp_file_path)
        staging = {(self.gc_path / d).resolve() for d in (self.temp_dir,) + APP_STAGING_DIRS}
        if resolved.parent not in staging:
            raise self.server.error(
                f"Temp file must sit directly in a staging directory "
                f"({', '.join(sorted({self.temp_dir} | set(APP_STAGING_DIRS)))}): "
                f"{temp_file_path}", 400
            )
        if resolved == original:
            raise self.server.error("Temp file cannot be the original file", 400)
        return resolved

    async def start_linked_print(
        self,
        original_filename: str,
        temp_file_path: str,
        modifications: List[Any],
        copy_metadata: bool,
        kind: str,
    ) -> Dict[str, Any]:
        """Start the copy at `temp_file_path` through a link named after
        `original_filename`, so print_stats and history name the original.
        `kind` is the HELIX_JOB_KINDS entry the job is marked with. A start
        that fails removes the copy."""
        self._validate_filename(original_filename)
        self._validate_filename(temp_file_path)
        original_resolved = self.resolve_original(original_filename)
        temp_path = self.gc_path / temp_file_path
        temp_resolved = self.resolve_temp(temp_file_path, original_resolved)

        if not temp_resolved.exists():
            raise self.server.error(
                f"Temp file not found: {temp_file_path}. "
                "Upload the modified file first via /server/files/upload", 400
            )

        try:
            self.check_free_space(0)
        except Exception:
            temp_resolved.unlink(missing_ok=True)
            raise

        # Use the provided temp path (client already uploaded it)
        temp_filename = temp_file_path
        logging.info(f"HelixPrint: Using uploaded temp file {temp_filename}")

        # Copy metadata (thumbnails) from original
        if copy_metadata:
            await self._copy_metadata(original_resolved, temp_resolved)

        # The symlink mirrors the original's whole gcodes-relative path under a
        # `full/` segment, so the filename Klipper reports names the original on
        # its own. The segment tells clients this shape from the bare-filename
        # links earlier plugin versions made directly in the symlink directory.
        # Klipper only opens .gcode/.g/.gco names, so a .3mf original is linked
        # under the .gcode name it stands for.
        link_name = original_filename
        if link_name.lower().endswith(".3mf"):
            link_name = link_name[: -len(".3mf")]
            if not link_name.lower().endswith(".gcode"):
                link_name += ".gcode"
        symlink_filename = f"{self.symlink_dir}/full/{link_name}"
        symlink_path = self.gc_path / symlink_filename

        # Validate symlink path: it must land inside the symlink directory.
        resolved_parent = self._validate_path_within_gcodes(symlink_path.parent)
        try:
            resolved_parent.relative_to((self.gc_path / self.symlink_dir).resolve())
        except ValueError:
            raise self.server.error(
                "Path traversal detected: symlink escapes the symlink directory", 400
            )

        # Create symlink atomically (handles race condition). Under the link
        # lock, so a storage cleanup cannot remove the directory in between.
        async with self.link_lock():
            try:
                symlink_path.parent.mkdir(parents=True, exist_ok=True)
                self._create_symlink_atomic(symlink_path, temp_path)
                logging.info(
                    f"HelixPrint: Created symlink {symlink_filename} -> {temp_filename}"
                )
            except Exception as e:
                # Clean up temp file and any directories made for the link
                temp_path.unlink(missing_ok=True)
                self._remove_symlink(symlink_path)
                raise self.server.error(f"Failed to create symlink: {e}", 500)

        # Track this print
        print_info = PrintInfo(
            original_filename=original_filename,
            temp_filename=temp_filename,
            symlink_filename=symlink_filename,
            modifications=modifications,
            start_time=time.time(),
        )
        self.active_prints[symlink_filename] = print_info

        # Persist to database for crash recovery
        await self._persist_print_info(print_info)

        # Start the print with symlink path (escape filename for G-code)
        self.mark_helix_initiated(
            symlink_filename, kind, self._history_name(original_filename)
        )
        safe_symlink = self._escape_gcode_string(symlink_filename)
        try:
            await self.klippy_apis.run_gcode(
                f'SDCARD_PRINT_FILE FILENAME="{safe_symlink}"'
            )
            logging.info(f"HelixPrint: Started print with {symlink_filename}")
        except Exception as e:
            # Clean up on print start failure
            self.unmark_helix_initiated(symlink_filename)
            self._remove_symlink(symlink_path)
            temp_path.unlink(missing_ok=True)
            del self.active_prints[symlink_filename]
            raise self.server.error(f"Failed to start print: {e}", 500)

        return {
            "original_filename": original_filename,
            "print_filename": symlink_filename,
            "temp_filename": temp_filename,
            "status": "printing",
        }

    def _history_name(self, original_filename: str) -> str:
        """The name _patch_history_entry gives a finished job: the original
        file, without the symlink dir prefix."""
        if original_filename.startswith(f"{self.symlink_dir}/"):
            return original_filename[len(self.symlink_dir) + 1 :]
        return original_filename

    def _remove_symlink(self, symlink_path: Path) -> None:
        """Remove a print symlink and any directories it leaves empty, up to
        (not including) the symlink directory."""
        if symlink_path.is_symlink():
            symlink_path.unlink()
        if self.gc_path is None:
            return
        root = self.gc_path / self.symlink_dir
        parent = symlink_path.parent
        while parent != root and root in parent.parents:
            try:
                parent.rmdir()
            except OSError:
                break  # not empty, or already gone
            parent = parent.parent

    def _create_symlink_atomic(self, symlink_path: Path, target_path: Path) -> None:
        """
        Create symlink atomically, handling existing files.

        Uses try/except pattern to avoid TOCTOU race conditions.
        """
        try:
            symlink_path.symlink_to(target_path)
        except FileExistsError:
            # Remove existing and retry
            if symlink_path.is_symlink() or symlink_path.exists():
                symlink_path.unlink()
            symlink_path.symlink_to(target_path)

    # =========================================================================
    # Metadata Handling
    # =========================================================================

    async def _copy_metadata(
        self, original_path: Path, temp_path: Path
    ) -> None:
        """Copy slicer metadata (thumbnails) from original to temp file."""
        if self.gc_path is None:
            return

        thumbs_dir = self.gc_path / ".thumbs"
        if not thumbs_dir.exists():
            return

        original_stem = original_path.stem
        temp_stem = temp_path.stem

        # Escape glob special characters in the stem
        escaped_stem = glob_module.escape(original_stem)

        # Find and link thumbnails for the original file
        for thumb in thumbs_dir.glob(f"{escaped_stem}*"):
            try:
                # Create symlink to original thumbnail with new name
                new_name = thumb.name.replace(original_stem, temp_stem)
                temp_thumb = thumbs_dir / new_name
                if not temp_thumb.exists():
                    temp_thumb.symlink_to(thumb)
                    logging.debug(
                        f"HelixPrint: Linked thumbnail {new_name} -> {thumb.name}"
                    )
            except Exception as e:
                logging.warning(f"HelixPrint: Failed to link thumbnail: {e}")

    # =========================================================================
    # Database Operations
    # =========================================================================

    async def _persist_print_info(self, print_info: PrintInfo) -> None:
        """Save print info to database for crash recovery."""
        record = {
            "original_filename": print_info.original_filename,
            "temp_filename": print_info.temp_filename,
            "symlink_filename": print_info.symlink_filename,
            "modifications": print_info.modifications,
            "created_at": time.time(),
            "cleanup_scheduled_at": None,
            "status": "active",
        }

        try:
            if self._use_sqlite:
                result = await self.database.execute_db_command(
                    f"""
                    INSERT INTO {HELIX_TEMP_TABLE}
                    (original_filename, temp_filename, symlink_filename,
                     modifications, created_at, status)
                    VALUES (?, ?, ?, ?, ?, ?)
                    """,
                    (
                        record["original_filename"],
                        record["temp_filename"],
                        record["symlink_filename"],
                        json.dumps(record["modifications"]),
                        record["created_at"],
                        record["status"],
                    ),
                )
                print_info.db_id = result.lastrowid
            elif self._use_namespace:
                # Use temp_filename as key (unique per print)
                await self.database.insert_item(
                    HELIX_NAMESPACE,
                    _ns_key(print_info.temp_filename),
                    record,
                )
        except Exception as e:
            logging.warning(f"HelixPrint: Failed to persist print info: {e}")

    # =========================================================================
    # Event Handlers
    # =========================================================================

    async def _on_history_changed(self, payload: Dict[str, Any]) -> None:
        """Record the history id of a print we staged.

        The id is history's own, assigned when it writes the row, and it is
        never part of Klipper's print_stats - so this event is the only place
        it can be read. It also carries the id in its payload rather than on
        the component, which matters: every handler of a Moonraker event runs
        under one asyncio.gather(), so a handler that reached into history for
        the live id would be racing history's own handler for it.
        """
        if payload.get("action") != "added":
            return
        job = payload.get("job") or {}
        job_id = job.get("job_id")
        if not job_id:
            return
        filename = job.get("filename", "")
        print_info = self.active_prints.get(filename)
        if print_info is not None:
            print_info.job_id = job_id
            logging.info(f"HelixPrint: Job started with ID {job_id}")
        await self._bind_job_marker(filename, job_id, job.get("start_time"))

    async def _on_klippy_ready(self) -> None:
        """Handle Klipper ready event - recover from any interrupted prints."""
        logging.debug("HelixPrint: Klipper ready, checking for interrupted prints")
        # Recovery logic would go here if needed

    async def _on_job_state_changed(
        self,
        job_event: Any,
        prev_stats: Dict[str, Any],
        new_stats: Dict[str, Any],
    ) -> None:
        """Handle job state changes to patch history."""
        state = new_stats.get("state", "")
        filename = new_stats.get("filename", "")

        # Check if this is one of our modified prints
        if not filename.startswith(f"{self.symlink_dir}/"):
            return

        print_info = self.active_prints.get(filename)
        if not print_info:
            logging.warning(f"HelixPrint: Unknown modified file: {filename}")
            return

        # Handle completion states
        if state in ("complete", "cancelled", "error"):
            logging.info(f"HelixPrint: Job finished ({state}): {filename}")

            # Patch history entry
            if self.history is not None:
                await self._patch_history_entry(print_info, state)

            # Schedule cleanup
            await self._schedule_cleanup(print_info)

            # Remove from active tracking
            del self.active_prints[filename]

    def _history_lock(self) -> asyncio.Lock:
        # Made inside the running loop: Python 3.8 binds a Lock to the loop
        # current when it is created.
        if self._history_rewrite_lock is None:
            self._history_rewrite_lock = asyncio.Lock()
        return self._history_rewrite_lock

    @staticmethod
    def drop_history_aux(existing: Any, name: str) -> List[Any]:
        """A history row's auxiliary_data without our `name` entry, every
        other entry kept. A value that is not a list is dropped rather than
        walked: iterating a dict would yield its keys and write them back over
        the real thing."""
        entries = existing if isinstance(existing, list) else []
        return [e for e in entries
                if not (isinstance(e, dict) and e.get("provider") == HELIX_AUX_PROVIDER
                        and e.get("name") == name)]

    @staticmethod
    def set_history_aux(existing: Any, name: str, value: Any, description: str) -> List[Any]:
        """A history row's auxiliary_data with our `name` entry set: every
        other entry kept, an earlier `name` entry of ours replaced."""
        if existing is not None and not isinstance(existing, list):
            logging.warning(
                "HelixPrint: Ignoring auxiliary_data of unexpected type "
                f"{type(existing).__name__}"
            )
        kept = HelixPrint.drop_history_aux(existing, name)
        kept.append({"provider": HELIX_AUX_PROVIDER, "name": name, "value": value,
                     "description": description, "units": None})
        return kept

    async def patch_history_job(
        self, job_id: Any, update: Callable[[Dict[str, Any]], bool]
    ) -> bool:
        """Rewrite one history row: `update` changes the row in place and
        returns whether to save it. Every rewrite of a row goes through here,
        one at a time, so two features changing the same finished job never
        save over each other's change. Best effort: needs v0.9.0's
        get_job/save_job, and any failure is logged and swallowed."""
        if not self.history or not job_id:
            return False

        if not hasattr(self.history, "get_job") or not hasattr(
            self.history, "save_job"
        ):
            logging.warning(
                "HelixPrint: History rewrite unavailable "
                "(history component has no get_job/save_job; needs Moonraker "
                "v0.9.0 or newer). The print itself is unaffected."
            )
            return False

        # PrinterJob lives in whichever module defined the live history
        # component, so ask that module rather than guessing an import path.
        job_class = getattr(
            sys.modules.get(type(self.history).__module__), "PrinterJob", None
        )
        if job_class is None:
            logging.warning(
                "HelixPrint: History rewrite unavailable "
                "(no PrinterJob alongside the history component)"
            )
            return False

        try:
            async with self._history_lock():
                job = await self.history.get_job(job_id)
                if not job:
                    logging.warning(f"HelixPrint: Job {job_id} not in history")
                    return False
                if not update(job):
                    return False
                # save_job REPLACEs the row addressed by job_id, and wants the
                # numeric id even though get_job also accepts the hex spelling.
                numeric_id = (
                    int(job_id, 16) if isinstance(job_id, str) else int(job_id)
                )
                await self.history.save_job(job_class(job), numeric_id)
            return True
        except Exception as e:
            logging.exception(f"HelixPrint: Failed to rewrite history job {job_id}: {e}")
            return False

    async def _patch_history_entry(
        self, print_info: PrintInfo, final_state: str
    ) -> None:
        """Rewrite the finished history entry so it names the original file.

        Cosmetic and best-effort: history shows the file the user picked
        instead of the symlink we printed through. Any failure is logged and
        swallowed so it can never surface in a print.
        """
        original = self._history_name(print_info.original_filename)

        def update(job: Dict[str, Any]) -> bool:
            job["auxiliary_data"] = self.set_history_aux(
                job.get("auxiliary_data"), "modifications",
                {
                    "modifications": print_info.modifications,
                    "temp_file": print_info.temp_filename,
                    "symlink": print_info.symlink_filename,
                    "original": print_info.original_filename,
                },
                "G-code modifications applied by HelixScreen",
            )
            job["filename"] = original
            return True

        if await self.patch_history_job(print_info.job_id, update):
            logging.info(
                f"HelixPrint: Patched history {print_info.job_id} "
                f"filename to '{original}'"
            )

    # =========================================================================
    # Cleanup Operations
    # =========================================================================

    async def _schedule_cleanup(self, print_info: PrintInfo) -> None:
        """Schedule cleanup of temp files after delay."""
        if self.gc_path is None:
            return

        # The link goes now, unless power-loss recovery may still resume
        # through it (a print that ended in error); then it goes with the copy.
        symlink_path = self.gc_path / print_info.symlink_filename
        if not self.recovery_protects(symlink_path, self.gc_path / print_info.temp_filename):
            self._remove_symlink(symlink_path)
            logging.debug(f"HelixPrint: Removed symlink {symlink_path}")

        # Also clean up thumbnail symlinks
        await self._cleanup_thumbnail_symlinks(print_info.temp_filename)

        # Update database status
        cleanup_time = time.time() + self.cleanup_delay
        try:
            if self._use_sqlite:
                await self.database.execute_db_command(
                    f"""
                    UPDATE {HELIX_TEMP_TABLE}
                    SET cleanup_scheduled_at = ?, status = ?
                    WHERE temp_filename = ?
                    """,
                    (cleanup_time, "pending_cleanup", print_info.temp_filename),
                )
            elif self._use_namespace:
                # Update record in namespace storage
                record = await self.database.get_item(
                    HELIX_NAMESPACE, _ns_key(print_info.temp_filename), None
                )
                if record:
                    record["cleanup_scheduled_at"] = cleanup_time
                    record["status"] = "pending_cleanup"
                    await self.database.update_item(
                        HELIX_NAMESPACE, _ns_key(print_info.temp_filename), record
                    )
                else:
                    logging.warning(
                        f"HelixPrint: Record not found for cleanup scheduling: "
                        f"{print_info.temp_filename}"
                    )
        except Exception as e:
            logging.warning(f"HelixPrint: Failed to update cleanup status: {e}")

        # Schedule delayed cleanup
        self.eventloop.delay_callback(
            self.cleanup_delay,
            self._cleanup_temp_file,
            print_info.temp_filename,
            print_info.symlink_filename,
        )

        logging.info(
            f"HelixPrint: Scheduled cleanup of {print_info.temp_filename} "
            f"in {self.cleanup_delay}s"
        )

    async def _cleanup_thumbnail_symlinks(self, temp_filename: str) -> None:
        """Clean up thumbnail symlinks for a temp file."""
        if self.gc_path is None:
            return

        thumbs_dir = self.gc_path / ".thumbs"
        if not thumbs_dir.exists():
            return

        temp_stem = Path(temp_filename).stem

        # Escape glob special characters
        escaped_stem = glob_module.escape(temp_stem)

        for thumb in thumbs_dir.glob(f"{escaped_stem}*"):
            if thumb.is_symlink():
                thumb.unlink()
                logging.debug(f"HelixPrint: Removed thumbnail symlink {thumb}")

    def recovery_protects(self, *paths: Path) -> bool:
        """Whether power-loss recovery still needs any of `paths` (print copies
        and links): its job may resume from it (HelixPlr.protected_paths), or a
        snapshot waits whose job record could not be read, so any of them may
        be the one (HelixPlr.resume_unaccounted)."""
        protected = getattr(self.plr, "protected_paths", None)
        if protected is None:
            return False
        if self.plr.resume_unaccounted():
            return True
        needed = set(protected())
        return any(os.path.abspath(str(p)) in needed or os.path.realpath(str(p)) in needed
                   for p in paths)

    async def _cleanup_temp_file(
        self, temp_filename: str, symlink_filename: Optional[str] = None
    ) -> None:
        """Delete a temp file, and its link when one is named, after the
        cleanup delay. While power-loss recovery may resume from either, both
        stay and the cleanup is tried again a delay later."""
        if self.gc_path is None:
            return

        temp_path = self.gc_path / temp_filename
        symlink_path = self.gc_path / symlink_filename if symlink_filename else None
        if self.recovery_protects(*[p for p in (temp_path, symlink_path) if p is not None]):
            logging.info(
                f"HelixPrint: Keeping {temp_filename}: power-loss recovery may resume from it"
            )
            self.eventloop.delay_callback(
                self.cleanup_delay, self._cleanup_temp_file, temp_filename, symlink_filename
            )
            return
        if symlink_path is not None:
            self._remove_symlink(symlink_path)
        file_deleted = False
        try:
            if temp_path.exists():
                temp_path.unlink()
                file_deleted = True
                logging.info(f"HelixPrint: Cleaned up {temp_filename}")
            else:
                # File already gone (manual deletion or previous cleanup)
                file_deleted = True
        except OSError as e:
            logging.error(f"HelixPrint: Failed to delete {temp_filename}: {e}")
            return  # Don't mark as cleaned if file delete failed

        # Only update database if file was actually deleted
        if not file_deleted:
            return

        try:
            if self._use_sqlite:
                await self.database.execute_db_command(
                    f"""
                    UPDATE {HELIX_TEMP_TABLE}
                    SET status = ?
                    WHERE temp_filename = ?
                    """,
                    ("cleaned", temp_filename),
                )
            elif self._use_namespace:
                record = await self.database.get_item(
                    HELIX_NAMESPACE, _ns_key(temp_filename), None
                )
                if record:
                    record["status"] = "cleaned"
                    await self.database.update_item(
                        HELIX_NAMESPACE, _ns_key(temp_filename), record
                    )
                else:
                    logging.debug(
                        f"HelixPrint: No record to update for cleaned file: {temp_filename}"
                    )
        except Exception as e:
            logging.warning(f"HelixPrint: Failed to update cleanup status: {e}")

    async def _namespace_items(self, namespace: str = HELIX_NAMESPACE) -> List[Any]:
        """(key, value) pairs of a namespace; empty when it does not exist
        (helix_file_queue.namespace_items)."""
        if helix_file_queue is None:
            return []
        return await helix_file_queue.namespace_items(self.database, namespace)

    async def _migrate_legacy_keys(self) -> None:
        """Re-store records a dotted string key left nested, one record per key.

        Each is re-inserted under its own temp_filename and the old top-level
        entry deleted. A value holding no record is left alone.
        """
        for key, value in await self._namespace_items():
            if isinstance(value, dict) and value.get("temp_filename") == key:
                continue
            records = _legacy_records(value)
            if not records:
                continue
            for record in records:
                await self.database.insert_item(
                    HELIX_NAMESPACE, _ns_key(record["temp_filename"]), record
                )
            await self.database.delete_item(HELIX_NAMESPACE, _ns_key(key))
            logging.info(
                f"HelixPrint: Re-keyed {len(records)} temp file record(s) stored as {key!r}"
            )

    def _sweep_dangling_links(self) -> int:
        """Remove print links whose temp file is gone, and the directories
        that leaves empty.

        Needs no database record, so a link a crash or a lost record left
        behind is still found. That matters most where symlink_dir is a
        visible directory (a Klipper that will not open files under a hidden
        one), because every leftover link shows in the file list. A link
        whose target still exists may belong to a print in progress and is
        kept.
        """
        if self.gc_path is None:
            return 0
        root = self.gc_path / self.symlink_dir
        if root.is_symlink() or not root.is_dir():
            return 0
        removed = 0
        for dirpath, _dirs, files in os.walk(root, followlinks=False):
            for name in files:
                link = Path(dirpath) / name
                if link.is_symlink() and not link.exists():
                    self._remove_symlink(link)
                    removed += 1
        return removed

    async def _startup_cleanup(self) -> None:
        """Clean up stale temp files on startup."""
        if self.gc_path is None:
            return
        removed = self._sweep_dangling_links()
        if removed:
            logging.info(f"HelixPrint: Removed {removed} print links with no file behind them")
        if not self._use_sqlite and not self._use_namespace:
            return

        now = time.time()

        try:
            # Get pending cleanup records
            pending_records: List[Dict[str, Any]] = []

            if self._use_sqlite:
                rows = await self.database.execute_db_command(
                    f"""
                    SELECT temp_filename, symlink_filename
                    FROM {HELIX_TEMP_TABLE}
                    WHERE status = 'pending_cleanup' AND cleanup_scheduled_at < ?
                    """,
                    (now,),
                )
                if rows:
                    pending_records = [dict(r) for r in rows]

            elif self._use_namespace:
                await self._migrate_legacy_keys()
                for key, record in await self._namespace_items():
                    if (
                        record.get("status") == "pending_cleanup"
                        and record.get("cleanup_scheduled_at")
                        and record["cleanup_scheduled_at"] < now
                    ):
                        pending_records.append(record)

            # Clean up each pending file
            cleaned_count = 0
            for record in pending_records:
                temp_filename = record["temp_filename"]
                symlink_filename = record["symlink_filename"]

                # Clean up files
                temp_path = self.gc_path / temp_filename
                symlink_path = self.gc_path / symlink_filename
                if self.recovery_protects(temp_path, symlink_path):
                    logging.info(f"HelixPrint: Keeping {temp_filename} for power-loss recovery")
                    self.eventloop.delay_callback(
                        self.cleanup_delay, self._cleanup_temp_file, temp_filename,
                        symlink_filename,
                    )
                    continue

                if temp_path.exists():
                    temp_path.unlink()
                self._remove_symlink(symlink_path)

                # Clean up thumbnail symlinks
                await self._cleanup_thumbnail_symlinks(temp_filename)

                # Update status
                if self._use_sqlite:
                    await self.database.execute_db_command(
                        f"""
                        UPDATE {HELIX_TEMP_TABLE}
                        SET status = ?
                        WHERE temp_filename = ?
                        """,
                        ("cleaned", temp_filename),
                    )
                elif self._use_namespace:
                    record["status"] = "cleaned"
                    await self.database.update_item(
                        HELIX_NAMESPACE, _ns_key(temp_filename), record
                    )
                cleaned_count += 1

            if cleaned_count > 0:
                logging.info(
                    f"HelixPrint: Startup cleanup removed {cleaned_count} stale files"
                )

            # Purge old database records to prevent unbounded growth
            purge_cutoff = now - DB_RECORD_MAX_AGE
            purged_count = 0

            if self._use_sqlite:
                deleted = await self.database.execute_db_command(
                    f"""
                    DELETE FROM {HELIX_TEMP_TABLE}
                    WHERE status = 'cleaned' AND created_at < ?
                    """,
                    (purge_cutoff,),
                )
                if deleted and deleted.rowcount > 0:
                    purged_count = deleted.rowcount

            elif self._use_namespace:
                for key, record in await self._namespace_items():
                    if (
                        record.get("status") == "cleaned"
                        and record.get("created_at")
                        and record["created_at"] < purge_cutoff
                    ):
                        await self.database.delete_item(HELIX_NAMESPACE, _ns_key(key))
                        purged_count += 1

            if purged_count > 0:
                logging.info(
                    f"HelixPrint: Purged {purged_count} old database records"
                )

        except Exception as e:
            logging.exception(f"HelixPrint: Startup cleanup failed: {e}")


def load_component(config: ConfigHelper) -> HelixPrint:
    """Factory function to load the HelixPrint component."""
    return HelixPrint(config)
