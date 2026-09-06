#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: XML-owned widgets must not be driven imperatively from C++.
#
# HelixScreen's UI contract is DATA in C++, APPEARANCE in XML, subjects connecting
# them (.claude/rules/declarative-ui.md). The signature of a violation
# is specific and detectable: a widget fetched out of an XML tree with
# lv_obj_find_by_name() is then mutated with an imperative setter instead of being
# driven by a subject binding.
#
# Flagged (on a widget that came from lv_obj_find_by_name):
#   lv_label_set_text/_fmt(w, ...)          → bind_text="subject"
#   lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN)  → <bind_flag_if_eq> / <if cond=...>
#   lv_obj_set_style_*(w, ...)              → XML style attr or bind_style
#   lv_obj_add_event_cb(w, cb, LV_EVENT_*)  → <event_cb trigger=... callback=.../>
#
# NOT flagged:
#   - Files that call lv_xml_register_widget(): the file IS the implementation of
#     a custom XML widget, so its internals have no XML to bind to. Structural.
#   - Widgets created in C++ (lv_*_create): procedural/canvas rendering, no XML.
#   - Structural events with no declarative equivalent: DELETE cleanup, draw hooks,
#     size/layout response, gestures and scrolling.
#   - Any line carrying a `// DECLARATIVE_OK: <reason>` comment.
#
# This is a RATCHET, not a wall. ~1400 imperative sites predate the gate: some were
# deliberate pragmatism from when the XML engine couldn't express what was needed,
# some are plain mistakes. Both are debt. The baseline freezes today's count so the
# debt can only shrink — new violations fail the build, and porting a site lowers
# the number.
#
# Usage:
#   check_imperative_ui.py                      # fail on any violation
#   check_imperative_ui.py --max-allowed 412     # ratcheting baseline
#   check_imperative_ui.py --summary             # counts only
#   check_imperative_ui.py --list                # every site, file:line
#   check_imperative_ui.py --rule text           # one rule only

import argparse
import os
import re
import sys

SCAN_DIRS = ['src', 'include']
SCAN_EXTS = ('.cpp', '.cc', '.h', '.hpp')

OPT_OUT = 'DECLARATIVE_OK'

# A file registering a custom XML widget is exempt wholesale — it builds the widget
# that XML instantiates, so there is no XML layer beneath it to bind against.
STRUCTURAL_FILE_MARKER = 'lv_xml_register_widget'

# Files whose entire purpose is imperative widget manipulation. The remote-control
# server implements `helix-screen ctl set_value/click/...`, which by definition
# reaches into an arbitrary live widget tree on command; there is no subject to bind.
STRUCTURAL_FILES = {
    'src/remote/remote_control_server.cpp',
}

# Events with no declarative equivalent in the XML engine.
STRUCTURAL_EVENTS = {
    'LV_EVENT_DELETE',              # RAII cleanup — explicitly allowed
    'LV_EVENT_DRAW_MAIN',           # custom draw
    'LV_EVENT_DRAW_MAIN_BEGIN',
    'LV_EVENT_DRAW_MAIN_END',
    'LV_EVENT_DRAW_POST',
    'LV_EVENT_DRAW_POST_BEGIN',
    'LV_EVENT_DRAW_POST_END',
    'LV_EVENT_DRAW_TASK_ADDED',
    'LV_EVENT_REFR_EXT_DRAW_SIZE',
    'LV_EVENT_SIZE_CHANGED',        # measured layout response
    'LV_EVENT_LAYOUT_CHANGED',
    'LV_EVENT_STYLE_CHANGED',
    'LV_EVENT_SCROLL',              # gesture / scroll handling
    'LV_EVENT_SCROLL_BEGIN',
    'LV_EVENT_SCROLL_END',
    'LV_EVENT_SCROLL_THROW_BEGIN',
    'LV_EVENT_GESTURE',
    'LV_EVENT_PRESSING',
    'LV_EVENT_PRESS_LOST',
    'LV_EVENT_LONG_PRESSED',
    'LV_EVENT_LONG_PRESSED_REPEAT',
}

