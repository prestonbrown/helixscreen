#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: Moonraker subscription handlers must not use .value() / .get<T>()
# on subscribed-status JSON without explicit type guards.
#
# Background: Moonraker delivers null for subscribed fields when the underlying
# Klipper object lacks them (e.g. Snapmaker U1's filament_motion_sensor reports
# no detection_count). nlohmann::json::value("k", default) and .get<T>() throw
# json::type_error::302 on null. An uncaught throw inside a subscription
# handler unwinds out of run() into main()'s top-level catch, exiting 134 and
# triggering a watchdog crash loop the user can't break out of.
#
# Documented in memory: feedback_moonraker_subscribed_null.md
#
# Approved patterns:
#   if (auto it = obj.find("k"); it != obj.end() && it->is_<type>()) {
#       v = it->get<T>();
#   }
#   v = helix::json::safe_int(obj, "k", default);   // (or safe_float/string/bool)
#
# Banned in subscription handlers (without // JSON_NULL_SAFE comment):
#   obj.value("k", default)                          // throws on null
#   obj["k"].get<T>()                                // throws on null/missing
#   if (obj.contains("k")) v = obj.value("k", ...);  // contains() returns true for null
#
# Per-line opt-out:
#   v = obj.value("foo", 0); // JSON_NULL_SAFE: caller already type-guarded
#
#
# ---------------------------------------------------------------------------
# RULE 2: const-qualified nlohmann::json operator[] (uncatchable)
# ---------------------------------------------------------------------------
#
# Rule 1 above targets a *catchable* defect: a throw out of a subscription
# handler is swallowed by one of four independent layers (the libhv onmessage
# trampoline, on_ws_message, each notify callback, the RPC success/error
# callbacks, UpdateQueue::process_pending), so it costs a dropped update, not
# the process.
#
# Rule 2 targets the class that is NOT catchable. NDEBUG is never defined in
# this build, so nlohmann's
# JSON_ASSERT is a live assert():
#
#   lib/libhv/cpputil/json.hpp:22182  const_reference operator[](key) const
#       auto it = m_data.m_value.object->find(key);
#       JSON_ASSERT(it != m_data.m_value.object->end());   // <-- SIGABRT (134)
#
#   lib/libhv/cpputil/json.hpp:22147  const_reference operator[](size_type) const
#       return m_data.m_value.array->operator[](idx);      // <-- no bounds check
#
# A missing key read through the CONST overload is an abort, not a throw: a
# surrounding `catch (const nlohmann::json::exception&)` provably cannot fire.
# Exit 134 is the watchdog-crash-loop signature. An out-of-range numeric index
# on a const array is a straight SIGSEGV.
#
# The canonical shape:
#
#   token.defer("tag", [this, response = std::move(response)]() {  // NOT mutable
#       const auto& s = response["result"]["status"]["configfile"]["settings"];
#
# A non-mutable lambda's operator() is const, so the by-value captured
# `response` is const, so operator[] resolves to the const overload. Each of
# those five sites sat inside a dead try/catch.
#
# This rule deliberately does NOT flag reads through a NON-const json — those
# use the mutating overload, which default-constructs the missing key. That is
# a data bug, not a crash, and there are ~1000 of them; widening to that set is
# the sweep #1139 deliberately deferred.
#
# Approved patterns (all present in the codebase already):
#   if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty())
#       return;
#   const auto& p = msg["params"][0];
#
#   if (response.contains("result") && response["result"].contains("status")) {
#       const auto& status = response["result"]["status"];
#
# Per-line opt-out (either token works):
#   const auto& x = resp["a"]["b"];  // JSON_CONST_OK: literal built above
#
# Usage:
#   ./scripts/check_subscription_null_safety.py [files...]
#   ./scripts/check_subscription_null_safety.py --staged-only
#   ./scripts/check_subscription_null_safety.py --const-subscript-list
#   (no args = scan src/ recursively)

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

from staged_content import staged_files

