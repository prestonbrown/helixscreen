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
        text = _blank_literals(_strip_comment(lines[i]))
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


def _blank_literals(text):
    """Blank the contents of string/char literals.

    A brace or semicolon inside a literal is not block structure, so counting
    it as one truncates the block at the literal's line.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        if text[i] in ('"', "'"):
            quote = text[i]
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    out[i] = out[i + 1] = " "
                    i += 2
                    continue
                out[i] = " "
                i += 1
        i += 1
    return "".join(out)


_CPP_KEYWORDS = {
    "if", "for", "while", "switch", "return", "else", "do", "catch", "sizeof",
    "try", "case", "default", "using", "typedef", "friend", "delete", "new",
    "throw", "static_cast", "const_cast", "reinterpret_cast", "dynamic_cast",
}

# Every standard C++ reserved keyword, for rejecting a line before a name is
# even captured. A blocklist of "keywords that don't introduce a
# declaration" fails OPEN: every keyword left off it resolves silently to
# the wrong line. Failing CLOSED instead - reject any leading keyword not
# explicitly allowed in _CPP_DECL_KEYWORDS below - means a keyword nobody
# thought of yields NotFound, not a silent wrong citation.
_CPP_ALL_KEYWORDS = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
    "bool", "break", "case", "catch", "char", "char8_t", "char16_t",
    "char32_t", "class", "compl", "concept", "const", "consteval",
    "constexpr", "constinit", "const_cast", "continue", "co_await",
    "co_return", "co_yield", "decltype", "default", "delete", "do",
    "double", "dynamic_cast", "else", "enum", "explicit", "export",
    "extern", "false", "float", "for", "friend", "goto", "if", "inline",
    "int", "long", "mutable", "namespace", "new", "noexcept", "not",
    "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "return", "short",
    "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "true", "try",
    "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
}

# Keywords that CAN legitimately lead a declaration: storage/cv/linkage
# specifiers, builtin type keywords, and the class-like introducers.
# `friend` is deliberately absent - `friend class X;` declares nothing
# named X in this scope - and every statement-leading keyword (`return`,
# `case`, `co_return`, ...) is absent by construction, not by enumeration.
_CPP_DECL_KEYWORDS = {
    "using", "typedef", "static", "const", "constexpr", "consteval",
    "constinit", "inline", "template", "virtual", "explicit", "extern",
    "mutable", "thread_local", "auto", "volatile", "register", "decltype",
    "operator", "struct", "class", "enum", "union", "alignas",
    "void", "bool", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "wchar_t", "char8_t", "char16_t", "char32_t",
}

_CPP_STATEMENT_KEYWORDS = _CPP_ALL_KEYWORDS - _CPP_DECL_KEYWORDS

_CPP_SCOPE = re.compile(
    r"^\s*(?:template\s*<[^>]*>\s*)?"
    r"(?:(?:class|struct|union|enum(?:\s+class)?)\s+(?:\w+\s+)?([A-Za-z_]\w*)"
    r"|namespace\s+([A-Za-z_][\w:]*))"
)
_CPP_FUNC = re.compile(
    r"^[\w:<>,&*\s~\[\]]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?\{"
)
# A declaration needs a type token before the name, separated by whitespace
# or a pointer/reference sigil - otherwise `spdlog::info(...)` (a qualified
# call, no type) and `counter_ = 0;` (an assignment, nothing before the name)
# both read as a name being declared. The leading keyword check rejects any
# line starting with a keyword outside _CPP_DECL_KEYWORDS the same way:
# `return foo(bar);` and `friend class Foo;` both start with a keyword that
# is not a declaration specifier.
_CPP_DECL = re.compile(
    r"^\s*(?!(?:" + "|".join(_CPP_STATEMENT_KEYWORDS) + r")\b)"
    r"[A-Za-z_][\w:<>,&*\s\[\]]*[\s*&]([A-Za-z_]\w*)\s*(?:\([^;]*\))?\s*(?:=[^;]+)?;"
)
_CPP_DEFINE = re.compile(r"^#define\s+([A-Za-z_]\w*)")

_XML_NAME = re.compile(r'<\s*[A-Za-z_][\w-]*[^>]*?\bname\s*=\s*"([^"]+)"')
_MAKE_VAR = re.compile(r"^([A-Za-z_][\w.]*)\s*:?[+?]?=")
_MAKE_TGT = re.compile(r"^([A-Za-z_][\w.%/$()-]*)\s*:(?!=)")
_PY_DEF = re.compile(r"^(\s*)(?:def|class)\s+([A-Za-z_]\w*)")
_SH_FUNC = re.compile(r"^([A-Za-z_]\w*)\s*\(\)\s*\{")
_BATS = re.compile(r'^@test\s+"([^"]+)"')
_MD_HEAD = re.compile(r"^(#{1,6})\s+(.+?)\s*$")
_JSON_KEY = re.compile(r'^\s*"([^"]+)"\s*:')


def definitions(lines, region, ext):
    """Every named definition inside `region`, as (name, Region) pairs."""
    scanner = {
        ".xml": _defs_xml,
        ".mk": _defs_make,
        ".py": _defs_python,
        ".sh": _defs_shell,
        ".bash": _defs_shell,
        ".bats": _defs_shell,
        ".md": _defs_markdown,
        ".json": _defs_json,
    }.get(ext, _defs_cpp)
    return scanner(lines, region)


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


def _defs_xml(lines, region):
    """Widgets keyed by their name= attribute; the region is the element."""
    out = []
    for i in range(region.start, region.end):
        m = _XML_NAME.search(lines[i])
        if m:
            out.append((m.group(1), Region(i, _xml_element_end(lines, i))))
    return out


def _xml_element_end(lines, start):
    if re.search(r"/\s*>", lines[start]):
        return start + 1
    m = re.search(r"<\s*([A-Za-z_][\w-]*)", lines[start])
    if not m:
        return start + 1
    tag = m.group(1)
    depth = 0
    for i in range(start, len(lines)):
        depth += len(re.findall(rf"<\s*{re.escape(tag)}\b(?![^>]*/\s*>)", lines[i]))
        depth -= len(re.findall(rf"</\s*{re.escape(tag)}\s*>", lines[i]))
        if i > start and depth <= 0:
            return i + 1
    return start + 1


def _defs_make(lines, region):
    out = []
    for i in range(region.start, region.end):
        for rex in (_MAKE_VAR, _MAKE_TGT):
            m = rex.match(lines[i])
            if m:
                out.append((m.group(1), Region(i, _indented_block_end(lines, i, "\t"))))
                break
    return out


def _defs_python(lines, region):
    out = []
    for i in range(region.start, region.end):
        m = _PY_DEF.match(lines[i])
        if m:
            indent = m.group(1)
            out.append((m.group(2), Region(i, _indented_block_end(lines, i, indent + " "))))
    return out


def _indented_block_end(lines, start, indent):
    for i in range(start + 1, len(lines)):
        if lines[i].strip() and not lines[i].startswith(indent):
            return i
    return len(lines)


def _defs_shell(lines, region):
    out = []
    for i in range(region.start, region.end):
        m = _SH_FUNC.match(lines[i]) or _BATS.match(lines[i])
        if m:
            out.append((m.group(1), Region(i, block_end(lines, i))))
    return out


def _defs_markdown(lines, region):
    """Headings; a heading owns everything up to the next heading of its level."""
    out = []
    for i in range(region.start, region.end):
        m = _MD_HEAD.match(lines[i])
        if not m:
            continue
        level = len(m.group(1))
        end = region.end
        for j in range(i + 1, region.end):
            n = _MD_HEAD.match(lines[j])
            if n and len(n.group(1)) <= level:
                end = j
                break
        out.append((m.group(2), Region(i, end)))
    return out


def _defs_json(lines, region):
    out = []
    for i in range(region.start, region.end):
        m = _JSON_KEY.match(lines[i])
        if m:
            out.append((m.group(1), Region(i, _indented_block_end(lines, i, " " * (len(lines[i]) - len(lines[i].lstrip())) + " "))))
    return out
