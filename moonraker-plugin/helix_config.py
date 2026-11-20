# SPDX-License-Identifier: GPL-3.0-or-later
"""
Klipper config read and edit for the HelixPrint plugin.

server.helix.config reads the config the way Klipper does (klippy/configfile.py):
includes are read where their line stands, relative to the including file,
globbed and sorted; a glob that matches nothing is legal and a missing plain
include is an error; each run of lines between includes is parsed on its own,
so a section never continues across an include; the SAVE_CONFIG block at the
end of the root file is read after everything else and supplies only the
options the regular config leaves undefined. Each option comes back with the
file and line that define it, every section included, whether or not Klipper
used it.

server.helix.config_edit writes edits addressed by (section, option) into the
file and line that define the option, atomically (temp file, fsync, rename)
after a timestamped backup, and returns a unified diff. It refuses an option
the SAVE_CONFIG block defines, since that block belongs to Klipper, and an edit
whose `expect` no longer matches the option's current value. It never restarts
Klipper: the screen decides on FIRMWARE_RESTART.

The parsing and planning functions here are pure and import nothing from
Moonraker; HelixConfig wires them to endpoints.
"""

from __future__ import annotations

import asyncio
import difflib
import glob
import logging
import os
import re
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

import helix_gcode_reader as reader

CONFIG_EDIT_FEATURE = "config_edit"
CONFIG_EDIT_FEATURE_VERSION = 1

# The file Klipper starts from, relative to Moonraker's config root.
ROOT_FILE = "printer.cfg"

# Backups live under this directory of the config root, at the edited file's
# own relative path: .helix_backups/conf.d/probe.cfg.helixbak-20261022-101500.
# Klipper's include globs never match a dot directory, so no [include dir/*]
# can read a backup as config.
BACKUP_DIR = ".helix_backups"
BACKUP_INFIX = ".helixbak-"
BACKUP_STAMP_FORMAT = "%Y%m%d-%H%M%S"
# Backups kept per file; older ones are deleted after each write.
BACKUPS_KEPT = 5

# Indent for continuation lines of a value written as several lines, when the
# option had none to copy.
CONTINUATION_INDENT = "  "

AUTOSAVE_HEADER_LINES = (
    "#*# <---------------------- SAVE_CONFIG ---------------------->",
    "#*# DO NOT EDIT THIS BLOCK OR BELOW. The contents are auto-generated.",
    "#*#",
)

# configparser's section header and option patterns (RawConfigParser.SECTCRE
# and OPTCRE without allow_no_value), as Klipper's parser applies them.
_SECTCRE = re.compile(r"\[(?P<header>.+)\]")
_OPTCRE = re.compile(r"(?P<option>.*?)\s*(?P<vi>[=:])\s*(?P<value>.*)$")


# The shared file-text read and atomic text write live in helix_gcode_reader,
# the one sibling every config-writing module already depends on; these names
# stay for this module's callers.
read_file_text = reader.read_file_text