# Widgets fetched out of an XML tree.
XML_LOOKUP_RE = re.compile(
    r'(?:^|[^\w.>])(\w+)\s*=\s*(?:lv_obj_find_by_name|helix::ui::find_by_name)\s*\(')

# Widgets built in C++ — procedural, not XML-owned.
CPP_CREATE_RE = re.compile(r'(?:^|[^\w.>])(\w+)\s*=\s*lv_\w+_create\s*\(')

# Appearance properties belong in XML (style_bg_color="#card_bg") or a bind_style.
# Geometry and layout properties are deliberately NOT listed: CLAUDE.md rule 10 keeps
# measured layout in C++, because a decision made from runtime pixel measurements
# (decide_nozzle_layout(), breakpoint-computed fonts) has no XML expression. Adding
# flex_*/pad_*/size/align here would flag correct code and get the gate switched off.
APPEARANCE_PROPS = (
    'bg_color|bg_opa|bg_grad_color|bg_grad_dir|bg_image_src|bg_image_opa|'
    'text_color|text_opa|text_decor|'
    'border_color|border_opa|border_width|border_side|'
    'outline_color|outline_opa|outline_width|'
    'shadow_color|shadow_opa|shadow_width|shadow_spread|'
    'radius|opa|opa_layered|image_recolor|image_recolor_opa|arc_color|line_color'
)

RULES = {
    'text':       re.compile(r'\blv_label_set_text(?:_fmt|_static)?\s*\(\s*([\w\.\->\[\]]+)'),
    'visibility': re.compile(r'\blv_obj_(?:add|remove)_flag\s*\(\s*([\w\.\->\[\]]+)\s*,\s*'
                             r'[^)]*LV_OBJ_FLAG_HIDDEN'),
    'style':      re.compile(r'\blv_obj_set_style_(?:' + APPEARANCE_PROPS +
                             r')\s*\(\s*([\w\.\->\[\]]+)'),
    'event':      re.compile(r'\blv_obj_add_event_cb\s*\(\s*([\w\.\->\[\]]+)'),
}

FIX = {
    'text':       'bind_text="subject" on the XML element',
    'visibility': '<bind_flag_if_eq subject=... flag="hidden"> or <if cond=...>',
    'style':      'XML style attribute (style_bg_color="#card_bg") or bind_style',
    'event':      '<event_cb trigger="clicked" callback="name"/> + lv_xml_register_event_cb()',
}


def base_ident(expr):
    """Reduce `data->label_[i]` to `label_` so it can be matched against tracked names."""
    expr = expr.split('[')[0]
    for sep in ('->', '.'):
        if sep in expr:
            expr = expr.split(sep)[-1]
    return expr


def event_of(src, start):
    """First LV_EVENT_* token within the add_event_cb call (may span lines)."""
    window = src[start:start + 400]
    depth = 0
    for i, ch in enumerate(window):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                window = window[:i]
                break
    m = re.search(r'\bLV_EVENT_[A-Z_]+', window)
    return m.group(0) if m else None


# Start of a top-level function body: a line beginning in column 0 that ends in `{`
# and looks like a signature. Good enough to keep a helper's `btn` parameter from
# colliding with an unrelated `btn` looked up in a different function.
FUNC_START_RE = re.compile(r'^[A-Za-z_][\w:<>,\s\*&~]*\([^;]*\)\s*(?:const\s*)?'
                           r'(?:noexcept\s*)?\{\s*$', re.MULTILINE)


def function_spans(src):
    """[(start, end)] byte ranges of top-level function bodies, plus a whole-file
    fallback span for code that doesn't parse into functions (macros, lambdas at
    file scope, class-body inline definitions)."""
    starts = [m.start() for m in FUNC_START_RE.finditer(src)]
    if not starts:
        return [(0, len(src))]
    spans = []
    for i, s in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(src)
        spans.append((s, end))
    # code before the first function still needs a home
    if starts[0] > 0:
        spans.append((0, starts[0]))
    return spans


