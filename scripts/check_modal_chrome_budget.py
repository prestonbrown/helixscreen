#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a modal's chrome must match the content cap it budgets against.
#
# Background (#1277). `#dialog_content_max` is a single global ladder whose
# per-breakpoint values are derived from ONE chrome shape — modal_dialog.xml's:
#
#     header + scrollable content + divider + button row  <=  85% of the screen
#
# The dialog root is `height="content"` with `style_max_height="85%"` and
# `scrollable="false"`. When the children total more than the cap, the root
# clamps and the overflow falls off the BOTTOM — which is the button row. The
# user is left with a modal they cannot dismiss.
#
# Measured on a 480x272 (micro) panel, ams_loading_error_modal.xml with a long
# fault string and the AFC diagram pinned below the scroll area:
#
#     card         y= 21  h=231  -> bottom 252   (clamped at exactly the cap)
#     scroll area  y= 50  h=140  -> bottom 190   (clamped at dialog_content_max)
#     AFC graphic  y=190  h= 38  -> bottom 228
#     button row   y=236  h= 27  -> bottom 263   <-- 11px PAST the card bottom
#
# LVGL cannot solve this in layout: lv_flex.c implements `grow` only, there is no
# CSS-style flex-shrink, so a child cannot be squeezed to make room. The budget
# has to be right up front.
#
# THE RULE
#   Everything except a divider and the button row lives INSIDE the scroll
#   container. Then the one global token is correct by construction.
#
#   Where extra chrome must stay visible while the text scrolls, the container
#   opts into the sibling token measured for that shape instead:
#
#     #dialog_content_pinned_max       ONE pinned block (a diagram, a status
#                                      row) below the scroll area
#     #dialog_content_tall_chrome_max  a SECOND button row with its divider
#                                      (klipper_recovery_dialog's restart row
#                                      plus Dismiss)
#
#   Both reserve exactly one extra block of height; which one is right depends
#   on the shape, because their ladders were measured against different chrome.
#   A shape with more than that (action_prompt_modal: AFC diagram + wrapping
#   prompt rows + a footer row) is beyond any single ladder — measure it on
#   device and mark the file MODAL_CHROME_OK.
#
# WHAT IS FLAGGED
#   - An element that follows a shared-cap container among its siblings and is
#     neither a divider nor the FIRST button row, when the container's token
#     has no budget left for it. Fix by moving the element inside the scroll
#     container, or by switching to the sibling token that reserves its height.
#   - A SECOND button row below the container while it is still on
#     #dialog_content_max — the tall-chrome shape; switch the container to
#     #dialog_content_tall_chrome_max.
#   - A card raised ABOVE the shared 85% cap (`style_max_height="90%"` &c) in
#     a file that uses a shared content token. Raising one card unsizes the
#     ladder arithmetic every modal shares — klipper_recovery_dialog carried
#     90% for exactly this reason before the tall-chrome ladder existed, and
#     porting it back onto the token is what retired the hack.
#
# NOT FLAGGED
#   - Dividers, and the first button row after the container. That is the
#     budgeted shape.
#   - Containers on a sibling token, which get one extra paid block — a pinned
#     block OR a second button row — before they are flagged again.
#   - Modals that size themselves without any shared content token (flat px
#     caps, favorite_macro_config_modal's 520). They opted out of the shared
#     budget and are the author's problem.
#   - Two sibling blocks that can never be visible together, because they are
#     bound to the same subject with different `ref_value`s. Their heights do
#     not sum (hidden_network_modal's form vs its connecting spinner).
#
# KNOWN GAP
#   A wrapping button row (`row_wrap`) is recognised as the budgeted row but
#   can grow to TWO+ rows of buttons when a macro supplies many long labels
#   (`ctl demo action-prompt-many`: seven material presets over three rows).
#   The single-row budget does not cover that growth; it was measured to fit
#   at five breakpoints and is re-measured, not assumed, when it changes.
#
# Opt out with `MODAL_CHROME_OK: <reason>` anywhere in the file.

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SCAN_DIRS = ('ui_xml',)