class ConfigEditRefused(Exception):
    """An edit the plugin will not write. `code` names the rule: autosave,
    moved, outside_root, invalid."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class OptionDef:
    """One definition of an option: where it is and what Klipper reads from it.
    Lines are 1-based."""

    def __init__(self, section: str, option: str, value_lines: List[str], file: str,
                 line: int, autosave: bool, indent: int) -> None:
        self.section = section
        self.option = option
        self.value_lines = value_lines
        self.file = file
        self.line = line
        self.end_line = line
        self.autosave = autosave
        self.indent = indent

    @property
    def value(self) -> str:
        # configparser joins continuation lines with newlines and drops
        # trailing blank ones.
        return "\n".join(self.value_lines).rstrip()

    def to_json(self) -> Dict[str, Any]:
        return {
            "value": self.value,
            "file": self.file,
            "line": self.line,
            "end_line": self.end_line,
            "autosave": self.autosave,
        }


class SectionDef:
    """One [section] header and the last line of its block, in one file."""

    def __init__(self, section: str, file: str, line: int, autosave: bool) -> None:
        self.section = section
        self.file = file
        self.line = line
        self.last_line = line
        self.autosave = autosave

    def to_json(self) -> Dict[str, Any]:
        return {"file": self.file, "line": self.line, "autosave": self.autosave}


class ConfigTree:
    """The config as Klipper reads it. `options` and `sections` are in read
    order; `files` lists each file read, once, in first-read order."""

    def __init__(self) -> None:
        self.files: List[str] = []
        self.options: List[OptionDef] = []
        self.sections: List[SectionDef] = []
        self.errors: List[str] = []

    def effective(self) -> Dict[str, Dict[str, OptionDef]]:
        """Each option's definition Klipper uses: the last in the regular
        config, else the SAVE_CONFIG block's."""
        regular: Dict[str, Dict[str, OptionDef]] = {}
        autosave: Dict[str, Dict[str, OptionDef]] = {}
        for opt in self.options:
            target = autosave if opt.autosave else regular
            target.setdefault(opt.section, {})[opt.option] = opt
        result: Dict[str, Dict[str, OptionDef]] = {s: dict(o) for s, o in regular.items()}
        for section, options in autosave.items():
            merged = result.setdefault(section, {})
            for name, opt in options.items():
                if name not in merged:
                    merged[name] = opt
        return result

    def option(self, section: str, option: str) -> Optional[OptionDef]:
        return self.effective().get(section, {}).get(option)

    def last_regular_section(self, section: str) -> Optional[SectionDef]:
        found = None
        for sec in self.sections:
            if sec.section == section and not sec.autosave:
                found = sec
        return found

    def section_names(self) -> List[str]:
        names: List[str] = []
        for sec in self.sections:
            if sec.section not in names:
                names.append(sec.section)
        return names

    def to_json(self, section: Optional[str] = None) -> Dict[str, Any]:
        effective = self.effective()
        sections: Dict[str, Any] = {}
        for name in self.section_names():
            if section is not None and name != section:
                continue
            sections[name] = {
                "options": {o: d.to_json() for o, d in effective.get(name, {}).items()},
                "defined_in": [s.to_json() for s in self.sections if s.section == name],
            }
        return {
            "root": ROOT_FILE,
            "files": list(self.files),
            "sections": sections,
            "errors": list(self.errors),
        }


# ============================================================================
# Reading
# ============================================================================

def split_autosave(lines: List[str]) -> Optional[int]:
    """Index of the first SAVE_CONFIG header line in the root file's lines, or
    None when there is no block or Klipper would read it as corrupted (and so
    as plain comments)."""
    header = len(AUTOSAVE_HEADER_LINES)
    start = None
    # Klipper finds the header after a newline, so never on the first line.
    for i in range(1, len(lines) - header + 1):
        if tuple(lines[i:i + header]) == AUTOSAVE_HEADER_LINES:
            start = i
            break
    if start is None:
        return None
    if any(line.startswith("#*# ") for line in lines[:start]):
        return None
    body = lines[start + header:]
    while body and not body[0].strip():
        body = body[1:]
    while body and not body[-1].strip():
        body = body[:-1]
    for line in body:
        if not line.startswith("#*#") or (len(line) >= 4 and not line.startswith("#*# ")):
            return None
    return start


def _strip_comment(raw: str) -> Tuple[str, bool]:
    """A line as configparser sees it once Klipper has cut it at the first
    '#': its value text, and whether configparser read a ';' comment on it
    (a line Klipper emptied is a blank line to configparser, not a comment)."""
    pos = raw.find("#")
    line = raw[:pos] if pos >= 0 else raw
    if line.strip().startswith(";"):
        return "", True
    # An inline ';' needs whitespace before it.
    index = -1
    while True:
        index = line.find(";", index + 1)
        if index == -1:
            break
        if index == 0 or line[index - 1].isspace():
            return line[:index].strip(), True
    return line.strip(), False


