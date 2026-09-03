#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generator: mirror LVGL's lv_event_code_t enum into the crash worker's
# code -> name table.
#
# The worker renders "code=42 (DELETE)" on every auto-filed crash issue. That
# label is often the whole diagnosis -- a DELETE on a widget names a teardown
# bug outright -- so a table that drifts from the enum does not merely look
# wrong, it points triage at the wrong subsystem. It drifted exactly that way:
# LVGL 9.5 inserted SINGLE/DOUBLE/TRIPLE_CLICKED and STATE_CHANGED, the
# hand-maintained table did not follow, and 58 of its 63 entries went stale
# while its own comment admitted the tests only spot-check a few codes.
#
# So the table is derived, not typed. The enum is the source of truth, the
# committed artifact is regenerated from it, and the gate proves the two match
# -- same contract as regen-tokens / regen-xml-schema.
#
# Usage:
#   gen_lvgl_event_codes.py            # rewrite the worker table in place
#   gen_lvgl_event_codes.py --check    # exit 1 if the table would change (CI)
#   gen_lvgl_event_codes.py --diff     # --check plus the changed lines
#
# Not emitted, deliberately:
#   - LV_EVENT_LAST: a count sentinel, never dispatched as a code
#   - LV_EVENT_PREPROCESS / LV_EVENT_MARKED_DELETING: bit flags OR'd onto a
#     code, not codes themselves. lvglEventCodeName() masks them off the way
#     lv_event_get_code() does (lib/lvgl/src/misc/lv_event.c:386), so they must
#     not collide with a table entry.

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ENUM_HEADER = os.path.join(REPO, 'lib', 'lvgl', 'src', 'misc', 'lv_event.h')
LV_CONF = os.path.join(REPO, 'lv_conf.h')
WORKER_TS = os.path.join(REPO, 'server', 'crash-worker', 'src', 'index.ts')

BEGIN = '  /* BEGIN GENERATED lv_event_code_t (scripts/gen_lvgl_event_codes.py) */'
END = '  /* END GENERATED lv_event_code_t */'

# Enumerators that carry a value but are not dispatchable event codes.
NOT_A_CODE = ('LV_EVENT_LAST', 'LV_EVENT_PREPROCESS', 'LV_EVENT_MARKED_DELETING')

ENUM_RE = re.compile(r'typedef enum\s*\{(.*?)\}\s*lv_event_code_t\s*;', re.S)
ENTRY_RE = re.compile(r'^(LV_EVENT_[A-Z0-9_]+)\s*(?:=\s*([0-9a-fA-Fx]+))?$')
COND_RE = re.compile(r'^#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)$')
DEFINE_RE = re.compile(r'^\s*#\s*define\s+(LV_[A-Z0-9_]+)\s+([0-9]+)\s*$', re.M)


def strip_comments(text):
    """Drop /* */ and // comments, including the multi-line /**< ... */ doc
    blocks LVGL hangs off enumerators."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def lv_conf_flags():
    """The build's own lv_conf.h, so a conditional enumerator is resolved the
    way the shipped binary numbers it rather than guessed."""
    if not os.path.exists(LV_CONF):
        return None
    return {m.group(1): int(m.group(2)) for m in DEFINE_RE.finditer(open(LV_CONF).read())}


def parse_enum(body, flags):
    """Walk the enum body, tracking value and #if state. Returns (code, NAME)
    pairs with the LV_EVENT_ prefix stripped."""
    out, value, skipping = [], 0, []
    for raw in strip_comments(body).splitlines():
        line = raw.strip()
        if not line:
            continue

        cond = COND_RE.match(line)
        if cond:
            kind, expr = cond.group(1), cond.group(2).strip()
            if kind == 'endif':
                if skipping:
                    skipping.pop()
            elif kind == 'else':
                if skipping:
                    skipping[-1] = not skipping[-1]
            elif kind in ('if', 'ifdef', 'elif'):
                sym = expr if kind != 'ifdef' else expr.split()[0] if expr else ''
                if not re.fullmatch(r'LV_[A-Z0-9_]+', sym):
                    raise SystemExit(
                        "gen_lvgl_event_codes: cannot evaluate '#%s %s' in lv_event_code_t.\n"
                        "  A conditional enumerator shifts every value after it, so guessing\n"
                        "  would silently mislabel codes. Teach this parser the condition."
                        % (kind, expr))
                skipping.append(flags.get(sym, 0) == 0)
            elif kind == 'ifndef':
                skipping.append(flags.get(expr.split()[0] if expr else '', 0) != 0)
            continue

        for part in (p.strip() for p in line.split(',')):
            if not part:
                continue
            entry = ENTRY_RE.match(part)
            if not entry:
                raise SystemExit(
                    "gen_lvgl_event_codes: unparsed enumerator %r in lv_event_code_t" % part)
            name, explicit = entry.group(1), entry.group(2)
            if explicit is not None:
                value = int(explicit, 0)
            if not any(skipping) and name not in NOT_A_CODE:
                out.append((value, name[len('LV_EVENT_'):]))
            value += 1
    return out


def render(pairs):
    """One line per entry, so a diff names the codes that moved."""
    width = max(len(str(code)) for code, _ in pairs)
    return '\n'.join('    %*d: "%s",' % (width, code, name) for code, name in pairs)


def rewrite(source, table):
    start = source.find(BEGIN)
    stop = source.find(END)
    if start < 0 or stop < 0:
        raise SystemExit('gen_lvgl_event_codes: markers missing from %s' % WORKER_TS)
    return source[:start] + BEGIN + '\n' + table + '\n' + source[stop:]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true',
                    help='do not write; exit 1 if the table would change')
    ap.add_argument('--diff', action='store_true',
                    help='implies --check; show the changed lines')
    args = ap.parse_args()

    if not os.path.exists(ENUM_HEADER):
        # Clean checkout without submodules: the artifact is committed, so
        # there is nothing to verify against and nothing to repair.
        print('⚠️  lib/lvgl not checked out — skipping LVGL event-code table')
        return 0

    flags = lv_conf_flags()
    if flags is None:
        print('⚠️  lv_conf.h not found — skipping LVGL event-code table')
        return 0

    enum = ENUM_RE.search(open(ENUM_HEADER).read())
    if not enum:
        raise SystemExit('gen_lvgl_event_codes: lv_event_code_t not found in %s' % ENUM_HEADER)

    pairs = parse_enum(enum.group(1), flags)
    if not pairs:
        raise SystemExit('gen_lvgl_event_codes: lv_event_code_t parsed empty')

    before = open(WORKER_TS).read()
    after = rewrite(before, render(pairs))

    if after == before:
        if args.check or args.diff:
            print('✅ LVGL event codes: worker table matches lv_event_code_t (%d codes)'
                  % len(pairs))
        else:
            print('✅ LVGL event codes: %d codes, already up to date' % len(pairs))
        return 0

    if args.check or args.diff:
        if args.diff:
            import difflib
            for line in difflib.unified_diff(before.splitlines(), after.splitlines(),
                                             'committed', 'lv_event_code_t', lineterm='', n=1):
                print('   %s' % line)
        print('❌ LVGL event codes: worker table has drifted from lv_event_code_t')
        print('   %s' % os.path.relpath(WORKER_TS, REPO))
        print('   Run: make regen-lvgl-event-codes')
        return 1

    open(WORKER_TS, 'w').write(after)
    print('✅ LVGL event codes: regenerated %d codes into %s'
          % (len(pairs), os.path.relpath(WORKER_TS, REPO)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