HANDLER_RE = re.compile(
    r'(?:^|\s)(?:[\w:<>~]+\s+)+'
    r'(?P<qual>[\w:]*::)?'
    r'(?P<name>update_from_status|update_from_subscription|update_from_notification'
    r'|update_from_backend|handle_subscription|apply_status_snapshot'
    r'|on_status_update|process_subscription_update)'
    r'\s*\([^;{]*\)\s*(?:const)?\s*(?:noexcept)?\s*\{',
    re.MULTILINE,
)

# nlohmann::json::value(key, default) takes a string-literal first arg.
# std::optional<T>::value() takes no args — exclude that to avoid false
# positives (e.g. tool_state.cpp `extruder_name.value() == ...`).
VALUE_CALL_RE = re.compile(r'\.value\s*\(\s*"')
GET_TYPE_RE = re.compile(r'\.get\s*<\s*(?:int|float|double|bool|std::string|long|short|uint\w*|int\w*)\s*>\s*\(')
# A guard on the same line or a recent enclosing line — any of these is sufficient
# to prove the .get<T>() can't fire on a null. .value() is never proven safe by
# this guard alone (it still throws on null even when the key is present), so
# .value() always needs the explicit JSON_NULL_SAFE opt-out.
IS_TYPE_GUARD_RE = re.compile(
    r'\.is_(?:number(?:_integer|_unsigned|_float)?|string|boolean|object|array|null)\s*\('
)
OPT_OUT = "JSON_NULL_SAFE"
GUARD_LOOKBACK = 15  # lines — covers typical inline-block sizes between
                     # the `is_<type>()` guard and the `.get<T>()` call.


def find_handler_bodies(text: str) -> list[tuple[str, int, int]]:
    """Return [(handler_name, body_start_line, body_end_line)]."""
    out: list[tuple[str, int, int]] = []
    for m in HANDLER_RE.finditer(text):
        # Brace match starting at the opening {
        open_pos = m.end() - 1
        depth = 0
        end = -1
        for i in range(open_pos, len(text)):
            c = text[i]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    end = i
                    break
        if end < 0:
            continue
        start_line = text.count('\n', 0, open_pos) + 1
        end_line = text.count('\n', 0, end) + 1
        out.append((m.group('name'), start_line, end_line))
    return out


def scan_file(path: Path) -> list[str]:
    """Return list of violation strings for this file."""
    try:
        text = path.read_text()
    except Exception:
        return []
    return scan_source(str(path), text)


def scan_source(path: str, text: str) -> list[str]:
    """scan_file's body, operating on already-sourced text - disk, or the
    STAGED blob a --staged-only caller reads instead. `path` is only carried
    into the report."""
    violations: list[str] = []
    bodies = find_handler_bodies(text)
    if not bodies:
        return []
    lines = text.splitlines()

    def has_recent_is_guard(ln: int) -> bool:
        """True if any of the last GUARD_LOOKBACK lines (incl. ln) contains
        an .is_<type>() check or a JSON_NULL_SAFE opt-out comment."""
        lo_g = max(1, ln - GUARD_LOOKBACK)
        for g in range(lo_g, ln + 1):
            if g <= 0 or g > len(lines):
                continue
            l = lines[g - 1]
            if OPT_OUT in l:
                return True
            if IS_TYPE_GUARD_RE.search(l):
                return True
        return False

    for handler, lo, hi in bodies:
        for ln in range(lo, hi + 1):
            if ln <= 0 or ln > len(lines):
                continue
            line = lines[ln - 1]
            if OPT_OUT in line:
                continue
            stripped = line.lstrip()
            if stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
                continue
            if VALUE_CALL_RE.search(line):
                # .value() is never safe via is_<type>() alone — even with a
                # type guard, a null delivery in a sibling field can flip the
                # type unexpectedly between the guard and the call. Always flag.
                violations.append(
                    f"{path}:{ln}: {handler}() uses .value() — throws on null. "
                    f"Use find()+is_<type>() or helix::json::safe_*. "
                    f"(suppress with `// JSON_NULL_SAFE: <reason>`)"
                )
            elif GET_TYPE_RE.search(line):
                if has_recent_is_guard(ln):
                    continue
                violations.append(
                    f"{path}:{ln}: {handler}() uses .get<T>() without an .is_<type>() guard. "
                    f"Use find()+is_<type>() or helix::json::safe_*. "
                    f"(suppress with `// JSON_NULL_SAFE: <reason>`)"
                )
    return violations


