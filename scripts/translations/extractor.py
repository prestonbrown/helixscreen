#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Extractor module - scans XML and C++ files for translatable text strings.

Extracts text from XML:
- text="..." attributes on any element
- label="...", description="...", title="...", subtitle="..." props
- value_tag="..." on <str> const elements (wizard step titles/subtitles)

Extracts text from C++:
- Strings passed to lv_label_set_text() and similar functions
- Return statements with string literals
- String literals that look like user-facing text

Skips:
- bind_text (dynamic text bound to subjects)
- $variable references
- #icon_* font references
- Empty strings and pure numbers
- Code-like strings (paths, format strings, log messages)
"""

import re
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Set, Dict, List, Tuple, Optional

from .cpp_tables import extract_table_strings

# Attributes that contain translatable text.
# placeholder_tag is the explicit translation key for text-input placeholders
# (mirrors how label/label_tag and text/translation_tag pair up); the textarea
# parser resolves it via lv_tr() at runtime. Non-translatable example/format
# placeholders (IPs, numerics, URLs) are dropped later by the skip patterns.
TEXT_ATTRIBUTES = {"text", "label", "description", "title", "subtitle", "placeholder_tag"}

# Attributes that ARE the translation key rather than a rendered default. They
# are extracted unconditionally -- see the note at the extraction site.
EXPLICIT_TAG_ATTRIBUTES = ("translation_tag", "label_tag")

# Inline element text: <text_muted>Foo</text_muted>. The C parser
# (lib/helix-xml/src/xml/lv_xml.c) applies this as text= + translation_tag=,
# so it is translatable by default. Matches an open tag (capturing its
# attribute blob) followed immediately by a text run. Deliberately does not
# require the matching close tag so text-before-child mixed content is caught.
INLINE_TEXT_RE = re.compile(
    r"<([A-Za-z_][\w-]*)((?:\s+[\w:.-]+=\"[^\"]*\")*)\s*>([^<]+)<"
)

_WS_RUN_RE = re.compile(r"[ \t\r\n]+")

# Attributes whose presence makes the parser DROP inline text (attribute wins).
_INLINE_CONFLICT_RE = re.compile(r'(?<![\w])(?:text|bind_text|translation_tag)="')

# XML comments, matched non-greedily across lines. Comment prose regularly
# contains angle-bracket examples (e.g. "<WidthSensorRole>(index))") that
# INLINE_TEXT_RE would otherwise mistake for a real open-tag + text run.
_XML_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)


def _blank_xml_comments(content: str) -> str:
    """Replace comment bodies with same-length filler (newlines kept) so
    inline-text scanning ignores commented-out markup while match offsets
    and line numbers still line up with the original file content."""
    return _XML_COMMENT_RE.sub(
        lambda m: "".join(c if c == "\n" else " " for c in m.group(0)), content
    )


def collapse_whitespace(text: str) -> str:
    """Trim + collapse whitespace runs to single spaces.

    MUST stay byte-identical to collapse_whitespace() in
    lib/helix-xml/src/xml/lv_xml.c — the collapsed string is the translation
    key on both sides.
    """
    return _WS_RUN_RE.sub(" ", text).strip(" ")

# Patterns to skip
VARIABLE_PATTERN = re.compile(r"\$\w+")  # $variable
# XML constant reference: `#name` resolves against a <string>/<px>/<color> const
# in globals.xml, so the literal is a lookup key and never user-facing text. The
# icon fonts (`#icon_*`) are the bulk of these, but any const reaching a
# translatable attribute matches — e.g. placeholder_text="#hex_placeholder".
# Lowercase-only, so a real `#RRGGBB` hex color is left to HEX_COLOR_PATTERN.
CONST_REF_PATTERN = re.compile(r"^#[a-z_][a-z0-9_]*$")
NUMERIC_PATTERN = re.compile(r"^[\d.]+%?$")  # 123 or 100%
# XML numeric character references: &#xF0026; or &#983078;
XML_NUMERIC_ENTITY_PATTERN = re.compile(r"&#x([0-9A-Fa-f]+);|&#(\d+);")
FONT_NAME_PATTERN = re.compile(r"^(mdi_icons_|noto_sans_)\w+$")  # Font names
HEX_COLOR_PATTERN = re.compile(r"^#[0-9A-Fa-f]{6}$")  # #RRGGBB hex colors
SIZE_ATTR_PATTERN = re.compile(r'^size=')  # XML size attribute values
XML_ATTR_VALUE_PATTERN = re.compile(r'(?:value|height)\s*=')  # Test/debug attribute strings
SIGNED_NUMERIC_PATTERN = re.compile(r'^[+-]\.?\d*\.?\d*$')  # +.005, -1, +0, -.1
PAREN_TECH_PATTERN = re.compile(r'^\(.{0,8}\)$')  # Short parenthesized tech values
CARET_DIRECTION_PATTERN = re.compile(r'^\^')  # Direction labels like ^ FRONT
SNAKE_CASE_PATTERN = re.compile(r'^[a-z][a-z0-9]*(_[a-z0-9]+)+$')  # snake_case identifiers
URL_PATTERN = re.compile(r'https?://')  # URLs
MATERIAL_TEMP_PATTERN = re.compile(r'^[A-Z]+ \d+$')  # Material presets like "PLA 205", "ABS 100"
# Temperature values: "60°C", "200°C", "210°C / 60°C", "200-230°C"
TEMP_VALUE_PATTERN = re.compile(r'^\d[\d\-–]*°C(\s*/\s*\d+°C)?$')
# Pure measurement values: "10mm", "5mm", "850g" (units handled by formatters)
MEASUREMENT_PATTERN = re.compile(r'^\d+(\.\d+)?\s*(mm|cm|g|kg|ml|l|s|ms)$')
# Numeric data placeholders: " 0 / 0", "0 / 0"
NUMERIC_PLACEHOLDER_PATTERN = re.compile(r'^\s*\d+\s*/\s*\d+\s*$')

# Short tokens and non-translatable exact strings (format/layout tokens).
NON_TRANSLATABLE = {"true", "false", "xl", "lg", "md", "sm", "xs", "#RRGGBB"}

# Source comments that suppress extraction of the string literal(s) they apply
# to. Two placement conventions are honored (both occur in the codebase): a
# trailing comment on the same line as the literal, and a standalone comment on
# the line immediately above it. Only the literal's own line (or the line
# directly below a standalone marker) is affected — a marker never suppresses
# beyond that.
#
# Two marker kinds with a deliberate difference on lv_tr() lines:
#   `// i18n: do not translate` — the string is not user-facing (identifiers,
#      product names). Matches any i18n comment containing the phrase (e.g.
#      "i18n: do not translate - product name"). Does NOT override an explicit
#      lv_tr(): on an lv_tr line it documents a non-translatable *substring* of a
#      translatable sentence (e.g. a product name inside it), so the sentence is
#      still extracted.
#   `// i18n: universal` — the whole string renders identically in every locale,
#      so it should not be a translation key at all. This DOES apply on lv_tr()
#      lines: runtime falls back to the tag (the English/universal text) for a
#      key absent from every pack, so dropping it is safe.
I18N_DO_NOT_TRANSLATE_RE = re.compile(
    r"//[^\n]*\bi18n:[^\n]*do\s+not\s+translate", re.IGNORECASE
)
I18N_UNIVERSAL_RE = re.compile(r"//[^\n]*\bi18n:\s*universal", re.IGNORECASE)

# File-level opt-out for XML files: an `i18n: skip-file` marker anywhere in an
# XML comment excludes the whole file from extraction. Used by dev/test-only
# panels (test_panel.xml, gcode_test_panel.xml, step_test_panel.xml) whose demo
# strings are never shown to end users and shouldn't pollute the locale packs.
I18N_SKIP_FILE_RE = re.compile(r"<!--[^>]*\bi18n:\s*skip-file", re.IGNORECASE)

# Language names displayed in their native script (never translated)
LANGUAGE_NAMES = {
    "Deutsch", "English", "Español", "Français", "Italiano", "Português",
    "Русский", "中文", "日本語", "한국어", "العربية", "हिन्दी", "Türkçe",
    "Nederlands", "Polski", "Svenska", "Norsk", "Dansk", "Suomi",
    "Čeština", "Magyar", "Română", "Українська", "Ελληνικά",
}

# C++ patterns that indicate translatable text.
# Patterns that may span adjacent string literals (C++ concatenates "a" "b" → "ab")
# capture the full run via ADJACENT_LITERALS_GROUP and are post-processed by
# _join_adjacent_literals().
ADJACENT_LITERALS_GROUP = r'((?:"(?:[^"\\]|\\.)*"\s*)+)'
# An lv_tr() argument, capturing its whole adjacent-literal run. Exported so
# the translation gates can enumerate the real runtime keys rather than
# re-deriving the pattern.
LV_TR_RUN_RE = re.compile(r"lv_tr\s*\(\s*" + ADJACENT_LITERALS_GROUP)
CPP_TRANSLATABLE_PATTERNS = [
    # lv_tr("text") - explicitly marked for translation (handles escaped quotes
    # and adjacent literal concatenation across multiple lines)
    r"lv_tr\s*\(\s*" + ADJACENT_LITERALS_GROUP,
    # lv_label_set_text(label, "text")
    r"lv_label_set_text\s*\([^,]+,\s*" + ADJACENT_LITERALS_GROUP,
    # return "Status Text"  (for status strings) — single literal only
    r'return\s+"([A-Z][a-z][^"]{2,30})"',
]

# Matches a single "..." C string literal (with escape handling)
_STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def _join_adjacent_literals(captured: str) -> str:
    """Join adjacent C++ string literals into a single string.

    C++ concatenates "foo" "bar" at compile time. The extractor captures the
    whole run; this pulls each quoted piece out and joins them.
    """
    pieces = _STRING_LITERAL_RE.findall(captured)
    return "".join(pieces)


# `\x` consumes hex digits greedily (C99 6.4.4.4), which is exactly why real
# call sites split the literal: "Heating to %d\xC2\xB0" "C" stops the escape at
# the closing quote instead of reading the C as a third hex digit.
_HEX_ESCAPE_RE = re.compile(r"\\x([0-9a-fA-F]+)")
_OCTAL_ESCAPE_RE = re.compile(r"\\([0-7]{1,3})")
# Universal character names: \uXXXX and \UXXXXXXXX, encoded by the compiler as
# UTF-8. ui_panel_belt_tension.cpp writes its em dash as —.
_UCN_ESCAPE_RE = re.compile(r"\\u([0-9a-fA-F]{4})|\\U([0-9a-fA-F]{8})")

# Single-character C escapes, mapped to the byte the compiler emits.
_SIMPLE_ESCAPES = {
    "n": b"\n",
    "t": b"\t",
    "r": b"\r",
    "a": b"\a",
    "b": b"\b",
    "f": b"\f",
    "v": b"\v",
    "e": b"\x1b",  # GNU extension
    "\\": b"\\",
    '"': b'"',
    "'": b"'",
    "?": b"?",
}


def _resolve_literal_bytes(body: str):
    """Resolve one C string-literal body to the bytes the compiler emits.

    Returns None if the literal contains a malformed escape, so callers can
    leave the input untouched rather than guess at corrupt input.
    """
    out = bytearray()
    i = 0
    n = len(body)
    while i < n:
        ch = body[i]
        if ch != "\\":
            out += ch.encode("utf-8")
            i += 1
            continue
        if i + 1 >= n:
            return None  # trailing lone backslash - not valid C
        nxt = body[i + 1]
        if nxt == "x":
            m = _HEX_ESCAPE_RE.match(body, i)
            if m is None:
                return None  # `\x` with no hex digits
            value = int(m.group(1), 16)
            if value > 0xFF:
                return None  # hex escape out of range for a byte
            out.append(value)
            i = m.end()
            continue
        if nxt in "01234567":
            m = _OCTAL_ESCAPE_RE.match(body, i)
            value = int(m.group(1), 8)
            if value > 0xFF:
                return None
            out.append(value)
            i = m.end()
            continue
        if nxt in "uU":
            m = _UCN_ESCAPE_RE.match(body, i)
            if m is None:
                return None
            out += chr(int(m.group(1) or m.group(2), 16)).encode("utf-8")
            i = m.end()
            continue
        if nxt in _SIMPLE_ESCAPES:
            out += _SIMPLE_ESCAPES[nxt]
            i += 2
            continue
        return None  # unknown escape
    return bytes(out)


def decode_c_escapes(literal: str) -> str:
    """Resolve the escapes in one C source literal the way the compiler does.

    The runtime translation key is whatever the compiler emits, so a source
    literal "%d\\xc2\\xb0" must extract as "%d°": the two escapes are raw BYTES
    that together form one UTF-8 character, not two Latin-1 code points (which
    is what codecs' "unicode_escape" would give).

    All C escapes are resolved, including `\\n`. A key holding a real newline
    still survives the runtime pack because generate_translations.py emits it as
    the numeric character reference `&#10;` - a literal newline in an XML
    attribute value would be normalized to a space (XML 1.0 s3.3.3), but a
    character reference is appended to the normalized value as-is.

    A malformed escape, or a byte run that is not valid UTF-8 (a truncated
    multi-byte sequence), leaves the literal untouched rather than substituting
    a replacement character, so the corruption stays visible to the translation
    gates instead of shipping as a mangled key.
    """
    if "\\" not in literal:
        return literal
    raw = _resolve_literal_bytes(literal)
    if raw is None:
        return literal
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return literal


def resolve_cpp_literal_run(captured: str) -> str:
    """Resolve a run of adjacent C++ string literals to its runtime value.

    C++ resolves escapes per literal TOKEN and concatenates the resulting BYTE
    sequences, so both steps have to happen in that order: resolving after the
    join would let a `\\x` escape run past the quote that was written to stop
    it, and decoding each token to text separately would break a UTF-8
    character deliberately split across two literals.
    """
    pieces = _STRING_LITERAL_RE.findall(captured)
    out = bytearray()
    for piece in pieces:
        raw = _resolve_literal_bytes(piece)
        if raw is None:
            return _join_adjacent_literals(captured)
        out += raw
    try:
        return bytes(out).decode("utf-8")
    except UnicodeDecodeError:
        return _join_adjacent_literals(captured)


def _marker_applies(content: str, literal_pos: int, marker_re) -> bool:
    """
    Return True if a suppression marker matching ``marker_re`` applies to the
    string literal at ``literal_pos``.

    Trailing form: the marker is a comment on the same line as the literal.
    Preceding form: the marker is a standalone comment (line starts with `//`)
    on the line immediately above. Only these two adjacencies count, so a marker
    never suppresses a literal further down the file.
    """
    line_start = content.rfind("\n", 0, literal_pos) + 1
    line_end = content.find("\n", literal_pos)
    if line_end == -1:
        line_end = len(content)
    this_line = content[line_start:line_end]
    if marker_re.search(this_line):
        return True

    # Preceding standalone marker on the immediately-previous line.
    if line_start >= 2:
        prev_end = line_start - 1
        prev_start = content.rfind("\n", 0, prev_end) + 1
        prev_line = content[prev_start:prev_end]
        if prev_line.lstrip().startswith("//") and marker_re.search(prev_line):
            return True

    return False

# C++ patterns to skip (not user-facing)
CPP_SKIP_PATTERNS = [
    r"spdlog::",  # Logging
    r"LOG_",  # Logging macros
    r"fmt::",  # Format strings
    r"\.c_str\(\)",  # Variable strings
    r"\{\}",  # Format placeholders
    r"\\x[0-9a-fA-F]",  # Hex escapes (icons)
    r"^[a-z_]+$",  # snake_case identifiers
    r"^/",  # Paths
    r"\.(cpp|h|xml|json|yml|py)$",  # File extensions
    r"^\[",  # Log prefixes like [Application]
    r"^https?://",  # URLs
    r"^\d",  # Starts with digit
    r"^%",  # Format strings
]


def _decode_xml_entities(text: str) -> str:
    """Decode XML entities including numeric character references.

    Handles named entities (&amp; etc.) and numeric references
    (&#xF0026; hex, &#983078; decimal) used for icon codepoints.
    """
    # Decode named entities
    text = text.replace("&amp;", "&")
    text = text.replace("&lt;", "<")
    text = text.replace("&gt;", ">")
    text = text.replace("&quot;", '"')
    text = text.replace("&apos;", "'")

    # Decode numeric character references (&#xHEX; and &#DECIMAL;)
    def _replace_numeric_entity(m):
        if m.group(1):  # hex: &#xNNNN;
            return chr(int(m.group(1), 16))
        else:  # decimal: &#NNNN;
            return chr(int(m.group(2)))

    text = XML_NUMERIC_ENTITY_PATTERN.sub(_replace_numeric_entity, text)
    return text


def should_skip_text(text: str) -> bool:
    """Determine if text should be skipped (not translatable)."""
    if not text or not text.strip():
        return True

    # Skip variable references
    if "$" in text:
        return True

    # Skip subject references (e.g., @spoolman_edit_save_text)
    if text.startswith("@"):
        return True

    # Skip XML constant references (icon fonts, string consts)
    if CONST_REF_PATTERN.match(text):
        return True

    # Skip pure numeric values
    if NUMERIC_PATTERN.match(text.strip()):
        return True

    # Skip icon codepoints (Unicode Private Use Area ranges)
    # BMP PUA: U+E000–U+F8FF, Supplementary PUA-A: U+F0000–U+FFFFD,
    # Supplementary PUA-B: U+100000–U+10FFFD
    if text and all(
        (0xE000 <= ord(c) <= 0xF8FF)
        or (0xF0000 <= ord(c) <= 0xFFFFD)
        or (0x100000 <= ord(c) <= 0x10FFFD)
        for c in text
    ):
        return True

    # Skip font names, hex colors, size attributes
    if FONT_NAME_PATTERN.match(text):
        return True
    if HEX_COLOR_PATTERN.match(text):
        return True
    if SIZE_ATTR_PATTERN.match(text):
        return True

    # Skip known non-translatable tokens
    if text in NON_TRANSLATABLE:
        return True

    # Skip test/debug strings containing value= or height= patterns
    if XML_ATTR_VALUE_PATTERN.search(text):
        return True

    # Skip punctuation-only strings that are 3 chars or fewer
    stripped = text.strip()
    if len(stripped) <= 3 and not any(c.isalpha() or c.isdigit() for c in stripped):
        return True

    # Skip signed numeric step values like +.005, -1, +0
    if SIGNED_NUMERIC_PATTERN.match(stripped) and stripped not in ('', '+', '-'):
        return True

    # Skip language names (always displayed in native script)
    if text in LANGUAGE_NAMES:
        return True

    # Skip short parenthesized technical values like (0), (2.4GHz)
    if PAREN_TECH_PATTERN.match(stripped):
        return True

    # Skip direction labels with caret like ^ FRONT
    if CARET_DIRECTION_PATTERN.match(stripped):
        return True

    # Skip strings containing literal \n (multi-line dropdown option labels)
    if r"\n" in text:
        return True

    # Skip strings containing actual newlines (multi-line code blocks from &#10;)
    if "\n" in text:
        return True

    # Skip snake_case identifiers (subject names like update_version_text)
    if SNAKE_CASE_PATTERN.match(stripped):
        return True

    # Skip strings containing URLs (shell commands, links)
    if URL_PATTERN.search(text):
        return True

    # Skip material+temperature presets like "PLA 205", "ABS 100"
    if MATERIAL_TEMP_PATTERN.match(stripped):
        return True

    # Skip temperature values like "60°C", "200-230°C", "210°C / 60°C"
    if TEMP_VALUE_PATTERN.match(stripped):
        return True

    # Skip pure measurement values like "10mm", "850g" (unit formatting is locale-specific
    # but should be handled by formatter utilities, not in translation strings)
    if MEASUREMENT_PATTERN.match(stripped):
        return True

    # Skip numeric data placeholders like " 0 / 0"
    if NUMERIC_PLACEHOLDER_PATTERN.match(stripped):
        return True

    return False


def _iter_inline_texts(content: str):
    """Yield (collapsed_text, match_start) for translatable inline text runs.

    Positions are relative to the original ``content`` (comments are blanked,
    not removed, so offsets and line numbers stay valid for the caller)."""
    scanned = _blank_xml_comments(content)
    for match in INLINE_TEXT_RE.finditer(scanned):
        attr_blob = match.group(2) or ""
        if _INLINE_CONFLICT_RE.search(attr_blob):
            continue
        text = collapse_whitespace(_decode_xml_entities(match.group(3)))
        if not text or text.startswith("$") or text.startswith("#"):
            continue
        if should_skip_text(text):
            continue
        yield text, match.start(3)


def should_skip_cpp_text(text: str) -> bool:
    """Determine if C++ text should be skipped (not user-facing)."""
    if should_skip_text(text):
        return True

    # Check against skip patterns
    for pattern in CPP_SKIP_PATTERNS:
        if re.search(pattern, text):
            return True

    # Skip very short strings (likely not user-facing)
    if len(text) < 2:
        return True

    # Skip strings that are all uppercase (likely constants/enums)
    if text.isupper() and "_" not in text:
        return True

    return False


def extract_strings_from_cpp(cpp_path: Path) -> Set[str]:
    """
    Extract translatable strings from a C++ source file.

    Looks for strings in lv_label_set_text() calls and return statements.

    Args:
        cpp_path: Path to the C++ file

    Returns:
        Set of unique translatable strings
    """
    result = set()

    try:
        with open(cpp_path, "r", encoding="utf-8") as f:
            content = f.read()
    except IOError as e:
        print(f"Warning: Failed to read {cpp_path}: {e}")
        return result

    for pattern in CPP_TRANSLATABLE_PATTERNS:
        is_lv_tr = "lv_tr" in pattern
        is_adjacent = ADJACENT_LITERALS_GROUP in pattern
        for match in re.finditer(pattern, content):
            # The key must equal what the compiler produces, not the source form.
            if is_adjacent:
                text = resolve_cpp_literal_run(match.group(1))
            else:
                text = decode_c_escapes(match.group(1))

            # lv_tr() strings are explicitly marked - always include them, EXCEPT
            # when a `// i18n: universal` marker applies (the string renders the
            # same in every locale, so it should not be a key). A `// i18n: do
            # not translate` marker does NOT override lv_tr(): the explicit call
            # wins, so on an lv_tr line it documents a non-translatable substring
            # (e.g. a product name inside a sentence) rather than the whole string.
            if is_lv_tr:
                if _marker_applies(content, match.start(1), I18N_UNIVERSAL_RE):
                    continue
                if text and text.strip():
                    result.add(text)
                continue

            # Honor a suppression marker on the literal's line (trailing) or the
            # line directly above it (standalone/preceding): either kind applies
            # to a non-lv_tr literal.
            if _marker_applies(content, match.start(1), I18N_DO_NOT_TRANSLATE_RE) or _marker_applies(
                content, match.start(1), I18N_UNIVERSAL_RE
            ):
                continue

            # Get surrounding context to check for skip patterns
            start = max(0, match.start() - 50)
            end = min(len(content), match.end() + 50)
            context = content[start:end]

            # Skip if context indicates non-translatable
            skip = False
            for skip_pattern in CPP_SKIP_PATTERNS[:4]:  # Check first few patterns on context
                if re.search(skip_pattern, context):
                    skip = True
                    break

            if skip:
                continue

            if not should_skip_cpp_text(text):
                result.add(text)

    # Static tables whose entries the UI translates through a variable
    # (lv_tr(def.display_name) and friends), which the call-site patterns above
    # cannot see. Suppression markers still apply, so a table row can opt out
    # with a trailing `// i18n: do not translate`.
    for text in extract_table_strings(content):
        for match in _STRING_LITERAL_RE.finditer(content):
            if match.group(1) != text:
                continue
            if _marker_applies(content, match.start(1), I18N_DO_NOT_TRANSLATE_RE) or _marker_applies(
                content, match.start(1), I18N_UNIVERSAL_RE
            ):
                break
        else:
            result.add(decode_c_escapes(text))

    return result


def extract_strings_from_cpp_directory(
    directory: Path, recursive: bool = True
) -> Set[str]:
    """
    Extract translatable strings from all C++ files in a directory.

    Args:
        directory: Directory to scan
        recursive: Whether to scan subdirectories

    Returns:
        Set of unique translatable strings
    """
    result = set()

    patterns = ["*.cpp", "*.h"]
    for pattern in patterns:
        if recursive:
            cpp_files = directory.rglob(pattern)
        else:
            cpp_files = directory.glob(pattern)

        for cpp_path in cpp_files:
            # Skip generated files
            if "generated" in str(cpp_path):
                continue
            strings = extract_strings_from_cpp(cpp_path)
            result.update(strings)

    return result


def extract_strings_from_xml(xml_path: Path) -> Set[str]:
    """
    Extract all translatable strings from an XML file.

    Uses regex-based extraction to handle LVGL's non-standard XML syntax
    (e.g., style_foo:state attributes with colons).

    Args:
        xml_path: Path to the XML file

    Returns:
        Set of unique translatable strings
    """
    result = set()

    try:
        with open(xml_path, "r", encoding="utf-8") as f:
            content = f.read()
    except IOError as e:
        print(f"Warning: Failed to read {xml_path}: {e}")
        return result

    # File-level opt-out: dev/test panels carry an `i18n: skip-file` marker.
    if I18N_SKIP_FILE_RE.search(content):
        return result

    # Check for bind_text on a per-element basis using regex
    # Elements with bind_text should not have their text extracted
    # Pattern: <tag ... bind_text="..." ... text="value" ...> or reverse order
    # We'll extract all text attributes, then filter out those on bind_text elements

    # First, find all elements with bind_text (these should skip text extraction)
    # This is a simplification - we extract text from the whole file and skip bind_text elements

    for attr in TEXT_ATTRIBUTES:
        # Match attr="value" including compound forms like primary_text="value"
        # but NOT bind_attr="value" (bind_text, bind_description, etc.)
        pattern = rf'{attr}="([^"]*)"'
        for match in re.finditer(pattern, content):
            # Check if this is a bind_ variant (not translatable)
            prefix_start = max(0, match.start() - 5)
            prefix = content[prefix_start:match.start()]
            if prefix.endswith("bind_"):
                continue

            text = match.group(1)

            # Get the line containing this match to check for bind_text
            line_start = content.rfind("\n", 0, match.start()) + 1
            line_end = content.find("\n", match.end())
            if line_end == -1:
                line_end = len(content)
            line = content[line_start:line_end]

            # Skip if this element has bind_text (overrides static text)
            if "bind_text=" in line:
                continue

            # Decode XML entities (named + numeric character references)
            text = _decode_xml_entities(text)

            if not should_skip_text(text):
                result.add(text)

    # Explicit translate-me markers. Unlike `text=`, these are NOT suppressed by
    # a sibling `bind_text=`: the tag is precisely what the widget re-resolves
    # through lv_label_set_translation_tag() when the language changes, so an
    # element that binds its live value and names its tag is asking for the tag
    # to be translated. The `text=` beside it is only a design-time placeholder.
    #
    # Without this, `<x bind_text="s" text="Processing..." translation_tag="Processing..."/>`
    # lost both halves to the bind_text skip below, and the string stayed
    # untranslated in all nine locales while looking marked-up in the XML.
    for attr in EXPLICIT_TAG_ATTRIBUTES:
        for match in re.finditer(rf'(?<![\w]){attr}="([^"]*)"', content):
            text = match.group(1)
            if not text or text.startswith(("$", "#")):
                continue
            text = _decode_xml_entities(text)
            if not should_skip_text(text):
                result.add(text)

    # Extract value_tag from <str> const elements (wizard step titles/subtitles)
    for match in re.finditer(r'value_tag="([^"]*)"', content):
        text = match.group(1)
        if not text or text.startswith("$"):
            continue
        text = _decode_xml_entities(text)
        if not should_skip_text(text):
            result.add(text)

    # Extract individual options from options_tag attributes
    # options_tag values use &#10; as separator in raw XML
    for match in re.finditer(r'options_tag="([^"]*)"', content):
        raw_value = match.group(1)
        if not raw_value or raw_value.startswith("$"):
            continue
        # Split on &#10; (raw XML entity, before decode)
        parts = raw_value.split("&#10;")
        for part in parts:
            decoded = _decode_xml_entities(part)
            if not should_skip_text(decoded):
                result.add(decoded)

    # Inline element text (parser-synthesized text= + translation_tag=)
    for text, _pos in _iter_inline_texts(content):
        result.add(text)

    return result


def extract_strings_with_locations(xml_path: Path) -> Dict[str, List[Tuple[str, int]]]:
    """
    Extract translatable strings with their source locations.

    Args:
        xml_path: Path to the XML file

    Returns:
        Dict mapping strings to list of (filename, line_number) tuples
    """
    result: Dict[str, List[Tuple[str, int]]] = {}

    # We need to track line numbers, so parse differently
    # ElementTree doesn't preserve line numbers well, so we'll use a simple approach
    try:
        with open(xml_path, "r", encoding="utf-8") as f:
            content = f.read()
    except IOError as e:
        print(f"Warning: Failed to read {xml_path}: {e}")
        return result

    # File-level opt-out: dev/test panels carry an `i18n: skip-file` marker.
    if I18N_SKIP_FILE_RE.search(content):
        return result

    filename = str(xml_path.name)

    # Parse with line tracking
    # Simple regex-based extraction for line numbers
    for attr in TEXT_ATTRIBUTES:
        # Match attr="value" including compound forms, but skip bind_ variants
        pattern = rf'{attr}="([^"]*)"'
        for match in re.finditer(pattern, content):
            # Check if this is a bind_ variant (not translatable)
            prefix_start = max(0, match.start() - 5)
            prefix = content[prefix_start:match.start()]
            if prefix.endswith("bind_"):
                continue

            text = match.group(1)

            # Decode XML entities (named + numeric character references)
            text = _decode_xml_entities(text)

            if should_skip_text(text):
                continue

            # Calculate line number
            line_num = content[: match.start()].count("\n") + 1

            if text not in result:
                result[text] = []
            result[text].append((filename, line_num))

    # Extract value_tag from <str> const elements (wizard step titles/subtitles)
    for match in re.finditer(r'value_tag="([^"]*)"', content):
        text = match.group(1)
        if not text or text.startswith("$"):
            continue
        text = _decode_xml_entities(text)
        if should_skip_text(text):
            continue
        line_num = content[: match.start()].count("\n") + 1
        if text not in result:
            result[text] = []
        result[text].append((filename, line_num))

    # Extract individual options from options_tag attributes
    for match in re.finditer(r'options_tag="([^"]*)"', content):
        raw_value = match.group(1)
        if not raw_value or raw_value.startswith("$"):
            continue
        parts = raw_value.split("&#10;")
        line_num = content[: match.start()].count("\n") + 1
        for part in parts:
            decoded = _decode_xml_entities(part)
            if should_skip_text(decoded):
                continue
            if decoded not in result:
                result[decoded] = []
            result[decoded].append((filename, line_num))

    for text, pos in _iter_inline_texts(content):
        line_num = content[:pos].count("\n") + 1
        result.setdefault(text, []).append((filename, line_num))

    return result


def extract_strings_from_directory(
    directory: Path, recursive: bool = True, pattern: str = "*.xml"
) -> Set[str]:
    """
    Extract translatable strings from all XML files in a directory.

    Args:
        directory: Directory to scan
        recursive: Whether to scan subdirectories
        pattern: Glob pattern for XML files

    Returns:
        Set of unique translatable strings
    """
    result = set()

    if recursive:
        xml_files = directory.rglob(pattern)
    else:
        xml_files = directory.glob(pattern)

    for xml_path in xml_files:
        strings = extract_strings_from_xml(xml_path)
        result.update(strings)

    return result


def extract_strings_with_all_locations(
    directory: Path, recursive: bool = True, pattern: str = "*.xml"
) -> Dict[str, List[Tuple[str, int]]]:
    """
    Extract translatable strings with locations from all XML files in a directory.

    Args:
        directory: Directory to scan
        recursive: Whether to scan subdirectories
        pattern: Glob pattern for XML files

    Returns:
        Dict mapping strings to list of (filename, line_number) tuples
    """
    result: Dict[str, List[Tuple[str, int]]] = {}

    if recursive:
        xml_files = directory.rglob(pattern)
    else:
        xml_files = directory.glob(pattern)

    for xml_path in xml_files:
        file_result = extract_strings_with_locations(xml_path)
        for text, locations in file_result.items():
            if text not in result:
                result[text] = []
            result[text].extend(locations)

    return result