# android/app/src/main/assets/ui_xml/ is a gradle copy of ui_xml/, not a source
# of truth. build/ and .worktrees/ are likewise derived trees.
SKIP_PARTS = ('android', 'build', '.worktrees', 'translations')

STANDARD_TOKEN = '#dialog_content_max'
PINNED_TOKEN = '#dialog_content_pinned_max'
TALL_TOKEN = '#dialog_content_tall_chrome_max'
SHARED_TOKENS = (STANDARD_TOKEN, PINNED_TOKEN, TALL_TOKEN)

# Extra non-chrome blocks each token's budget has already paid for.
TOKEN_BUDGET = {STANDARD_TOKEN: 0, PINNED_TOKEN: 1, TALL_TOKEN: 1}

# The cap the shared budget is derived from. A card raised above it while
# using a shared content token has unsized the ladder for everyone.
DEFAULT_ROOT_CAP_PCT = 85

OPT_OUT = 'MODAL_CHROME_OK'

# Chrome the budget already accounts for, allowed to follow the scroll container.
DIVIDER_TAGS = {'divider_horizontal'}
BUTTON_ROW_TAGS = {'modal_button_row'}


def is_button_row(el):
    """Is this element the modal's button row?

    modal_dialog.xml and several modals hand-roll the row rather than using the
    modal_button_row component, so the tag name alone does not identify it. Two
    hand-rolled shapes exist:

      - fixed height:  <lv_obj height="#button_height" flex_flow="row">
      - wrapping:      <lv_obj name="button_container" flex_flow="row_wrap"
                               height="content">   (action_prompt_modal)

    The wrapping form is deliberately recognised. Flagging it would be a false
    positive — it IS the budgeted button row — but note it can grow to TWO rows
    when enough buttons wrap, which the single-row budget does not cover.
    """
    if el.tag in BUTTON_ROW_TAGS:
        return True
    if el.get('height') == '#button_height':
        return True
    name = el.get('name') or ''
    flex = el.get('flex_flow') or ''
    return 'button' in name and flex.startswith('row')


def is_divider(el):
    return el.tag in DIVIDER_TAGS


def raised_cap_pct(value):
    """The percentage of a `style_max_height` like '90%', or None."""
    if value is None or not value.endswith('%'):
        return None
    try:
        return int(value[:-1])
    except ValueError:
        return None


def hidden_bindings(el):
    """{subject: ref_value} for each `hidden` flag binding on this element."""
    out = {}
    for binding in el:
        if not binding.tag.startswith('bind_flag_if'):
            continue
        if binding.get('flag') != 'hidden':
            continue
        subject = binding.get('subject')
        if subject is not None:
            out[subject] = (binding.tag, binding.get('ref_value'))
    return out


def mutually_exclusive(a, b):
    """Can these two siblings never be on screen at the same time?

    hidden_network_modal shows either its entry form or its connecting spinner,
    switched by `hidden_connecting`: the form hides on 1, the spinner hides on 0.
    Their heights never sum, so counting the second against the first's budget
    is a false positive — and a gate that fires on correct markup gets disabled.
    """
    a_bind, b_bind = hidden_bindings(a), hidden_bindings(b)
    for subject, (a_tag, a_ref) in a_bind.items():
        if subject not in b_bind:
            continue
        b_tag, b_ref = b_bind[subject]
        # Same predicate, different trigger value -> exactly one is visible.
        if a_tag == b_tag and a_ref != b_ref:
            return True
    return False


