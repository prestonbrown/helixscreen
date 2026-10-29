# SPDX-License-Identifier: GPL-3.0-or-later
"""Klipper companions: Klipper-side modules the plugin installs.

Some features need code in Klipper's own process, which only a klippy extras
module can provide (an MCU's commands, the motion queue). The plugin ships
those modules under klippy/ and links them into <klipper_path>/klippy/extras/,
with each one's config section in <config>/helix_companions/<name>.cfg. The
installer adds a single [include helix_companions/*.cfg] to printer.cfg; an
empty glob is legal in Klipper, so with no companions the printer boots as
before.

A section naming a module that is missing stops Klipper from booting, so a
module goes in before its section and its section comes out before it. When
Klipper did stop on one of our sections (a firmware update can replace the
extras directory), the module is put back and Klipper restarted: the one
restart the plugin sends unasked, and never during a job.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple

import helix_printer_cfg

COMPANIONS_FEATURE = "klipper_companions"
COMPANIONS_FEATURE_VERSION = 1
INCLUDE_DIR = "helix_companions"
INCLUDE_MARKER = "# Added by HelixScreen: loads the Klipper modules HelixPrint features install"
INCLUDE_LINE = "[include helix_companions/*.cfg]"
# The module files this plugin put into Klipper's extras, so an uninstall can
# take them away without knowing where Klipper lives.
LINKS_FILE = ".links.json"

Action = Tuple[str, ...]


@dataclass(frozen=True)
class Companion:
    name: str
    files: Tuple[str, ...]
    feature: str
    # Installed only on a printer with at least one of these Klipper objects.
    needs_any: Tuple[str, ...]
    version: int


COMPANIONS: Tuple[Companion, ...] = (
    Companion(
        name="helix_pa_gauge",
        files=("helix_pa_gauge.py", "helix_pa_fit.py"),
        feature="pa_gauge",
        needs_any=("prtouch_v3", "load_cell", "load_cell_probe"),
        version=1,
    ),
)


def _ours_stopped_klipper(error: str, name: str) -> bool:
    return ("'%s'" % name) in error and "not a valid config section" in error


def plan_actions(
    companions: Iterable[Companion],
    *,
    enabled: Set[str],
    objects: Optional[Sequence[str]],
    installed: Dict[str, Dict[str, bool]],
    klipper_path: Optional[str],
    klippy_error: str,
    job_active: bool,
) -> List[Action]:
    """What to do now. `objects` is Klipper's object list, None while Klippy
    is not ready. `installed[name]` holds `files`, `cfg` and `current` (the
    linked files match the bundled ones)."""
    actions: List[Action] = []
    repaired = False
    for c in companions:
        state = installed.get(c.name, {})
        present = state.get("files") or state.get("cfg")
        if klipper_path is None:
            if c.feature in enabled:
                actions.append(("unavailable", c.name, "klipper_path_unknown"))
            continue
        if klippy_error and _ours_stopped_klipper(klippy_error, c.name):
            actions.append(("install", c.name))
            repaired = True
            continue
        if objects is None:
            continue
        wanted = c.feature in enabled and any(o in objects for o in c.needs_any)
        if wanted and not (state.get("files") and state.get("cfg") and state.get("current")):
            actions += [("install", c.name), ("restart_pending", c.name)]
        elif not wanted and present:
            actions += [("remove", c.name), ("restart_pending", c.name)]
    if repaired and not job_active:
        actions.append(("restart",))
    return actions


# --- the files --------------------------------------------------------------

class RealFs:
    def link(self, src: str, dst: str) -> None:
        if os.path.lexists(dst):
            os.remove(dst)
        os.symlink(src, dst)

    def copy(self, src: str, dst: str) -> None:
        tmp = dst + ".helix-tmp"
        shutil.copyfile(src, tmp)
        os.replace(tmp, dst)

    def write(self, path: str, text: str) -> None:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".helix-tmp"
        with open(tmp, "w") as f:
            f.write(text)
        os.replace(tmp, path)

    def remove(self, path: str) -> None:
        if os.path.lexists(path):
            os.remove(path)


def extras_dir(klipper_path: str) -> str:
    return os.path.join(klipper_path, "klippy", "extras")


def section_path(c: Companion, config_dir: str) -> str:
    return os.path.join(config_dir, INCLUDE_DIR, c.name + ".cfg")


def section_text(c: Companion) -> str:
    return "[%s]\n# Managed by the HelixPrint plugin (%s feature). Removed with it.\n" % (
        c.name, c.feature)


def install(c: Companion, fs: Any, *, plugin_dir: str, klipper_path: str, config_dir: str) -> None:
    """Module files first, then the section that loads them."""
    for name in c.files:
        src = os.path.join(plugin_dir, "klippy", name)
        dst = os.path.join(extras_dir(klipper_path), name)
        try:
            fs.link(src, dst)
        except OSError:
            fs.copy(src, dst)
    fs.write(section_path(c, config_dir), section_text(c))


def remove(c: Companion, fs: Any, *, klipper_path: str, config_dir: str) -> None:
    """The section first, so Klipper never reads one whose module is gone."""
    fs.remove(section_path(c, config_dir))
    for name in c.files:
        fs.remove(os.path.join(extras_dir(klipper_path), name))


def ensure_include(config_dir: str) -> bool:
    """Add the include to printer.cfg (backed up first). True when it changed."""
    cfg = Path(config_dir) / "printer.cfg"
    text = read_text(cfg)
    new_text = helix_printer_cfg.add_include(text, INCLUDE_MARKER, INCLUDE_LINE)
    if new_text is None:
        return False
    helix_printer_cfg.write_change(cfg, text, new_text)
    return True


def read_text(path: Path) -> str:
    with open(str(path), encoding="utf-8") as f:
        return f.read()


def _links_path(config_dir: str) -> Path:
    return Path(config_dir) / INCLUDE_DIR / LINKS_FILE


def record_links(config_dir: str, klipper_path: str, c: Companion, present: bool) -> None:
    path = _links_path(config_dir)
    try:
        links = json.loads(path.read_text())
    except (OSError, ValueError):
        links = {}
    files = set(links.get("files", []))
    paths = {os.path.join(extras_dir(klipper_path), name) for name in c.files}
    files = (files | paths) if present else (files - paths)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"files": sorted(files)}, indent=1))


def uninstall_companions(config_dir: Path) -> Optional[str]:
    """The uninstaller's whole companion step: the include out of printer.cfg,
    the sections' directory gone, then every module file the plugin put into
    Klipper. None on success, or the problem as a string."""
    config_dir = Path(config_dir)
    cfg = config_dir / "printer.cfg"
    try:
        if cfg.exists():
            text = read_text(cfg)
            new_text, removed = helix_printer_cfg.strip_include(text, INCLUDE_MARKER, INCLUDE_LINE)
            if removed:
                helix_printer_cfg.write_change(cfg, text, new_text)
        try:
            files = json.loads(_links_path(str(config_dir)).read_text()).get("files", [])
        except (OSError, ValueError):
            files = []
        if (config_dir / INCLUDE_DIR).is_dir():
            shutil.rmtree(str(config_dir / INCLUDE_DIR))
        for f in files:
            if os.path.lexists(f):
                os.remove(f)
    except (OSError, UnicodeDecodeError) as e:
        return str(e)
    return None


def installed_state(c: Companion, *, plugin_dir: str, klipper_path: str,
                    config_dir: str) -> Dict[str, bool]:
    """What is on disk now: files present, section present, and whether each
    linked or copied file matches the bundled one."""
    current = True
    files = True
    for name in c.files:
        dst = os.path.join(extras_dir(klipper_path), name)
        src = os.path.join(plugin_dir, "klippy", name)
        if not os.path.exists(dst):
            files = current = False
            continue
        try:
            with open(dst, "rb") as a, open(src, "rb") as b:
                current = current and a.read() == b.read()
        except OSError:
            current = False
    return {"files": files, "cfg": os.path.exists(section_path(c, config_dir)),
            "current": current}


# --- Moonraker side ---------------------------------------------------------

class CompanionManager:
    """Keeps the installed companions matching the enabled features. Syncs at
    Klippy start and ready, and when a screen asks after the user switched a
    feature (opt-ins are read lazily, so nothing else announces the change)."""

    def __init__(self, helix: Any, plugin_dir: Optional[str] = None, fs: Any = None) -> None:
        self.helix = helix
        self.plugin_dir = plugin_dir or os.path.dirname(os.path.realpath(__file__))
        self.fs = fs or RealFs()
        self.state: Dict[str, Dict[str, Any]] = {}
        self._lock = asyncio.Lock()

    def start(self) -> None:
        server = self.helix.server
        for event in ("server:klippy_ready", "server:klippy_started"):
            server.register_event_handler(event, self.sync)
        server.register_endpoint("/server/helix/companions/sync", ["POST"], self._handle_sync)

    async def _handle_sync(self, web_request: Any) -> Dict[str, Any]:
        await self.sync()
        return self.status()

    def status(self) -> Dict[str, Any]:
        return {name: dict(entry) for name, entry in self.state.items()}

    async def _facts(self) -> Tuple[Optional[str], Optional[List[str]], str, bool]:
        apis = self.helix.klippy_apis
        try:
            info = await apis.get_klippy_info()
        except Exception:
            info = {}
        klipper_path = info.get("klipper_path") or None
        error = info.get("state_message", "") if info.get("state") == "error" else ""
        objects: Optional[List[str]] = None
        job_active = False
        if info.get("state") == "ready":
            try:
                objects = list(await apis.get_object_list())
                stats = await apis.query_objects({"print_stats": ["state"]})
                job_active = stats.get("print_stats", {}).get("state") in ("printing", "paused")
            except Exception:
                objects = None
        return klipper_path, objects, error, job_active

    async def sync(self, *_: Any) -> None:
        async with self._lock:
            await self._sync()

    async def _sync(self) -> None:
        klipper_path, objects, error, job_active = await self._facts()
        config_dir = self.helix.file_manager.get_directory("config")
        enabled = {c.feature for c in COMPANIONS if await self.helix.feature_enabled(c.feature)}
        installed = {}
        if klipper_path:
            installed = {c.name: installed_state(c, plugin_dir=self.plugin_dir,
                                                 klipper_path=klipper_path, config_dir=config_dir)
                         for c in COMPANIONS}
        actions = plan_actions(COMPANIONS, enabled=enabled, objects=objects, installed=installed,
                               klipper_path=klipper_path, klippy_error=error,
                               job_active=job_active)
        by_name = {c.name: c for c in COMPANIONS}
        before = self.status()
        for action in actions:
            kind = action[0]
            c = by_name.get(action[1]) if len(action) > 1 else None
            try:
                if kind == "install":
                    install(c, self.fs, plugin_dir=self.plugin_dir,
                            klipper_path=klipper_path, config_dir=config_dir)
                    record_links(config_dir, klipper_path, c, present=True)
                    ensure_include(config_dir)
                    self.state[c.name] = {"installed": True, "restart_pending": False,
                                          "reason": None}
                elif kind == "remove":
                    remove(c, self.fs, klipper_path=klipper_path, config_dir=config_dir)
                    record_links(config_dir, klipper_path, c, present=False)
                    self.state[c.name] = {"installed": False, "restart_pending": False,
                                          "reason": None}
                elif kind == "restart_pending":
                    self.state.setdefault(c.name, {"installed": False, "reason": None})
                    self.state[c.name]["restart_pending"] = True
                elif kind == "unavailable":
                    self.state[c.name] = {"installed": False, "restart_pending": False,
                                          "reason": action[2]}
                elif kind == "restart":
                    await self._restart()
            except OSError as e:
                logging.exception("HelixPrint: companion %s failed", action)
                if c is not None:
                    self.state[c.name] = {"installed": False, "restart_pending": False,
                                          "reason": "%s_failed: %s" % (kind, e)}
        for c in COMPANIONS:
            self.state.setdefault(c.name, {"installed": bool(installed.get(c.name, {}).get("cfg")),
                                           "restart_pending": False, "reason": None})
        if self.status() != before:
            self.helix.notify_features_changed()

    async def _restart(self) -> None:
        apis = self.helix.klippy_apis
        do_restart = getattr(apis, "do_restart", None)
        if do_restart is not None:
            await do_restart("RESTART")
        else:
            await apis.run_gcode("RESTART")