# =========================================================================
# RULE 2 — const-qualified json operator[] (uncatchable SIGABRT / SIGSEGV)
# =========================================================================

CONST_OPT_OUT = ("JSON_CONST_OK", OPT_OUT)

# Known-unfixed sites. Keyed by "<path>::<expression>" — deliberately NOT by
# line number, so unrelated edits above a site don't churn this table. The
# value is how many times that exact expression appears in that file.
#
# A new key, or a higher count on an existing key, fails the gate. Fixing a
# site makes its key surplus; the gate reports that so the entry can be
# deleted. Every entry below is a genuine unguarded const operator[]; each was
# triaged as unreachable-in-practice at the time of writing (the JSON is built
# locally a few lines above, or the missing-key branch is excluded upstream),
# which is why they are baselined rather than fixed.
CONST_SUBSCRIPT_BASELINE: dict[str, int] = {
    # Comparator over a locally-built array; both entries always carry "name".
    'src/remote/remote_control_server.cpp::a["name"]': 1,
    'src/remote/remote_control_server.cpp::b["name"]': 1,
    # Same shape: std::sort over the extruder prefix, which entry_is_extruder
    # has already filtered on entry.contains("name").
    'src/ui/panel_widgets/temp_graph_widget.cpp::a["name"]': 1,
    'src/ui/panel_widgets/temp_graph_widget.cpp::b["name"]': 1,
    # Chamber-mock frames: every read is on json MoonrakerClientMock built a
    # few lines above, and the tests REQUIRE the enclosing frame key first -
    # a missing key means the mock regressed, which should abort, not skip.
}

_NEUTRALISE = set('{}[]()')