class _Chunk:
    """configparser's state over one run of lines Klipper parses as a unit."""

    def __init__(self, tree: ConfigTree) -> None:
        self.tree = tree
        self.section: Optional[SectionDef] = None
        self.option: Optional[OptionDef] = None
        self.indent_level = 0

    def feed(self, raw: str, file: str, lineno: int, autosave: bool) -> None:
        value, comment = _strip_comment(raw)
        if not value:
            if not comment and self.section is not None and self.option is not None:
                self.option.value_lines.append("")
            return
        indent = len(raw) - len(raw.lstrip())
        if self.section is not None and self.option is not None and indent > self.indent_level:
            self.option.value_lines.append(value)
            self.option.end_line = lineno
            self.section.last_line = lineno
            return
        self.indent_level = indent
        mo = _SECTCRE.match(value)
        if mo:
            self.section = SectionDef(mo.group("header"), file, lineno, autosave)
            self.tree.sections.append(self.section)
            self.option = None
            return
        if self.section is None:
            self.tree.errors.append(f"{file}:{lineno}: option outside any section")
            return
        mo = _OPTCRE.match(value)
        name = mo.group("option").rstrip().lower() if mo else ""
        if not name:
            self.tree.errors.append(f"{file}:{lineno}: not an option")
            self.option = None
            return
        self.option = OptionDef(self.section.section, name, [mo.group("value").strip()],
                                file, lineno, autosave, indent)
        self.tree.options.append(self.option)
        self.section.last_line = lineno


def _display_path(path: str, root_dir: str) -> str:
    rel = os.path.relpath(path, root_dir)
    if rel == os.pardir or rel.startswith(os.pardir + os.sep):
        return path
    return rel.replace(os.sep, "/")


def read_config(root_dir: Path, read_text: Optional[Callable[[str], str]] = None,
                root_file: str = ROOT_FILE) -> ConfigTree:
    """The include-resolved config under `root_dir`, as Klipper reads it.
    `read_text` reads a file by absolute path (the edit planner passes one
    that serves its in-memory copies); a file it cannot read is an error in
    the tree, never an exception."""
    tree = ConfigTree()
    root_abs = os.path.abspath(str(root_dir))
    reader = read_text or read_file_text
    reading: List[str] = []

    def load(path: str) -> Optional[List[str]]:
        try:
            data = reader(path)
        except (OSError, UnicodeDecodeError) as e:
            tree.errors.append(f"{_display_path(path, root_abs)}: {e}")
            return None
        return data.replace("\r\n", "\n").split("\n")

    def parse_file(path: str, lines: List[str], chunk: _Chunk,
                   autosave_at: Optional[int]) -> _Chunk:
        name = _display_path(path, root_abs)
        if name not in tree.files:
            tree.files.append(name)
        reading.append(path)
        end = autosave_at if autosave_at is not None else len(lines)
        for index in range(end):
            raw = lines[index]
            cut = raw.find("#")
            line = raw[:cut] if cut >= 0 else raw
            mo = _SECTCRE.match(line)
            header = mo.group("header") if mo else None
            if header is not None and header.startswith("include "):
                spec = header[8:].strip()
                include_files(path, spec, index + 1)
                chunk = _Chunk(tree)
                continue
            chunk.feed(raw, name, index + 1, False)
        reading.pop()
        return chunk

    def include_files(source: str, spec: str, lineno: int) -> None:
        pattern = os.path.join(os.path.dirname(source), spec)
        # Klipper's own glob: no recursive=True, so ** is one path segment.
        matches = sorted(p for p in glob.glob(pattern) if os.path.isfile(p))
        if not matches and not glob.has_magic(pattern):
            tree.errors.append(
                f"{_display_path(source, root_abs)}:{lineno}: include file "
                f"'{spec}' does not exist"
            )
            return
        for match in matches:
            target = os.path.abspath(match)
            if target in reading:
                tree.errors.append(
                    f"{_display_path(source, root_abs)}:{lineno}: recursive include of "
                    f"'{spec}'"
                )
                continue
            lines = load(target)
            if lines is not None:
                parse_file(target, lines, _Chunk(tree), None)

    root_path = os.path.join(root_abs, root_file)
    lines = load(root_path)
    if lines is None:
        return tree
    autosave_at = split_autosave(lines)
    chunk = parse_file(root_path, lines, _Chunk(tree), autosave_at)
    if autosave_at is not None:
        name = _display_path(root_path, root_abs)
        for index in range(autosave_at + len(AUTOSAVE_HEADER_LINES), len(lines)):
            raw = lines[index]
            if not raw.startswith("#*#"):
                continue
            chunk.feed(raw[4:], name, index + 1, True)
    return tree


# ============================================================================
# Planning edits
# ============================================================================

