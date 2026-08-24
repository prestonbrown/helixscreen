#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: bg-thread callbacks must not call `token.expired()` followed by
# `this`/`self`/`api_` member access on the bg thread itself. That's the L081
# Mechanism C anti-pattern (cluster:pstat-async-delete) — it races on `this`
# destruction and causes UAF crashes that look unrelated in backtraces.
#
# Background: AsyncLifetimeGuard's `token.expired()` is meant to be called
# inside `tok.defer(...)` (which marshals to the main thread atomically).
# Calling it bare on a bg thread, then dereferencing `this`/`self`/`api_`,
# is a TOCTOU race. The runtime detector emits "cluster:pstat-async-delete
# Mechanism C" warnings; this script catches the pattern at commit time so
# new instances fail CI immediately.
#
# Approved patterns:
#
#   // Direct defer — wrap bg work in defer, no expired() check needed.
#   api_->rest().get_x([this, tok = lifetime_.token()](const Resp& r) {
#       tok.defer("Class::on_x", [this, r]() { member_ = r; });
#   });
#
#   // bg_cb wrapper — even cleaner.
#   api_->rest().get_x(lifetime_.bg_cb("Class::on_x",
#                                      [this](const Resp& r) { member_ = r; }));
#
#   // Bg-thread expired check that ONLY exits the lambda, no member touch:
#   if (tok.expired()) return;   // followed only by `return;` or local-only work
#
# Banned (without // L081_OK comment):
#
#   [this, tok](const Resp& r) {
#       if (tok.expired()) return;   // bg-thread check
#       member_ = r;                  // bg-thread member mutation — UAF risk
#   }
#
#   // Same race, spelled through ThumbnailLoadContext. `ctx.is_valid()` calls
#   // `lifetime_token->expired()` internally, so this is the form above with
#   // an extra layer — not a safer alternative to it.
#   [self, ctx](const std::string& error) {
#       if (!ctx.is_valid()) return;  // bg-thread check
#       spdlog::warn("[{}] ...", self->get_name(), error);   // UAF risk
#   }
#
# Per-line opt-out:
#
#   if (tok.expired()) return; // L081_OK: synchronous wait wrapper, see write_adventurer_json
#
# Usage:
#   ./scripts/check_l081_anti_pattern.py [files...]
#   ./scripts/check_l081_anti_pattern.py --staged-only
#   (no args = scan src/ recursively)

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

# Match `if (token.expired()) return;` or variants:
#   if (tok.expired()) return;
#   if (token.expired()) { return; }
#   if (token.expired() || other_cond) return;
#   if (token && token->expired()) return;           // optional<LifetimeToken>
#   if (auto t = wp.lock(); t && t->expired()) ...   // shared_ptr<LifetimeToken>
# Both `.expired()` and `->expired()` access patterns. The runtime detector
# fires on either (it lives inside LifetimeToken::expired() itself); the lint
# previously only matched `.expired()`, letting optional/shared_ptr<Token>
# callsites in AfcConfigManager etc. evade gating until telemetry surfaced them.
# `expired_no_lvgl()` is the audited-callsite spelling (see
# include/async_lifetime_guard.h). It skips the *runtime* report, so this
# static gate is the only thing left watching those sites — match it here or
# renaming a call becomes a way to opt out of both checks at once.
EXPIRED_CHECK_RE = re.compile(
    r'\bif\s*\([^)]*\b(?P<varname>[a-zA-Z_]\w*)\s*(?:\.|->)\s*'
    r'expired(?:_no_lvgl)?\s*\(\)[^)]*\)'
)

# `ThumbnailLoadContext::is_valid()` is `LifetimeToken::expired()` wearing a
# different name — read include/thumbnail_load_context.h:71, it calls
# `lifetime_token->expired()` directly. So `if (!ctx.is_valid()) return;` on a
# background thread is the identical TOCTOU, and matching only `expired()` let
# five of them accumulate in ui_panel_print_select.cpp's thumbnail callbacks,
# each one dereferencing `self` on an HttpExecutor worker (#960, bundle
# 6F3QJLFG). The context's own header documented the bare-check form as the
# usage example, so they were written to spec.
#
# Scoped by RECEIVER NAME rather than by method name. `is_valid()` is a common
# predicate in this tree — themes, calibration results, gcode index entries and
# stream sources all have one, ~25 call sites that have nothing to do with
# lifetimes. Every ThumbnailLoadContext is named `ctx`/`context`/`*_ctx` by
# convention, and that convention is what makes this precise instead of noisy.
# A context under some other name is missed; the runtime detector remains the
# source of truth, exactly as for the `expired()` forms above.
CTX_VALID_CHECK_RE = re.compile(
    r'\bif\s*\([^)]*\b(?P<varname>ctx|context|\w+_ctx)\s*(?:\.|->)\s*is_valid\s*\(\)[^)]*\)'
)

