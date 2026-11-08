# SPDX-License-Identifier: GPL-3.0-or-later
"""
Signal macro migration: rename the legacy phase-tracking marker blocks to the
hidden signal macro names at every Klipper start (#2006).

HelixScreen before 1.1 wrapped HELIX_PHASE_* and HELIX_READY calls in marker
blocks inside the printer's PRINT_START. HelixScreen now names those macros
with a leading underscore, so Fluidd and Mainsail leave them out of their
macro lists. This module gives each block's call line that underscore,
through strip_phase_tracking's rename mode, so the printer keeps every signal
once the visible names are gone.

It writes nothing until Klipper's loaded config (configfile.settings) defines
every hidden name the rename would call: a rename before that would answer
`Unknown command` at the next restart. It never restarts Klipper; the
renamed lines load at the next restart, and until then the loaded macros keep
calling the visible names, which the macro pack still defines.

Switched by `migrate_signal_macros` under [helix_print] in moonraker.conf,
on by default. On a printer whose firmware owns the HelixScreen install
(Snapmaker U1 under paxx, Z-Mod, Forge-X) it only reports what it would do.
"""

import asyncio
import logging
import os
import time
from typing import TYPE_CHECKING, Any, Dict, Iterable, List, Optional

import strip_phase_tracking as strip

if TYPE_CHECKING:
    from helix_print import HelixPrint

SIGNAL_MIGRATION_FEATURE = "signal_macro_migration"
SIGNAL_MIGRATION_FEATURE_VERSION = 1
CONFIG_OPTION = "migrate_signal_macros"
DB_NAMESPACE = "helix_print"
DB_KEY = "signal_migration_last"
ENDPOINT = "/server/helix/signal_migration"

# Firmwares that own the HelixScreen install, and with it the config this
# migration would edit, by the markers the installer detects them with:
# the Snapmaker U1's extended firmware (paxx) by its firmware-managed guard
# (scripts/lib/installer/main.sh#_refuse_if_firmware_managed), Z-Mod and
# Forge-X by detect_mod_flavor and host_profile_probe
# (scripts/lib/installer/platform.sh, scripts/lib/installer/host_profile.sh).
# A chroot path matches only where Moonraker runs outside it; a Moonraker
# inside one sees the mod tree Forge-X binds at /opt/config.
FIRMWARE_MANAGED_MARKERS = (
    ("paxx", "oem/apps/helixscreen", os.path.isdir),
    ("paxx", "etc/hooks/lmd.d/30-helixscreen.sh", os.path.isfile),
    ("zmod", "ZMOD", os.path.isfile),
    ("zmod", "usr/data/.mod/.zmod/usr/bin", os.path.isdir),
    ("forge_x", "usr/data/config/mod/.shell/common.sh", os.path.isfile),
    ("forge_x", "usr/data/config/mod/.shell/platform.sh", os.path.isfile),
    ("forge_x", "opt/config/mod/.shell/common.sh", os.path.isfile),
    ("forge_x", "opt/config/mod/.shell/platform.sh", os.path.isfile),
    ("forge_x", "opt/config/mod/.root", os.path.isdir),
    ("forge_x", "usr/data/.mod/.forge-x/usr/bin", os.path.isdir),
    ("forge_x", "data/.mod/.forge-x/usr/bin", os.path.isdir),
)


def firmware_owner(root: str = "") -> Optional[str]:
    """The firmware that owns the HelixScreen install, or None."""
    for owner, rel, check in FIRMWARE_MANAGED_MARKERS:
        if check(os.path.join(root or "/", rel)):
            return owner
    return None


def firmware_managed(root: str = "") -> bool:
    """Whether the printer's firmware owns the HelixScreen install, and with
    it the files this migration would edit."""
    return firmware_owner(root) is not None


def missing_hidden_names(settings: Any, needed: Iterable[str]) -> List[str]:
    """The names in `needed` that no [gcode_macro] in Klipper's loaded config
    defines, sorted. Klipper lowercases its section keys; unreadable settings
    define nothing."""
    keys = {k.lower() for k in settings} if isinstance(settings, dict) else set()
    return sorted(name for name in set(needed) if f"gcode_macro {name.lower()}" not in keys)


def _empty_report(state: str) -> Dict[str, Any]:
    return {"state": state, "missing": [], "planned": [], "edited": [], "skipped": [],
            "failed": [], "at": time.time()}