# Characters that would end a section header or an option name early, or start
# a comment or another line, when written into a config file
_SECTION_FORBIDDEN = frozenset("\r\n[]#;")
_OPTION_FORBIDDEN = _SECTION_FORBIDDEN | frozenset("=: \t")


def _valid_name(name: str, forbidden: frozenset) -> bool:
    return not any(c in forbidden or ord(c) < 32 for c in name)


def _parse_edit(raw: Any) -> Dict[str, Any]:
    if not isinstance(raw, dict):
        raise ConfigEditRefused("invalid", "each edit must be an object")
    section = raw.get("section")
    option = raw.get("option")
    action = raw.get("action", "set")
    if not isinstance(section, str) or not section:
        raise ConfigEditRefused("invalid", "edit has no section")
    if not isinstance(option, str) or not option.strip():
        raise ConfigEditRefused("invalid", f"[{section}] edit has no option")
    if not _valid_name(section, _SECTION_FORBIDDEN) or section != section.strip():
        raise ConfigEditRefused("invalid", f"section name {section!r} is not one Klipper reads")
    if not _valid_name(option.strip(), _OPTION_FORBIDDEN):
        raise ConfigEditRefused(
            "invalid", f"[{section}] option name {option!r} is not one Klipper reads")
    if action not in ("set", "remove"):
        raise ConfigEditRefused("invalid", f"[{section}] {option}: unknown action '{action}'")
    if "expect" not in raw:
        raise ConfigEditRefused(
            "invalid", f"[{section}] {option}: an edit carries the value it expects to replace"
        )
    expect = raw["expect"]
    if expect is not None and not isinstance(expect, str):
        raise ConfigEditRefused("invalid", f"[{section}] {option}: expect is a string or null")
    value = raw.get("value")
    if action == "set" and not isinstance(value, str):
        raise ConfigEditRefused("invalid", f"[{section}] {option}: set needs a string value")
    return {"section": section, "option": option.strip().lower(), "action": action,
            "value": value, "expect": expect}


def _value_lines(option: str, delimiter_text: str, value: str, indent: str) -> List[str]:
    parts = value.split("\n")
    first = f"{option}{delimiter_text}{parts[0]}".rstrip()
    return [first] + [indent + p if p else "" for p in parts[1:]]


def _comment_tail(raw: str) -> str:
    """The inline comment of a line, with the whitespace before it."""
    pos = raw.find("#")
    for index in range(len(raw)):
        if raw[index] == ";" and index > 0 and raw[index - 1].isspace():
            if pos < 0 or index < pos:
                pos = index
            break
    if pos <= 0:
        return ""
    start = pos
    while start > 0 and raw[start - 1].isspace():
        start -= 1
    return raw[start:]


def _set_lines(lines: List[str], opt: OptionDef, value: str) -> List[str]:
    """`lines` with the definition of `opt` replaced by `value`, keeping its
    key spelling, delimiter and inline comment."""
    raw = lines[opt.line - 1]
    mo = re.match(r"(\s*)(.*?)(\s*[=:]\s*)", raw)
    prefix, key, delimiter = (mo.group(1), mo.group(2), mo.group(3)) if mo else ("", opt.option, ": ")
    if not delimiter.endswith(" ") and value:
        delimiter += " "
    indent = CONTINUATION_INDENT
    if opt.end_line > opt.line:
        follow = lines[opt.line]
        indent = follow[:len(follow) - len(follow.lstrip())] or CONTINUATION_INDENT
    new = _value_lines(prefix + key, delimiter, value, indent)
    if "\n" not in value:
        new[0] += _comment_tail(raw)
    return lines[:opt.line - 1] + new + lines[opt.end_line:]


def _add_lines(lines: List[str], after_line: int, option: str, value: str) -> List[str]:
    new = _value_lines(option, ": ", value, CONTINUATION_INDENT)
    return lines[:after_line] + new + lines[after_line:]


def _remove_lines(lines: List[str], opt: OptionDef) -> List[str]:
    out = list(lines)
    for index in range(opt.line - 1, opt.end_line):
        if out[index].strip():
            out[index] = "#" + out[index]
    return out