# Both spellings of the same bg-thread lifetime gate.
LIFETIME_CHECK_RES = (EXPIRED_CHECK_RE, CTX_VALID_CHECK_RE)

# Patterns that count as "this access" on the line(s) following the expired check.
# Conservative — flags only obvious member dereferences. Misses some implicit
# `this->method(args)` calls that look like free-function calls; that's why
# the runtime detector remains the source of truth.
THIS_ACCESS_RE = re.compile(
    r'\b(?:this|self|api_)\s*(?:->|\.)'  # this->, self->, api_->
    r'|\bemit_event\s*\('                 # member fn (idiomatic in this repo)
    r'|\bget_name\s*\('                   # virtual member fn
    r'|\b\w+_\s*(?:=|\.|\->)'             # member-trailing-underscore deref/assign
    r'|\b\w+_\s*\.\s*(?:store|load|exchange|fetch_add|fetch_sub|notify_one|notify_all)\s*\('
    r'|\b\w+_mutex_\b'                    # member mutex name pattern
    r'|\block_guard\s*<\s*std::mutex\s*>\s*\w+\s*\(\s*\w*_'  # locking a member mutex
)

OPT_OUT = "L081_OK"
LOOKAHEAD_LINES = 10  # max lines to scan after the expired check inside the lambda

# Mechanism D opt-out (different name so it can't be confused with Mechanism C):
# `// L081_FREEZE_OK: <reason>` on the defer/queue_update line silences this check.
OPT_OUT_FREEZE = "L081_FREEZE_OK"

# Window to scan inside a register_method_callback / register_notify_update for
# the inner defer. Most callbacks fit in this many lines; longer ones generally
# split into helper methods and the helper itself will be flagged on its own.
FREEZE_OUTER_LOOKAHEAD = 80

# Within a tok.defer / queue_update body, how many lines to scan for a subject
# mutation that would justify making it critical.
FREEZE_INNER_LOOKAHEAD = 25

# A "register" entry point — the WS-thread notification subscription path.
# These are the only sites where a freeze drop strands foundational state at
# startup; widget code and one-shot RPC callbacks have other safety nets.
REGISTER_CB_RE = re.compile(
    r'\b(?:register_method_callback|register_notify_update)\s*\('
)

# Mechanism D (HTTP variant): an HTTP request via the api_ wrapper. The
# success callback runs on the HTTP worker thread, so a `tok.defer(...)`
# inside it lands on UpdateQueue — and gets dropped during scoped_freeze.
# When the HTTP fetch fires once at startup (first metadata, first history,
# first WLED discovery, first PRINT_START analysis), the dropped result
# strands the UI until the next user-triggered fetch — which for fire-once
# bootstraps never comes. Confirmed regression sweep on 2026-05-11 found
# 4 sites matching this pattern across PrintHistoryManager, PrintSelectPanel,
# MacroModificationManager, and LedController.
#
# We match the most common entry shapes:
#   api_->files().get_*(...) / get_metadata / metascan_file / etc.
#   api_->history().get_*(...)
#   api_->queue().get_*(...)
#   api_->rest().get_*(...) / post_*
#   api_->transfers().download_*(...)
# plus a few helpers that themselves wrap api_ calls:
#   analyzer_.analyze(api_, ...)
#   wled_.fetch_*(...)
HTTP_CB_ENTRY_RE = re.compile(
    r'\b(?:api_|api)\s*->\s*(?:files|history|queue|rest|transfers|server|machine|'
    r'authorization|database|update_manager|announcements|webcam|spoolman)'
    r'\s*\(\s*\)\s*\.\s*\w+\s*\('
    r'|\banalyzer_\s*\.\s*analyze\s*\('
    r'|\bwled_\s*\.\s*fetch_\w+\s*\('
)

# Plain (non-critical) defer/queue_update inside a register lambda.
# Match `tok.defer(`, `token.defer(`, `lifetime_.defer(`, and
# `helix::ui::queue_update(` — but NOT their `_critical` variants. We
# intentionally exclude observe_int_sync / observe_string callbacks because
# those already fire on the main thread (no marshal needed).
PLAIN_DEFER_RE = re.compile(
    r'\b(?:tok|token|lifetime_)\s*\.\s*defer\s*\('
    r'|\bhelix::ui::queue_update\s*\('
)

