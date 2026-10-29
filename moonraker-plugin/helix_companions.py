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

import os
import shutil
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple

COMPANIONS_FEATURE = "klipper_companions"
COMPANIONS_FEATURE_VERSION = 1
INCLUDE_DIR = "helix_companions"

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
