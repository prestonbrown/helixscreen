# SPDX-License-Identifier: GPL-3.0-or-later
"""
Extract translatable strings from static C++ tables.

The call-site patterns in ``extractor.py`` find a literal only where it is
written into the call: ``lv_tr("Purge")``. A large part of the UI does not work
that way. Widget definitions, device sections/actions and toolchange step models
are static tables of ``const char*`` / ``std::string``, and the UI translates
them at render time through a variable::

    lv_tr(def.display_name)               ui_widget_catalog_overlay.cpp
    lv_tr(section.description.c_str())    ui_ams_device_operations_overlay.cpp
    lv_tr(s.label.c_str())                ui_ams_sidebar.cpp

A regex over the call site sees ``def.display_name``, not the 40 strings the
table holds, so none of them were ever offered for translation. The whole
home-screen widget catalog and the AFC/Happy Hare/ACE device-settings surface
rendered in English in all nine languages, and every render logged a
``lv_translation_get: tag is not found`` line -- 1445 of them, 294 KB, inside a
single debug bundle's ring buffer.

The previous answer to this was a hand-written ``*_translation_hints_()``
function naming each literal a second time (see ``ams_state.cpp``). That works
but it is a copy that has to be kept in sync by hand, and a row added without a
matching hint is silently untranslated again. These parsers read the tables
themselves, so adding a row is enough.

Parsing is by STRUCTURE, not by file: any file declaring one of these shapes is
covered, including backends added later.
"""

import re
from typing import Iterator, List, Optional, Set

# A single "..." C string literal, honouring backslash escapes.
_STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# `.field = "value"` designated initializer.
_DESIGNATED_RE = re.compile(r'\.(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"')


def _find_matching(text: str, open_pos: int) -> int:
    """
    Index of the brace/paren closing the one at ``open_pos``, or -1.

    String literals are skipped so a brace inside one cannot unbalance the walk.
    """
    pairs = {"{": "}", "(": ")", "[": "]"}
    opener = text[open_pos]
    closer = pairs.get(opener)
    if closer is None:
        return -1

    depth = 0
    i = open_pos
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            m = _STRING_RE.match(text, i)
            i = m.end() if m else i + 1
            continue
        if c == "'":
            # Character literal, including '\'' and '\\'.
            i += 3 if text[i + 1 : i + 2] != "\\" else 4
            continue
        if c == opener:
            depth += 1
        elif c == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _split_top_level(body: str) -> List[str]:
    """Split ``body`` on commas that are not nested in brackets or a literal."""
    fields: List[str] = []
    depth = 0
    start = 0
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == '"':
            m = _STRING_RE.match(body, i)
            i = m.end() if m else i + 1
            continue
        if c in "{([":
            depth += 1
        elif c in "})]":
            depth -= 1
        elif c == "," and depth == 0:
            fields.append(body[start:i])
            start = i + 1
        i += 1
    fields.append(body[start:])
    return fields


def _literal_at(field: str) -> Optional[str]:
    """
    The string literal a positional field holds, or None.

    ``nullptr``, an enum value or a number yields None. Adjacent literals are
    joined the way the compiler joins them.
    """
    field = field.strip()
    if not field or field.startswith("nullptr") or field.startswith("NULL"):
        return None
    pieces = _STRING_RE.findall(field)
    if not pieces:
        return None
    # Reject a field that is a call or comparison happening to contain a literal
    # (e.g. `foo("x")`), which is not a plain table cell.
    without = _STRING_RE.sub("", field)
    if re.search(r"[A-Za-z_]\w*\s*\(", without):
        return None
    return "".join(pieces)


def _iter_records(text: str, start: int, end: int) -> Iterator[str]:
    """
    Yield the body of every ``{...}`` group between start and end, at any depth.

    Depth-agnostic on purpose. Counting levels down to the rows means encoding
    how clang-format happened to lay the table out -- a `switch` around the
    `return {...}` arms adds one, and reading a row at the wrong level silently
    yields a whole nested record as a "field" (that is how `{"cut", "Cut tip"}`
    once extracted as the string `cutCut tip`). Callers pair this with a shape
    predicate instead, which does not care how deeply the row is nested.
    """
    i = start
    while i < end:
        c = text[i]
        if c == '"':
            m = _STRING_RE.match(text, i)
            i = m.end() if m else i + 1
            continue
        if c == "{":
            close = _find_matching(text, i)
            if close == -1 or close > end:
                return
            body = text[i + 1 : close]
            yield body
            yield from _iter_records(body, 0, len(body))
            i = close + 1
            continue
        i += 1


