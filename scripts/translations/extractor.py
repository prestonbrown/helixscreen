#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Extractor module - scans XML and C++ files for translatable text strings.

Extracts text from XML:
- text="..." attributes on any element
- label="...", description="...", title="...", subtitle="..." props

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

# Attributes that contain translatable text
TEXT_ATTRIBUTES = {"text", "label", "description", "title", "subtitle"}

# Patterns to skip
VARIABLE_PATTERN = re.compile(r"\$\w+")  # $variable
ICON_PATTERN = re.compile(r"^#icon_")  # #icon_xxx
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

# Short tokens and non-translatable exact strings
NON_TRANSLATABLE = {"true", "false", "xl", "lg", "md", "sm", "xs", "#RRGGBB"}

# Language names displayed in their native script (never translated)
LANGUAGE_NAMES = {
    "Deutsch", "English", "Español", "Français", "Italiano", "Português",
    "Русский", "中文", "日本語", "한국어", "العربية", "हिन्दी", "Türkçe",
    "Nederlands", "Polski", "Svenska", "Norsk", "Dansk", "Suomi",
    "Čeština", "Magyar", "Română", "Українська", "Ελληνικά",
}

# C++ patterns that indicate translatable text
CPP_TRANSLATABLE_PATTERNS = [
    # lv_tr("text") - explicitly marked for translation (handles escaped quotes)
    r'lv_tr\s*\(\s*"((?:[^"\\]|\\.)+)"',
    # lv_label_set_text(label, "text")
    r'lv_label_set_text\s*\([^,]+,\s*"((?:[^"\\]|\\.)+)"',
    # return "Status Text"  (for status strings)
    r'return\s+"([A-Z][a-z][^"]{2,30})"',
]

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

    # Skip icon font references
    if ICON_PATTERN.match(text):
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
        for match in re.finditer(pattern, content):
            text = match.group(1)

            # lv_tr() strings are explicitly marked - always include them
            if is_lv_tr:
                if text and text.strip():
                    result.add(text)
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