def _new_section_lines(lines: List[str], section: str, option: str, value: str) -> List[str]:
    """`lines` of the root file with a new section appended above its
    SAVE_CONFIG block, or at the end."""
    at = split_autosave(lines)
    if at is None:
        at = len(lines)
        while at > 0 and not lines[at - 1].strip():
            at -= 1
    block = ["", f"[{section}]"] + _value_lines(option, ": ", value, CONTINUATION_INDENT)
    if at < len(lines):
        block.append("")
    return lines[:at] + block + lines[at:]


class EditPlan:
    """The files an edit list changes: original and new text by display path."""

    def __init__(self) -> None:
        self.original: Dict[str, str] = {}
        self.changed: Dict[str, str] = {}

    def diff(self) -> str:
        out: List[str] = []
        for name in sorted(self.changed):
            out.extend(difflib.unified_diff(
                self.original[name].splitlines(keepends=True),
                self.changed[name].splitlines(keepends=True),
                fromfile=f"a/{name}", tofile=f"b/{name}",
            ))
        return "".join(out)


def plan_edits(root_dir: Path, raw_edits: List[Any],
               read_text: Optional[Callable[[str], str]] = None) -> EditPlan:
    """The new text of every file `raw_edits` touches, applied one after
    another against the config as each leaves it. Raises ConfigEditRefused
    and changes nothing when any edit is refused."""
    if not isinstance(raw_edits, list) or not raw_edits:
        raise ConfigEditRefused("invalid", "no edits")
    edits = [_parse_edit(e) for e in raw_edits]
    root_abs = os.path.abspath(str(root_dir))
    reader = read_text or read_file_text
    plan = EditPlan()

    def path_of(name: str) -> str:
        return name if os.path.isabs(name) else os.path.join(root_abs, name)

    def current_text(path: str) -> str:
        name = _display_path(path, root_abs)
        if name in plan.changed:
            return plan.changed[name]
        return reader(path)

    def lines_of(name: str) -> Tuple[List[str], bool]:
        text = current_text(path_of(name))
        if name not in plan.original:
            plan.original[name] = text
        crlf = "\r\n" in text
        return text.replace("\r\n", "\n").split("\n"), crlf

    def store(name: str, lines: List[str], crlf: bool) -> None:
        text = "\n".join(lines)
        plan.changed[name] = text.replace("\n", "\r\n") if crlf else text

    for edit in edits:
        section, option = edit["section"], edit["option"]
        label = f"[{section}] {option}"
        tree = read_config(Path(root_abs), current_text)
        current = tree.option(section, option)
        if current is not None and current.autosave:
            raise ConfigEditRefused(
                "autosave", f"{label} is in the SAVE_CONFIG block, which Klipper owns"
            )
        found = current.value if current is not None else None
        if found != edit["expect"]:
            raise ConfigEditRefused(
                "moved", f"{label} changed since it was read: expected "
                f"{edit['expect']!r}, found {found!r}"
            )
        target = current.file if current is not None else None
        if target is None:
            sec = tree.last_regular_section(section)
            target = sec.file if sec is not None else ROOT_FILE
        if os.path.isabs(target):
            raise ConfigEditRefused("outside_root", f"{label} is defined outside the config root")

        lines, crlf = lines_of(target)
        if edit["action"] == "remove":
            if current is None:
                continue
            lines = _remove_lines(lines, current)
        elif current is not None:
            lines = _set_lines(lines, current, edit["value"])
        else:
            sec = tree.last_regular_section(section)
            if sec is not None:
                lines = _add_lines(lines, sec.last_line, option, edit["value"])
            else:
                lines = _new_section_lines(lines, section, option, edit["value"])
        store(target, lines, crlf)

    for name in [n for n, text in plan.changed.items() if text == plan.original[n]]:
        del plan.changed[name]
    return plan


# ============================================================================
# Writing
# ============================================================================

def backup_base(root: Path, name: str) -> Path:
    """Where the backups of config file `name` go, before their stamp."""
    return Path(root) / BACKUP_DIR / f"{name}{BACKUP_INFIX}"


def backup_name(root: Path, name: str, now: float) -> Path:
    """A free timestamped backup path for config file `name`."""
    stamp = time.strftime(BACKUP_STAMP_FORMAT, time.localtime(now))
    base = str(backup_base(root, name))
    candidate = Path(base + stamp)
    n = 1
    while candidate.exists():
        candidate = Path(f"{base}{stamp}-{n}")
        n += 1
    return candidate