# Anything that looks like writing to an lv_subject. Conservative — we miss
# wrapper setter methods, but those usually delegate to one of these eventually
# and the runtime fix (use queue_critical at the dispatch site) covers both.
SUBJECT_MUTATION_RE = re.compile(
    r'\blv_subject_(?:set|copy)_\w+\s*\('
    r'|\bbump_\w+_version\s*\('               # AmsState pattern
    r'|\bsync_backend\s*\('                   # AmsState entry point
    r'|\bemit_event\s*\('                     # backend → AmsState mirror trigger
)


def file_lines(path: Path) -> list[str]:
    try:
        return path.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return []


def _blank_noise(src: str) -> str:
    """Blank out comments and string/char literals, preserving every offset and
    newline so line numbers still map back to the original source.

    Same helper as check_raw_this_queue_update.py. Without it a comment that
    merely *describes* the anti-pattern — including the fix advice this script
    prints — is reported as a violation.
    """
    out = list(src)
    n = len(src)
    i = 0
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
            continue
        if c in '"\'':
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == c or src[j] == '\n':
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
            continue
        i += 1
    return ''.join(out)


def code_lines(path: Path) -> list[str]:
    """`file_lines` with comments and literals blanked, line-for-line aligned.

    Match patterns against these; keep using the raw lines for the `L081_OK`
    opt-out (which lives in a comment) and for the snippet shown to the user.
    """
    try:
        return _blank_noise(path.read_text(encoding='utf-8', errors='replace')).splitlines()
    except OSError:
        return []


def is_only_return(line: str) -> bool:
    """True if the line after the expired-check predicate is just `return;` (with optional braces)."""
    s = line.strip()
    return s in ('return;', '{ return; }', '{return;}', '{ return; }')


def trailing_after_paren(line: str) -> str:
    """Return whatever follows the closing `)` of the if-condition on the same line."""
    # naive: find first ')' after 'expired()' — fine for the codebase's style
    idx = line.find('expired_no_lvgl()')
    if idx < 0:
        idx = line.find('expired()')
    if idx < 0:
        return ''
    rest = line[idx:]
    paren_depth = 0
    for i, ch in enumerate(rest):
        if ch == '(':
            paren_depth += 1
        elif ch == ')':
            paren_depth -= 1
            if paren_depth == 0:
                return rest[i + 1:].strip()
    return ''


DEFER_CALL_RE = re.compile(r'\b\w+\s*\.\s*defer(?:_critical)?\s*\(')


def scan_file(path: Path, checks: tuple = LIFETIME_CHECK_RES) -> list[tuple[int, str, str]]:
    """Return list of (line_no, snippet, kind) for each suspect site in `path`.

    Two patterns flagged (both fire the runtime `bg_tok_expired_check` warning
    when reached on a bg thread):

    - **UAF** (`kind="uaf"`): bg-thread `tok.expired()` followed by direct
      `this`/member access BEFORE any `tok.defer(...)`. Real
      use-after-free risk if the owner is destroyed between check and access.

    - **REDUNDANT** (`kind="redundant"`): bg-thread `tok.expired()` immediately
      followed by `tok.defer(...)` with no member access in between. Not a
      UAF, but the bare check is dead code — `tok.defer()` already performs an
      atomic expiration check on the main thread. The bare check trips the
      runtime detector for nothing. Swept across the codebase 2026-05-13.

    The `// L081_OK: <reason>` per-line opt-out silences both forms.
    """
    lines = file_lines(path)
    if not lines:
        return []
    # Patterns match against `code` (comments/literals blanked); `lines` stays
    # raw so the L081_OK opt-out and the reported snippet still read correctly.
    code = code_lines(path)
    hits: list[tuple[int, str, str, str]] = []
    for i, line in enumerate(lines):
        if OPT_OUT in line:
            continue
        # Remember WHICH spelling matched so the report can quote the guard the
        # author actually wrote — telling someone their `ctx.is_valid()` is a
        # "bare tok.expired()" reads as a false positive and gets dismissed.
        matched = next((r for r in checks if r.search(code[i])), None)
        if matched is None:
            continue
        spelling = ('ctx.is_valid()' if matched is CTX_VALID_CHECK_RE
                    else 'tok.expired()')
        # If the rest of the same line is just `return;`, the lambda body
        # is a no-op past the gate — neither UAF nor redundant-with-defer.
        # The check might still trip the runtime detector, but the lint
        # only flags forms that have a clearer fix path.
        same_line_after = trailing_after_paren(code[i])
        if same_line_after.startswith('return'):
            continue
        # Look ahead a few lines INSIDE the lambda for either a `.defer(`
        # call (REDUNDANT) or a `this`/member access (UAF). Stop at obvious
        # block-end markers so we don't bleed into the enclosing scope.
        for j in range(i + 1, min(i + 1 + LOOKAHEAD_LINES, len(lines))):
            if OPT_OUT in lines[j]:
                break
            look = code[j]
            stripped = look.strip()
            if stripped == '':
                continue
            if stripped in ('}', '});', '},', '});;'):
                break
            if DEFER_CALL_RE.search(look):
                snippet = '\n  '.join(lines[i:j + 1])
                hits.append((i + 1, snippet, 'redundant', spelling))
                break
            if THIS_ACCESS_RE.search(look):
                snippet = '\n  '.join(lines[i:j + 1])
                hits.append((i + 1, snippet, 'uaf', spelling))
                break
    return hits