def scan_file(path):
    try:
        src = open(path, errors='ignore').read()
    except OSError:
        return []

    if STRUCTURAL_FILE_MARKER in src:
        return []
    if path.replace(os.sep, '/') in STRUCTURAL_FILES:
        return []

    lines = src.split('\n')
    hits = []

    # Track XML-owned names per function, so a parameter in one function can't be
    # confused with a looked-up widget in another.
    for start, end in function_spans(src):
        body = src[start:end]
        xml_owned = set(XML_LOOKUP_RE.findall(body))
        if not xml_owned:
            continue
        # A name also assigned from a C++ create in this scope is ambiguous — drop it
        # rather than risk a false positive.
        xml_owned -= set(CPP_CREATE_RE.findall(body))
        if not xml_owned:
            continue

        for rule, pattern in RULES.items():
            for m in pattern.finditer(body):
                target = base_ident(m.group(1))
                if target not in xml_owned:
                    continue
                # only count a use that follows its lookup
                lookup_pos = min((lm.start() for lm in XML_LOOKUP_RE.finditer(body)
                                  if lm.group(1) == target), default=None)
                if lookup_pos is None or m.start() < lookup_pos:
                    continue
                lineno = src.count('\n', 0, start + m.start()) + 1
                line = lines[lineno - 1]
                if OPT_OUT in line:
                    continue
                # allow the opt-out on the line above, for multi-line calls
                if lineno >= 2 and OPT_OUT in lines[lineno - 2]:
                    continue
                if rule == 'event':
                    ev = event_of(body, m.start())
                    if ev is None or ev in STRUCTURAL_EVENTS:
                        continue
                hits.append((path, lineno, rule, line.strip()[:100]))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='Pass if total violations <= N (ratcheting baseline). '
                         'Default: fail on any.')
    ap.add_argument('--summary', action='store_true', help='Per-rule counts only')
    ap.add_argument('--list', action='store_true', help='Print every site')
    ap.add_argument('--rule', choices=sorted(RULES), help='Restrict to one rule')
    ap.add_argument('paths', nargs='*', help='Files to scan (default: src/ and include/)')
    args = ap.parse_args()

    if args.paths:
        targets = [p for p in args.paths if p.endswith(SCAN_EXTS) and os.path.isfile(p)]
    else:
        targets = []
        for d in SCAN_DIRS:
            for root, _, files in os.walk(d):
                targets += [os.path.join(root, f) for f in files if f.endswith(SCAN_EXTS)]

    hits = []
    for path in sorted(targets):
        hits += scan_file(path)
    if args.rule:
        hits = [h for h in hits if h[2] == args.rule]

    by_rule = {}
    for _, _, rule, _ in hits:
        by_rule[rule] = by_rule.get(rule, 0) + 1
    total = len(hits)

    if args.list:
        for path, lineno, rule, line in hits:
            print(f'{path}:{lineno}: [{rule}] {line}')
        print()

    if args.summary or args.list:
        for rule in sorted(RULES):
            n = by_rule.get(rule, 0)
            if not args.rule or args.rule == rule:
                print(f'  {rule:<11} {n:>5}   → {FIX[rule]}')
        print(f'  {"TOTAL":<11} {total:>5}')

    limit = args.max_allowed
    if limit is None:
        if total:
            print(f'❌ Imperative UI: {total} XML-owned widgets driven from C++.')
            return 1
        print('✅ Imperative UI: no XML-owned widget driven from C++.')
        return 0

    if total > limit:
        print(f'❌ Imperative UI: {total} violations exceeds baseline ({limit}).')
        print('   New code must bind subjects in XML, not mutate XML widgets from C++.')
        print('   Run: python3 scripts/check_imperative_ui.py --list')
        return 1
    if total < limit:
        print(f'✅ Imperative UI: {total} (baseline {limit} — ratchet the baseline down)')
        return 0
    print(f'✅ Imperative UI: {total} == baseline ({limit})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
