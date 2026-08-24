#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""
XML Formatter with Attribute Wrapping

Formats LVGL XML component files with:
- 2-space indentation
- Attribute wrapping at ~120 character line width
- Preservation of XML comments (including spacing)
- Smart attribute grouping on lines
- Preservation of blank lines between logical sections

Generator-owned XML (see GENERATED_DIRS) is skipped - those files are rewritten by
`make`, so formatting them only makes the tree go dirty on the next build. XML that
belongs to another toolchain (see FOREIGN_DIRS) is skipped too - it is not LVGL
component XML and does not follow this style.

Usage:
    ./scripts/format-xml.py ui_xml/*.xml           # Format files in-place
    ./scripts/format-xml.py --check ui_xml/*.xml   # Check without modifying
    ./scripts/format-xml.py --diff ui_xml/*.xml    # Show diff of changes
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Optional
from html import escape as html_escape

try:
    from lxml import etree
except ImportError:
    print("Error: lxml is required. Install with: pip install lxml", file=sys.stderr)
    print("Or run: make venv-setup", file=sys.stderr)
    sys.exit(1)


# Configuration
LINE_WIDTH = 120
INDENT = "  "  # 2 spaces

# LVGL state selectors are written `style_bg_opa:checked="0"`. Every XML parser reads
# that colon as a namespace prefix and rejects the file ("Namespace prefix style_bg_opa
# for checked is not defined"), so filament_panel, input_shaper_panel and
# theme_editor_overlay were unformattable and silently drifted. Swap the colon for a
# sentinel before parsing and swap it back on the way out.
#
# The sentinel MUST be made of XML NameChars. A pretty separator like U+2999 is not one,
# and the parser then trips over the sentinel instead of the colon, reporting the far more
# confusing "Specification mandates value for attribute style_bg_opa".
#
# The pattern only fires in attribute-name position (whitespace, name, colon, name, `=`,
# quote), so colons inside values and text are untouched: `href="http://x"`, `t="9:30"`
# and `<p>see: this</p>` all survive a round trip.
STATE_COLON_SENTINEL = "__LVGL_STATE_COLON__"
STATE_COLON_RE = re.compile(r'(\s)([A-Za-z_][\w.-]*):([A-Za-z_][\w.-]*)(\s*=\s*["\'])')


def _escape_state_colons(content: str) -> str:
    """Hide LVGL state-selector colons from the XML parser."""
    return STATE_COLON_RE.sub(
        lambda m: f"{m.group(1)}{m.group(2)}{STATE_COLON_SENTINEL}{m.group(3)}{m.group(4)}",
        content,
    )


def _unescape_state_colons(content: str) -> str:
    """Restore LVGL state-selector colons after formatting."""
    return content.replace(STATE_COLON_SENTINEL, ":")

# Directories whose XML is written by a generator, not by hand. Formatting these
# starts a fight nobody wins: `make` regenerates them on every build (mk/translations.mk
# -> generate_translations.py), which strips the wrapping this script adds, so a
# formatted tree goes dirty on the next compile and `make format` re-wraps it right
# back. The generator owns its output; run it, don't reformat it. Callers pass file
# lists from `find ui_xml -name '*.xml'`, so the skip lives here rather than in each
# of the three call sites (mk/format.mk format + format-staged, quality-checks.sh).
GENERATED_DIRS = ("ui_xml/translations",)

# XML that is not an LVGL component file and does not answer to this formatter's
# house style. android/ holds AndroidManifest.xml and res/values/*.xml, which the
# Android toolchain (AAPT, Android Studio, AGP's manifest merger) owns and formats
# by its own conventions; reflowing them to 2-space/120-col LVGL style produces
# churn in files nobody reads through an LVGL lens. `make format` and the
# non-staged check only walk ui_xml/, so these are reachable only through the
# staged-file paths (mk/format.mk format-staged, quality-checks.sh --staged-only),
# which feed in every staged *.xml in the repo. format-staged FORMATS and re-stages
# what it is given, so without this the pre-commit hook would silently rewrite a
# manifest as a side effect of an unrelated commit.
FOREIGN_DIRS = ("android",)


def _under(filepath: Path, dirs: tuple) -> bool:
    posix = filepath.as_posix()
    return any(
        posix == d or posix.startswith(d + "/") or f"/{d}/" in posix for d in dirs
    )


def is_skipped(filepath: Path) -> Optional[str]:
    """Reason this file is not ours to format, or None if it is."""
    if _under(filepath, GENERATED_DIRS):
        return "generated"
    if _under(filepath, FOREIGN_DIRS):
        return "not LVGL XML"
    return None

# Attributes that should come first (in order)
PRIORITY_ATTRS = ["name", "extends", "width", "height"]


def escape_attr_value(value: str) -> str:
    """Escape attribute value for XML output."""
    # Escape &, <, > and quotes
    result = value.replace("&", "&amp;")
    result = result.replace("<", "&lt;")
    result = result.replace(">", "&gt;")
    result = result.replace('"', "&quot;")
    # Preserve newlines as &#10; (LVGL XML requires this for dropdown options)
    result = result.replace("\n", "&#10;")
    return result


def order_attributes(attrs: dict) -> list[tuple[str, str]]:
    """Order attributes with priority attrs first, then original order."""
    result = []
    remaining = dict(attrs)

    # Add priority attributes first (if present)
    for attr in PRIORITY_ATTRS:
        if attr in remaining:
            result.append((attr, remaining.pop(attr)))

    # Add remaining attributes in original order (dict preserves insertion order in Python 3.7+)
    for attr, value in remaining.items():
        result.append((attr, value))

    return result


def format_attributes_wrapped(attrs: list[tuple[str, str]], indent: str, tag: str) -> str:
    """
    Format attributes with smart wrapping.

    Groups related attributes on the same line when they fit within LINE_WIDTH.
    Returns the full opening tag string (without closing > or />).
    """
    if not attrs:
        return f"{indent}<{tag}"

    # First try single line
    attr_str = " ".join(f'{k}="{escape_attr_value(v)}"' for k, v in attrs)
    single_line = f"{indent}<{tag} {attr_str}"

    if len(single_line) <= LINE_WIDTH:
        return single_line

    # Need to wrap - build lines greedily
    # Continuation lines align after the tag name
    attr_indent = indent + " " * (len(tag) + 2)  # +2 for "< " before tag

    lines = [f"{indent}<{tag}"]
    current_line = attr_indent
    first_attr = True

    for key, value in attrs:
        attr_text = f'{key}="{escape_attr_value(value)}"'

        if first_attr:
            # First attribute goes on same line as tag
            lines[0] += f" {attr_text}"
            first_attr = False
        elif len(current_line) + len(attr_text) + 1 <= LINE_WIDTH:
            # Fits on current line
            if current_line == attr_indent:
                current_line += attr_text
            else:
                current_line += " " + attr_text
        else:
            # Start new line
            if current_line != attr_indent:
                lines.append(current_line)
            current_line = attr_indent + attr_text

    # Don't forget the last line
    if current_line != attr_indent:
        lines.append(current_line)

    return "\n".join(lines)


def count_blank_lines(text: str) -> int:
    """Count the number of blank lines in whitespace text."""
    if not text:
        return 0
    # Count newlines beyond the first one (which is just normal element separation)
    newlines = text.count('\n')
    return max(0, newlines - 1)


def format_element(
    elem: etree._Element, indent_level: int = 0
) -> list[str]:
    """Format a single element and its children, returning lines of output."""
    lines = []
    indent = INDENT * indent_level
    tag = elem.tag

    # Handle comments specially - preserve internal spacing
    if callable(elem.tag):  # This is a comment or PI
        comment_text = elem.text or ""
        # Preserve the comment exactly as-is (including internal whitespace)
        lines.append(f"{indent}<!--{comment_text}-->")
        return lines

    # Get ordered attributes
    attrs = order_attributes(dict(elem.attrib))

    # Build opening tag with smart wrapping
    opening = format_attributes_wrapped(attrs, indent, tag)

    # Check if element has children or text
    has_children = len(elem) > 0
    has_text = elem.text and elem.text.strip()

    if not has_children and not has_text:
        # Self-closing tag
        lines.append(f"{opening}/>")
    else:
        # Opening tag
        lines.append(f"{opening}>")

        # Handle text content
        if has_text:
            # Escape text content
            escaped_text = html_escape(elem.text.strip(), quote=False)
            lines.append(f"{indent}{INDENT}{escaped_text}")

        # Check for blank lines before first child (in elem.text whitespace)
        if has_children and elem.text:
            blank_count = count_blank_lines(elem.text)
            lines.extend([''] * blank_count)

        # Handle children
        for child in elem:
            child_lines = format_element(child, indent_level + 1)
            lines.extend(child_lines)

            # Handle tail text (text after child element)
            if child.tail:
                if child.tail.strip():
                    escaped_tail = html_escape(child.tail.strip(), quote=False)
                    lines.append(f"{indent}{INDENT}{escaped_tail}")
                else:
                    # Preserve blank lines between sibling elements
                    blank_count = count_blank_lines(child.tail)
                    lines.extend([''] * blank_count)

        # Closing tag
        lines.append(f"{indent}</{tag}>")

    return lines


def preprocess_xml(content: str) -> str:
    """
    Preprocess XML content to fix common issues before parsing.

    Fixes:
    - Unescaped & in attribute values (e.g., text="Movement & Home" -> text="Movement &amp; Home")

    Does NOT modify:
    - Comments (<!-- Movement & Home --> stays as-is)
    - Already escaped entities (&amp; &lt; etc.)
    """
    import re

    def escape_ampersand_in_attr(match):
        """Escape & in attribute value, but not if already an entity."""
        attr_name = match.group(1)
        quote = match.group(2)
        value = match.group(3)

        # Escape & that aren't already entities
        def fix_amp(m):
            after = m.group(1)
            if re.match(r'^(amp|lt|gt|quot|apos|#\d+|#x[0-9a-fA-F]+);', after):
                return '&' + after  # Already escaped
            return '&amp;' + after

        fixed_value = re.sub(r'&(.{0,10})', fix_amp, value)
        return f'{attr_name}={quote}{fixed_value}{quote}'

    # Match attribute="value" or attribute='value' patterns
    # Group 1: attribute name and =
    # Group 2: opening quote
    # Group 3: value content
    content = re.sub(
        r'(\w+)=(["\'])([^"\']*)\2',
        escape_ampersand_in_attr,
        content
    )

    return content


def format_xml_file(content: str) -> str:
    """Format an XML file content string, returning formatted content."""
    # Preprocess to fix common issues
    content = preprocess_xml(content)

    # Hide LVGL state-selector colons; restored just before returning. The sentinel is
    # rejected up front rather than silently mangled, since a file that already contains
    # it would round-trip into a colon it never had.
    if STATE_COLON_SENTINEL in content:
        raise ValueError(
            f"file contains the reserved sentinel {STATE_COLON_SENTINEL!r}; "
            "rename that attribute or change STATE_COLON_SENTINEL"
        )
    content = _escape_state_colons(content)

    # Parse the XML while preserving comments and whitespace (for blank line detection)
    parser = etree.XMLParser(remove_blank_text=False, remove_comments=False)
    try:
        root = etree.fromstring(content.encode("utf-8"), parser)
    except etree.XMLSyntaxError as e:
        raise ValueError(f"XML parse error: {e}")

    # Build output
    lines = ['<?xml version="1.0"?>']

    # Get the original file to extract leading comments (before root element)
    # This preserves copyright and license comments
    # A multi-line comment must be carried across lines: its opening line
    # starts with "<!--" but does not close, so testing both delimiters on one
    # line sent it to the root-element break below, deleting the comment AND
    # cutting the prolog scan short. Continuation lines keep their original
    # indentation — these comments are mostly aligned state tables.
    content_lines = content.split("\n")
    in_comment = False
    for line in content_lines:
        stripped = line.strip()
        if in_comment:
            lines.append(line.rstrip())
            if "-->" in stripped:
                in_comment = False
            continue
        if stripped.startswith("<!--"):
            if stripped.endswith("-->"):
                lines.append(stripped)
            else:
                lines.append(line.rstrip())
                in_comment = True
        elif stripped.startswith("<?xml"):
            continue  # Skip XML declaration, we add our own
        elif stripped.startswith("<"):
            break  # Hit the root element

    # Format the root element and all children
    root_lines = format_element(root, 0)
    lines.extend(root_lines)

    # Ensure trailing newline
    return _unescape_state_colons("\n".join(lines) + "\n")


def process_file(
    filepath: Path, check_only: bool = False, show_diff: bool = False
) -> tuple[bool, Optional[str], bool]:
    """
    Process a single XML file.

    Returns:
        (needs_formatting, diff_output, failed)
        - needs_formatting: True if file needs formatting
        - diff_output: Diff string if show_diff=True, else None
        - failed: True if the file could not be read or parsed

    `failed` exists because a file this script cannot parse is not a formatted file.
    Both outcomes used to return (False, None), so main() counted an unreadable file as
    clean and --check printed "All files properly formatted" and exited 0 for a file it
    never looked at. That is how the three LVGL state-selector layouts drifted unnoticed.
    """
    try:
        original = filepath.read_text(encoding="utf-8")
    except Exception as e:
        print(f"Error reading {filepath}: {e}", file=sys.stderr)
        return False, None, True

    try:
        formatted = format_xml_file(original)
    except ValueError as e:
        print(f"Error formatting {filepath}: {e}", file=sys.stderr)
        return False, None, True

    if original == formatted:
        return False, None, False

    # File needs formatting
    if show_diff:
        import difflib

        diff = difflib.unified_diff(
            original.splitlines(keepends=True),
            formatted.splitlines(keepends=True),
            fromfile=str(filepath),
            tofile=str(filepath) + " (formatted)",
        )
        diff_output = "".join(diff)
    else:
        diff_output = None

    if not check_only:
        filepath.write_text(formatted, encoding="utf-8")

    return True, diff_output, False


def main():
    parser = argparse.ArgumentParser(
        description="Format LVGL XML files with attribute wrapping"
    )
    parser.add_argument("files", nargs="+", type=Path, help="XML files to format")
    parser.add_argument(
        "--check",
        action="store_true",
        help="Check if files need formatting without modifying them",
    )
    parser.add_argument(
        "--diff", action="store_true", help="Show diff of changes"
    )
    parser.add_argument(
        "--quiet", "-q", action="store_true", help="Suppress output except errors"
    )

    args = parser.parse_args()

    files_needing_format = []
    errors = 0

    for filepath in args.files:
        if not filepath.exists():
            print(f"Error: {filepath} does not exist", file=sys.stderr)
            errors += 1
            continue

        if not filepath.suffix == ".xml":
            print(f"Skipping non-XML file: {filepath}", file=sys.stderr)
            continue

        skip_reason = is_skipped(filepath)
        if skip_reason:
            if not args.quiet:
                print(f"Skipping {skip_reason} file: {filepath}", file=sys.stderr)
            continue

        needs_format, diff_output, failed = process_file(
            filepath, check_only=args.check, show_diff=args.diff
        )

        if failed:
            errors += 1
            continue

        if needs_format:
            files_needing_format.append(filepath)
            if args.diff and diff_output:
                print(diff_output)
            elif not args.quiet:
                if args.check:
                    print(f"Needs formatting: {filepath}")
                else:
                    print(f"Formatted: {filepath}")

    # Summary
    if not args.quiet:
        if errors:
            print(f"\n{errors} file(s) could not be processed", file=sys.stderr)
        if args.check:
            if files_needing_format:
                print(
                    f"\n{len(files_needing_format)} file(s) need formatting",
                    file=sys.stderr,
                )
            elif not errors:
                # Only claim a clean bill of health for files actually parsed.
                print("All files properly formatted")

    # Exit code: 1 if any files needed formatting (for --check mode)
    if args.check and files_needing_format:
        sys.exit(1)
    elif errors:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