def scan_file_freeze_drop(path: Path) -> list[tuple[int, str]]:
    """Return list of (line_no, snippet) for each Mechanism D suspect in `path`.

    Pattern flagged: a `register_method_callback(...)` or
    `register_notify_update(...)` lambda whose body uses plain `tok.defer(...)` /
    `queue_update(...)` (NOT `*_critical`) to dispatch a callback that mutates
    an lv_subject. Such dispatches are silently dropped during
    `UpdateQueue::scoped_freeze()` — if the dropped event is a first-fire
    baseline establishment (Klipper subscription frame, notify_klippy_ready,
    etc.) the subject stays at its default forever and any widget observing
    it goes stale.

    Heuristic, with intentional false-positive tolerance. Per-line opt-out:
    `// L081_FREEZE_OK: <reason>` on the defer/queue_update line.

    Companion to scan_file (Mechanism C, bg-thread member access).
    """
    lines = file_lines(path)
    if not lines:
        return []
    hits: list[tuple[int, str]] = []
    n = len(lines)
    i = 0
    while i < n:
        line = lines[i]
        if not REGISTER_CB_RE.search(line):
            i += 1
            continue
        # Found a notification-subscription registration. Scan the next
        # FREEZE_OUTER_LOOKAHEAD lines (or until the call's enclosing ';')
        # for non-critical defers. Track brace/paren depth roughly so we
        # don't bleed past the registration.
        end = min(i + FREEZE_OUTER_LOOKAHEAD, n)
        depth = 0
        saw_open = False
        scanned_to = i
        for j in range(i, end):
            scanned_to = j
            for ch in lines[j]:
                if ch == '(':
                    depth += 1
                    saw_open = True
                elif ch == ')':
                    depth -= 1
            if saw_open and depth <= 0 and ';' in lines[j]:
                break
        outer_end = scanned_to
        # Within the lambda body, look for plain defers.
        k = i
        while k <= outer_end:
            defer_line = lines[k]
            # Opt-out can be on the defer line OR in the 3 lines above (block
            # comment style). Looking ahead is handled by the inner-body scan.
            preceding_opt_out = any(
                OPT_OUT_FREEZE in lines[m] for m in range(max(0, k - 3), k)
            )
            if (PLAIN_DEFER_RE.search(defer_line)
                    and OPT_OUT_FREEZE not in defer_line
                    and not preceding_opt_out):
                # Validate that the next FREEZE_INNER_LOOKAHEAD lines contain a
                # subject mutation. Without that, it's likely a benign dispatch
                # (e.g., kicking off an HTTP fetch with no subject side effect).
                inner_end = min(k + FREEZE_INNER_LOOKAHEAD, outer_end + 1)
                mutates = False
                for m in range(k, inner_end):
                    if OPT_OUT_FREEZE in lines[m]:
                        # Annotation can also live on the inner subject-mutation line.
                        mutates = False
                        break
                    if SUBJECT_MUTATION_RE.search(lines[m]):
                        mutates = True
                        break
                if mutates:
                    snippet = '\n  '.join(lines[k:min(k + 6, n)])
                    hits.append((k + 1, snippet))
            k += 1
        i = outer_end + 1
    return hits