def _is_bool(field: str) -> bool:
    return field.strip() in ("true", "false")


def _is_number(field: str) -> bool:
    return bool(re.fullmatch(r"[-+]?\d+(\.\d+)?f?", field.strip()))


def _positional(record: str, indices) -> Set[str]:
    """Literals at the given positional field indices of one record."""
    found: Set[str] = set()
    fields = _split_top_level(record)
    for idx in indices:
        if idx < len(fields):
            lit = _literal_at(fields[idx])
            if lit:
                found.add(lit)
    return found


def _block_after(content: str, marker_re) -> Optional[tuple]:
    """(start, end) of the first ``{...}`` block following a marker match."""
    m = marker_re.search(content)
    if not m:
        return None
    brace = content.find("{", m.end())
    if brace == -1:
        return None
    close = _find_matching(content, brace)
    if close == -1:
        return None
    return brace + 1, close


# ---------------------------------------------------------------------------
# Table shapes
# ---------------------------------------------------------------------------

# PanelWidgetDef: id, display_name, icon, description,
#                 hardware_gate_subject, hardware_gate_hint, ...
# display_name and description render in the widget catalog; hardware_gate_hint
# is the "Requires ..." / "No ... detected" line under an unavailable widget.
_WIDGET_DEFS_RE = re.compile(r"s_widget_defs\s*=\s*")
_WIDGET_FIELDS = (1, 3, 5)

# DeviceSection: id, label, display_order, description
_DEVICE_SECTION_FN_RE = re.compile(r"std::vector\s*<\s*DeviceSection\s*>\s*\w+\s*\([^)]*\)\s*")
_SECTION_FIELDS = (1, 3)

# DeviceAction: id, label, icon, section, description, type, ...
# disable_reason (13) is excluded on purpose: the UI only logs it (see
# ui_ams_device_section_detail_overlay.cpp), it is never shown.
_DEVICE_ACTION_POSITIONAL_RE = re.compile(r"\b(?:DA|DeviceAction)\s*\{")
_ACTION_FIELDS = (1, 4)

# OperationStep phase templates: {token, label, optional}
_PHASE_TEMPLATE_RE = re.compile(r"::toolchange_phase_template\s*\([^)]*\)[^{]*")
_PHASE_FIELDS = (1,)

# Designated-initializer records (afc_defaults.cpp style) name their fields, so
# position does not apply. Only these two are user-visible.
_DESIGNATED_FIELDS = {"label", "description"}


def _is_widget_row(fields: List[str]) -> bool:
    """
    PanelWidgetDef row: id, display_name, icon, description, gate_subject,
    gate_hint, then default_enabled and the layout numbers. The first four cells
    are literals (never nullptr); the two gate cells are a literal or nullptr.
    """
    if len(fields) < 7:
        return False
    if any(_literal_at(fields[i]) is None for i in (0, 1, 2, 3)):
        return False
    return _is_bool(fields[6])


def _is_section_row(fields: List[str]) -> bool:
    """DeviceSection row: id, label, display_order, description."""
    return (
        len(fields) == 4
        and _literal_at(fields[0]) is not None
        and _literal_at(fields[1]) is not None
        and _is_number(fields[2])
        and _literal_at(fields[3]) is not None
    )


def _is_phase_row(fields: List[str]) -> bool:
    """Phase-template row: {token, label, optional}."""
    return (
        len(fields) == 3
        and _literal_at(fields[0]) is not None
        and _literal_at(fields[1]) is not None
        and _is_bool(fields[2])
    )


def _collect(content: str, span, shape, indices) -> Set[str]:
    """Literals from every record in ``span`` whose fields match ``shape``."""
    found: Set[str] = set()
    for record in _iter_records(content, *span):
        fields = _split_top_level(record)
        if shape(fields):
            found |= _positional(record, indices)
    return found


def _extract_widget_defs(content: str) -> Set[str]:
    span = _block_after(content, _WIDGET_DEFS_RE)
    if not span:
        return set()
    return _collect(content, span, _is_widget_row, _WIDGET_FIELDS)