def clean_source(src: str) -> str:
    """Blank out comments and neutralise bracket characters inside string and
    character literals. The result has the same length as `src`, so every
    offset still maps back to the original text."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                out[i] = ' '
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            out[i] = out[i + 1] = ' '
            i += 2
            while i < n and not (src[i] == '*' and i + 1 < n and src[i + 1] == '/'):
                if src[i] != '\n':
                    out[i] = ' '
                i += 1
            if i + 1 < n:
                out[i] = out[i + 1] = ' '
                i += 2
        elif c in '"\'':
            quote = c
            i += 1
            while i < n:
                if src[i] == '\\':
                    i += 2
                    continue
                if src[i] == quote or src[i] == '\n':
                    break
                if src[i] in _NEUTRALISE:
                    out[i] = '_'
                i += 1
            i += 1
        else:
            i += 1
    return ''.join(out)


def _match_brace(text: str, open_pos: int) -> int:
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i
    return len(text) - 1


# A lambda introducer. `spec` captures anything between the parameter list and
# the body, which is where `mutable` would appear.
LAMBDA_RE = re.compile(
    r'\[(?P<cap>[^\[\]]*)\]\s*(?:\((?P<params>[^()]*)\))?\s*'
    r'(?P<spec>(?:mutable\b|constexpr\b|noexcept\b|\s)*)'
    r'(?:->[^{;]*)?\{'
)
# Local binding: `const auto& x = ...`. The trailing `=` is required on
# purpose — without it this also matches a *parameter* declaration, whose
# scope is the function body rather than "up to the next closing brace", and
# mis-scoping a parameter leaks constness across sibling functions.
CONST_BIND_RE = re.compile(
    r'\bconst\s+(?:nlohmann::)?(?:auto|json|basic_json)\s*&\s*(?P<name>\w+)\s*(?==[^=])'
)
CONST_JSON_PARAM_RE = re.compile(r'\bconst\s+(?:nlohmann::)?json\s*&\s*(?P<name>\w+)\b')
FOR_CONST_RE = re.compile(r'\bfor\s*\(\s*const\s+auto\s*&\s*(?P<name>\w+)\s*:')

_ROOT = r'[A-Za-z_]\w*(?:\s*\.\s*[A-Za-z_]\w*)*'
_SUB = r'(?:\s*\[\s*"(?:[^"\\]|\\.)*"\s*\])'
PATH_RE = re.compile(rf'(?<![\w.>])(?P<root>{_ROOT})(?P<subs>{_SUB}+)')
INDEX_RE = re.compile(rf'(?<![\w.>])(?P<root>{_ROOT})(?P<subs>{_SUB}+)\s*\[\s*(?P<idx>[^\]"\s][^\]]*)\]')
KEY_RE = re.compile(r'\[\s*"((?:[^"\\]|\\.)*)"\s*\]')
GUARD_RE = re.compile(
    rf'(?<![\w.>])(?P<root>{_ROOT})(?P<subs>{_SUB}*)\s*\.\s*'
    r'(?:contains|find)\s*\(\s*"(?P<key>(?:[^"\\]|\\.)*)"\s*\)'
)


def _split_top_level(s: str) -> list[str]:
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in '([{<':
            depth += 1
        elif ch in ')]}>':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append(''.join(cur))
    return [p.strip() for p in parts if p.strip()]


def const_scopes(clean: str) -> list[tuple[str, int, int]]:
    """[(identifier, scope_start_offset, scope_end_offset)] for every binding
    that makes an identifier const-qualified."""
    scopes: list[tuple[str, int, int]] = []

    for m in LAMBDA_RE.finditer(clean):
        cap = m.group('cap') or ''
        if cap.lstrip().startswith('['):  # C++ attribute, e.g. [[nodiscard]]
            continue
        if re.search(r'\bmutable\b', m.group('spec') or ''):
            continue  # mutable => operator() is non-const => captures are not const
        body_open = m.end() - 1
        body_end = _match_brace(clean, body_open)
        for item in _split_top_level(cap):
            if item in ('this', '=', '&', '*this') or item.startswith('&'):
                continue  # by-reference captures do not acquire constness
            nm = re.match(r'(\w+)\s*(?:=|$)', item)
            if nm:
                scopes.append((nm.group(1), body_open, body_end))
        for pm in CONST_JSON_PARAM_RE.finditer(m.group('params') or ''):
            scopes.append((pm.group('name'), body_open, body_end))

    for m in CONST_BIND_RE.finditer(clean):
        depth, end = 0, len(clean) - 1
        for i in range(m.end(), len(clean)):
            if clean[i] == '{':
                depth += 1
            elif clean[i] == '}':
                if depth == 0:
                    end = i
                    break
                depth -= 1
        scopes.append((m.group('name'), m.end(), end))

    for m in CONST_JSON_PARAM_RE.finditer(clean):
        b = clean.find('{', m.end())
        if b < 0 or clean.count('\n', m.end(), b) > 3:
            continue  # not the body that immediately follows this signature
        scopes.append((m.group('name'), b, _match_brace(clean, b)))

    for m in FOR_CONST_RE.finditer(clean):
        b = clean.find('{', m.end())
        if b < 0 or clean.count('\n', m.end(), b) > 2:
            continue
        scopes.append((m.group('name'), b, _match_brace(clean, b)))

    return scopes


def _top_level_blocks(clean: str) -> list[tuple[int, int]]:
    """Outermost brace blocks — function bodies. A guard anywhere earlier in
    the same function body counts as proof. Scoping proofs any tighter than
    this is what made a previous audit's scanner cry wolf: real guards
    routinely sit 12-48 lines above the read, in an enclosing `if` or an early
    return."""
    out: list[tuple[int, int]] = []
    depth, start = 0, -1
    for i, c in enumerate(clean):
        if c == '{':
            if depth == 0:
                start = i
            depth += 1
        elif c == '}':
            depth -= 1
            if depth <= 0:
                if start >= 0:
                    out.append((start, i))
                    start = -1
                depth = 0
    return out


def scan_file_const_subscript(path: Path) -> list[tuple[str, str, int, str]]:
    """Return [(key, expression, line, source_text)] for unguarded const
    operator[] reads. `key` is the baseline-table key."""
    try:
        src = path.read_text(errors='replace')
    except Exception:
        return []
    return scan_source_const_subscript(path.as_posix(), src)


def scan_source_const_subscript(rel: str, src: str) -> list[tuple[str, str, int, str]]:
    """scan_file_const_subscript's body, operating on already-sourced text -
    disk, or the STAGED blob a --staged-only caller reads instead."""
    # Cheap bail-out: every finding needs a string-literal subscript somewhere.
    # Do NOT also require the token `const` — the canonical shape acquires its
    # constness from a non-mutable lambda's by-value capture, which spells no
    # `const` at all.
    if '["' not in src:
        return []

    clean = clean_source(src)
    scopes = const_scopes(clean)
    if not scopes:
        return []
    blocks = _top_level_blocks(clean)
    src_lines = src.splitlines()

    def is_const(name: str, off: int) -> bool:
        base = name.split('.')[0].strip()
        return any(n == base and s <= off <= e for n, s, e in scopes)

    def block_of(off: int) -> tuple[int, int]:
        for s, e in blocks:
            if s <= off <= e:
                return (s, e)
        return (0, len(clean))

    proofs: dict[tuple[str, ...], list[int]] = {}
    for m in GUARD_RE.finditer(clean):
        root = re.sub(r'\s+', '', m.group('root'))
        keys = KEY_RE.findall(m.group('subs') or '')
        proofs.setdefault(tuple([root] + keys + [m.group('key')]), []).append(m.start())

    findings: list[tuple[str, str, int, str]] = []

    def line_of(off: int) -> int:
        return src.count('\n', 0, off) + 1

    def opted_out(ln: int) -> bool:
        text = src_lines[ln - 1] if 0 < ln <= len(src_lines) else ''
        return any(tok in text for tok in CONST_OPT_OUT)

    # -- 2a: string-key subscript on a const json. A const object subscripted
    #        with a string literal is nlohmann::json in practice; std::map and
    #        friends have no const operator[] at all, so this will not compile
    #        for any other type.
    for m in PATH_RE.finditer(clean):
        off = m.start()
        tail = clean[m.end():m.end() + 4].lstrip()
        if tail.startswith('=') and not tail.startswith('=='):
            continue  # assignment target => object is being mutated => not const
        root = re.sub(r'\s+', '', m.group('root'))
        if not is_const(root, off):
            continue
        keys = KEY_RE.findall(m.group('subs'))
        bs, _ = block_of(off)
        prefix = [root]
        for k in keys:
            if any(bs <= p < off for p in proofs.get(tuple(prefix + [k]), ())):
                prefix.append(k)
                continue
            ln = line_of(off)
            if not opted_out(ln):
                expr = '.'.join(prefix) + f'["{k}"]'
                findings.append((f'{rel}::{expr}', expr, ln,
                                 src_lines[ln - 1].strip() if ln <= len(src_lines) else ''))
            break

    # -- 2b: numeric / dynamic index on a const json. Out of range on the const
    #        overload is a raw std::vector::operator[] — SIGSEGV, no throw.
    for m in INDEX_RE.finditer(clean):
        off = m.start()
        root = re.sub(r'\s+', '', m.group('root'))
        if not is_const(root, off):
            continue
        pathexpr = root + re.sub(r'\s+', '', m.group('subs'))
        idx = re.sub(r'\s+', '', m.group('idx'))
        bs, _ = block_of(off)
        before = re.sub(r'\s+', '', clean[bs:off])
        pe = re.escape(pathexpr)
        if re.search(pe + r'\.(?:size|empty)\(', before):
            continue
        if re.search(pe + r'\.contains\(' + re.escape(idx) + r'\)', before):
            continue
        ln = line_of(off)
        if opted_out(ln):
            continue
        expr = f'{pathexpr}[{idx}]'
        findings.append((f'{rel}::{expr}', expr, ln,
                         src_lines[ln - 1].strip() if ln <= len(src_lines) else ''))

    return findings


def report_const_subscript(findings: list[tuple[str, str, int, str]],
                           summary: bool, full_scan: bool) -> int:
    observed: dict[str, int] = {}
    detail: dict[str, list[tuple[int, str, str]]] = {}
    for key, expr, ln, text in findings:
        observed[key] = observed.get(key, 0) + 1
        detail.setdefault(key, []).append((ln, expr, text))

    new_sites: list[str] = []
    for key, count in observed.items():
        if count > CONST_SUBSCRIPT_BASELINE.get(key, 0):
            new_sites.append(key)

    if new_sites:
        n_reads = sum(len(detail[k]) for k in new_sites)
        print(f"❌ Uncatchable const json operator[]: {n_reads} unguarded read(s) "
              f"at {len(new_sites)} new site(s).")
        for key in sorted(new_sites):
            for ln, expr, text in detail[key]:
                file = key.split('::', 1)[0]
                print(f"   {file}:{ln}: {expr} — const operator[] with no "
                      f"contains()/find()/size() guard")
                if not summary and text:
                    print(f"        | {text[:110]}")
        print()
        print("   NDEBUG is not defined in this build, so nlohmann's JSON_ASSERT is a")
        print("   live assert(): a missing key read through the CONST overload is an")
        print("   uncatchable SIGABRT (exit 134 — the watchdog-crash-loop signature),")
        print("   and an out-of-range numeric index is a SIGSEGV. A surrounding")
        print("   catch (const nlohmann::json::exception&) CANNOT fire. See 530fcea39.")
        print("   A json is const here because it is captured by value in a")
        print("   non-mutable lambda, bound to a `const auto&`, or a `const json&` param.")
        print("   Fix pattern:")
        print('     if (!resp.contains("result") || !resp["result"].contains("status"))')
        print('         return;')
        print('     const auto& status = resp["result"]["status"];')
        print("   Or make the read fallible: .value(), .at(), or find()+is_<type>().")
        print("   Suppress per-line: `// JSON_CONST_OK: <reason>`")
        return 1

    # Only meaningful on a full scan — under --staged-only or an explicit file
    # list, an unscanned file trivially contributes zero and would be reported
    # as "now fixed".
    stale = ([k for k, v in CONST_SUBSCRIPT_BASELINE.items() if observed.get(k, 0) < v]
             if full_scan else [])
    total = sum(observed.values())
    if stale:
        print(f"✅ Uncatchable const json operator[]: {total} "
              f"(baseline {sum(CONST_SUBSCRIPT_BASELINE.values())} — "
              f"{len(stale)} entr{'y' if len(stale) == 1 else 'ies'} now fixed, "
              f"please drop from CONST_SUBSCRIPT_BASELINE)")
        for k in sorted(stale):
            print(f"     stale baseline entry: {k}")
    else:
        print(f"✅ Uncatchable const json operator[]: {total} == baseline")
    return 0


def collect_files(args: argparse.Namespace, suffixes=('.cpp',)) -> Iterable[tuple[str, str]]:
    """Yield (path, text) for every file to scan, content already sourced.

    --staged-only reads each staged file's INDEX content - what the commit
    will contain, not whatever the working tree holds right now.
    """
    roots = ['src'] if suffixes == ('.cpp',) else ['src', 'include']
    if args.staged_only:
        # The baselines count only the roots a full scan walks, so a staged file
        # outside them (a test) would report its long-standing reads as new.
        for path, text in staged_files(suffixes=suffixes):
            if Path(path).parts[0] in roots:
                yield (path, text)
        return
    if args.files:
        for f in args.files:
            p = Path(f)
            if p.suffix in suffixes and p.exists():
                try:
                    yield (str(p), p.read_text())
                except OSError:
                    continue
        return
    seen: set[Path] = set()
    for root in roots:
        for suf in suffixes:
            for p in Path(root).rglob('*' + suf):
                if p in seen:
                    continue
                seen.add(p)
                try:
                    yield (str(p), p.read_text())
                except OSError:
                    continue


def run_rule1(args: argparse.Namespace) -> int:
    all_violations: list[str] = []
    for path, text in collect_files(args):
        all_violations.extend(scan_source(path, text))

    count = len(all_violations)

    # Baseline-mode: pre-existing violations are tolerated up to a ceiling that
    # ratchets down. New code that pushes count above the ceiling fails CI.
    if args.max_allowed is not None:
        if count > args.max_allowed:
            print(f"❌ Subscription null-safety: {count} violations exceeds baseline ({args.max_allowed}).")
            if not args.summary:
                # Show only the *new* violations isn't trivial without diff context;
                # show all so the dev can spot which file they touched.
                for v in all_violations:
                    print(f"   {v}")
            print(f"   New code introduced {count - args.max_allowed} violation(s).")
            print("   Background: feedback_moonraker_subscribed_null.md / f75b961d8.")
            print("   Fix pattern:")
            print("     auto it = obj.find(\"k\");")
            print("     if (it != obj.end() && it->is_number_integer()) v = it->get<int>();")
            print("   Or use helix::json::safe_int / safe_float / safe_bool / safe_string.")
            print("   Suppress per-line: `// JSON_NULL_SAFE: <reason>`")
            return 1
        if count < args.max_allowed:
            print(f"✅ Subscription null-safety: {count} (baseline {args.max_allowed} — please ratchet down)")
        else:
            print(f"✅ Subscription null-safety: {count} == baseline ({args.max_allowed})")
        return 0

    if all_violations:
        print("❌ Subscription null-safety violations:")
        for v in all_violations:
            print(f"   {v}")
        print()
        print(f"   {count} violation(s) found.")
        print("   Background: feedback_moonraker_subscribed_null.md / f75b961d8.")
        print("   Fix pattern:")
        print("     auto it = obj.find(\"k\");")
        print("     if (it != obj.end() && it->is_number_integer()) v = it->get<int>();")
        print("   Or use helix::json::safe_int / safe_float / safe_bool / safe_string.")
        return 1

    print("✅ Subscription handlers null-safe")
    return 0


def run_rule2(args: argparse.Namespace) -> int:
    findings: list[tuple[str, str, int, str]] = []
    for path, text in collect_files(args, suffixes=('.cpp', '.h')):
        findings.extend(scan_source_const_subscript(path, text))
    if args.const_subscript_list:
        for key, expr, ln, text in sorted(findings):
            print(f"{key.split('::', 1)[0]}:{ln}: {expr}")
            print(f"     | {text[:110]}")
        print(f"--- {len(findings)} const-subscript site(s)")
        return 0
    full_scan = not args.files and not args.staged_only
    return report_const_subscript(findings, args.summary, full_scan)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('files', nargs='*', help='Files to check (default: scan src/)')
    ap.add_argument('--staged-only', action='store_true', help='Check only staged files')
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='Pass if total rule-1 violations ≤ N (ratcheting baseline). '
                         'Default: fail on any.')
    ap.add_argument('--summary', action='store_true',
                    help='Print only summary count, not per-line violations')
    ap.add_argument('--rule', choices=('1', '2', 'all'), default='all',
                    help='1 = subscription-handler null safety, '
                         '2 = uncatchable const json operator[], all = both (default)')
    ap.add_argument('--const-subscript-list', action='store_true',
                    help='List every rule-2 site (baselined or not) and exit 0')
    args = ap.parse_args()

    repo_root = subprocess.run(
        ['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True, check=False,
    ).stdout.strip()
    if repo_root:
        import os
        os.chdir(repo_root)

    rc = 0
    if args.rule in ('1', 'all') and not args.const_subscript_list:
        rc |= run_rule1(args)
    if args.rule in ('2', 'all'):
        rc |= run_rule2(args)
    return rc


if __name__ == '__main__':
    sys.exit(main())