def scan_file_http_cb_freeze_drop(path: Path) -> list[tuple[int, str]]:
    """Return list of (line_no, snippet) for each Mechanism D (HTTP variant) site.

    Pattern: an HTTP request via `api_->...()` whose success/error lambda body
    uses plain `tok.defer(...)` / `queue_update(...)` (NOT `*_critical`). The
    success callback runs on the HTTP worker thread, so the defer routes
    through UpdateQueue and gets dropped during `scoped_freeze()`. For first-
    fire baseline-state fetches (metadata, history, WLED discovery, PRINT_START
    analysis) that drop permanently strands the UI.

    Unlike the subscription-callback variant, we DON'T require a subject
    mutation in the deferred body — most HTTP cb defers mutate member state
    that backs widgets indirectly (file_list_, cached_analysis_, etc.) rather
    than touching subjects directly. The lint surface is wider as a result;
    use per-line opt-out for sites where the work is genuinely safe to drop
    (user-initiated retry-on-demand, etc.).

    Per-line opt-out: `// L081_FREEZE_OK: <reason>` on the defer/queue_update
    line.

    Heuristic with intentional false-positive tolerance.
    """
    lines = file_lines(path)
    if not lines:
        return []
    hits: list[tuple[int, str]] = []
    n = len(lines)
    i = 0
    while i < n:
        line = lines[i]
        if not HTTP_CB_ENTRY_RE.search(line):
            i += 1
            continue
        # Scan forward for the matching ');' that closes this HTTP call.
        end = min(i + FREEZE_OUTER_LOOKAHEAD, n)
        depth = 0
        saw_open = False
        scanned_to = i
        for j in range(i, end):
            scanned_to = j
            for ch in lines[j]:
                if ch == '(':
                    depth += 1
                    saw_open = True
                elif ch == ')':
                    depth -= 1
            if saw_open and depth <= 0 and ';' in lines[j]:
                break
        outer_end = scanned_to
        # Within the call body (which contains the cb lambdas), look for
        # plain (non-critical) defers.
        k = i
        while k <= outer_end:
            defer_line = lines[k]
            preceding_opt_out = any(
                OPT_OUT_FREEZE in lines[m] for m in range(max(0, k - 3), k)
            )
            if (PLAIN_DEFER_RE.search(defer_line)
                    and OPT_OUT_FREEZE not in defer_line
                    and not preceding_opt_out):
                snippet = '\n  '.join(lines[k:min(k + 4, n)])
                hits.append((k + 1, snippet))
            k += 1
        i = outer_end + 1
    return hits


