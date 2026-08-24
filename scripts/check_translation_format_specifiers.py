#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate that translated format strings preserve their source placeholders.

A runtime-translated format string (passed via lv_tr() to snprintf or
fmt::format) MUST keep the same placeholders as its English source. If a
translation adds an extra one, the format call reads an argument that was never
passed:
  * printf/snprintf -> reads a garbage stack slot -> SIGSEGV
    (crash #1073: the French '%d additional fan%s' had two %s).
  * fmt::format     -> throws fmt::format_error at runtime.

This guard covers both styles:
  1. printf-style — scans src/ for printf-family / *_fmt calls whose format arg
     is an lv_tr("LITERAL"); a translation must use the same conversion
     specifier sequence (count + type). Order matters for C varargs unless
     positional ($) specifiers are used, in which case the multiset must match.
  2. {fmt}-style — scans src/ for fmt::format(lv_tr("LITERAL"), ...); a
     translation must not require MORE args than the source (extra fields throw).

Both passes are gated on real format-sink usage so literal '%' / '{}' in
descriptive prose strings never false-positive.

A third pass guards the keys themselves: a stored key or value holding a
literal \\xNN escape can never match the byte sequence the compiler emits, so
the lookup falls through and the escape text is rendered to the user verbatim.

Exit status: 0 = all good, 1 = mismatch(es) found.
"""

import argparse
import re
import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / "src"
TRANS_DIR = REPO_ROOT / "translations"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
from translations.extractor import resolve_cpp_literal_run  # noqa: E402

# A single C/printf conversion specifier (handles flags, width, precision,
# length modifiers, and positional %N$ args). %% is excluded by the caller.
SPEC_RE = re.compile(
    r"%(?:(\d+)\$)?[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|z|j|t)?"
    r"([diouxXeEfFgGaAcspn%])"
)

# printf-family / format-sink function names whose format arg may be translated.
FORMAT_SINK_RE = re.compile(
    r"\b(?:std::)?(?:v?sn?printf|f?printf|asprintf|"
    r"lv_\w*_fmt|lv_snprintf)\s*\("
)

# fmt::format(lv_tr("..."), ...) — runtime-checked, throws on too few args.
FMT_SINK_RE = re.compile(r"(?:fmt|std)::format\s*\(")

# An lv_tr("...") argument. Captures the whole run of adjacent literals, since
# C++ concatenates "a" "b" at compile time and the lookup key is the joined,
# escape-resolved result -- the same normalization the extractor applies when it
# writes the key into the YAML.
LV_TR_LITERAL_RE = re.compile(r'lv_tr\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)')


def lv_tr_tag(match) -> str:
    """The translation key an lv_tr() match resolves to at runtime."""
    return resolve_cpp_literal_run(match.group(1))

# A {fmt} replacement field (after {{/}} escapes are stripped).
FMT_FIELD_RE = re.compile(r"\{([^{}]*)\}")


def fmt_arg_count(s: str) -> int:
    """Number of positional args a {fmt} string requires.

    Auto-numbered fields ({}) count sequentially; explicit indices ({N}) extend
    the requirement to max-index+1. {{ and }} are literal braces, not fields.
    """
    stripped = s.replace("{{", "").replace("}}", "")
    auto = 0
    max_index = -1
    for field in FMT_FIELD_RE.finditer(stripped):
        spec = field.group(1)
        head = spec.split(":", 1)[0]
        if head.isdigit():
            max_index = max(max_index, int(head))
        elif head == "":
            auto += 1
    return max(auto, max_index + 1)


def conversion_type(conv: str) -> str:
    if conv in "diouxX":
        return "int"
    if conv in "eEfFgGaA":
        return "float"
    if conv == "s":
        return "str"
    if conv == "c":
        return "char"
    if conv == "p":
        return "ptr"
    return conv


def specifiers(s: str):
    """Return list of (positional_index_or_None, normalized_type)."""
    out = []
    for m in SPEC_RE.finditer(s):
        if m.group(2) == "%":
            continue
        out.append((m.group(1), conversion_type(m.group(2))))
    return out


def _collect_sink_tags(sink_re) -> set:
    """Format strings passed via lv_tr() to a call matching sink_re."""
    tags = set()
    for path in SRC_DIR.rglob("*.cpp"):
        text = path.read_text(encoding="utf-8", errors="replace")
        # For each sink call, scan the call's argument region (up to the next
        # ';') for the first lv_tr literal — that's the format argument.
        for sink in sink_re.finditer(text):
            region = text[sink.end():sink.end() + 600]
            stmt_end = region.find(";")
            if stmt_end != -1:
                region = region[:stmt_end]
            lit = LV_TR_LITERAL_RE.search(region)
            if lit:
                tags.add(lv_tr_tag(lit))
    return tags


def collect_format_tags() -> set:
    """printf-style source formats (only those that carry specifiers)."""
    return {t for t in _collect_sink_tags(FORMAT_SINK_RE) if specifiers(t)}


def collect_fmt_tags() -> set:
    """{fmt}-style source formats (only those that carry replacement fields)."""
    return {t for t in _collect_sink_tags(FMT_SINK_RE) if fmt_arg_count(t) > 0}


def _iter_locales():
    for yml in sorted(TRANS_DIR.glob("*.yml")):
        locale = yml.stem
        if locale == "en":
            continue  # en is the source; nothing to compare against
        data = yaml.safe_load(yml.read_text(encoding="utf-8")) or {}
        trans = data.get("translations", data)
        if isinstance(trans, dict):
            yield locale, trans


# A C escape sequence surviving into a stored translation string. The extractor
# resolves all of these, so one reaching the YAML means the key was captured in
# its source form and can never match what the compiler emits. Matching any
# backslash would be equally correct today (no translation legitimately contains
# one), but naming the escapes keeps the failure message specific.
UNRESOLVED_ESCAPE_RE = re.compile(r"\\(?:x[0-9a-fA-F]|[nrtabfve0-7\\\"']|u[0-9a-fA-F]{4})")


def check_unresolved_escapes() -> list:
    """Report every YAML key or value still carrying a literal C escape.

    lv_tr("%d\\xc2\\xb0") looks up a key containing the degree sign, and
    lv_tr("a\\nb") looks up one containing a real newline, because the compiler
    resolved the escape. A key stored as the characters of the escape sequence
    misses forever, and LVGL falls back to rendering the tag -- so the user sees
    the raw escape text in every locale, including English.
    """
    problems = []
    for yml in sorted(TRANS_DIR.glob("*.yml")):
        data = yaml.safe_load(yml.read_text(encoding="utf-8")) or {}
        trans = data.get("translations", data)
        if not isinstance(trans, dict):
            continue
        for key, val in trans.items():
            if UNRESOLVED_ESCAPE_RE.search(key):
                problems.append((yml.stem, "key", key))
            if isinstance(val, str) and UNRESOLVED_ESCAPE_RE.search(val):
                problems.append((yml.stem, "value", val))
    return problems


def check(verbose: bool) -> int:
    escapes = check_unresolved_escapes()
    if escapes:
        print("✗ Translation strings carrying an unresolved C escape:\n")
        for locale, kind, text in escapes:
            print(f"  [{locale}] {kind}: {text!r}")
        print(
            f"\n{len(escapes)} occurrence(s). A C escape is resolved by the compiler, so\n"
            "a key holding the escape literally never matches at runtime and the raw\n"
            "text is rendered instead. Store the characters the escape encodes in\n"
            "translations/<locale>.yml, then run: make translations"
        )
        return 1

    printf_tags = collect_format_tags()
    fmt_tags = collect_fmt_tags()
    if verbose:
        print(
            f"Discovered {len(printf_tags)} printf and {len(fmt_tags)} "
            "{fmt} translated format string(s) in src/"
        )

    problems = []
    for locale, trans in _iter_locales():
        # printf-style: specifier sequence (count + type) must match.
        for tag in printf_tags:
            val = trans.get(tag)
            if not isinstance(val, str):
                continue
            # An untranslated (empty) value is not a mismatch: at runtime LVGL
            # falls back to the tag itself (the English source), so every
            # specifier is present and snprintf reads no missing arg. Flagging
            # empty here is a false positive — the first untranslated %d key
            # would otherwise fail the gate the moment translation-sync adds it.
            # The {fmt} loop below already tolerates empty (0 fields <= source).
            if not val.strip():
                continue
            src = specifiers(tag)
            tr = specifiers(val)
            src_types = [t for _, t in src]
            tr_types = [t for _, t in tr]
            positional = any(p for p, _ in tr) or any(p for p, _ in src)
            ok = (
                sorted(tr_types) == sorted(src_types)
                if positional
                else tr_types == src_types
            )
            if not ok:
                kind = (
                    "printf EXTRA-SPECIFIER (crash risk)"
                    if len(tr) > len(src)
                    else "printf MISMATCH"
                )
                problems.append((kind, locale, tag, val, src_types, tr_types))

        # {fmt}-style: translation must not require MORE args than the source
        # (too many fields -> fmt::format_error thrown at runtime).
        for tag in fmt_tags:
            val = trans.get(tag)
            if not isinstance(val, str):
                continue
            src_n = fmt_arg_count(tag)
            tr_n = fmt_arg_count(val)
            if tr_n > src_n:
                problems.append(
                    ("fmt EXTRA-FIELD (throw risk)", locale, tag, val,
                     f"{src_n} arg(s)", f"{tr_n} arg(s)")
                )

    if problems:
        print("✗ Translation format mismatches found:\n")
        for kind, locale, tag, val, st, tt in problems:
            print(f"  [{kind}] locale={locale}")
            print(f"     source : {tag!r}   {st}")
            print(f"     {locale:<6}: {val!r}   {tt}")
            print()
        print(f"{len(problems)} mismatch(es). A translated format string must use the")
        print("same placeholders (count/type/order) as its source — otherwise")
        print("snprintf/fmt::format reads a missing arg and crashes or throws.")
        return 1

    if verbose:
        print("✓ All translated format strings preserve their source placeholders.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    sys.exit(check(args.verbose))


if __name__ == "__main__":
    main()
