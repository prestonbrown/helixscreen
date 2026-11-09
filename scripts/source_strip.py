#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Blank comments (and optionally literals) out of source text for lint gates.

A gate that pattern-matches source must not fire on a comment that describes
the anti-pattern, on a `//` inside a URL string, or on a name that appears only
in a log message. Every gate that needs that calls one of these:

    from source_strip import blank_cxx, blank_xml_comments

    code = blank_cxx(text)                     # comments + whole literals blanked
    code = blank_cxx(text, strings="body")     # literal bodies blanked, quotes kept
    code = blank_cxx(text, strings="keep")     # comments only; literals verbatim
    code = blank_cxx(text, preprocessor=True)  # also blank #directive lines
    xml = blank_xml_comments(text)             # <!-- ... --> blanked

Both are length-preserving: every removed character becomes a space and every
newline stays, so an offset or line number found in the result indexes the
original text. A gate can slice the original source with offsets from the
blanked copy.

C/C++ lexing rules, shared by every mode:
  - `//` runs to the end of the line; `/* */` to its terminator or EOF.
  - "..." and '...' honour backslash escapes and end at an unescaped newline,
    so an unbalanced quote cannot swallow the rest of the file.
  - A quote inside a numeric literal (`1'000`, `0xFF'FF`) is a digit
    separator, not a char literal.
  - Raw strings `R"d(...)d"` (with an optional u8/u/U/L prefix) end only at
    their own delimiter. In "body" mode a raw string is blanked whole.

`strings` picks what happens to a literal: "blank" (the default) clears it
quotes and all, "body" keeps the quote characters so a gate can still see that
an argument WAS a string literal, "keep" leaves it untouched (the scan only
needs comment boundaries to be right).
"""
from __future__ import annotations

import re

STRING_MODES = ("blank", "body", "keep")

_RAW_OPEN = re.compile(r'(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(')
_IDENT = re.compile(r"[A-Za-z0-9_]")
_XML_COMMENT = re.compile(r"<!--.*?-->", re.S)


def _blank_span(out: list[str], start: int, end: int) -> None:
    for k in range(start, end):
        if out[k] != "\n":
            out[k] = " "


def _is_digit_separator(src: str, i: int) -> bool:
    """True when the quote at i sits inside a numeric literal like 1'000."""
    j = i
    while j > 0 and (src[j - 1].isalnum() or src[j - 1] in "_."):
        j -= 1
    return j < i and src[j].isdigit()


def _raw_open(src: str, i: int):
    """Match a raw-string opener starting at i, or None."""
    if i > 0 and _IDENT.match(src[i - 1]):
        return None
    return _RAW_OPEN.match(src, i)


def blank_cxx(src: str, *, strings: str = "blank", preprocessor: bool = False) -> str:
    """Blank C/C++ comments, and literals per `strings`, preserving offsets.

    `preprocessor=True` also blanks every `#` directive line, continuation
    lines included, for a scan that tracks brace depth and must not count a
    brace inside a macro body.
    """
    if strings not in STRING_MODES:
        raise ValueError(f"strings must be one of {STRING_MODES}, not {strings!r}")
    out = list(src)
    n = len(src)
    i = 0
    line_start = True
    while i < n:
        c = src[i]
        if c == "\n":
            line_start = True
            i += 1
            continue
        if preprocessor and line_start and c == "#":
            # The whole directive, through backslash-continued lines.
            j = i
            while True:
                nl = src.find("\n", j)
                if nl < 0:
                    j = n
                    break
                if src[nl - 1] == "\\" or (src[nl - 1] == "\r" and src[nl - 2 : nl - 1] == "\\"):
                    j = nl + 1
                    continue
                j = nl
                break
            # Blank from the start of the line so leading indentation stays blank too.
            _blank_span(out, i, j)
            i = j
            continue
        if line_start and c in " \t\r\f\v":
            i += 1
            continue
        line_start = False
        if c == "/" and src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            _blank_span(out, i, j)
            i = j
            continue
        if c == "/" and src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            _blank_span(out, i, j)
            i = j
            continue
        if c in "uULR":
            m = _raw_open(src, i)
            if m:
                close = ")" + m.group(1) + '"'
                end = src.find(close, m.end())
                j = n if end < 0 else end + len(close)
                if strings != "keep":
                    _blank_span(out, i, j)
                i = j
                continue
        if c == '"' or (c == "'" and not _is_digit_separator(src, i)):
            j = i + 1
            while j < n:
                ch = src[j]
                if ch == "\\":
                    j += 2
                    continue
                if ch == c or ch == "\n":
                    break
                j += 1
            j = min(j, n)
            closed = j < n and src[j] == c
            if strings == "blank":
                _blank_span(out, i, j + 1 if closed else j)
            elif strings == "body":
                _blank_span(out, i + 1, j)
            i = j + 1 if closed else j
            continue
        i += 1
    blanked = "".join(out)
    assert len(blanked) == n, "blanking must preserve length"
    return blanked


def blank_xml_comments(src: str) -> str:
    """Blank every <!-- ... --> to spaces, keeping newlines and offsets."""
    return _XML_COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), src)


def blank_cxx_lines(lines: list[str], **kwargs) -> list[str]:
    """blank_cxx over a list of lines, returning the same number of lines."""
    return blank_cxx("\n".join(lines), **kwargs).split("\n")