def find_violations(path):
    """Yield (kind, where, detail) tuples for every over-budget shape.

    kind 'pinned_block': an unbudgeted element below a shared-cap container.
    kind 'raised_cap':   a card cap above 85% in a file using a shared token.
    """
    text = path.read_text(encoding='utf-8', errors='replace')
    if OPT_OUT in text:
        return

    try:
        root = ET.fromstring(text)
    except ET.ParseError:
        # Malformed XML is the xmllint pass's job, not this gate's. Staying
        # silent here beats reporting a parse error as a chrome violation.
        return

    uses_shared = any(
        el.get('style_max_height') in SHARED_TOKENS for el in root.iter()
    )

    # A card raised above 85% while budgeting its content against a shared
    # ladder: the extra room is per-dialog arithmetic that unsizes the ladder
    # for every other modal. The fix is the sibling token that reserves the
    # extra chrome, never a taller card.
    for view in root.iter('view'):
        pct = raised_cap_pct(view.get('style_max_height'))
        if pct is not None and pct > DEFAULT_ROOT_CAP_PCT and uses_shared:
            yield ('raised_cap', view.get('name') or view.tag,
                   view.get('style_max_height'))

    for parent in root.iter():
        children = list(parent)
        for idx, child in enumerate(children):
            token = child.get('style_max_height')
            if token not in SHARED_TOKENS:
                continue

            # Dividers and the FIRST button row are the budgeted chrome;
            # everything after that must be paid for out of the token's budget.
            budget = TOKEN_BUDGET[token]
            button_rows_seen = 0
            first_button_row = None

            for sibling in children[idx + 1:]:
                if is_divider(sibling):
                    continue
                if is_button_row(sibling):
                    button_rows_seen += 1
                    if button_rows_seen == 1:
                        first_button_row = sibling
                        continue  # the one row every budget covers
                    # State-switched rows never co-render: a modal whose button
                    # row swaps with debug_bundle_state-style bindings shows
                    # exactly one row at a time, so the extra rows cost no
                    # chrome. Compare against the FIRST row (the budgeted one),
                    # same predicate as the pinned-block exemption below.
                    if first_button_row is not None and mutually_exclusive(first_button_row, sibling):
                        continue
                    # A second button row is the tall-chrome shape: it costs
                    # a budget slot like a pinned block.
                    if budget > 0:
                        budget -= 1
                        continue
                    yield ('pinned_block', sibling.tag, token)
                    continue
                if mutually_exclusive(child, sibling):
                    continue
                if budget > 0:
                    budget -= 1
                    continue
                yield ('pinned_block', sibling.tag, token)


def iter_xml_files(repo_root):
    for scan_dir in SCAN_DIRS:
        base = repo_root / scan_dir
        if not base.is_dir():
            continue
        for path in sorted(base.rglob('*.xml')):
            if any(part in SKIP_PARTS for part in path.parts):
                continue
            yield path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--list', action='store_true', help='list every violation')
    ap.add_argument('--repo-root', default='.', help='repository root')
    args = ap.parse_args()

    repo_root = Path(args.repo_root).resolve()

    violations = []
    for path in iter_xml_files(repo_root):
        for kind, where, detail in find_violations(path):
            violations.append((path.relative_to(repo_root), kind, where, detail))

    if not violations:
        print('✅ Modal chrome budget: every pinned block is accounted for')
        return 0

    print(f'❌ Modal chrome budget: {len(violations)} violation(s)\n')
    for rel, kind, where, detail in violations:
        if kind == 'raised_cap':
            print(f'  {rel}: card "{where}" is capped at {detail}, above the '
                  f'shared {DEFAULT_ROOT_CAP_PCT}%')
        else:
            print(f'  {rel}: <{where}> follows a {detail} container')

    print(
        '\n'
        'Each of these sits BELOW the scroll area and outside the budget that\n'
        'the container\'s token was sized for, so the modal overruns its 85% cap\n'
        'and the button row is clipped off the bottom of the card.\n'
        '\n'
        'Fix by one of:\n'
        '  - moving the element INSIDE the scroll container (preferred), or\n'
        f'  - switching that container to the sibling token that reserves its\n'
        f'    height — {PINNED_TOKEN} for one pinned block,\n'
        f'    {TALL_TOKEN} for a second button row — when it must stay\n'
        '    visible while the text scrolls.\n'
        'A raised card cap is never the fix: it unsizes the shared ladder.\n'
        '\n'
        f'Deliberate exception? Add "{OPT_OUT}: <reason>" to the file.'
    )
    return 1


if __name__ == '__main__':
    sys.exit(main())
