# Doc Citation Anchors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace line-number doc citations with named anchors so code motion never rewrites a doc.

**Architecture:** A new `scripts/doc_anchors.py` parses a citation into a path plus a `/`-separated segment path, resolves each segment inside the region the previous one found, and refuses to guess when a segment matches zero or more than one place. Committed docs carry no line numbers; `make docs-pinned` renders a numbered copy on demand. `scripts/check_doc_refs.py` keeps its path, link, index and staleness checks and loses its three citation checks.

**Tech Stack:** Python 3 (stdlib only), pytest (`tests/python/`), GNU make, bash git hooks.

**Spec:** `docs/devel/plans/2026-09-02-doc-citation-anchors-design.md`

**State at handoff:** Tasks 1-6 are complete on `refactor/doc-citation-anchors`, rebased
onto the trunk, 127 tests. Tasks 7-9 are NOT started. The old pipeline is still live and
untouched, which is deliberate - Task 9 retires it.

**Scope has grown since this plan was written.** The work now lands on BOTH `main` and
`release/1.0`. Convert them close together: the window between one branch converted and
the other is worse than either end state, because anchor-form conflicts with line-form on
every cited line rather than only the drifted ones.

**Numbers re-measured against the trunk** (the plan's original 730/41/261 were taken
before a trunk swap): **742 citations, 42 docs, 268 cited files** on main; 731 sidecar
rows on `release/1.0`, whose corpus differs in content though not much in size.

**Known defect to fix in Task 7's chain builder**, found by measuring rather than by
review: `_innermost_chain` picks a one-line LOCAL VARIABLE declaration over the enclosing
function, because its region is smaller. `auto err = ...` anchors to `err`, and other
`err` declarations in the file make it Ambiguous. The same flaw makes a wrapped
constructor with exactly one member initializer register that member as a spurious
definition (`Foo::Foo(int a)` / `: a_(a) {` yields `['Foo','a_']`). Both are the same
root cause - `_CPP_DECL` matching things that are not declarations - and both belong in
Task 7.

## Global Constraints

- Python 3, standard library only. No new dependencies.
- Every new script gets `# SPDX-License-Identifier: GPL-3.0-or-later`.
- Comments describe the code as it is. No commit SHAs, no "used to", no
  narration of what a review or this migration found. Short issue refs only.
- Tests live in `tests/python/`, use pytest and `tmp_path`, and import the
  script under test via `sys.path.insert(0, str(REPO_ROOT / "scripts"))`.
  Run them with `pytest tests/python/test_doc_anchors.py -v`.
- The resolver never picks a winner. Zero matches raises `NotFound`, two or
  more raises `Ambiguous` carrying the candidates.
- Line numbers are 1-based in citations and user-facing output; `Region`
  offsets are 0-based and half-open internally.
- Nothing this plan writes may put a line number into a committed doc.

---

### Task 1: Citation grammar and parser

**Files:**
- Create: `scripts/doc_anchors.py`
- Test: `tests/python/test_doc_anchors.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `Segment(text: str, is_snippet: bool)`,
  `Citation(path: str, segments: tuple[Segment, ...])`,
  `parse_citation(text: str) -> Citation`,
  `format_citation(c: Citation) -> str`.

- [ ] **Step 1: Write the failing test**

Create `tests/python/test_doc_anchors.py`:

```python
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/doc_anchors.py.

