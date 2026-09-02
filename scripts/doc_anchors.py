#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Doc citation anchors: resolve `path#segment/segment` to a location.
#
# A citation names a file and, optionally, a path of segments into it. Each
# segment is resolved inside the region the previous segment found, so one
# grammar covers C++ scope nesting, XML nesting, make variables and test names:
#
#   src/printer/printer_state.cpp#update_from_status
#   include/ui_nav_manager.h#PanelRequest/overlay_root
#   src/application/application.cpp#instance/"shutdown_requested"
#
# A quoted segment is literal text; a bare segment is a name the language
# scanner recognises as a definition. Resolution never guesses: a segment that
# matches nothing, or more than one place, is an error naming the candidates.

import re
from dataclasses import dataclass


@dataclass(frozen=True)
class Segment:
    text: str
    is_snippet: bool


@dataclass(frozen=True)
class Citation:
    path: str
    segments: tuple


# A segment is either a double-quoted literal (backslash escapes allowed, so a
# snippet may contain a quote or a slash) or a bare name. Bare names keep `::`
# and `~` so a namespace-qualified or destructor anchor stays one segment.
_SEGMENT_RE = re.compile(r'"((?:[^"\\]|\\.)*)"|([A-Za-z_][\w:~.-]*)')


def parse_citation(text):
    """Parse `path` or `path#seg/seg/...` into a Citation."""
    if "#" not in text:
        return Citation(path=text, segments=())
    path, fragment = text.split("#", 1)
    segments = []
    i = 0
    while i < len(fragment):
        if fragment[i] == "/":
            i += 1
            continue
        m = _SEGMENT_RE.match(fragment, i)
        if not m:
            raise ValueError(f"bad segment in {text!r} at offset {i}")
        if m.group(1) is not None:
            segments.append(Segment(_unescape(m.group(1)), True))
        else:
            segments.append(Segment(m.group(2), False))
        i = m.end()
    if not segments:
        raise ValueError(f"citation {text!r} has a '#' but no segments")
    return Citation(path=path, segments=tuple(segments))


def format_citation(citation):
    """Inverse of parse_citation."""
    if not citation.segments:
        return citation.path
    parts = []
    for seg in citation.segments:
        parts.append(f'"{_escape(seg.text)}"' if seg.is_snippet else seg.text)
    return citation.path + "#" + "/".join(parts)


def _unescape(text):
    return re.sub(r"\\(.)", r"\1", text)


def _escape(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')