def staged_files() -> list[Path]:
    try:
        out = subprocess.run(
            ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACM'],
            capture_output=True, text=True, check=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []
    return [Path(p) for p in out.splitlines() if p.endswith(('.cpp', '.h', '.hpp', '.cc'))]


# Directories scanned for Mechanism C (bg-thread tok.expired() + member access).
# src/ui/ is intentionally excluded because observer callbacks (queue_update +
# observe_int_sync) run on the main thread, where `tok.expired()` is fine —
# the script can't tell observer cbs from HTTP cbs without AST-level context.
DEFAULT_SCAN_DIRS = (
    'src/printer',
    'src/calibration',
    'src/led',
    'src/print',
    'src/sensors',
    'src/system',
    'src/api',
    'src/network',
    'src/bluetooth',
)

# Mechanism D (freeze-drop) is restricted by REGISTER_CB_RE to register_method_callback
# / register_notify_update entry points, which are inherently bg-thread WS handlers.
# That restriction makes scanning src/ui/ safe — observer-cb false positives don't
# apply. Files like src/ui/job_queue_state.cpp register notification handlers and
# must be linted for Mechanism D even though they're excluded from Mechanism C.
FREEZE_SCAN_DIRS = DEFAULT_SCAN_DIRS + ('src/ui',)

# Directories scanned for the `ctx.is_valid()` spelling ONLY. The exclusion of
# src/ui/ above is about generic `tok.expired()`, whose false-positive case is a
# main-thread observer callback. That case cannot arise here: a
# ThumbnailLoadContext exists only to be handed to ThumbnailCache::fetch_*, and
# those callbacks are invoked from an HttpExecutor worker or a
# ThumbnailProcessor pool thread — never from an observer. So the pattern is
# unambiguously background wherever it appears, and src/ui/ is exactly where it
# appears (5 of the 7 sites in the tree). Excluding src/ui/ from this spelling
# too is what kept ui_panel_print_select.cpp's callbacks unlinted (#960).
CTX_ONLY_SCAN_DIRS = ('src/ui',)


def discover_files(roots: Iterable[Path]) -> list[Path]:
    out: list[Path] = []
    for root in roots:
        if root.is_file():
            out.append(root)
            continue
        if not root.is_dir():
            continue
        for p in root.rglob('*'):
            if p.suffix in ('.cpp', '.h', '.hpp', '.cc') and 'build/' not in str(p):
                out.append(p)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--staged-only', action='store_true',
                        help='Check only staged-for-commit files (for pre-commit hook)')
    parser.add_argument('--http-cb-audit', action='store_true',
                        help='Also run the HTTP response callback freeze-drop audit '
                             '(broad heuristic — surfaces candidates that may need '
                             'defer_critical or an L081_FREEZE_OK opt-out). Opt-in '
                             'because it returns ~100 hits today; intended for '
                             'periodic manual review, not as a CI gate.')
    parser.add_argument('files', nargs='*', help='Files to check (default: scan src/)')
    args = parser.parse_args()

    if args.staged_only:
        staged = staged_files()
        mech_c_files = [f for f in staged
                        if f.exists()
                        and any(str(f).startswith(d) for d in DEFAULT_SCAN_DIRS)]
        ctx_only_files = [f for f in staged
                          if f.exists()
                          and any(str(f).startswith(d) for d in CTX_ONLY_SCAN_DIRS)]
        freeze_files = [f for f in staged
                        if f.exists()
                        and any(str(f).startswith(d) for d in FREEZE_SCAN_DIRS)]
    elif args.files:
        # An explicitly named file is checked for every pattern regardless of
        # directory — naming it is the request. Keeps `<gate> src/ui/foo.cpp`
        # useful for spot-checks without loosening the default scan.
        files = [Path(f) for f in args.files if Path(f).exists()]
        mech_c_files = files
        ctx_only_files = []
        freeze_files = files
    else:
        mech_c_files = discover_files([Path(d) for d in DEFAULT_SCAN_DIRS])
        ctx_only_files = discover_files([Path(d) for d in CTX_ONLY_SCAN_DIRS])
        freeze_files = discover_files([Path(d) for d in FREEZE_SCAN_DIRS])

    # Skip the detector implementation itself and the header that defines the API.
    skip = {Path('src/system/async_lifetime_guard.cpp'),
            Path('include/async_lifetime_guard.h')}
    mech_c_files = [f for f in mech_c_files if f not in skip]
    freeze_files = [f for f in freeze_files if f not in skip]

    total = 0
    scan_plan = ([(f, LIFETIME_CHECK_RES) for f in mech_c_files]
                 + [(f, (CTX_VALID_CHECK_RE,)) for f in ctx_only_files])
    for f, checks in scan_plan:
        hits = scan_file(f, checks)
        for line_no, snippet, kind, spelling in hits:
            if kind == 'uaf':
                print(f"{f}:{line_no}: L081 Mechanism C: bg-thread {spelling} "
                      f"followed by `this`/member access")
            else:  # 'redundant'
                print(f"{f}:{line_no}: L081 Mechanism C (redundant guard): bg-thread "
                      f"{spelling} immediately before tok.defer() — bare check is "
                      f"dead code and trips the runtime detector")
            for ln in snippet.split('\n'):
                print(f"    {ln}")
            print(f"    Fix: drop the bare `if (X.expired()) return;` line — "
                  f"`X.defer(...)` already performs an atomic expiration check on the "
                  f"main thread. For non-defer bodies, wrap the work in "
                  f"`tok.defer(\"Class::method\", [this, ...]() {{ ... }})` or "
                  f"`lifetime_.bg_cb(\"Class::method\", [this](...) {{ ... }})`.")
            print(f"    Opt-out (only if you really need bg-thread `expired()`): "
                  f"add `// {OPT_OUT}: <reason>` on the same line.")
            print()
            total += 1

    # Mechanism D (freeze-drop) is obsolete: UpdateQueue now buffers callbacks
    # while frozen and splices them into pending_ when the freeze releases, so
    # there are no silent drops to detect. The Mechanism D / HTTP-variant scan
    # helpers (scan_file_freeze_drop, scan_file_http_cb_freeze_drop) are kept
    # in the source for now in case the buffer-not-drop behavior is ever rolled
    # back, but they are no longer invoked. The --http-cb-audit flag is also
    # a no-op.
    _ = args.http_cb_audit
    _ = freeze_files

    if total > 0:
        print(f"❌ {total} L081 anti-pattern site(s) found. See "
              f"include/async_lifetime_guard.h for the canonical fix pattern.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
