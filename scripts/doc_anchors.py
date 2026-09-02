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

import argparse
import os
import re
import sys
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

_CPP_LEADING_TOKEN = re.compile(r"^\s*([A-Za-z_]\w*)\b")


def _cpp_leading_keyword_blocks_definition(text):
    """True if the line's first token is a keyword that cannot start a
    definition - shared by every C++ scanner path so `case`/`return`/`friend`/
    etc. are rejected identically everywhere, not by whichever structural
    accident (a missing brace, an unmatched paren) each path happens to hit."""
    m = _CPP_LEADING_TOKEN.match(text)
    return bool(m and m.group(1) in _CPP_STATEMENT_KEYWORDS)


_CPP_SCOPE = re.compile(
    r"^\s*(?:template\s*<[^>]*>\s*)?"
    r"(?:(?:class|struct|union|enum(?:\s+class)?)\s+(?:\w+\s+)?([A-Za-z_]\w*)"
    r"|namespace\s+([A-Za-z_][\w:]*))"
)
# A regex alone cannot tell a function definition from a call that takes a
# lambda: `f(a, [](int x) { ... })` reaches a `{` on the same line without
# ever closing f's own parens, and `x.g().h([](...) {` closes an empty `g()`
# before running into a method call. Both shapes end the line in a brace, so
# the candidate name's own parens have to be walked and balanced by hand, and
# only qualifier tokens are allowed between that closing paren and the brace
# opening its body.
_CPP_FUNC_NAME = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
_CPP_FUNC_PREFIX_OK = re.compile(r"[\w:<>,&*\s~\[\]]*")
_CPP_FUNC_QUALIFIER = re.compile(
    r"(?:\s*(?:const|noexcept|override|final|mutable|&&|&))*"
    r"(?:\s*->\s*[\w:<>,*\s]+)?\s*"
)
# A single colon not part of a `::` scope operator opens a constructor's
# member-initializer list. `case EXPR():` has the same shape, but its leading
# keyword is rejected by _cpp_leading_keyword_blocks_definition before this
# ever runs, so by the time a candidate reaches here the colon can only be an
# initializer list. Qualifiers (e.g. `noexcept`) may still precede it.
_CPP_CTOR_INIT_COLON = re.compile(r"(?<!:):(?!:)")


def _cpp_func_definition(text):
    """Name of the function whose body this line opens, or None."""
    if _cpp_leading_keyword_blocks_definition(text):
        return None
    blank = _blank_literals(text)
    for m in _CPP_FUNC_NAME.finditer(blank):
        start = m.start(1)
        if not _CPP_FUNC_PREFIX_OK.fullmatch(blank[:start]):
            continue
        depth, j, close = 1, m.end(), None
        while j < len(blank):
            if blank[j] == "(":
                depth += 1
            elif blank[j] == ")":
                depth -= 1
                if depth == 0:
                    close = j
                    break
            j += 1
        if close is None:
            continue
        brace = blank.find("{", close + 1)
        if brace == -1:
            continue
        between = blank[close + 1 : brace]
        if _CPP_FUNC_QUALIFIER.fullmatch(between):
            return m.group(1)
        colon = _CPP_CTOR_INIT_COLON.search(between)
        if colon:
            qualifiers, init_list = between[: colon.start()], between[colon.end() :]
            if (
                _CPP_FUNC_QUALIFIER.fullmatch(qualifiers)
                and init_list.count("(") == init_list.count(")")
            ):
                return m.group(1)
    return None


