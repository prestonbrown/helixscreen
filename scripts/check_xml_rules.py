#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""XML rules: six properties of ui_xml/ that the app loads without a word.

Each rule names one shape in ui_xml/ that breaks quietly at runtime: a glyph
that renders broken, a token that resolves to nothing, a modal whose buttons
fall off the card. All of them are the same kind of check, a pattern searched
in the ui_xml files, so they share one script. It lists ui_xml/ once, opens
each file at most once per run and hands every rule the same text; each rule
keeps its own section below with its reason, scope and marker.

  header-actions            anything but <header_action> in a header's actions
                            slot, or one styled, unlabelled or dead (no marker;
                            skips ui_xml/translations/ and
                            ui_xml/header_action.xml)
  overlay-width             XML naming a navigation width class, which push time
                            decides (#1178) (no marker; a --staged-only run
                            reads the staged blobs)
  icon-names                an <icon src="NAME"> absent from
                            include/ui_icon_codepoints.h
  responsive-token-scope    a responsive token declared below the top level of
                            ui_xml/, which discovery never reads (#1211)
  modal-chrome-budget       a modal whose chrome overruns the content cap it
                            budgets against (#1277) (MODAL_CHROME_OK: <reason>)
  shipped-spacing-tokens    a token theme_manager_get_spacing() reads that only
                            a dev panel declares, and release packaging deletes
                            (reads mk/cross.mk, src/ and include/ as well)

Usage:
  check_xml_rules.py                                  # every rule
  check_xml_rules.py --rule NAME [--rule NAME ...]    # the named rules only
  check_xml_rules.py --root DIR ...                   # resolve the tree under DIR
  check_xml_rules.py --staged-only ...                # overlay-width reads the
                                                      # staged XML blobs
  check_xml_rules.py --list ...                       # icon-names lists every
                                                      # scanned reference
  check_xml_rules.py --rule overlay-width FILE...     # those files, not ui_xml/
  check_xml_rules.py --rule responsive-token-scope FILE...
  check_xml_rules.py --rule shipped-spacing-tokens --ui-xml DIR --src DIR
                          --cross-mk FILE

One rule alone prints what it finds and nothing else, and exits with its own
code. With no --rule, or several, each rule prints the section qc_xml_rules
(scripts/qc/xml_rules.sh) shows: its header, the verdict line, and a Run: hint
when it fails. --list adds a listing to icon-names and changes nothing for the
others, which already print every finding when they fail.

FILE arguments replace the ui_xml/ scan of overlay-width and
responsive-token-scope, and are refused for any other rule. --ui-xml, --src and
--cross-mk set what shipped-spacing-tokens reads, and are refused without that
rule; a relative one resolves under the tree.

Without --root the tree is the one its gate runs from: the repository holding
the current directory for overlay-width, responsive-token-scope and
shipped-spacing-tokens, the current directory for modal-chrome-budget, and the
repository this script is in for header-actions and icon-names. --staged-only
reads the index of the repository holding the current directory, so it takes no
--root.

Exit 0 when every rule that ran passes, 1 when one fails, and 2 when
icon-names is the only failure and found no <icon> tag at all: its pattern has
stopped matching, which is not a clean tree.
"""

from __future__ import annotations

import argparse
import io
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
import xml.parsers.expat
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

from path_scope import in_skipped_dir
from source_strip import blank_cxx, blank_xml_comments
from staged_content import staged_files

SCRIPT_ROOT = Path(__file__).resolve().parent.parent

# HELIX_XML_RULES_TRACE_READS=FILE: the loader appends the path of every file it
# opens. tests/shell/test_xml_rules_gate.bats counts them.
TRACE_READS = os.environ.get("HELIX_XML_RULES_TRACE_READS")

# android/app/src/main/assets/ui_xml/ is a gradle copy of ui_xml/
# (android/app/build.gradle: from('../../ui_xml')), not a source of truth;
# build/ and .worktrees/ are copies as well.
COPIED_TREES = ("android", "build", ".worktrees")


# ====================================================================
# Shared helpers
# ====================================================================
def script_root() -> Path:
    """The repository this script is in."""
    return SCRIPT_ROOT


def repository_root() -> Path:
    """The git repository holding the current directory, or that directory outside one."""
    top = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=False
    ).stdout.strip()
    return Path(top).resolve() if top else Path.cwd().resolve()


def cwd_root() -> Path:
    """The current directory."""
    return Path.cwd().resolve()


# Where a rule's tree is when --root is absent.
TREES = {"script": script_root, "repository": repository_root, "directory": cwd_root}


def line_of(text: str, index: int) -> int:
    """The 1-based line of offset `index` in `text`."""
    return text.count("\n", 0, index) + 1


def as_read_text(data: bytes, errors: str = "strict") -> str:
    """`data` as Path.read_text(encoding="utf-8", errors=errors) returns it."""
    text = data.decode("utf-8", errors)
    # Text mode ends a line at \r\n and at a lone \r too.
    return text.replace("\r\n", "\n").replace("\r", "\n") if "\r" in text else text


class XmlReadError(Exception):
    """A file that cannot be read, or decoded as UTF-8."""

    def __init__(self, path: Path, exc: Exception) -> None:
        super().__init__(f"{path}: cannot be read ({exc})")
        self.path = path


class Reads:
    """Every file this run opens, each at most once."""

    def __init__(self) -> None:
        self.opens = 0
        self._raw: dict[Path, bytes] = {}
        self._text: dict[tuple[Path, str], str] = {}

    def raw(self, path: Path) -> bytes:
        if path not in self._raw:
            self.opens += 1
            if TRACE_READS:
                with open(TRACE_READS, "a", encoding="utf-8") as trace:
                    trace.write(f"{path}\n")
            try:
                self._raw[path] = path.read_bytes()
            except OSError as exc:
                raise XmlReadError(path, exc) from exc
        return self._raw[path]

    def text(self, path: Path, errors: str = "strict") -> str:
        key = (path, errors)
        if key not in self._text:
            try:
                self._text[key] = as_read_text(self.raw(path), errors)
            except UnicodeDecodeError as exc:
                raise XmlReadError(path, exc) from exc
        return self._text[key]

    def readable(self, path: Path) -> Optional[str]:
        """A file's text, or None when it cannot be read or decoded as UTF-8."""
        try:
            return self.text(path)
        except XmlReadError:
            return None


class Node:
    """One element of a parsed file."""

    __slots__ = ("tag", "attrs", "line", "children")

    def __init__(self, tag: str, attrs: dict[str, str], line: int, children: list) -> None:
        self.tag = tag
        self.attrs = attrs
        # The line the start tag ends on, which is where a report points.
        self.line = line
        self.children = children


class LineTree:
    """A file parsed into Nodes, or as far as a malformed file parses.

    `error` says why the parse stopped. `root` then holds every element the
    parser reached, and `open_at_stop` the ones whose end tag it never saw.
    """

    __slots__ = ("root", "error", "open_at_stop")

    def __init__(self, root, error, open_at_stop) -> None:
        self.root = root
        self.error = error
        self.open_at_stop = open_at_stop


# header-actions reads a LineTree. modal-chrome-budget reads ElementTree's own
# tree (ET.fromstring) straight from the text. They are not one parser:
# ElementTree turns expat's namespace processing on, which refuses a name with
# an unbound prefix (an LVGL style_bg_color:checked), and the plain parser
# takes it.
def parse_lines(data: bytes) -> LineTree:
    """Parse `data` with expat, keeping each element's start line."""
    parser = xml.parsers.expat.ParserCreate()
    stack: list[Node] = []
    root: Optional[Node] = None

    def start(tag: str, attrs: dict[str, str]) -> None:
        nonlocal root
        node = Node(tag, attrs, parser.CurrentLineNumber, [])
        if stack:
            stack[-1].children.append(node)
        elif root is None:
            root = node
        stack.append(node)

    def end(tag: str) -> None:
        stack.pop()

    parser.StartElementHandler = start
    parser.EndElementHandler = end
    error = None
    try:
        parser.Parse(data, True)
    except xml.parsers.expat.ExpatError as exc:
        error = str(exc)
    return LineTree(root, error, frozenset(id(node) for node in stack))


class XmlFiles:
    """The *.xml files under one directory, each read through the run's Reads."""

    def __init__(self, directory: Path, root: Path, reads: Reads) -> None:
        self.directory = directory
        self.root = root
        self.reads = reads
        self.paths = sorted(directory.rglob("*.xml")) if directory.is_dir() else []
        self._trees: dict[Path, LineTree] = {}
        self._rel: dict[Path, str] = {}

    def rel(self, path: Path) -> str:
        """`path` from the root, the way a report names it."""
        rel = self._rel.get(path)
        if rel is None:
            try:
                rel = path.relative_to(self.root).as_posix()
            except ValueError:
                rel = str(path)
            self._rel[path] = rel
        return rel

    def lines(self, path: Path) -> LineTree:
        """`path` parsed once into a LineTree."""
        if path not in self._trees:
            self._trees[path] = parse_lines(self.reads.raw(path))
        return self._trees[path]


class Run:
    """What a rule reads, and how it reports."""

    def __init__(
        self,
        root_override: Optional[Path],
        given: list[str],
        staged: bool,
        list_mode: bool,
        ui_xml: Optional[str] = None,
        src: Optional[list[str]] = None,
        cross_mk: Optional[str] = None,
    ) -> None:
        self.root_override = root_override
        # FILE arguments, as typed.
        self.given = given
        self.staged = staged
        self.list_mode = list_mode
        self.ui_xml = ui_xml
        self.src = src or []
        self.cross_mk = cross_mk
        self.reads = Reads()
        self._roots: dict[str, Path] = {}
        self._dirs: dict[Path, XmlFiles] = {}

    def root(self, rule: "Rule") -> Path:
        if self.root_override is not None:
            return self.root_override
        if rule.tree not in self._roots:
            self._roots[rule.tree] = TREES[rule.tree]()
        return self._roots[rule.tree]

    def xml(self, rule: "Rule", directory: Optional[Path] = None) -> XmlFiles:
        """The *.xml files under `directory`, by default the tree's ui_xml/."""
        root = self.root(rule)
        if directory is None:
            directory = root / "ui_xml"
        if directory not in self._dirs:
            self._dirs[directory] = XmlFiles(directory, root, self.reads)
        return self._dirs[directory]

    def input(self, rule: "Rule", given: Optional[str], default: str) -> tuple[Path, str]:
        """A rule's named input: `given`, else the tree's `default`.

        A relative `given` resolves under the tree. The second item is what a
        report prints for the input: `given` as typed, `default` from the tree.
        """
        if given:
            path = Path(given)
            return (path if path.is_absolute() else self.root(rule) / path), str(path)
        return self.root(rule) / default, default


# ====================================================================
# header-actions
# ====================================================================
# <header_bar-actions> and <overlay_panel-actions> are where a screen puts its
# header buttons, and ui_xml/header_action.xml owns how every one of them looks:
# height, circle or pill, fill by role, collapse on small screens. So:
#
#   errors (exit 1)
#     - an element in the slot other than <header_action> or a bind_* that acts
#       on the slot itself (motion_panel hides its whole slot below the small
#       tier)
#     - a child of a header_action other than bind_* and event_cb
#     - a header_action attribute outside its API (name, icon, text, text_tag,
#       role, collapse, callback, hidden): a width, height or color is the
#       component's call
#     - a role other than primary / neutral / danger, a collapse other than
#       auto / never, or a header_action with neither icon nor text
#     - a header_action with neither callback= nor an event_cb child: a dead
#       button
#   warnings (exit 0)
#     - more than 3 actions in one header, or more than one primary
#     - a header_action with both callback= and an event_cb child: one tap,
#       two calls
#
# A header_action outside a slot (the AMS panels' own header rows) gets the
# same attribute and child checks. Scope is every *.xml under ui_xml/ except
# ui_xml/translations/ and the component itself, in the repository this script
# is in. A file that does not parse is an error naming it.
SLOT_TAGS = {"header_bar-actions", "overlay_panel-actions"}
ACTION = "header_action"
ACTION_ATTRS = {"name", "icon", "text", "text_tag", "role", "collapse", "callback", "hidden"}
ROLES = {"primary", "neutral", "danger"}
COLLAPSE = {"auto", "never"}
ACTION_CHILDREN_EXTRA = {"event_cb"}
MAX_ACTIONS = 3


def is_binding(tag: str) -> bool:
    return tag.startswith("bind_")


def header_action_findings(tree: LineTree, rel: str) -> tuple[list[str], list[str]]:
    """(errors, warnings) of one file, in document order.

    An element is judged at its start, before its children, and at its end,
    after them, which is the order a parse reports them in. An element a
    malformed file left open has no end to judge.
    """
    errors: list[str] = []
    warnings: list[str] = []
    slots: list[dict] = []  # open slots: {"actions", "primaries", "tag"}
    actions: list[dict] = []  # open header_actions: {"where", "name", "callback", "event_cb"}

    def check_action(attrs: dict[str, str], in_slot: bool, where: str) -> None:
        name = attrs.get("name", "<unnamed>")
        for attr in sorted(set(attrs) - ACTION_ATTRS):
            errors.append(
                f"{where}: header_action {name} sets {attr}=: its look belongs to "
                f"ui_xml/header_action.xml (props: {', '.join(sorted(ACTION_ATTRS))})"
            )
        role = attrs.get("role", "neutral")
        if role not in ROLES:
            errors.append(
                f"{where}: header_action {name} role=\"{role}\" is not one of "
                f"{', '.join(sorted(ROLES))}"
            )
        collapse = attrs.get("collapse", "auto")
        if collapse not in COLLAPSE:
            errors.append(
                f"{where}: header_action {name} collapse=\"{collapse}\" is not one "
                f"of {', '.join(sorted(COLLAPSE))}"
            )
        if not attrs.get("icon") and not attrs.get("text"):
            errors.append(f"{where}: header_action {name} has neither icon nor text")
        if in_slot and slots:
            slots[-1]["actions"] += 1
            if role == "primary":
                slots[-1]["primaries"] += 1

    # (element, its parent's tag, whether its end is judged, whether it has
    # started): every element is taken twice, before its children and after.
    pending = []
    if tree.root is not None:
        pending.append((tree.root, None, id(tree.root) not in tree.open_at_stop, False))
    while pending:
        node, parent, closed, started = pending.pop()
        tag, where = node.tag, f"{rel}:{node.line}"
        if started:
            if not closed:
                continue
            if tag == ACTION:
                action = actions.pop()
                if not action["callback"] and not action["event_cb"]:
                    errors.append(
                        f"{action['where']}: header_action {action['name']} has no "
                        f"callback= and no event_cb child: a button that does nothing"
                    )
                elif action["callback"] and action["event_cb"]:
                    warnings.append(
                        f"{action['where']}: header_action {action['name']} has both "
                        f"callback= and an event_cb child, so one tap calls both"
                    )
            if tag in SLOT_TAGS:
                slot = slots.pop()
                if slot["actions"] > MAX_ACTIONS:
                    warnings.append(
                        f"{where}: <{tag}> has {slot['actions']} actions; a header holds "
                        f"{MAX_ACTIONS} at most before the title starves"
                    )
                if slot["primaries"] > 1:
                    warnings.append(
                        f"{where}: <{tag}> has {slot['primaries']} primary actions; a "
                        f"screen has one main action"
                    )
            continue
        if parent in SLOT_TAGS and tag != ACTION and not is_binding(tag):
            errors.append(
                f"{where}: <{tag}> in <{parent}>: the actions slot takes only "
                f"<header_action> (and bind_* on the slot itself)"
            )
        if parent == ACTION and not (is_binding(tag) or tag in ACTION_CHILDREN_EXTRA):
            errors.append(
                f"{where}: <{tag}> inside <header_action>: only bind_* and "
                f"event_cb children act on it"
            )
        if parent == ACTION and tag == "event_cb" and actions:
            actions[-1]["event_cb"] = True
        if tag == ACTION:
            check_action(node.attrs, in_slot=parent in SLOT_TAGS, where=where)
            actions.append(
                {
                    "where": where,
                    "name": node.attrs.get("name", "<unnamed>"),
                    "callback": bool(node.attrs.get("callback")),
                    "event_cb": False,
                }
            )
        if tag in SLOT_TAGS:
            slots.append({"actions": 0, "primaries": 0, "tag": tag})
        pending.append((node, parent, closed, True))
        for child in reversed(node.children):
            pending.append((child, tag, id(child) not in tree.open_at_stop, False))

    if tree.error is not None:
        errors.append(f"{rel}: not parseable XML ({tree.error})")
    return errors, warnings


def header_actions(run: Run) -> int:
    files = run.xml(RULES["header-actions"])
    errors: list[str] = []
    warnings: list[str] = []
    for path in files.paths:
        rel = files.rel(path)
        if rel.startswith("ui_xml/translations/") or rel == "ui_xml/header_action.xml":
            continue
        found, warned = header_action_findings(files.lines(path), rel)
        errors += found
        warnings += warned

    for w in warnings:
        print(f"warning: {w}")
    for e in errors:
        print(f"error: {e}")
    if errors:
        print(
            f"{len(errors)} header action error(s): see ui_xml/header_action.xml and "
            f"docs/devel/LVGL9_XML_GUIDE.md § \"Slots: filling a component from outside\""
        )
        return 1
    return 0


# ====================================================================
# overlay-width
# ====================================================================
# Overlay width is decided at push time, never in XML.
#
# Background (#1178). theme_manager registers two overlay widths:
#
#   overlay_width_transient    = screen - nav - space_lg
#   overlay_width_destination  = screen - nav
#
# They are not spacing options. A destination occludes the backdrop entirely:
# it is a place you park, and its drill-downs are part of it. A transient layer
# leaves the backdrop showing at the leading edge: you opened it over
# something and will return.
#
# Which one an overlay gets depends on HOW THE USER REACHED IT, not on the
# overlay itself. fan_control_overlay is a transient layer when opened from
# Controls and a drill-down when opened from Settings > Fans. power_panel and
# settings_sensors have the same split. No static `width=` attribute can be
# correct for those, so NavigationManager::push_overlay() resolves the class
# against the live stack and writes the width.
#
# Before the gate existed, authors picked a constant by hand. 20 panels ended
# up gapped and 36 full with no stated rule, `overlay_panel.xml` defaulted to
# the minority variant so new panels silently inherited the wrong look, and
# console_settings_overlay rendered 16px WIDER than the console_panel it was
# pushed from.
#
# WHAT IS FLAGGED
#   1. Any XML naming #overlay_width_destination. Destination-ness is declared
#      in C++ by IPanelLifecycle::is_destination(), never in markup.
#   2. A navigation width constant passed as `width=` to an element that
#      extends overlay_panel. The component already bakes in the transient
#      width; push_overlay upgrades it. Hand-picking either constant there is
#      the exact mistake that produced #1178.
#
# NOT FLAGGED
#   - Hand-rolled overlay roots (extends="lv_obj" with align="right_mid") using
#     width="#overlay_width_transient". Those predate the overlay_panel
#     component and still need a born-width, because setup() runs before the
#     push and several of them measure their own layout there. They may name
#     the transient constant and nothing else.
#   - A deliberate custom width on an overlay_panel: widget_catalog_overlay is
#     width="70%" so the home grid stays visible behind it. Neither navigation
#     class applies. Such an overlay MUST call
#     NavigationManager::set_overlay_width_unmanaged() or push_overlay will
#     overwrite the width it chose.
#
# Scope is every *.xml under ui_xml/ outside the copied trees, in the
# repository holding the current directory. A --staged-only run reads the
# staged *.xml blobs of that repository's index instead, for what the commit
# will contain. FILE arguments are the files named. A file that cannot be read
# is passed over.
DESTINATION_CONST = "overlay_width_destination"

# <foo ... extends="overlay_panel" ...> spanning newlines, up to the closing >
OVERLAY_PANEL_ELEMENT = re.compile(
    r'<[A-Za-z_][\w.-]*\b[^>]*?extends="overlay_panel"[^>]*?>', re.DOTALL
)
# width= naming either navigation class. A custom width (70%, a px literal) is
# legal on an overlay_panel and is covered by set_overlay_width_unmanaged().
CLASS_WIDTH_ATTR = re.compile(r'\bwidth\s*=\s*"#overlay_width_(?:transient|destination)"')


def overlay_width_sources(run: Run) -> list[tuple[str, str]]:
    """(path, text) of every file to scan, content already sourced."""
    if run.given:
        out = []
        for f in run.given:
            text = run.reads.readable(Path(f))
            if text is not None:
                out.append((f, text))
        return out

    if run.staged:
        return [
            (path, text)
            for path, text in staged_files(suffixes=(".xml",))
            if not in_skipped_dir(path, ".", COPIED_TREES)
        ]

    files = run.xml(RULES["overlay-width"])
    out = []
    for path in files.paths:
        if in_skipped_dir(path, files.root, COPIED_TREES):
            continue
        text = run.reads.readable(path)
        if text is not None:
            out.append((files.rel(path), text))
    return out


def overlay_width_violations(path: str, text: str) -> list[tuple[str, int, str]]:
    """(path, line, message) for each violation in one file's text."""
    out: list[tuple[str, int, str]] = []
    # Both patterns name a navigation width class, so a file that spells none
    # has no violation.
    if "overlay_width_" not in text:
        return out

    for m in re.finditer(re.escape(DESTINATION_CONST), text):
        out.append(
            (
                path,
                line_of(text, m.start()),
                f"names #{DESTINATION_CONST}. Destination width is declared in C++ "
                "by overriding IPanelLifecycle::is_destination(), not in XML.",
            )
        )

    for m in OVERLAY_PANEL_ELEMENT.finditer(text):
        w = CLASS_WIDTH_ATTR.search(m.group(0))
        if w:
            out.append(
                (
                    path,
                    line_of(text, m.start() + w.start()),
                    "hand-picks a navigation width class on an overlay_panel. "
                    "push_overlay() resolves it from how the user got here; drop the "
                    "width attribute.",
                )
            )

    return out


def overlay_width(run: Run) -> int:
    findings: list[tuple[str, int, str]] = []
    for path, text in overlay_width_sources(run):
        findings += overlay_width_violations(path, text)

    if not findings:
        return 0

    print("Overlay width must be resolved at push time, not set in XML (#1178).\n")
    for path, line, msg in sorted(findings):
        print(f"  {path}:{line}: {msg}")
    print(f"\n{len(findings)} violation(s). See include/overlay_class.h.")
    return 1


# ====================================================================
# icon-names
# ====================================================================
# Every icon name written in ui_xml/ must exist in the codepoint table.
#
# `<icon src="NAME">` resolves NAME through helix::ui::icon::lookup_codepoint()
# at runtime. A name that is not in the table does not fail, does not abort,
# and does not stop the panel from building: it logs one warning and
# substitutes `image_broken_variant`. The user sees a broken-image glyph next
# to real text and has no way to tell it apart from a deliberate one.
#
# The sibling `text="#icon_name"` path cannot have this bug: those consts are
# generated into globals.xml from the same header by gen_icon_consts.py, so an
# unknown name is a missing const and the XML fails loudly. `src=` has no such
# generator standing behind it.
#
# Three ways a literal icon name reaches an <icon src=>:
#
#     1. DIRECT      <icon src="alert"/>
#     2. PROP DEFAULT   a component whose <icon> takes src="$p", where
#                       <prop name="p" default="radiator"/> supplies the name
#     3. INSTANCE SITE  <heater_icon src="thermometer"/>, the literal handed
#                       to that same $p at an instantiation
#
# A name that arrives as a bare `$param` with no literal anywhere is not
# checkable here and is not reported.
#
# KNOWN BLIND SPOT
#   Icon names built or chosen in C++ (kPowerIcons[], favorite-macro configs,
#   anything passed to lookup_codepoint() from a string variable) are outside
#   this rule.
#
# CORPUS ASSERTION
#   The output carries how many <icon> tags were seen, and finding none at all
#   is exit 2 rather than a pass. A pattern that silently stops matching
#   reports "0 problems" forever and reads exactly like a passing check; the
#   count is the only thing that tells the two apart. Tags rather than
#   resolved names, because a tree whose icons all arrive through $params has
#   no literals to count and is not thereby broken.
#
# Scope is every *.xml under ui_xml/ and include/ui_icon_codepoints.h, in the
# repository this script is in; a missing header is an error. --list prints
# every scanned reference with an ok or BAD mark before the verdict.
# {"name", "\xF3..."} in the codepoint table.
RE_REGISTERED = re.compile(r'\{\s*"([a-z0-9_]+)"\s*,')
# An <icon ...> tag, captured whole so attributes can be picked out of it.
RE_ICON_TAG = re.compile(r"<icon\b[^>]*>")
RE_ATTR = re.compile(r'\b([a-z_][a-z0-9_]*)\s*=\s*"([^"]*)"')
# <prop name="src" type="string" default="radiator"/>
RE_PROP = re.compile(r"<prop\b[^>]*>")


def is_literal(value: str) -> bool:
    """A name that can be resolved statically: no $param and no #const indirection."""
    return bool(value) and "$" not in value and "#" not in value


def listing_order(row) -> tuple:
    """Where a reference comes in a listing: by file, line, name, then how.

    Two references can share a line (two instance sites in one tag), and a tie
    left to insertion order follows the hash seed through the set of parameter
    names a component feeds its icons.
    """
    name, path, line, how = row
    return (str(path), line, name, how)


def icon_names(run: Run) -> int:
    root = run.root(RULES["icon-names"])
    codepoints = root / "include" / "ui_icon_codepoints.h"
    if not codepoints.exists():
        print(f"error: {codepoints} not found", file=sys.stderr)
        return 1
    known = set(RE_REGISTERED.findall(codepoints.read_text(encoding="utf-8")))

    files = run.xml(RULES["icon-names"])
    direct, prop_defaults, instance_sites = [], [], []
    icon_tags = 0
    # component stem -> prop names that feed an <icon src=>
    icon_props: dict = {}

    for path in files.paths:
        text = run.reads.text(path)
        # The tag pattern opens with the literal "<icon".
        if "<icon" not in text:
            continue
        for m in RE_ICON_TAG.finditer(text):
            attrs = dict(RE_ATTR.findall(m.group(0)))
            icon_tags += 1
            src = attrs.get("src")
            if src is None:
                continue
            if is_literal(src):
                direct.append((src, files.rel(path), line_of(text, m.start()), "src="))
            elif src.startswith("$"):
                icon_props.setdefault(path.stem, set()).add(src[1:])

    # The second pass needs the full icon_props map, so it runs after every file is seen.
    for path in files.paths:
        text = run.reads.text(path)
        props = icon_props.get(path.stem)
        if props:
            for m in RE_PROP.finditer(text):
                attrs = dict(RE_ATTR.findall(m.group(0)))
                name, default = attrs.get("name"), attrs.get("default")
                if name in props and default is not None and is_literal(default):
                    prop_defaults.append(
                        (default, files.rel(path), line_of(text, m.start()), f"<prop {name}> default")
                    )

        for component, params in icon_props.items():
            # The pattern opens with the literal "<component", so a file that
            # never instantiates the component is not searched for it.
            if f"<{component}" not in text:
                continue
            for m in re.finditer(rf"<{re.escape(component)}\b[^>]*>", text):
                attrs = dict(RE_ATTR.findall(m.group(0)))
                for param in params:
                    value = attrs.get(param)
                    if value is not None and is_literal(value):
                        instance_sites.append(
                            (
                                value,
                                files.rel(path),
                                line_of(text, m.start()),
                                f"<{component} {param}=>",
                            )
                        )

    everything = direct + prop_defaults + instance_sites

    if run.list_mode:
        for name, path, line, how in sorted(everything, key=listing_order):
            mark = "ok " if name in known else "BAD"
            print(f"{mark} {name:28} {path}:{line} ({how})")

    print(
        f"Scanned {len(everything)} icon references ({icon_tags} <icon> tags) across "
        f"{len(set(r[1] for r in everything))} files: {len(direct)} direct, "
        f"{len(prop_defaults)} prop defaults, {len(instance_sites)} instance sites"
    )
    print(f"Codepoint table: {len(known)} registered names")
    # stdout block-buffers to a pipe and stderr does not, so without this the
    # failure list below lands ABOVE the counts it is supposed to follow.
    sys.stdout.flush()

    # Zero <icon> tags anywhere means the tag pattern stopped matching, not
    # that the tree is clean. Findings are reported first: real hits prove the
    # matcher works, so they are never masked by this.
    bad = [r for r in everything if r[0] not in known]
    if not bad and icon_tags == 0:
        print(
            "\n❌ No <icon> tags found at all.\n"
            "   The scan pattern no longer matches the XML. Fix this script, not the tree.",
            file=sys.stderr,
        )
        return 2

    if not bad:
        print("✅ Every icon name resolves to a registered codepoint")
        return 0

    print(f"\n❌ {len(bad)} icon name(s) are not in {codepoints.name}:\n", file=sys.stderr)
    for name, path, line, how in sorted(bad, key=listing_order):
        print(f"   {path}:{line}  {how}  '{name}'", file=sys.stderr)
    print(
        "\nEach renders image_broken_variant at runtime with only a log warning.\n"
        "Use a registered name, or add the codepoint to include/ui_icon_codepoints.h\n"
        "(a NEW glyph also needs `make regen-fonts` and a rebuild; an alias to a\n"
        "codepoint already in the table does not).",
        file=sys.stderr,
    )
    return 1


# ====================================================================
# responsive-token-scope
# ====================================================================
# Token auto-discovery is top-level-only. It lists files with
# theme_manager_find_xml_files() (src/ui/theme/theme_token_scan.cpp) and
# recursive=false, which skips every subdirectory, so only ui_xml/*.xml is
# ever parsed for `<px name="foo_small">` / `<string name="foo_small">`. A
# responsive token declared in ui_xml/components/ or anywhere else below the
# top level is never registered, and every `#foo` that references it silently
# resolves to nothing. Neither side warns (prestonbrown/helixscreen#1211).
#
# Recursing was the obvious fix and is the wrong one. Discovery is
# alphabetical last-wins, so a component's `nav_width_small` would shadow the
# top-level token globally. Discovery stays top-level-only; this rule makes
# the constraint loud instead of silent.
#
# FLAGGED
#   A `<px>` or `<string>` declaration whose name ends in a breakpoint suffix
#   (_micro, _tiny, _small, _medium, _large, _xlarge, _xxlarge) in any ui_xml
#   file that is not directly in the top level of ui_xml/.
#
# NOT FLAGGED
#   - Component-local `<consts>` with no breakpoint suffix. ui_xml/components/
#     files declare grid_gap, pin_dot_size, tray_default_height and similar;
#     those resolve through the component's own const scope, not through
#     discovery.
#   - References. `height="#row_height_small"` from a variant file is exactly
#     what variants are for: only the DECLARATION has to sit at the top level.
#   - `name=` on anything else: a widget handle or an <api> <prop> is a
#     different namespace and never reaches token discovery.
#
# Location is decided by the last `ui_xml` path component, so an explicitly
# passed file (a fixture, one from another checkout) is judged the same way as
# ui_xml/components/foo.xml from the repository root; a path with no ui_xml
# component cannot be placed and is skipped. Scope is every *.xml under ui_xml/
# outside the copied trees, in the repository holding the current directory;
# a top-level file is never opened. FILE arguments are the files named. A
# file that cannot be read is passed over.
RESPONSIVE_SUFFIXES = ("_micro", "_tiny", "_small", "_medium", "_large", "_xlarge", "_xxlarge")

# <px name="..."> / <string name="...">, the two element types
# theme_manager_parse_all_xml_for_suffix() reads. Attributes may wrap onto the
# next line, so match across newlines.
DECL_RE = re.compile(r'<(px|string)\b[^>]*?\bname\s*=\s*"([^"]*)"', re.DOTALL)


def is_in_subdirectory(path: Path) -> bool:
    """True when the file sits below the top level of a ui_xml/ tree."""
    parts = path.parts
    for i in range(len(parts) - 1, -1, -1):
        if parts[i] == "ui_xml":
            return len(parts) - i > 2
    return False


def responsive_token_scope(run: Run) -> int:
    name_of = str
    if run.given:
        paths = [Path(f) for f in run.given]
    else:
        files = run.xml(RULES["responsive-token-scope"])
        paths = [p for p in files.paths if not in_skipped_dir(p, files.root, COPIED_TREES)]
        name_of = files.rel

    findings: list[tuple[str, int, str]] = []
    for path in paths:
        if not is_in_subdirectory(path):
            continue
        text = run.reads.readable(path)
        if text is None:
            continue
        # The declaration pattern opens with "<px" or "<string".
        if "<px" not in text and "<string" not in text:
            continue
        text = blank_xml_comments(text)
        for m in DECL_RE.finditer(text):
            name = m.group(2)
            if name.endswith(RESPONSIVE_SUFFIXES):
                out = (name_of(path), line_of(text, m.start()), f'<{m.group(1)} name="{name}">')
                findings.append(out)

    if not findings:
        if not run.given:
            print(f"✓ responsive tokens: {len(paths)} ui_xml file(s), none declared in a subdirectory")
        return 0

    print("Responsive design tokens must be declared at the top level of ui_xml/ (#1211).\n")
    for path, line, decl in sorted(findings):
        print(f"  {path}:{line}: {decl} is never registered. Move it to ui_xml/globals.xml.")
    print(
        f"\n{len(findings)} violation(s). theme_manager_find_xml_files() skips subdirectories, so\n"
        "token auto-discovery only ever parses top-level ui_xml/*.xml — a token declared\n"
        "below that is silently missing and every `#token` reading it resolves to nothing.\n"
        "\n"
        "It is top-level-only on purpose: discovery is alphabetical last-wins, so letting a\n"
        "variant directory declare tokens would let a variant token shadow its base token\n"
        "globally rather than only while that variant is active.\n"
        "\n"
        "Declare the whole responsive triplet in ui_xml/globals.xml and reference it with\n"
        "#token from the variant file."
    )
    return 1


# ====================================================================
# modal-chrome-budget
# ====================================================================
# A modal's chrome must match the content cap it budgets against.
#
# Background (#1277). `#dialog_content_max` is a single global ladder whose
# per-breakpoint values are derived from ONE chrome shape (modal_dialog.xml's):
#
#     header + scrollable content + divider + button row  <=  85% of the screen
#
# The dialog root is `height="content"` with `style_max_height="85%"` and
# `scrollable="false"`. When the children total more than the cap, the root
# clamps and the overflow falls off the BOTTOM, which is the button row. The
# user is left with a modal they cannot dismiss.
#
# Measured on a 480x272 (micro) panel, ams_loading_error_modal.xml with a long
# fault string and the AFC diagram pinned below the scroll area:
#
#     card         y= 21  h=231  -> bottom 252   (clamped at exactly the cap)
#     scroll area  y= 50  h=140  -> bottom 190   (clamped at dialog_content_max)
#     AFC graphic  y=190  h= 38  -> bottom 228
#     button row   y=236  h= 27  -> bottom 263   <-- 11px PAST the card bottom
#
# LVGL cannot solve this in layout: lv_flex.c implements `grow` only, there is
# no CSS-style flex-shrink, so a child cannot be squeezed to make room. The
# budget has to be right up front.
#
# THE RULE
#   Everything except a divider and the button row lives INSIDE the scroll
#   container. Then the one global token is correct by construction.
#
#   Where extra chrome must stay visible while the text scrolls, the container
#   opts into #dialog_content_pinned_max, measured for ONE extra block (a
#   diagram, a status row, a second button row) below the scroll area.
#   klipper_recovery_dialog uses #dialog_content_recovery_max, measured on
#   that card, with no extra block budgeted. A shape with more than that
#   (action_prompt_modal: AFC diagram + wrapping prompt rows + a footer row)
#   is beyond any single ladder: measure it on device and mark the file
#   MODAL_CHROME_OK.
#
# WHAT IS FLAGGED
#   - An element that follows a shared-cap container among its siblings and is
#     neither a divider nor the FIRST button row, when the container's token
#     has no budget left for it. Fix by moving the element inside the scroll
#     container, or by switching to the sibling token that reserves its height.
#   - A SECOND button row below the container while it is still on
#     #dialog_content_max; switch the container to #dialog_content_pinned_max.
#   - A card raised ABOVE the shared 85% cap (`style_max_height="90%"` &c) in
#     a file that uses a shared content token. Raising one card unsizes the
#     ladder arithmetic every modal shares.
#
# NOT FLAGGED
#   - Dividers, and the first button row after the container. That is the
#     budgeted shape.
#   - Containers on #dialog_content_pinned_max, which get one extra paid block
#     (a pinned block OR a second button row) before they are flagged again.
#   - Modals that size themselves without any shared content token (flat px
#     caps, favorite_macro_config_modal's 520). They opted out of the shared
#     budget and are the author's problem.
#   - Two sibling blocks that can never be visible together, because they are
#     bound to the same subject with different `ref_value`s. Their heights do
#     not sum (hidden_network_modal's form vs its connecting spinner).
#
# KNOWN GAP
#   A wrapping button row (`row_wrap`) is recognised as the budgeted row but
#   can grow to TWO+ rows of buttons when a macro supplies many long labels
#   (`ctl demo action-prompt-many`: seven material presets over three rows).
#   The single-row budget does not cover that growth; it was measured to fit
#   at five breakpoints and is re-measured, not assumed, when it changes.
#
# Opt out with `MODAL_CHROME_OK: <reason>` anywhere in the file.
#
# Scope is every *.xml under ui_xml/ outside the copied trees and
# ui_xml/translations/, in the current directory. A file that does not parse is
# left to the xmllint pass; one that cannot be read is an error naming it.
STANDARD_TOKEN = "#dialog_content_max"
PINNED_TOKEN = "#dialog_content_pinned_max"
RECOVERY_TOKEN = "#dialog_content_recovery_max"
SHARED_TOKENS = (STANDARD_TOKEN, PINNED_TOKEN, RECOVERY_TOKEN)
MODAL_SKIP = COPIED_TREES + ("translations",)

# Extra non-chrome blocks each token's budget has already paid for.
TOKEN_BUDGET = {STANDARD_TOKEN: 0, PINNED_TOKEN: 1, RECOVERY_TOKEN: 0}

# The cap the shared budget is derived from. A card raised above it while
# using a shared content token has unsized the ladder for everyone.
DEFAULT_ROOT_CAP_PCT = 85

OPT_OUT = "MODAL_CHROME_OK"

# Chrome the budget already accounts for, allowed to follow the scroll container.
DIVIDER_TAGS = {"divider_horizontal"}
BUTTON_ROW_TAGS = {"modal_button_row"}


def is_button_row(el, footer_targets=frozenset()):
    """Is this element the modal's button row?

    A frame component's footer slot container (the target of its
    `<slot name="footer">`) is where a dialog puts its button row, so it
    counts as the row.

    modal_dialog.xml and several modals hand-roll the row rather than using the
    modal_button_row component, so the tag name alone does not identify it. Two
    hand-rolled shapes exist:

      - fixed height:  <lv_obj height="#modal_button_height" flex_flow="row"> (or #button_height)
      - wrapping:      <lv_obj name="button_container" flex_flow="row_wrap"
                               height="content">   (action_prompt_modal)

    The wrapping form is deliberately recognised. Flagging it would be a false
    positive (it IS the budgeted button row), but note it can grow to TWO rows
    when enough buttons wrap, which the single-row budget does not cover.
    """
    if el.tag in BUTTON_ROW_TAGS:
        return True
    if (el.get("name") or "") in footer_targets:
        return True
    # A wrapper whose only widget is the row, so the row and its leading divider
    # hide as a unit (klipper_recovery_dialog). Bindings are not widgets.
    widgets = [c for c in el if not c.tag.startswith("bind_")]
    if len(widgets) == 1 and widgets[0].tag in BUTTON_ROW_TAGS:
        return True
    if el.get("height") in ("#modal_button_height", "#button_height"):
        return True
    name = el.get("name") or ""
    flex = el.get("flex_flow") or ""
    return "button" in name and flex.startswith("row")


def is_divider(el):
    return el.tag in DIVIDER_TAGS


def raised_cap_pct(value):
    """The percentage of a `style_max_height` like '90%', or None."""
    if value is None or not value.endswith("%"):
        return None
    try:
        return int(value[:-1])
    except ValueError:
        return None


def hidden_bindings(el):
    """{subject: ref_value} for each `hidden` flag binding on this element."""
    out = {}
    for binding in el:
        if not binding.tag.startswith("bind_flag_if"):
            continue
        if binding.get("flag") != "hidden":
            continue
        subject = binding.get("subject")
        if subject is not None:
            out[subject] = (binding.tag, binding.get("ref_value"))
    return out


def mutually_exclusive(a, b):
    """Can these two siblings never be on screen at the same time?

    hidden_network_modal shows either its entry form or its connecting spinner,
    switched by `hidden_connecting`: the form hides on 1, the spinner hides on 0.
    Their heights never sum, so counting the second against the first's budget
    is a false positive, and a gate that fires on correct markup gets disabled.
    """
    a_bind, b_bind = hidden_bindings(a), hidden_bindings(b)
    for subject, (a_tag, a_ref) in a_bind.items():
        if subject not in b_bind:
            continue
        b_tag, b_ref = b_bind[subject]
        # Same predicate, different trigger value -> exactly one is visible.
        if a_tag == b_tag and a_ref != b_ref:
            return True
    return False


def find_violations(text):
    """Yield (kind, where, detail) for every over-budget shape in one file's text.

    kind 'pinned_block': an unbudgeted element below a shared-cap container.
    kind 'raised_cap':   a card cap above 85% in a file using a shared token.
    """
    if OPT_OUT in text:
        return
    # Every violation sits under a style_max_height on a shared token, so a
    # file without the attribute has none, whether or not it parses.
    if "style_max_height" not in text:
        return

    try:
        root = ET.fromstring(text)
    except ET.ParseError:
        # Malformed XML is the xmllint pass's job, not this rule's. Staying
        # silent here beats reporting a parse error as a chrome violation.
        return

    footer_targets = frozenset(
        slot.get("target") for slot in root.iter("slot")
        if slot.get("name") == "footer" and slot.get("target"))

    uses_shared = any(
        el.get("style_max_height") in SHARED_TOKENS for el in root.iter()
    )

    # A card raised above 85% while budgeting its content against a shared
    # ladder: the extra room is per-dialog arithmetic that unsizes the ladder
    # for every other modal. The fix is the pinned token that reserves the
    # extra chrome, never a taller card.
    for view in root.iter('view'):
        pct = raised_cap_pct(view.get('style_max_height'))
        if pct is not None and pct > DEFAULT_ROOT_CAP_PCT and uses_shared:
            yield ('raised_cap', view.get('name') or view.tag,
                   view.get('style_max_height'))

    for parent in root.iter():
        children = list(parent)
        for idx, child in enumerate(children):
            token = child.get('style_max_height')
            if token not in SHARED_TOKENS:
                continue

            # Dividers and the FIRST button row are the budgeted chrome;
            # everything after that must be paid for out of the token's budget.
            budget = TOKEN_BUDGET[token]
            button_rows_seen = 0
            first_button_row = None

            for sibling in children[idx + 1:]:
                if is_divider(sibling):
                    continue
                if is_button_row(sibling, footer_targets):
                    button_rows_seen += 1
                    if button_rows_seen == 1:
                        first_button_row = sibling
                        continue  # the one row every budget covers
                    # State-switched rows never co-render: a modal whose button
                    # row swaps with debug_bundle_state-style bindings shows
                    # exactly one row at a time, so the extra rows cost no
                    # chrome. Compare against the FIRST row (the budgeted one),
                    # same predicate as the pinned-block exemption below.
                    if first_button_row is not None and mutually_exclusive(first_button_row, sibling):
                        continue
                    # A second button row costs a budget slot like a pinned
                    # block.
                    if budget > 0:
                        budget -= 1
                        continue
                    yield ('pinned_block', sibling.tag, token)
                    continue
                if mutually_exclusive(child, sibling):
                    continue
                if budget > 0:
                    budget -= 1
                    continue
                yield ('pinned_block', sibling.tag, token)


def modal_chrome_budget(run: Run) -> int:
    files = run.xml(RULES["modal-chrome-budget"])
    violations = []
    for path in files.paths:
        if in_skipped_dir(path, files.root, MODAL_SKIP):
            continue
        rel = files.rel(path)
        for kind, where, detail in find_violations(run.reads.text(path, errors="replace")):
            violations.append((rel, kind, where, detail))

    if not violations:
        print("✅ Modal chrome budget: every pinned block is accounted for")
        return 0

    print(f"❌ Modal chrome budget: {len(violations)} violation(s)\n")
    for rel, kind, where, detail in violations:
        if kind == "raised_cap":
            print(f"  {rel}: card \"{where}\" is capped at {detail}, above the "
                  f"shared {DEFAULT_ROOT_CAP_PCT}%")
        else:
            print(f"  {rel}: <{where}> follows a {detail} container")

    print(
        "\n"
        "Each of these sits BELOW the scroll area and outside the budget that\n"
        "the container's token was sized for, so the modal overruns its 85% cap\n"
        "and the button row is clipped off the bottom of the card.\n"
        "\n"
        "Fix by one of:\n"
        "  - moving the element INSIDE the scroll container (preferred), or\n"
        f"  - switching that container to {PINNED_TOKEN}, which reserves\n"
        "    one extra block (a pinned block or a second button row), when it\n"
        "    must stay visible while the text scrolls.\n"
        "A raised card cap is never the fix: it unsizes the shared ladder.\n"
        "\n"
        f"Deliberate exception? Add \"{OPT_OUT}: <reason>\" to the file."
    )
    return 1


# ====================================================================
# shipped-spacing-tokens
# ====================================================================
# Every spacing token read from C++ survives release packaging.
#
# `theme_manager_get_spacing("foo")` resolves `foo` from a const that
# theme_manager registered by scanning the top level of ui_xml/ for the
# `foo_small` / `foo_medium` / `foo_large` triplet. A missing const is not an
# error: lv_xml_get_const_silent() returns nullptr, get_spacing returns 0, and
# every caller silently takes its hardcoded fallback.
#
# Release packaging is where a token can go missing. `release-copy-xml-config`
# in mk/cross.mk stages ui_xml/ and then deletes the DEV_PANEL_XML files, so a
# token declared only in a dev-only panel exists in the development tree and on
# no shipped device. Nothing warns on either side, and every affected widget
# renders at its fallback size on every breakpoint, which on the MEDIUM tier
# usually matches the token exactly, so the desktop and the 800x480 panels look
# right while the small and large ones do not.
#
# WHAT IS FLAGGED
#   A token passed to theme_manager_get_spacing() whose `<px>` declaration,
#   the bare name or any of the _small/_medium/_large variants, is declared
#   ONLY in files that release packaging deletes. That is a token shipping code
#   depends on and shipped XML does not carry.
#
# NOT FLAGGED
#   - Tokens with no `<px>` declaration anywhere in ui_xml/. border_radius and
#     button_height are registered from C++ tables per breakpoint, never from
#     XML; packaging cannot take away what XML never provided.
#   - Tokens whose declarations live in shipped files, complete or not. An
#     incomplete responsive set is theme_manager_validate_constant_sets()'s
#     job and it reports one at startup.
#   - Declarations in ui_xml subdirectories: token discovery never reads them,
#     which is the responsive-token-scope rule.
#
# The DEV_PANEL_XML list is read from mk/cross.mk so this rule and packaging
# cannot disagree about which files ship.
#
# Scope is the top-level *.xml of ui_xml/, every C++ source under src/ and
# include/, and mk/cross.mk, in the repository holding the current directory.
# --ui-xml, --src and --cross-mk name each instead. A missing directory or
# makefile, or a makefile with no DEV_PANEL_XML, is an error: a rule that
# found no dev panels would report every tree clean.
SOURCE_DIRS = ("src", "include")
SOURCE_SUFFIXES = (".cpp", ".cc", ".h", ".hpp")

# The three tiers theme_manager requires for a complete responsive set. The
# optional tiers (_micro/_tiny/_xlarge/_xxlarge) fall back to these, so losing
# one of them alone cannot strand a token.
REQUIRED_SUFFIXES = ("_small", "_medium", "_large")

CALL_RE = re.compile(r'theme_manager_get_spacing\(\s*"([^"]*)"')

PX_DECL_RE = re.compile(r'<px\b[^>]*?\bname\s*=\s*"([^"]*)"', re.DOTALL)

DEV_PANEL_RE = re.compile(r"^DEV_PANEL_XML\s*:?=\s*(.*)$")


def read_dev_panel_xml(path: Path) -> set[str]:
    """The XML basenames release packaging deletes, per mk/cross.mk."""
    names: list[str] = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        m = DEV_PANEL_RE.match(line)
        if not m:
            continue
        value = m.group(1)
        # Honour make's backslash line continuations.
        while value.endswith("\\") and i + 1 < len(lines):
            i += 1
            value = value[:-1] + " " + lines[i].strip()
        names += value.split()
        break
    return set(names)


def declared_tokens(files: XmlFiles, directory: Path) -> dict[str, set[str]]:
    """px token name -> basenames of the top-level ui_xml files declaring it."""
    out: dict[str, set[str]] = {}
    for path in files.paths:
        if path.parent != directory or not path.is_file():
            continue
        text = files.reads.readable(path)
        if text is None:
            continue
        text = blank_xml_comments(text)
        for m in PX_DECL_RE.finditer(text):
            out.setdefault(m.group(1), set()).add(path.name)
    return out


def spacing_callers(src_dirs: list[tuple[Path, Callable[[Path], str]]]) -> dict[str, list[tuple[str, int]]]:
    """token -> [(file, line)] for every theme_manager_get_spacing() call."""
    refs: dict[str, list[tuple[str, int]]] = {}
    for directory, shown in src_dirs:
        for path in sorted(directory.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            try:
                data = path.read_bytes()
            except OSError:
                continue
            if b"theme_manager_get_spacing" not in data:  # skip the costly blanking
                continue
            try:
                text = blank_cxx(as_read_text(data), strings="keep")
            except UnicodeDecodeError:
                continue
            for m in CALL_RE.finditer(text):
                refs.setdefault(m.group(1), []).append((shown(path), line_of(text, m.start())))
    return refs


def shipped_spacing_tokens(run: Run) -> int:
    rule = RULES["shipped-spacing-tokens"]
    root = run.root(rule)
    ui_xml, ui_xml_name = run.input(rule, run.ui_xml, "ui_xml")
    cross_mk, cross_mk_name = run.input(rule, run.cross_mk, "mk/cross.mk")
    if run.src:
        src_dirs = []
        for given in run.src:
            typed = Path(given)
            directory = typed if typed.is_absolute() else root / typed
            # A directory named on the command line reports its files as typed.
            src_dirs.append((directory, lambda p, t=typed, d=directory: str(t / p.relative_to(d))))
    else:
        src_dirs = [(root / d, lambda p: p.relative_to(root).as_posix()) for d in SOURCE_DIRS]

    if not ui_xml.is_dir():
        print(f"error: {ui_xml_name} is not a directory")
        return 1
    if not cross_mk.is_file():
        print(f"error: {cross_mk_name} not found — cannot read DEV_PANEL_XML")
        return 1

    dev_panels = read_dev_panel_xml(cross_mk)
    if not dev_panels:
        print(f"error: no DEV_PANEL_XML assignment found in {cross_mk_name}")
        return 1

    declared = declared_tokens(run.xml(rule, ui_xml), ui_xml)
    refs = spacing_callers(src_dirs)

    findings: list[tuple[str, str, set[str], list[tuple[str, int]]]] = []
    for token in sorted(refs):
        for name in (token,) + tuple(token + s for s in REQUIRED_SUFFIXES):
            files = declared.get(name)
            if files and files <= dev_panels:
                findings.append((token, name, files, refs[token]))

    if not findings:
        print(
            f"✓ shipped spacing tokens: {len(refs)} token(s) read from C++, "
            "none declared only in dev-panel XML"
        )
        return 0

    print("Spacing tokens read from C++ must be declared in XML that release packaging ships.\n")
    for token, name, files, callers in findings:
        where = ", ".join(sorted(files))
        print(f'  <px name="{name}"> is declared only in {where}, which release packaging deletes.')
        for path, line in callers:
            print(f'    read as theme_manager_get_spacing("{token}") at {path}:{line}')
    print(
        f"\n{len(findings)} token declaration(s) missing from a packaged release.\n"
        "release-copy-xml-config (mk/cross.mk) deletes DEV_PANEL_XML from the staged\n"
        "ui_xml/, so on every shipped device these consts do not exist,\n"
        "theme_manager_get_spacing() returns 0 and each caller falls back to its\n"
        "hardcoded default at every breakpoint.\n"
        "\n"
        "Move the declaration to a shipped top-level ui_xml file — ui_xml/globals.xml\n"
        "for a token a widget reads, or the feature's own *_tokens.xml."
    )
    return 1


# ====================================================================
# The table
# ====================================================================
class Rule:
    __slots__ = ("check", "header", "passed", "tree", "takes_files")

    def __init__(
        self,
        check: Callable[[Run], int],
        header: str,
        passed: Callable[[str], str],
        tree: str = "repository",
        takes_files: bool = False,
    ) -> None:
        self.check = check
        # The first line of the rule's section in a qc_xml_rules transcript.
        self.header = header
        # What the section prints when the rule passes, given the rule's own output.
        self.passed = passed
        # Which tree --root overrides: a key of TREES.
        self.tree = tree
        # FILE arguments replace the ui_xml/ scan.
        self.takes_files = takes_files


def last_line(output: str) -> str:
    return output.rstrip("\n").rsplit("\n", 1)[-1]


RULES = {
    "header-actions": Rule(
        header_actions,
        header="🔘 Checking header actions are header_action...",
        passed=lambda out: f"{out}✅ Every header button is a header_action",
        tree="script",
    ),
    "overlay-width": Rule(
        overlay_width,
        header="📐 Checking overlay width declarations...",
        passed=lambda out: "✅ No hand-picked overlay widths",
        takes_files=True,
    ),
    "icon-names": Rule(
        icon_names,
        header="🖼️  Checking icon names resolve to codepoints...",
        passed=last_line,
        tree="script",
    ),
    "responsive-token-scope": Rule(
        responsive_token_scope,
        header="📏 Checking responsive token placement...",
        passed=lambda out: "✅ Responsive tokens are all top-level",
        takes_files=True,
    ),
    "modal-chrome-budget": Rule(
        modal_chrome_budget,
        header="📐 Checking modal chrome budget...",
        passed=last_line,
        tree="directory",
    ),
    "shipped-spacing-tokens": Rule(
        shipped_spacing_tokens,
        header="📏 Checking spacing tokens ship in a release...",
        passed=last_line,
    ),
}


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", type=Path, help="resolve the tree under this directory")
    ap.add_argument(
        "--rule", dest="rules", action="append", choices=list(RULES), help="run only this rule"
    )
    ap.add_argument(
        "--staged-only",
        action="store_true",
        help="overlay-width reads the staged XML blobs of the repository the command runs in",
    )
    ap.add_argument(
        "--list", action="store_true", help="icon-names also lists every scanned reference"
    )
    ap.add_argument(
        "--ui-xml", help="shipped-spacing-tokens: directory of token declarations (default: ui_xml)"
    )
    ap.add_argument(
        "--src",
        action="append",
        help="shipped-spacing-tokens: directory of get_spacing() calls, repeatable "
        "(default: src and include)",
    )
    ap.add_argument(
        "--cross-mk",
        help="shipped-spacing-tokens: makefile holding DEV_PANEL_XML (default: mk/cross.mk)",
    )
    ap.add_argument(
        "files", nargs="*", help="files for overlay-width or responsive-token-scope, not ui_xml/"
    )
    args = ap.parse_args(argv)

    names = args.rules or list(RULES)
    if args.files:
        not_for = [n for n in names if not RULES[n].takes_files]
        if not_for:
            ap.error(f"FILE arguments do not apply to rule(s): {', '.join(not_for)}")
    if (args.ui_xml or args.src or args.cross_mk) and "shipped-spacing-tokens" not in names:
        ap.error("--ui-xml, --src and --cross-mk apply to shipped-spacing-tokens only")
    if args.staged_only and args.root:
        ap.error("--staged-only reads the index of the repository the command runs in; no --root")

    run = Run(
        args.root.resolve() if args.root else None,
        args.files,
        args.staged_only,
        args.list,
        args.ui_xml,
        args.src or [],
        args.cross_mk,
    )

    # One rule alone reports itself. Several, or all, print the sections of a
    # qc_xml_rules transcript.
    sections = len(names) != 1
    results = []
    for name in names:
        rule = RULES[name]
        if not sections:
            try:
                results.append(rule.check(run))
            except XmlReadError as exc:
                print(f"error: {exc}", file=sys.stderr)
                results.append(1)
            sys.stdout.flush()
            continue

        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            try:
                status, unreadable = rule.check(run), ""
            except XmlReadError as exc:
                status, unreadable = 1, f"error: {exc}\n"
        output = out.getvalue() + err.getvalue() + unreadable
        print(rule.header)
        if status == 0:
            verdict = rule.passed(output)
            if verdict:
                print(verdict)
        else:
            if output:
                print(output, end="" if output.endswith("\n") else "\n")
            print(f"   Run: python3 scripts/check_xml_rules.py --rule {name} --list")
        print()
        results.append(status)

    # icon-names exits 2 when its pattern found nothing at all, and keeps that
    # code unless another rule failed outright.
    if 2 in results and 1 not in results:
        return 2
    return 1 if any(results) else 0


if __name__ == "__main__":
    sys.exit(main())