def _extract_device_sections(content: str) -> Set[str]:
    found: Set[str] = set()
    for m in _DEVICE_SECTION_FN_RE.finditer(content):
        brace = content.find("{", m.end())
        if brace == -1:
            continue
        close = _find_matching(content, brace)
        if close == -1:
            continue
        found |= _collect(content, (brace + 1, close), _is_section_row, _SECTION_FIELDS)
    return found


def _extract_device_actions(content: str) -> Set[str]:
    found: Set[str] = set()

    # Positional `DA{...}` / `DeviceAction{...}` (ams_backend_ace.cpp style).
    for m in _DEVICE_ACTION_POSITIONAL_RE.finditer(content):
        brace = content.index("{", m.start())
        close = _find_matching(content, brace)
        if close == -1:
            continue
        found |= _positional(content[brace + 1 : close], _ACTION_FIELDS)

    # Designated initializers inside `actions.push_back({...})`
    # (afc_defaults.cpp / hh_defaults.cpp style). Scoping to the push_back call
    # keeps `.label =` on unrelated structs out.
    for m in re.finditer(r"\w*actions\w*\.(?:push_back|emplace_back)\s*\(", content):
        paren = content.index("(", m.start())
        close = _find_matching(content, paren)
        if close == -1:
            continue
        for field, value in _DESIGNATED_RE.findall(content[paren:close]):
            if field in _DESIGNATED_FIELDS and value.strip():
                found.add(value)
    return found


# A local builder lambda: `auto add_button = [&](std::string id, std::string
# label, std::string section)`. hh_defaults.cpp declares its rows this way
# instead of as initializers, so there is no record to walk -- but the lambda
# NAMES its parameters, which says which argument is the display label far more
# reliably than counting positions would.
_HELPER_LAMBDA_RE = re.compile(r"\bauto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(")
_HELPER_PARAM_NAMES = {"label", "description"}
_BUILDS_ROW_RE = re.compile(r"\bDeviceAction\b|\bDeviceSection\b")


def _helper_label_indices(params: str) -> List[int]:
    """Indices of the parameters named `label` / `description`."""
    indices = []
    for i, param in enumerate(_split_top_level(params)):
        name = re.sub(r"\s*=.*$", "", param).strip().rstrip("&*")
        tail = name.split()[-1] if name.split() else ""
        if tail in _HELPER_PARAM_NAMES:
            indices.append(i)
    return indices


def _extract_helper_calls(content: str) -> Set[str]:
    found: Set[str] = set()
    for m in _HELPER_LAMBDA_RE.finditer(content):
        name = m.group(1)
        paren = m.end() - 1
        close = _find_matching(content, paren)
        if close == -1:
            continue
        indices = _helper_label_indices(content[paren + 1 : close])
        if not indices:
            continue
        # A parameter called `label` is not enough on its own. Validation
        # helpers name their argument the same way -- `require_string("motor
        # state")` reports which FIELD was missing, and translating those
        # yielded keys like "sync state" and "distance". Require the body to
        # actually build a row, which the validators never do.
        body_open = content.find("{", close)
        if body_open == -1:
            continue
        body_close = _find_matching(content, body_open)
        if body_close == -1 or not _BUILDS_ROW_RE.search(content[body_open:body_close]):
            continue
        for call in re.finditer(r"\b" + re.escape(name) + r"\s*\(", content):
            call_open = call.end() - 1
            if call_open <= close:  # the declaration itself
                continue
            call_close = _find_matching(content, call_open)
            if call_close == -1:
                continue
            found |= _positional(content[call_open + 1 : call_close], indices)
    return found


def _extract_phase_templates(content: str) -> Set[str]:
    found: Set[str] = set()
    for m in _PHASE_TEMPLATE_RE.finditer(content):
        brace = content.find("{", m.end())
        if brace == -1:
            continue
        close = _find_matching(content, brace)
        if close == -1:
            continue
        found |= _collect(content, (brace + 1, close), _is_phase_row, _PHASE_FIELDS)
    return found


_TABLE_EXTRACTORS = (
    _extract_widget_defs,
    _extract_helper_calls,
    _extract_device_sections,
    _extract_device_actions,
    _extract_phase_templates,
)


def extract_table_strings(content: str) -> Set[str]:
    """
    Every translatable literal held in a static table in ``content``.

    Returns an empty set for a file with none of these shapes, so it is safe to
    call on every C++ source.
    """
    found: Set[str] = set()
    for extractor in _TABLE_EXTRACTORS:
        found |= extractor(content)
    return {s for s in found if s.strip()}