# A declaration needs a type token before the name, separated by whitespace
# or a pointer/reference sigil - otherwise `spdlog::info(...)` (a qualified
# call, no type) and `counter_ = 0;` (an assignment, nothing before the name)
# both read as a name being declared. `_cpp_leading_keyword_blocks_definition`
# rejects a line starting with a keyword outside _CPP_DECL_KEYWORDS the same
# way: `return foo(bar);` and `friend class Foo;` both start with a keyword
# that is not a declaration specifier.
_CPP_DECL = re.compile(
    r"^\s*[A-Za-z_][\w:<>,&*\s\[\]]*[\s*&]([A-Za-z_]\w*)\s*(?:\([^;]*\))?\s*(?:=[^;]+)?;"
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

# GitHub's heading-anchor slug, common subset: lowercase, drop anything that
# is not a word character/space/hyphen, spaces to hyphens. Doc-to-doc
# citations in this repo use this form (it is what actually works as a
# browser fragment); doc_anchors' own anchors use the literal heading text.
# Both need to resolve, since a citation cannot say which convention wrote it.
_SLUG_STRIP_RE = re.compile(r"[^\w\s-]")


def _github_heading_slug(text):
    return _SLUG_STRIP_RE.sub("", text.lower()).replace(" ", "-")


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


def _match_cpp_define(text):
    m = _CPP_DEFINE.match(text)
    return m.group(1) if m else None


def _match_cpp_decl(text):
    if _cpp_leading_keyword_blocks_definition(text):
        return None
    m = _CPP_DECL.match(text)
    return m.group(1) if m else None


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
        for matcher in (_match_cpp_define, _cpp_func_definition, _match_cpp_decl):
            name = matcher(text)
            if name and name not in _CPP_KEYWORDS:
                out.append((name, Region(i, block_end(lines, i))))
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
    """Headings; a heading owns everything up to the next heading of its level.

    Each heading registers under its literal text and, when that differs, its
    GitHub anchor slug - the two conventions a `#fragment` citation into a
    markdown file may use. Skipping the slug when it equals the literal text
    avoids registering an already-lowercase single-word heading twice under
    the same name, which would turn its own resolution ambiguous.
    """
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
        heading = m.group(2)
        span = Region(i, end)
        out.append((heading, span))
        slug = _github_heading_slug(heading)
        if slug != heading:
            out.append((slug, span))
    return out


def _defs_json(lines, region):
    out = []
    for i in range(region.start, region.end):
        m = _JSON_KEY.match(lines[i])
        if m:
            out.append((m.group(1), Region(i, _indented_block_end(lines, i, " " * (len(lines[i]) - len(lines[i].lstrip())) + " "))))
    return out


def _locate(path, repo_root, relative_to):
    """The on-disk file a citation's path names, or None.

    Repo-root-relative wins on a name collision: it is the only candidate
    that names one specific file rather than "whatever happens to sit next
    to the citing document", so it is tried first and the rest are a
    fallback for a path the author wrote relative to their own document
    (doc-dir-relative, then that directory's parent). The parent-directory
    fallback is unconditional: citations get checked from documents both
    inside and outside docs/devel, and gating it on the citing doc's location
    would make resolution depend on an accident of that path for no
    reader-visible benefit.
    """
    candidates = [os.path.join(str(repo_root), path)]
    if relative_to is not None:
        candidates.append(os.path.join(str(relative_to), path))
        candidates.append(os.path.join(os.path.dirname(str(relative_to)), path))
    for cand in candidates:
        if os.path.isfile(cand):
            return cand
    return None


def locate(citation_text, repo_root=".", relative_to=None):
    """(on-disk path, 1-based line number) for a citation, or raise.

    Same resolution rules as `resolve` - `relative_to` is the directory a
    doc-relative path is resolved against, typically the directory of the
    document that wrote the citation. `resolve` is this minus the path: a
    consumer that needs to point AT the resolved file, such as a rendered
    link, needs the path too.
    """
    citation = parse_citation(citation_text)
    full = _locate(citation.path, repo_root, relative_to)
    if full is None:
        raise FileNotFoundError(citation.path)
    with open(full, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")
    ext = os.path.splitext(citation.path)[1]
    region = resolve_segments(lines, citation.segments, ext)
    return full, region.start + 1


def resolve(citation_text, repo_root=".", relative_to=None):
    """1-based line number for a citation, or raise.

    `relative_to` is the directory a doc-relative path (as opposed to a
    repo-root-relative one) is resolved against - typically the directory of
    the document that wrote the citation. Omitted, only repo-root-relative
    paths resolve.
    """
    _, line = locate(citation_text, repo_root=repo_root, relative_to=relative_to)
    return line


# A backticked citation, optionally with a `#fragment`. The one definition of
# what a citation looks like — check_doc_refs.py's PATH_RE matches the same
# shape and is meant to derive from this rather than keep its own copy.
CITE_RE = re.compile(r"`([A-Za-z0-9_./-]+\.[A-Za-z0-9]+(?:#[^`]+)?)`")
# A fence opens on ``` or ~~~ (3 or more of either character) and closes only
# on a marker using the SAME character, at least as long as the one that
# opened it - CommonMark's rule, so a stray `~~~` inside a ``` block does not
# end it early.
_FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")


def iter_citations(paths, problems=None):
    """(doc, doc_line, citation_text) for every citation outside a fence.

    A fenced code block is where a doc shows citation syntax rather than
    making a claim about the tree, so citations inside one are skipped. A
    document this cannot fully read - unopenable, or ending with a fence
    still open - is not silently treated as clean: pass a list as `problems`
    to receive one human-readable line per such document, in addition to
    whatever citations were found before the problem.
    """
    out = []
    for path in paths:
        try:
            fh = open(path, encoding="utf-8", errors="replace")
        except OSError as exc:
            if problems is not None:
                problems.append(f"{path}: cannot open: {exc}")
            continue
        with fh:
            fence_marker = fence_opened_at = None
            for lineno, line in enumerate(fh, start=1):
                m = _FENCE_RE.match(line)
                if m:
                    marker = m.group(1)
                    if fence_marker is None:
                        fence_marker, fence_opened_at = marker, lineno
                    elif marker[0] == fence_marker[0] and len(marker) >= len(fence_marker):
                        fence_marker = None
                    continue
                if fence_marker is not None:
                    continue
                for cm in CITE_RE.finditer(line):
                    out.append((str(path), lineno, cm.group(1)))
            if fence_marker is not None and problems is not None:
                problems.append(f"{path}:{fence_opened_at}: fence never closed")
    return out


def check(paths, repo_root="."):
    """Advisory findings: unresolvable, ambiguous, or malformed citations,
    plus any document a fenced-code problem or a read error kept from being
    fully scanned.

    Must never raise: this is `--check`'s whole implementation, and that mode
    is advisory - it exists to report broken citations, so a citation (or a
    document) too broken to even parse has to become a finding, not a
    traceback that hides every OTHER finding behind it.
    """
    findings = []
    for doc, lineno, text in iter_citations(paths, problems=findings):
        if "#" not in text:
            continue
        try:
            resolve(text, repo_root=repo_root, relative_to=os.path.dirname(doc))
        except FileNotFoundError:
            findings.append(f"{doc}:{lineno}: no such file: {text}")
        except ValueError as exc:
            findings.append(f"{doc}:{lineno}: malformed citation {text!r}: {exc}")
        except (NotFound, Ambiguous) as exc:
            findings.append(f"{doc}:{lineno}: {text}: {exc}")
    return findings


def render(paths, out_dir, repo_root=".", problems=None):
    """Write a copy of each doc with citations expanded to `path:line` links.

    The pinned copy is the one to read in a terminal or publish; its line
    numbers are generated on demand, never committed, so moved code cannot
    make them stale in git. Citation discovery reuses `iter_citations`, so a
    citation inside a fenced example is left as literal text exactly as
    `check` leaves it out of its findings. A document a fenced-code problem
    or a read error kept `iter_citations` from fully scanning is still
    rendered from what it did find - pass a list as `problems` to learn
    which document that happened to and why, instead of it passing silently.

    The pinned tree holds copies of the docs only, not the source they cite,
    so a link points at the cited file where it actually lives in the
    working tree rather than at a path inside the pinned tree that was never
    written. A doc whose own pinned destination would fall outside `out_dir`
    (for instance, one given by a path outside `repo_root`) is skipped
    rather than written somewhere a caller did not ask for; that skip is
    recorded in `problems` too.
    """
    out_dir = str(out_dir)
    out_root = os.path.abspath(out_dir)
    pinned_lines = {}
    for doc, lineno, _ in iter_citations(paths, problems=problems):
        pinned_lines.setdefault(doc, set()).add(lineno)

    written = 0
    for path in paths:
        doc = str(path)
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        dest = os.path.join(out_dir, os.path.relpath(doc, str(repo_root)))
        if os.path.commonpath([out_root, os.path.abspath(dest)]) != out_root:
            if problems is not None:
                problems.append(f"{doc}: pinned path falls outside {out_dir}, skipped")
            continue
        dest_dir = os.path.dirname(dest) or "."
        lines = text.split("\n")
        to_pin = pinned_lines.get(doc, ())
        rendered = [
            _pin_line(line, repo_root, os.path.dirname(doc), dest_dir) if i in to_pin else line
            for i, line in enumerate(lines, start=1)
        ]
        os.makedirs(dest_dir, exist_ok=True)
        with open(dest, "w", encoding="utf-8") as fh:
            fh.write("\n".join(rendered))
        written += 1
    return written


def _pin_line(line, repo_root, relative_to, dest_dir):
    """Expand every citation on a line already known to be outside a fence.

    The link's href is a relpath from `dest_dir` (where the pinned copy of
    this doc lives) to the citation's resolved file in the real working
    tree - not a path inside the pinned tree, which holds no source at all.
    """

    def replace(m):
        text = m.group(1)
        if "#" not in text:
            return m.group(0)
        citation = parse_citation(text)
        try:
            full, lineno = locate(text, repo_root=repo_root, relative_to=relative_to)
        except (NotFound, Ambiguous, FileNotFoundError, ValueError):
            return m.group(0)
        href = os.path.relpath(full, dest_dir)
        return f"[`{citation.path}:{lineno}`]({href}#L{lineno})"

    return CITE_RE.sub(replace, line)


def _default_doc_targets():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import check_doc_refs as refs
    seen, out = set(), []
    for t in list(refs.scan_targets()) + list(refs.scan_devel_targets(["docs/devel"])):
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resolve", metavar="CITATION",
                        help="print path:line for one citation")
    parser.add_argument("--check", nargs="*", metavar="PATH",
                        help="report unresolvable citations (advisory, always exit 0)")
    parser.add_argument("--render", metavar="OUT_DIR",
                        help="render docs with citations expanded to real line numbers")
    args = parser.parse_args(argv)
    if args.resolve:
        citation = parse_citation(args.resolve)
        try:
            line = resolve(args.resolve)
        except FileNotFoundError:
            print(f"{citation.path}: no such file", file=sys.stderr)
            return 1
        except (NotFound, Ambiguous) as exc:
            print(f"{citation.path}: {exc}", file=sys.stderr)
            return 1
        print(f"{citation.path}:{line}")
        return 0
    if args.check is not None:
        # --check is advisory: its whole purpose is surfacing broken
        # citations, so nothing it examines - a malformed citation, an
        # unreadable doc, a bug check() itself does not yet know about - may
        # ever turn into a nonzero exit or an uncaught traceback in place of
        # a finding. The except below is the backstop for the last case.
        paths = args.check or _default_doc_targets()
        try:
            findings = check(paths)
        except Exception as exc:
            findings = [f"internal error while checking: {exc}"]
        for f in findings:
            print(f)
        n = len(findings)
        print("✅ Citation anchors: all resolve" if not n
              else f"⚠️  Citation anchors: {n} finding(s), advisory")
        return 0
    if args.render is not None:
        problems = []
        written = render(_default_doc_targets(), args.render, problems=problems)
        for p in problems:
            print(p)
        print(f"✓ rendered {written} doc(s) into {args.render}")
        return 0
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