def _write_durably(path: Path, data: bytes) -> None:
    with open(str(path), "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


atomic_write = reader.atomic_write_text


def prune_backups(root: Path, name: str, keep: int = BACKUPS_KEPT) -> List[Path]:
    """Delete all but the newest `keep` backups of config file `name`."""
    base = backup_base(root, name)
    backups = sorted(base.parent.glob(glob.escape(base.name) + "*"), key=lambda p: p.name)
    removed = backups[:-keep] if keep > 0 else backups
    for old in removed:
        try:
            old.unlink()
        except OSError:
            pass
    return removed


def write_plan(root_dir: Path, plan: EditPlan, now: float,
               replace: Callable[[str, str], None] = os.replace) -> List[Dict[str, str]]:
    """Back each changed file up and write it atomically. A file whose text
    changed since it was planned is refused before anything is written; a
    write that fails puts the files already written back from their backups."""
    root = Path(root_dir)
    for name, original in plan.original.items():
        if name in plan.changed and read_file_text(str(root / name)) != original:
            raise ConfigEditRefused("moved", f"{name} changed while the edit was prepared")
    backups: List[Dict[str, str]] = []
    written: List[Tuple[str, Path, Path]] = []
    try:
        for name in sorted(plan.changed):
            path = root / name
            backup = backup_name(root, name, now)
            backup.parent.mkdir(parents=True, exist_ok=True)
            _write_durably(backup, path.read_bytes())
            backups.append({"file": name, "backup": _display_path(str(backup), str(root))})
            atomic_write(path, plan.changed[name], replace)
            written.append((name, path, backup))
    except BaseException:
        for _, path, backup in written:
            try:
                atomic_write(path, read_file_text(str(backup)))
            except OSError:
                logging.exception(f"HelixConfig: could not restore {path} from {backup}")
        raise
    for name, _, _ in written:
        prune_backups(root, name)
    return backups


# ============================================================================
# Moonraker wiring
# ============================================================================

class HelixConfig:
    """server.helix.config and server.helix.config_edit inside a running
    Moonraker. helix_print.py creates one in component_init."""

    def __init__(self, helix: Any) -> None:
        self.helix = helix
        self.server = helix.server
        self._lock = asyncio.Lock()

    def start(self) -> None:
        self.server.register_endpoint("/server/helix/config", ["GET"], self.handle_config)
        self.server.register_endpoint(
            "/server/helix/config_edit", ["POST"], self.handle_config_edit
        )

    def config_root(self) -> Path:
        fm = self.helix.file_manager
        path = fm.get_directory("config") if fm is not None else ""
        if not path:
            raise self.server.error("Config directory unknown", 503)
        return Path(path)

    async def _check_enabled(self) -> None:
        if not await self.helix.feature_enabled(CONFIG_EDIT_FEATURE):
            raise self.server.error("Config read and edit is unavailable", 503)

    async def handle_config(self, web_request: Any) -> Dict[str, Any]:
        await self._check_enabled()
        section = web_request.get_str("section", "") or None
        root = self.config_root()
        tree = await self.server.get_event_loop().run_in_thread(read_config, root)
        return tree.to_json(section)

    async def handle_config_edit(self, web_request: Any) -> Dict[str, Any]:
        await self._check_enabled()
        edits = web_request.get("edits", [])
        dry_run = web_request.get_boolean("dry_run", False)
        root = self.config_root()
        loop = self.server.get_event_loop()
        async with self._lock:
            try:
                plan = await loop.run_in_thread(plan_edits, root, edits)
                backups: List[Dict[str, str]] = []
                if not dry_run and plan.changed:
                    backups = await loop.run_in_thread(write_plan, root, plan, time.time())
            except ConfigEditRefused as e:
                raise self.server.error(f"{e.code}: {e}", 409)
            except OSError as e:
                raise self.server.error(f"Config write failed: {e}", 500)
        if not dry_run and plan.changed:
            logging.info(
                "HelixConfig: wrote %d edit(s) to %s", len(edits), ", ".join(sorted(plan.changed))
            )
        return {
            "dry_run": dry_run,
            "files": sorted(plan.changed),
            "backups": backups,
            "diff": plan.diff(),
        }