Covers the citation grammar, the region resolver's refuse-to-guess contract,
and the per-language definition scanners.
"""

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from doc_anchors import (  # noqa: E402
    Citation,
    Segment,
    format_citation,
    parse_citation,
)


def test_path_only_citation_has_no_segments():
    c = parse_citation("src/printer/printer_state.cpp")
    assert c.path == "src/printer/printer_state.cpp"
    assert c.segments == ()


def test_single_identifier_segment():
    c = parse_citation("src/printer/printer_state.cpp#update_from_status")
    assert c.path == "src/printer/printer_state.cpp"
    assert c.segments == (Segment("update_from_status", False),)


def test_nested_identifier_segments():
    c = parse_citation("include/ui_nav_manager.h#PanelRequest/overlay_root")
    assert c.segments == (
        Segment("PanelRequest", False),
        Segment("overlay_root", False),
    )


def test_snippet_segment():
    c = parse_citation('src/application/application.cpp#instance/"shutdown_requested"')
    assert c.segments == (
        Segment("instance", False),
        Segment("shutdown_requested", True),
    )


def test_slash_inside_a_snippet_does_not_split_it():
    c = parse_citation('mk/cross.mk#PLATFORM_TARGET/"lib/lvgl"')
    assert c.segments == (
        Segment("PLATFORM_TARGET", False),
        Segment("lib/lvgl", True),
    )


def test_escaped_quote_inside_a_snippet():
    c = parse_citation(r'src/a.cpp#f/"says \"hi\""')
    assert c.segments[1] == Segment('says "hi"', True)


def test_qualified_identifier_segment_keeps_colons():
    c = parse_citation("src/ui/led_widget.cpp#helix::ui/attach")
    assert c.segments[0] == Segment("helix::ui", False)


@pytest.mark.parametrize(
    "text",
    [
        "src/printer/printer_state.cpp",
        "src/printer/printer_state.cpp#update_from_status",
        "include/ui_nav_manager.h#PanelRequest/overlay_root",
        'src/application/application.cpp#instance/"shutdown_requested"',
        r'src/a.cpp#f/"says \"hi\""',
    ],
)
def test_format_round_trips_parse(text):
    assert format_citation(parse_citation(text)) == text


def test_empty_fragment_is_an_error():
    with pytest.raises(ValueError):
        parse_citation("src/a.cpp#")


def test_unterminated_snippet_is_an_error():
    with pytest.raises(ValueError):
        parse_citation('src/a.cpp#f/"never closed')
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/python/test_doc_anchors.py -v`
Expected: FAIL, collection error `ModuleNotFoundError: No module named 'doc_anchors'`

- [ ] **Step 3: Write minimal implementation**

Create `scripts/doc_anchors.py`:

```python
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/python/test_doc_anchors.py -v`
Expected: PASS, 13 passed

- [ ] **Step 5: Commit**

```bash
git add scripts/doc_anchors.py tests/python/test_doc_anchors.py
git commit -m "feat(docs): citation grammar for named anchors

Claude-Session: https://claude.ai/code/session_01BihGmbQmuuasfezWmYSweh"
```

---

### Task 2: Region resolver and the refuse-to-guess contract

**Files:**
- Modify: `scripts/doc_anchors.py`
- Test: `tests/python/test_doc_anchors.py`

**Interfaces:**
- Consumes: `Segment`, `Citation`, `parse_citation` from Task 1.
- Produces: `Region(start: int, end: int)` (0-based, half-open),
  `NotFound(segment)`, `Ambiguous(segment, candidates)`,
  `resolve_segments(lines: list[str], segments, ext: str) -> Region`,
  `definitions(lines, region, ext) -> list[tuple[str, Region]]`.

- [ ] **Step 1: Write the failing test**

Append to `tests/python/test_doc_anchors.py`:

```python
from doc_anchors import (  # noqa: E402
    Ambiguous,
    NotFound,
    Region,
    resolve_segments,
)

CPP = """\
namespace helix {

class PanelRequest {
  public:
    lv_obj_t* overlay_root;
    void reset();
};

void PanelRequest::reset() {
    overlay_root = nullptr;
}

void instance() {
    bool shutdown_requested = false;
    if (shutdown_requested) {
        return;
    }
}

}  // namespace helix
""".split("\n")


def test_identifier_resolves_to_its_definition():
    r = resolve_segments(CPP, parse_citation("a.cpp#instance").segments, ".cpp")
    assert CPP[r.start].strip() == "void instance() {"


def test_nested_segment_resolves_inside_its_parent():
    segs = parse_citation("a.h#PanelRequest/overlay_root").segments
    r = resolve_segments(CPP, segs, ".h")
    assert CPP[r.start].strip() == "lv_obj_t* overlay_root;"


def test_snippet_segment_resolves_inside_its_parent():
    segs = parse_citation('a.cpp#instance/"shutdown_requested = false"').segments
    r = resolve_segments(CPP, segs, ".cpp")
    assert CPP[r.start].strip() == "bool shutdown_requested = false;"


def test_a_snippet_scoped_to_a_parent_ignores_matches_elsewhere():
    # "reset" appears both as a member declaration and as a definition; scoping
    # to the class picks out exactly one without the resolver guessing.
    segs = parse_citation("a.h#PanelRequest/reset").segments
    r = resolve_segments(CPP, segs, ".h")
    assert CPP[r.start].strip() == "void reset();"


def test_zero_matches_raises_not_found():
    with pytest.raises(NotFound):
        resolve_segments(CPP, parse_citation("a.cpp#no_such_thing").segments, ".cpp")


def test_two_matches_raises_ambiguous_with_candidates():
    dupe = ["void f() {", "}", "void f() {", "}"]
    with pytest.raises(Ambiguous) as excinfo:
        resolve_segments(dupe, parse_citation("a.cpp#f").segments, ".cpp")
    assert len(excinfo.value.candidates) == 2


def test_ambiguous_snippet_raises_rather_than_taking_the_first():
    segs = parse_citation('a.cpp#instance/"shutdown_requested"').segments
    with pytest.raises(Ambiguous):
        resolve_segments(CPP, segs, ".cpp")


def test_no_segments_resolves_to_the_whole_file():
    r = resolve_segments(CPP, (), ".cpp")
    assert r == Region(0, len(CPP))
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/python/test_doc_anchors.py -v`
Expected: FAIL, `ImportError: cannot import name 'Ambiguous'`

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/doc_anchors.py`:

```python
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/python/test_doc_anchors.py -v`
Expected: FAIL, `NameError: name 'definitions' is not defined`. That is the
next task; `definitions` is written in Task 3 and these tests go green there.
Confirm the failure is the NameError and not a parser regression:

Run: `pytest tests/python/test_doc_anchors.py -v -k "parse or format"`
Expected: PASS, Task 1's tests still green

- [ ] **Step 5: Commit**

```bash
git add scripts/doc_anchors.py tests/python/test_doc_anchors.py
git commit -m "feat(docs): region resolver that refuses to guess

A segment matching zero places raises NotFound, two or more raises Ambiguous
with the candidate lines. There is no first-match path.

Claude-Session: https://claude.ai/code/session_01BihGmbQmuuasfezWmYSweh"
```

---

### Task 3: Per-language definition scanners

**Files:**
- Modify: `scripts/doc_anchors.py`
- Test: `tests/python/test_doc_anchors.py`

**Interfaces:**
- Consumes: `Region`, `block_end`, `_strip_comment` from Task 2.
- Produces: `definitions(lines, region, ext) -> list[tuple[str, Region]]`,
  dispatching on `ext` to `_defs_cpp`, `_defs_xml`, `_defs_make`,
  `_defs_python`, `_defs_shell`, `_defs_markdown`, `_defs_json`.

- [ ] **Step 1: Write the failing test**

Append to `tests/python/test_doc_anchors.py`:

```python
def _resolve(text, citation, ext):
    lines = text.split("\n")
    r = resolve_segments(lines, parse_citation(citation).segments, ext)
    return lines[r.start].strip()


def test_cpp_class_then_member():
    assert _resolve(
        "\n".join(CPP), "a.h#PanelRequest/overlay_root", ".h"
    ) == "lv_obj_t* overlay_root;"


def test_cpp_namespace_qualifies_a_free_function():
    src = "namespace helix {\nvoid attach() {\n}\n}\n"
    assert _resolve(src, "a.cpp#helix/attach", ".cpp") == "void attach() {"


def test_cpp_define():
    src = "#define HELIX_MAX 4\n"
    assert _resolve(src, "a.h#HELIX_MAX", ".h") == "#define HELIX_MAX 4"


def test_xml_name_attribute():
    src = '<view>\n  <lv_obj name="carousel_host">\n  </lv_obj>\n</view>\n'
    assert _resolve(src, "a.xml#carousel_host", ".xml") == '<lv_obj name="carousel_host">'


def test_xml_nested_names_scope():
    src = (
        '<view>\n'
        '  <lv_obj name="outer">\n'
        '    <lv_label name="inner"/>\n'
        '  </lv_obj>\n'
        '  <lv_label name="inner"/>\n'
        '</view>\n'
    )
    # Two widgets are named "inner"; scoping to "outer" resolves without guessing.
    assert _resolve(src, "a.xml#outer/inner", ".xml") == '<lv_label name="inner"/>'


def test_make_variable():
    src = "TIER_FONT_SRCS := $(FONTS_ALL)\n"
    assert _resolve(src, "fonts.mk#TIER_FONT_SRCS", ".mk") == "TIER_FONT_SRCS := $(FONTS_ALL)"


def test_make_target():
    src = "regen-doc-links: regen-doc-anchors\n\techo hi\n"
    assert _resolve(src, "tools.mk#regen-doc-links", ".mk").startswith("regen-doc-links:")


def test_python_nested_def_scopes():
    src = "class A:\n    def run(self):\n        pass\n\ndef run():\n    pass\n"
    assert _resolve(src, "a.py#A/run", ".py") == "def run(self):"


def test_shell_function():
    src = "qc_doc_refs() {\n  echo hi\n}\n"
    assert _resolve(src, "quality-checks.sh#qc_doc_refs", ".sh") == "qc_doc_refs() {"


def test_bats_test_name():
    src = '@test "temp files use unit helpers" {\n  true\n}\n'
    assert _resolve(
        src, 'a.bats#"temp files use unit helpers"', ".bats"
    ).startswith("@test")


def test_markdown_heading():
    src = "# Top\n\ntext\n\n## Console sink\n\nmore\n"
    assert _resolve(src, "LOGGING.md#Console sink", ".md") == "## Console sink"


def test_json_key():
    src = '{\n  "printers": {\n    "ad5m": 1\n  }\n}\n'
    assert _resolve(src, "db.json#printers", ".json") == '"printers": {'


def test_unknown_extension_falls_back_to_cpp_scanner():
    src = "void f() {\n}\n"
    assert _resolve(src, "a.inc#f", ".inc") == "void f() {"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/python/test_doc_anchors.py -v`
Expected: FAIL, `NameError: name 'definitions' is not defined`

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/doc_anchors.py`:

```python
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/python/test_doc_anchors.py -v`
Expected: PASS, all Task 1-3 tests green

- [ ] **Step 5: Commit**

```bash
git add scripts/doc_anchors.py tests/python/test_doc_anchors.py
git commit -m "feat(docs): per-language definition scanners for citation anchors

Claude-Session: https://claude.ai/code/session_01BihGmbQmuuasfezWmYSweh"
```

---

### Task 4: Resolve a citation against the working tree

**Files:**
- Modify: `scripts/doc_anchors.py`
- Test: `tests/python/test_doc_anchors.py`

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: `resolve(citation_text: str, repo_root=".") -> int` returning a
  1-based line number, raising `NotFound` / `Ambiguous` / `FileNotFoundError`.
  CLI: `doc_anchors.py --resolve '<citation>'` printing `path:line`.

- [ ] **Step 1: Write the failing test**

Append to `tests/python/test_doc_anchors.py`:

```python
import subprocess

from doc_anchors import resolve  # noqa: E402

SCRIPT = REPO_ROOT / "scripts" / "doc_anchors.py"


def test_resolve_returns_a_one_based_line(tmp_path):
    (tmp_path / "a.cpp").write_text("// header\nvoid f() {\n}\n", encoding="utf-8")
    assert resolve("a.cpp#f", repo_root=tmp_path) == 2


def test_resolve_path_only_returns_line_one(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    assert resolve("a.cpp", repo_root=tmp_path) == 1


def test_resolve_missing_file_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        resolve("nope.cpp#f", repo_root=tmp_path)


def test_resolve_cli_prints_path_and_line():
    out = subprocess.run(
        [sys.executable, str(SCRIPT), "--resolve", "scripts/doc_anchors.py#resolve"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    assert out.startswith("scripts/doc_anchors.py:")
    assert int(out.rsplit(":", 1)[1]) > 0


def test_resolve_cli_exits_nonzero_on_ambiguity(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\nvoid f() {\n}\n", encoding="utf-8")
    p = subprocess.run(
        [sys.executable, str(SCRIPT), "--resolve", "a.cpp#f"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
    )
    assert p.returncode != 0
    assert "matches lines" in (p.stdout + p.stderr)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/python/test_doc_anchors.py -v -k resolve_`
Expected: FAIL, `ImportError: cannot import name 'resolve'`

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/doc_anchors.py`:

```python
import argparse
import os
import sys


def resolve(citation_text, repo_root="."):
    """1-based line number for a citation, or raise."""
    citation = parse_citation(citation_text)
    full = os.path.join(str(repo_root), citation.path)
    if not os.path.isfile(full):
        raise FileNotFoundError(citation.path)
    with open(full, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")
    ext = os.path.splitext(citation.path)[1]
    region = resolve_segments(lines, citation.segments, ext)
    return region.start + 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resolve", metavar="CITATION",
                        help="print path:line for one citation")
    args = parser.parse_args(argv)
    if args.resolve:
        citation = parse_citation(args.resolve)
        try:
            line = resolve(args.resolve)
        except (NotFound, Ambiguous, FileNotFoundError) as exc:
            print(f"{citation.path}: {exc}", file=sys.stderr)
            return 1
        print(f"{citation.path}:{line}")
        return 0
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

Move the `import argparse` / `import os` / `import sys` lines up to join the
existing imports at the top of the file rather than leaving them mid-module.

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/python/test_doc_anchors.py -v`
Expected: PASS

Then sanity-check it against real code:

Run: `python3 scripts/doc_anchors.py --resolve 'src/application/application.cpp#instance'`
Expected: a `src/application/application.cpp:<line>` line, exit 0

- [ ] **Step 5: Commit**

```bash
git add scripts/doc_anchors.py tests/python/test_doc_anchors.py
git commit -m "feat(docs): resolve a citation to a line, with a --resolve CLI

Claude-Session: https://claude.ai/code/session_01BihGmbQmuuasfezWmYSweh"
```

---

### Task 5: Check mode over the real docs

**Files:**
- Modify: `scripts/doc_anchors.py`
- Test: `tests/python/test_doc_anchors.py`

**Interfaces:**
- Consumes: `resolve`, `parse_citation`; `check_doc_refs.PATH_RE`,
  `check_doc_refs.scan_targets`, `check_doc_refs.scan_devel_targets`.
- Produces: `iter_citations(paths) -> list[tuple[str, int, str]]` as
  `(doc, doc_line, citation_text)`, and `check(paths) -> list[str]` findings.
  CLI: `doc_anchors.py --check [PATH...]`, exit 0 always (advisory).

- [ ] **Step 1: Write the failing test**

Append to `tests/python/test_doc_anchors.py`:

```python
from doc_anchors import check, iter_citations  # noqa: E402


def test_iter_citations_skips_fenced_code_blocks(tmp_path):
    doc = tmp_path / "d.md"
    doc.write_text(
        "prose `src/a.cpp#f` here\n"
        "```\n"
        "`src/b.cpp#g`\n"
        "```\n"
        "and `src/c.cpp#h`\n",
        encoding="utf-8",
    )
    found = [c for _, _, c in iter_citations([doc])]
    assert found == ["src/a.cpp#f", "src/c.cpp#h"]


def test_check_reports_a_missing_name(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#nope`\n", encoding="utf-8")
    findings = check([doc], repo_root=tmp_path)
    assert len(findings) == 1
    assert "nope" in findings[0]


def test_check_reports_ambiguity(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\nvoid f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#f`\n", encoding="utf-8")
    findings = check([doc], repo_root=tmp_path)
    assert len(findings) == 1
    assert "matches lines" in findings[0]


def test_check_is_silent_when_everything_resolves(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#f`\n", encoding="utf-8")
    assert check([doc], repo_root=tmp_path) == []


def test_check_cli_exits_zero_even_with_findings(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    (tmp_path / "d.md").write_text("see `a.cpp#nope`\n", encoding="utf-8")
    p = subprocess.run(
        [sys.executable, str(SCRIPT), "--check", "d.md"],
        cwd=tmp_path, capture_output=True, text=True,
    )
    assert p.returncode == 0
    assert "nope" in p.stdout
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/python/test_doc_anchors.py -v -k "check or iter_citations"`
Expected: FAIL, `ImportError: cannot import name 'check'`

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/doc_anchors.py`:

```python
# A citation is a backticked path, optionally with a `#fragment`. Fenced code
# blocks are skipped: a fence is where a doc shows citation syntax rather than
# making a claim about the tree.
_CITE_RE = re.compile(r"`([A-Za-z0-9_./-]+\.[A-Za-z0-9]+(?:#[^`]+)?)`")
_FENCE_RE = re.compile(r"^\s*(```|~~~)")


def iter_citations(paths):
    """(doc, doc_line, citation_text) for every citation outside a fence."""
    out = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as fh:
            in_fence = False
            for lineno, line in enumerate(fh, start=1):
                if _FENCE_RE.match(line):
                    in_fence = not in_fence
                    continue
                if in_fence:
                    continue
                for m in _CITE_RE.finditer(line):
                    out.append((str(path), lineno, m.group(1)))
    return out


def check(paths, repo_root="."):
    """Advisory findings: unresolvable or ambiguous citations."""
    findings = []
    for doc, lineno, text in iter_citations(paths):
        if "#" not in text:
            continue
        try:
            resolve(text, repo_root=repo_root)
        except FileNotFoundError:
            findings.append(f"{doc}:{lineno}: no such file: {text}")
        except (NotFound, Ambiguous) as exc:
            findings.append(f"{doc}:{lineno}: {text}: {exc}")
    return findings
```

Extend `main()` with a `--check` branch that defaults to the same doc set
`check_doc_refs.py` scans, prints each finding, and always returns 0:

```python
    parser.add_argument("--check", nargs="*", metavar="PATH",
                        help="report unresolvable citations (advisory, always exit 0)")
    ...
    if args.check is not None:
        paths = args.check or _default_doc_targets()
        findings = check(paths)
        for f in findings:
            print(f)
        n = len(findings)
        print(f"✅ Citation anchors: all resolve" if not n
              else f"⚠️  Citation anchors: {n} finding(s), advisory")
        return 0
```

```python
def _default_doc_targets():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import check_doc_refs as refs
    seen, out = set(), []
    for t in list(refs.scan_targets()) + list(refs.scan_devel_targets(["docs/devel"])):
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/python/test_doc_anchors.py -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add scripts/doc_anchors.py tests/python/test_doc_anchors.py
git commit -m "feat(docs): advisory --check for citation anchors

Claude-Session: https://claude.ai/code/session_01BihGmbQmuuasfezWmYSweh"
```

---

### Task 6: Render mode and `make docs-pinned`

**Files:**
- Modify: `scripts/doc_anchors.py`
- Modify: `mk/tools.mk`
- Modify: `.gitignore`
- Test: `tests/python/test_doc_anchors.py`

**Interfaces:**
- Consumes: `iter_citations`, `resolve`, `parse_citation`.
- Produces: `render(paths, out_dir, repo_root=".") -> int` (files written),
  and `make docs-pinned`.

- [ ] **Step 1: Write the failing test**

Append to `tests/python/test_doc_anchors.py`:

```python
from doc_anchors import render  # noqa: E402


def test_render_expands_citations_to_path_and_line(tmp_path):
    (tmp_path / "a.cpp").write_text("// header\nvoid f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#f` for details\n", encoding="utf-8")
    out = tmp_path / "pinned"
    assert render([doc], out, repo_root=tmp_path) == 1
    text = (out / "d.md").read_text(encoding="utf-8")
    assert "`a.cpp:2`" in text
    assert "#L2" in text


def test_render_leaves_an_unresolvable_citation_alone(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("see `a.cpp#nope`\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    assert "`a.cpp#nope`" in (out / "d.md").read_text(encoding="utf-8")


def test_render_does_not_touch_fenced_examples(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n}\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text("```\n`a.cpp#f`\n```\n", encoding="utf-8")
    out = tmp_path / "pinned"
    render([doc], out, repo_root=tmp_path)
    assert "`a.cpp#f`" in (out / "d.md").read_text(encoding="utf-8")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/python/test_doc_anchors.py -v -k render`
Expected: FAIL, `ImportError: cannot import name 'render'`

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/doc_anchors.py`:

```python
def render(paths, out_dir, repo_root="."):
    """Write a copy of each doc with citations expanded to `path:line` links.

    The pinned copy is the one to read in a terminal or publish; it is
    generated, never committed, so its line numbers cannot go stale in git.
    """
    out_dir = str(out_dir)
    written = 0
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        pinned = []
        in_fence = False
        for line in text.split("\n"):
            if _FENCE_RE.match(line):
                in_fence = not in_fence
                pinned.append(line)
                continue
            pinned.append(line if in_fence else _pin_line(line, repo_root))
        dest = os.path.join(out_dir, os.path.relpath(str(path), str(repo_root)))
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        with open(dest, "w", encoding="utf-8") as fh:
            fh.write("\n".join(pinned))
        written += 1
    return written


def _pin_line(line, repo_root):
    def replace(m):
        text = m.group(1)
        if "#" not in text:
            return m.group(0)
        citation = parse_citation(text)
        try:
            lineno = resolve(text, repo_root=repo_root)
        except (NotFound, Ambiguous, FileNotFoundError, ValueError):
            return m.group(0)
        return f"[`{citation.path}:{lineno}`]({citation.path}#L{lineno})"

    return _CITE_RE.sub(replace, line)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/python/test_doc_anchors.py -v`
Expected: PASS

- [ ] **Step 5: Wire the make target**

Add to `mk/tools.mk`, replacing the `regen-doc-anchors` / `check-doc-anchors`
block:

```make
# ==============================================================================
# Doc citations: named anchors, line numbers rendered on demand
# ==============================================================================
# A committed citation names a place, never a line:
#
#   `src/printer/printer_state.cpp#update_from_status`
#
# Code that moves changes nothing. Code that is RENAMED breaks the anchor, and
# that is the one case where the sentence around the citation needs re-reading,
# so it is the one case that gets reported.
#
# Targets:
#   make docs-pinned       - render docs with real line numbers into build/
#   make check-doc-anchors - advisory report; what pre-push runs

.PHONY: docs-pinned check-doc-anchors

docs-pinned:
	$(ECHO) "$(BLUE)[GEN]$(RESET) rendering pinned docs into build/docs-pinned"
	$(Q)python3 scripts/doc_anchors.py --render build/docs-pinned
	$(ECHO) "$(GREEN)✓ build/docs-pinned - generated, never committed$(RESET)"

check-doc-anchors:
	$(Q)python3 scripts/doc_anchors.py --check
```

Add a `--render OUT_DIR` branch to `main()` mirroring `--check`, defaulting to
`_default_doc_targets()`.

Add to `.gitignore`:

```
build/docs-pinned/
```

- [ ] **Step 6: Verify the target end to end**

Run: `make docs-pinned && head -30 build/docs-pinned/docs/devel/LOGGING.md`
Expected: exit 0, and citations in the output carry `:NNN` and `#LNNN`

Run: `git status --porcelain build/`
Expected: empty, the output is ignored

- [ ] **Step 7: Commit**

```bash
git add scripts/doc_anchors.py tests/python/test_doc_anchors.py mk/tools.mk .gitignore
git commit -m "feat(docs): make docs-pinned renders line numbers on demand

Claude-Session: https://claude.ai/code/session_01BihGmbQmuuasfezWmYSweh"
```

---

### Task 7: Migration converter, report only

**Files:**
- Create: `scripts/migrate_doc_citations.py`
- Test: `tests/python/test_migrate_doc_citations.py`

**Interfaces:**
- Consumes: `doc_anchors.parse_citation`, `doc_anchors.definitions`,
  `doc_anchors.resolve`, `doc_anchors.format_citation`, `doc_anchors.Region`.
- Produces: `propose(doc, lineno, path, line_number, repo_root) -> Proposal`
  where `Proposal(citation: str, confidence: str, note: str)` and confidence is
  one of `"automatic"`, `"needs-snippet"`, `"file-level"`, `"ambiguous"`.
  CLI: `migrate_doc_citations.py --report` (writes nothing).

- [ ] **Step 1: Write the failing test**

Create `tests/python/test_migrate_doc_citations.py`:

```python
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/migrate_doc_citations.py.

The converter turns `path:NNN` into `path#anchor`. It proposes; it never picks
a winner silently, and every proposal carries a confidence a human can filter.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from migrate_doc_citations import propose  # noqa: E402


def test_line_on_a_definition_proposes_that_name(tmp_path):
    (tmp_path / "a.cpp").write_text("// hdr\nvoid f() {\n}\n", encoding="utf-8")
    p = propose("a.cpp", 2, repo_root=tmp_path)
    assert p.citation == "a.cpp#f"
    assert p.confidence == "automatic"


def test_line_inside_a_function_proposes_the_enclosing_name(tmp_path):
    (tmp_path / "a.cpp").write_text(
        "void f() {\n    int x = 1;\n}\n", encoding="utf-8"
    )
    p = propose("a.cpp", 2, repo_root=tmp_path)
    assert p.citation == "a.cpp#f"


def test_a_name_that_would_be_ambiguous_gets_a_snippet(tmp_path):
    (tmp_path / "a.cpp").write_text(
        "void f() {\n    int x = 1;\n}\nvoid f() {\n}\n", encoding="utf-8"
    )
    p = propose("a.cpp", 2, repo_root=tmp_path)
    assert p.confidence in ("needs-snippet", "ambiguous")
    if p.confidence == "needs-snippet":
        assert '"' in p.citation


def test_file_with_no_definitions_degrades_to_file_level(tmp_path):
    (tmp_path / "a.txt").write_text("just\nprose\n", encoding="utf-8")
    p = propose("a.txt", 2, repo_root=tmp_path)
    assert p.citation == "a.txt"
    assert p.confidence == "file-level"


def test_every_proposal_round_trips_through_the_resolver(tmp_path):
    (tmp_path / "a.cpp").write_text("void f() {\n    int x = 1;\n}\n", encoding="utf-8")
    from doc_anchors import resolve

    p = propose("a.cpp", 2, repo_root=tmp_path)
    if p.confidence in ("automatic", "needs-snippet"):
        assert resolve(p.citation, repo_root=tmp_path) > 0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/python/test_migrate_doc_citations.py -v`
Expected: FAIL, `ModuleNotFoundError: No module named 'migrate_doc_citations'`

- [ ] **Step 3: Write minimal implementation**

Create `scripts/migrate_doc_citations.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One-shot converter: `path:NNN` citations become `path#anchor` citations.
#
# For each cited line it finds the innermost named definition containing that
# line and proposes it as an anchor. When the name alone would resolve to more
# than one place it appends a snippet segment taken from the cited line, so the
# proposal is unambiguous by construction. Every proposal is re-resolved before
# it is offered, and anything that still will not resolve is reported rather
# than written.
#
# Usage:
#   migrate_doc_citations.py --report          # writes nothing
#   migrate_doc_citations.py --apply           # rewrites the docs

import argparse
import os
import re
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from doc_anchors import (  # noqa: E402
    Ambiguous,
    Citation,
    NotFound,
    Region,
    Segment,
    definitions,
    format_citation,
    resolve,
)

LINE_CITE_RE = re.compile(r"`([A-Za-z0-9_./-]+\.[A-Za-z0-9]+):(\d+)(?:-\d+)?`")


@dataclass(frozen=True)
class Proposal:
    citation: str
    confidence: str
    note: str = ""


def propose(path, line_number, repo_root="."):
    """Propose an anchor citation for `path:line_number`."""
    full = os.path.join(str(repo_root), path)
    with open(full, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")
    ext = os.path.splitext(path)[1]
    target = line_number - 1

    chain = _innermost_chain(lines, ext, target)
    if not chain:
        return Proposal(path, "file-level", "no named definition contains this line")

    candidate = Citation(path=path, segments=tuple(Segment(n, False) for n in chain))
    text = format_citation(candidate)
    try:
        resolve(text, repo_root=repo_root)
        return Proposal(text, "automatic")
    except Ambiguous:
        pass
    except (NotFound, ValueError):
        return Proposal(path, "file-level", "anchor chain did not resolve")

    snippet = lines[target].strip()
    if not snippet:
        return Proposal(text, "ambiguous", "cited line is blank, cannot disambiguate")
    withsnip = Citation(
        path=path, segments=candidate.segments + (Segment(snippet, True),)
    )
    text2 = format_citation(withsnip)
    try:
        resolve(text2, repo_root=repo_root)
        return Proposal(text2, "needs-snippet", "name alone was ambiguous")
    except (NotFound, Ambiguous, ValueError) as exc:
        return Proposal(text, "ambiguous", str(exc))


def _innermost_chain(lines, ext, target):
    """Names of the nested definitions containing line index `target`."""
    chain = []
    region = Region(0, len(lines))
    while True:
        containing = [
            (name, r)
            for name, r in definitions(lines, region, ext)
            if r.start <= target < r.end and not (r.start == region.start and r.end == region.end)
        ]
        if not containing:
            break
        name, region = min(containing, key=lambda nr: nr[1].end - nr[1].start)
        chain.append(name)
    return chain
```

Add a `main()` with `--report` that walks every doc, matches `LINE_CITE_RE`
outside fences, calls `propose`, and prints one TSV row per citation
(`doc`, `doc_line`, `old`, `new`, `confidence`, `note`) plus a summary count
per confidence.

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/python/test_migrate_doc_citations.py -v`
Expected: PASS

- [ ] **Step 5: Produce the real report and read it**

Run: `python3 scripts/migrate_doc_citations.py --report > /tmp/citation-migration.tsv; tail -8 /tmp/citation-migration.tsv`
Expected: a summary showing roughly 730 rows, most `automatic`

Run: `awk -F'\t' '$5!="automatic"' /tmp/citation-migration.tsv | head -40`
Expected: the rows a human has to look at

**Do not proceed to Task 8 until a human has read the non-automatic rows.**
The spec's own risk section says the resolver mis-picks when a cited comment
sits inside a function, and this report is where that surfaces.

- [ ] **Step 6: Commit**

```bash
git add scripts/migrate_doc_citations.py tests/python/test_migrate_doc_citations.py
git commit -m "feat(docs): citation migration converter, report only

Claude-Session: https://claude.ai/code/session_01BihGmbQmuuasfezWmYSweh"
```

---

### Task 8: Run the migration

**Files:**
- Modify: every doc carrying a `path:NNN` citation (about 41 files)
- Modify: `scripts/migrate_doc_citations.py` (add `--apply`)

**Interfaces:**
- Consumes: `propose` from Task 7.
- Produces: `apply_to(doc, repo_root) -> (changed: int, skipped: list)`. This
  task is the content rewrite, kept in its own commit so 730 citation changes
  are reviewable without new Python underneath.

- [ ] **Step 1: Add `--apply`**

Append to `scripts/migrate_doc_citations.py`:

```python
FENCE_RE = re.compile(r"^\s*(```|~~~)")


def apply_to(doc, repo_root="."):
    """Rewrite one doc's line citations in place. Returns (changed, skipped).

    A proposal the resolver could not confirm is left exactly as it was: a
    citation that does not resolve is worth a human's attention, and silently
    rewriting it into a different broken form would hide it.
    """
    with open(doc, encoding="utf-8") as fh:
        original = fh.read()
    out, skipped, changed = [], [], 0
    in_fence = False
    for lineno, line in enumerate(original.split("\n"), start=1):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            out.append(line)
            continue
        if in_fence:
            out.append(line)
            continue

        def replace(m):
            nonlocal changed
            path, num = m.group(1), int(m.group(2))
            if not os.path.isfile(os.path.join(str(repo_root), path)):
                skipped.append((lineno, m.group(0), "path does not resolve"))
                return m.group(0)
            p = propose(path, num, repo_root=repo_root)
            if p.confidence == "ambiguous":
                skipped.append((lineno, m.group(0), p.note))
                return m.group(0)
            changed += 1
            return f"`{p.citation}`"

        out.append(LINE_CITE_RE.sub(replace, line))
    text = "\n".join(out)
    if text != original:
        with open(doc, "w", encoding="utf-8") as fh:
            fh.write(text)
    return changed, skipped
```

Wire `--apply [PATH...]` into `main()`: default to `_default_doc_targets()`,
call `apply_to` per doc, print a per-doc changed count, then print every
skipped citation and exit 1 if any were skipped, so the migration cannot be
called done while citations remain unconverted.

- [ ] **Step 2: Dry-run the apply on one doc**

Run: `python3 scripts/migrate_doc_citations.py --apply docs/devel/PAGE_SCROLL_BUTTONS.md && git diff --stat docs/devel/PAGE_SCROLL_BUTTONS.md`
Expected: the doc changes, no line numbers remain outside fences

Run: `git diff docs/devel/PAGE_SCROLL_BUTTONS.md | head -40`
Expected: `path:NNN` replaced by `path#name`, prose otherwise untouched

- [ ] **Step 3: Verify the rewritten doc resolves**

Run: `python3 scripts/doc_anchors.py --check docs/devel/PAGE_SCROLL_BUTTONS.md`
Expected: `✅ Citation anchors: all resolve`

- [ ] **Step 4: Apply to everything**

Run: `python3 scripts/migrate_doc_citations.py --apply`
Expected: a summary, plus a list of any citations left alone

- [ ] **Step 5: Verify the whole set**

Run: `python3 scripts/doc_anchors.py --check`
Expected: `✅ Citation anchors: all resolve`, exit 0

Run: `grep -rnE '`[A-Za-z0-9_./-]+\.[a-z]+:[0-9]+`' docs/ --include='*.md' | grep -v '```' | head`
Expected: no output outside fenced examples

Run: `make docs-pinned && grep -c ':[0-9]' build/docs-pinned/docs/devel/ARCHITECTURE.md`
Expected: a non-zero count, proving the pinned render still produces numbers

- [ ] **Step 6: Prove the new resolver agrees with the old anchors**

This is the one check that the migration preserved meaning rather than merely
producing citations that parse. The old sidecar still exists at this point and
holds the line every citation used to name; Task 9 deletes it, so this
comparison is only possible here.

```bash
python3 - <<'EOF'
import subprocess, sys
sys.path.insert(0, 'scripts')
from doc_anchors import resolve, iter_citations, parse_citation

# old sidecar: doc, cited-path, line, resolved-path, line-hash, context-hash
old = {}
for row in open('scripts/doc_cite_anchors.tsv'):
    if row.startswith('#') or not row.strip():
        continue
    doc, cited, line, path, _, _ = row.rstrip('\n').split('\t')
    old.setdefault((doc, path), []).append(int(line))

agree = disagree = unchecked = 0
mismatches = []
for doc, _, text in iter_citations(subprocess.run(
        ['git', 'ls-files', 'docs/', '*.md'], capture_output=True, text=True,
        check=True).stdout.split()):
    if '#' not in text:
        continue
    c = parse_citation(text)
    try:
        line = resolve(text)
    except Exception:
        unchecked += 1
        continue
    known = old.get((doc, c.path))
    if not known:
        unchecked += 1
    elif line in known:
        agree += 1
    else:
        disagree += 1
        mismatches.append((doc, text, line, known))

print(f"agree={agree} disagree={disagree} unchecked={unchecked}")
for m in mismatches[:25]:
    print("  MISMATCH", m)
EOF
```

Expected: `disagree` is 0, or every mismatch is one a human has looked at and
accepted (the spec notes the old anchors themselves included a baselined set
pointing at blank and low-information lines, so a small number of deliberate
improvements is legitimate). Do not proceed to Task 9 with unexplained
mismatches: Task 9 deletes the only evidence.

- [ ] **Step 7: Commit**

```bash
git add docs/ scripts/migrate_doc_citations.py
git commit -m "docs: convert line-number citations to named anchors

Claude-Session: https://claude.ai/code/session_01BihGmbQmuuasfezWmYSweh"
```

---

### Task 9: Retire the old machinery and rewire the gates

**Files:**
- Delete: `scripts/doc_cite_anchors.py`, `scripts/doc_cite_anchors.tsv`,
  `scripts/doc_cite_anchor_baseline.txt`, `scripts/doc_cite_symbol_baseline.txt`,
  `scripts/gen_doc_links.py`
- Modify: `scripts/check_doc_refs.py` (remove `check_line_refs`,
  `check_symbol_cites`, `check_anchors`, `load_cite_baseline`, and their
  `main()` branches)
- Modify: `scripts/quality-checks.sh` (`qc_doc_refs` body, `qc_doc_links`
  removal, `QC_ALL`, `QC_SERIAL`, `qc_trigger_re`, `qc_wanted`)
- Modify: `.githooks/pre-push`
- Modify: `mk/tools.mk` (drop `regen-doc-links`, `check-doc-links`,
  `regen-doc-anchors`), `Makefile` (`.PHONY` list, `help` text)

**Interfaces:**
- Consumes: `doc_anchors.check` from Task 5.
- Produces: nothing new.

- [ ] **Step 1: Confirm what the old gate still passes before removing it**

Run: `python3 scripts/check_doc_refs.py`
Expected: green on all six checks, on the migrated tree

If `Cited line numbers` or `Citation anchors` report findings here, stop:
Task 8 left something behind and the removal would hide it.

- [ ] **Step 2: Strip the three citation checks from `check_doc_refs.py`**

Delete `check_line_refs`, `_resolve_cited`, `_cite_end`, `check_symbol_cites`,
`check_anchors`, `load_cite_baseline`, and the `main()` blocks that call them.
Keep `check_refs`, `check_links`, `check_index`, `check_stale`, and the shared
scanning helpers (`PATH_RE`, `EXEMPT_SUBSTRINGS`, `prune_dirs`, `repo_files`,
`uninitialized_submodules`, `scan_targets`, `scan_devel_targets`,
`unwrap_links`, `trailing_range`) since `doc_anchors.py` imports them.

Update `PATH_RE` to accept a `#fragment` suffix so a citation with an anchor is
still recognised as a path mention:

```python
PATH_RE = re.compile(
    r'`([A-Za-z0-9_./-]+\.(?:' + _EXT + r')'
    r'(?:#[^`]+|:[A-Za-z0-9_]+\(\))?)`')
```

`check_refs` must strip anything after `#` before testing the path.

- [ ] **Step 3: Verify the surviving checks still work**

Run: `python3 scripts/check_doc_refs.py`
Expected: four checks reported (`Doc references`, `Doc links`, `Doc index`),
all green, exit 0

- [ ] **Step 4: Rewire `quality-checks.sh`**

Replace the `qc_doc_refs` body's auto-fix block with a plain pass/fail on
`check_doc_refs.py` (no regen hint, nothing to re-pin). Delete `qc_doc_links`
entirely and remove it from `QC_ALL` and `QC_SERIAL`. In `qc_trigger_re`,
change the `qc_doc_refs` pattern to `\.md$|^scripts/check_doc_refs\.py$` and
delete the `qc_doc_links` case. In `qc_wanted`, delete both `qc_doc_refs`
special cases: the deletion wake-up and the `doc_cite_anchors.tsv`
cited-paths block.

- [ ] **Step 5: Add the advisory anchor check to pre-push**

In `.githooks/pre-push`, after the quality-checks invocation:

```bash
# Advisory: a renamed or deleted symbol breaks a doc anchor. It never blocks a
# push, because a doc pointing at a name that moved is a stale sentence, not a
# broken build.
python3 scripts/doc_anchors.py --check || true
```

- [ ] **Step 6: Drop the retired make targets**

In `mk/tools.mk` delete `regen-doc-links`, `check-doc-links` and
`regen-doc-anchors` and the comment block describing the two-generator
pipeline. In `Makefile`, remove `regen-doc-links check-doc-links
regen-doc-anchors` from the `.PHONY` list and replace the `regen-doc-links`
help line with:

```make
	echo "  $${G}docs-pinned$${X}       - Render docs with line numbers into build/"; \
```

- [ ] **Step 7: Delete the retired scripts and data**

```bash
rm scripts/doc_cite_anchors.py \
   scripts/doc_cite_anchors.tsv \
   scripts/doc_cite_anchor_baseline.txt \
   scripts/doc_cite_symbol_baseline.txt \
   scripts/gen_doc_links.py
```

Use plain `rm`, not `git rm`.

- [ ] **Step 8: Prove nothing still references them**

Run: `grep -rn 'doc_cite_anchors\|doc_cite_anchor_baseline\|doc_cite_symbol_baseline\|gen_doc_links\|regen-doc-links\|check-doc-links\|regen-doc-anchors' --exclude-dir=.git . | grep -v docs/devel/plans/`
Expected: no output

- [ ] **Step 9: Run the full gate sweep**

Run: `./scripts/quality-checks.sh 2>&1 | tail -30`
Expected: exit 0, no doc-citation section failures

Run: `pytest tests/python/test_doc_anchors.py tests/python/test_migrate_doc_citations.py -v`
Expected: PASS

Run: `bash tests/shell/test_code_lint.bats` (if the doc gates are asserted there)
Expected: PASS

- [ ] **Step 10: Prove the churn is actually gone**

This is the acceptance test for the whole plan. Touch a cited line in a way
that moves everything below it, and confirm no doc changes:

```bash
sed -i '20i\
// scratch line' src/printer/printer_state.cpp
python3 scripts/doc_anchors.py --check
git status --porcelain docs/
```

Expected: the check stays green and `git status` reports no doc changes.
Then undo the edit:

```bash
sed -i '20d' src/printer/printer_state.cpp
git diff --stat src/printer/printer_state.cpp
```

Expected: empty diff.

- [ ] **Step 11: Delete the plan and design docs**

Per `docs/CLAUDE.md`, plans are scaffolding and are deleted when the work
ships. Durable knowledge from the design belongs in `mk/tools.mk`'s comment
block and `scripts/doc_anchors.py`'s header, both written in Task 6 and
Task 1.

```bash
rm docs/devel/plans/2026-09-02-doc-citation-anchors.md \
   docs/devel/plans/2026-09-02-doc-citation-anchors-design.md
```

- [ ] **Step 12: Commit**

```bash
git add -u
git add .githooks/pre-push mk/tools.mk Makefile scripts/ tests/
git commit -m "build(docs): retire the citation line-number pipeline

Named anchors replace the content-hash sidecar, so check_doc_refs keeps its
path, link, index and staleness checks and loses its three citation checks.
The anchor check is advisory in pre-push; pre-commit no longer wakes on the
261 cited source files.

Claude-Session: https://claude.ai/code/session_01BihGmbQmuuasfezWmYSweh"
```

---

## Self-review notes

**Spec coverage.** Anchor grammar is Task 1. Refuse-to-guess is Task 2.
Language scanners are Task 3. `--resolve` is Task 4. The advisory gate is
Task 5 and wired in Task 9 Step 5. `make docs-pinned` as a target only is
Task 6. Text-fragment links: see the open item below. Migration report is
Task 7, the rewrite is Task 8, deletions and the `check_doc_refs.py` split
are Task 9.

**One deviation from the spec, deliberate.** The spec calls for `#:~:text=`
fragments in the committed docs. This plan emits `#L` fragments in the
*pinned* render (Task 6) and leaves committed citations as plain code spans.
Adding text-fragment links to the committed docs is a small follow-on: a
`_link_line` pass in `render`-style form applied to the committed files. It is
separated because it is the only piece that rewrites committed docs on a
schedule of its own, and because its value was measured as "free upside, never
worse" rather than required. Decide before starting Task 6 whether to fold it
in; if yes, it is one more step in Task 6 emitting
`[`path#name`](path#:~:text=<urlencoded name>)`.


## Follow-on: the bare `:NNN` continuations

Roughly 435-606 bare `` `:782` `` shorthand citations remain as line numbers. They are
NOT converted, deliberately.

They were never anchored by the old pipeline either - zero of them appear in
`doc_cite_anchors.tsv`, and `check_doc_refs.py` records them as a known gap it declined
to fix because "11 of 12 hand-checked were wrong, so bootstrapping them would freeze
the rot".

The decisive argument is stronger than that hand-check. A maintainer hand-repinning the
full citations on those very lines, twice, reading the prose closely enough to catch that
`AUTOSAVE_MIN_CONFIDENCE` pointed at an alias-matching doc comment, still did not notice
the bare `:375` sitting beside it was 39 lines off. **A bare `:N` carries no name to check
a rewrite against**, so no automated conversion can verify itself - it would turn hundreds
of invisible wrong answers into confident-looking anchors, which inverts the point of the
design.

The fix is prose edits, not a regex. When someone does that pass: **sort by distance from
the antecedent, not top to bottom.** A bare ref immediately adjacent to its antecedent
(`` `path.cpp:100`-137 `` style ranges) is usually right, because it was written and
checked in one breath. The rot concentrates in refs far from their antecedent and in
reading-order lists where the surrounding text moved independently.