class HelixSignalMigration:
    def __init__(self, helix: "HelixPrint", enabled: bool) -> None:
        self.helix = helix
        self.server = helix.server
        self.enabled = enabled
        self._lock: Optional[asyncio.Lock] = None
        self._report: Dict[str, Any] = _empty_report("nothing")

    def start(self) -> None:
        self.server.register_event_handler("server:klippy_ready", self.on_klippy_ready)
        self.server.register_endpoint(ENDPOINT, ["GET"], self.handle_report)

    async def switch(self) -> bool:
        return self.enabled and not firmware_managed()

    async def load(self) -> None:
        """The last stored report, so the result of a rename survives the
        Moonraker restart that usually follows it."""
        db = self.helix.database
        if db is None:
            return
        try:
            stored = await db.get_item(DB_NAMESPACE, DB_KEY, None)
        except Exception as e:
            logging.debug("HelixPrint: signal migration report not read: %s", e)
            return
        if isinstance(stored, dict) and "state" in stored:
            self._report = stored

    def report(self) -> Dict[str, Any]:
        return self._report

    async def handle_report(self, web_request: Any) -> Dict[str, Any]:
        return self.report()

    async def on_klippy_ready(self, *args: Any) -> None:
        try:
            await self.run_once()
        except Exception:
            logging.exception("HelixPrint: signal macro migration failed")

    def _get_lock(self) -> asyncio.Lock:
        # Made inside the running loop: Python 3.8 binds a Lock to the loop
        # current when it is created.
        if self._lock is None:
            self._lock = asyncio.Lock()
        return self._lock

    def _config_dir(self) -> str:
        fm = self.helix.file_manager
        return fm.get_directory("config") if fm is not None else ""

    async def _settings(self) -> Any:
        apis = self.helix.klippy_apis
        if apis is None:
            return None
        try:
            status = await apis.query_objects({"configfile": ["settings"]})
        except Exception as e:
            logging.info("HelixPrint: signal macro migration: configfile query failed: %s", e)
            return None
        return ((status or {}).get("configfile") or {}).get("settings")

    async def run_once(self) -> Dict[str, Any]:
        """Plan, gate, and rename. Returns this run's report; one whose state
        is "nothing" does not replace a stored "done", which keeps saying what
        the rename did."""
        async with self._get_lock():
            report = await self._run()
            await self._keep(report)
            return report

    async def _run(self) -> Dict[str, Any]:
        config_dir = self._config_dir()
        if not config_dir:
            report = _empty_report("failed")
            report["failed"] = [{"path": "", "reason": "config directory unknown"}]
            return report
        run_in_thread = self.server.get_event_loop().run_in_thread
        plan = await run_in_thread(strip.run_tree, config_dir, "rename", True)

        firmware = firmware_managed()
        acting = self.enabled and not firmware
        logging.info("HelixPrint: signal macro migration: %s",
                     plan.summary_line("renaming" if acting else "would rename"))

        if not plan.would_edit:
            return self._from_tree(_empty_report(_settled_state(plan)), plan, config_dir)
        if not acting:
            return self._from_tree(
                _empty_report("firmware_managed" if firmware else "disabled"), plan, config_dir)

        needed = plan.names_needed()
        missing = missing_hidden_names(await self._settings(), needed)
        if missing:
            logging.info("HelixPrint: signal macro migration: waiting for Klipper to load %s",
                         ", ".join(missing))
            report = self._from_tree(_empty_report("waiting_for_pack"), plan, config_dir)
            report["missing"] = missing
            return report

        done = await run_in_thread(strip.run_tree, config_dir, "rename", False, needed)
        clean_run = done.edited and done.exit_status() == strip.EXIT_OK
        state = "done" if clean_run else _settled_state(done)
        report = self._from_tree(_empty_report(state), done, config_dir)
        if done.edited:
            logging.info("HelixPrint: signal macro migration: renamed %d call(s); the change "
                         "loads at the next Klipper restart",
                         sum(entry["blocks"] for entry in done.edited))
        return report

    @staticmethod
    def _from_tree(report: Dict[str, Any], tree: Any, config_dir: str) -> Dict[str, Any]:
        def rel(path: str) -> str:
            return os.path.relpath(path, os.path.realpath(config_dir))

        report["planned"] = [{"path": rel(e["path"]), "blocks": e["blocks"]}
                             for e in tree.would_edit]
        report["edited"] = [{"path": rel(e["path"]), "backup": e["backup"],
                             "blocks": e["blocks"]} for e in tree.edited]
        report["skipped"] = [{"path": rel(e["path"]), "reason": e["reason"]}
                             for e in tree.skipped]
        report["failed"] = [{"path": rel(e["path"]), "reason": e["reason"]}
                            for e in tree.failed]
        return report

    async def _keep(self, report: Dict[str, Any]) -> None:
        if report["state"] == "nothing" and self._report.get("state") == "done":
            return
        changed = report["state"] != self._report.get("state") or report["state"] == "done"
        self._report = report
        db = self.helix.database
        if db is not None:
            try:
                await db.insert_item(DB_NAMESPACE, DB_KEY, report)
            except Exception as e:
                logging.warning("HelixPrint: signal migration report not stored: %s", e)
        if changed:
            self.helix.notify_features_changed()


def _settled_state(tree: Any) -> str:
    """The state of a tree with nothing (more) to rename."""
    status = tree.exit_status()
    if status == strip.EXIT_FAILED:
        return "failed"
    if status == strip.EXIT_NEEDS_ATTENTION:
        return "needs_attention"
    return "nothing"
