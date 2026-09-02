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


@dataclass(frozen=True)
class Region:
    """A half-open span of 0-based line indices."""

    start: int
    end: int


class NotFound(Exception):
    def __init__(self, segment):
        self.segment = segment
        super().__init__(f"no match for segment {segment.text!r}")


class Ambiguous(Exception):
    def __init__(self, segment, candidates):
        self.segment = segment
        self.candidates = candidates
        lines = ", ".join(str(r.start + 1) for r in candidates)
        super().__init__(f"segment {segment.text!r} matches lines {lines}")


def resolve_segments(lines, segments, ext):
    """Narrow the file to the region named by segments, or raise."""
    region = Region(0, len(lines))
    for segment in segments:
        if segment.is_snippet:
            hits = [
                Region(i, i + 1)
                for i in range(region.start, region.end)
                if segment.text in lines[i]
            ]
        else:
            hits = [r for name, r in definitions(lines, region, ext) if name == segment.text]
        if not hits:
            raise NotFound(segment)
        if len(hits) > 1:
            raise Ambiguous(segment, hits)
        region = hits[0]
    return region


def block_end(lines, start):
    """Exclusive end of the brace block opening at or after `start`.

    A definition with no brace on its line owns that line alone, which is what
    a member declaration or a forward declaration should resolve to.
    """
    depth = 0
    opened = False
    for i in range(start, len(lines)):
        text = _strip_comment(lines[i])
        for ch in text:
            if ch == "{":
                depth += 1
                opened = True
            elif ch == "}":
                depth -= 1
                if opened and depth == 0:
                    return i + 1
        if not opened and ";" in text:
            return i + 1
    return start + 1


def _strip_comment(text):
    return re.sub(r"//.*$", "", text)


_CPP_KEYWORDS = {
    "if", "for", "while", "switch", "return", "else", "do", "catch", "sizeof",
    "try", "case", "default", "using", "typedef", "friend", "delete", "new",
    "throw", "static_cast", "const_cast", "reinterpret_cast", "dynamic_cast",
}

_CPP_SCOPE = re.compile(
    r"^\s*(?:template\s*<[^>]*>\s*)?"
    r"(?:(?:class|struct|union|enum(?:\s+class)?)\s+(?:\w+\s+)?([A-Za-z_]\w*)"
    r"|namespace\s+([A-Za-z_][\w:]*))"
)
_CPP_FUNC = re.compile(
    r"^[\w:<>,&*\s~\[\]]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?\{"
)
_CPP_DECL = re.compile(r"^[\w:<>,&*\s\[\]]*?\b([A-Za-z_]\w*)\s*(?:\([^;]*\))?\s*(?:=[^;]+)?;")
_CPP_DEFINE = re.compile(r"^#define\s+([A-Za-z_]\w*)")


def definitions(lines, region, ext):
    """Every named definition inside `region`, as (name, Region) pairs."""
    return _defs_cpp(lines, region)


def _defs_cpp(lines, region):
    out = []
    for i in range(region.start, region.end):
        text = _strip_comment(lines[i])
        m = _CPP_SCOPE.match(text)
        if m:
            name = m.group(1) or m.group(2)
            if name:
                out.append((name, Region(i, block_end(lines, i))))
                continue
        for rex in (_CPP_DEFINE, _CPP_FUNC, _CPP_DECL):
            m = rex.match(text)
            if m and m.group(1) not in _CPP_KEYWORDS:
                out.append((m.group(1), Region(i, block_end(lines, i))))
                break
    return out
