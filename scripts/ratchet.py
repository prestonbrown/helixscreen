#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The under-baseline verdict every count ratchet shares.

A ratchet's baseline may fall and never rise. When a count comes in under its baseline,
the slack would absorb the next violation without a word, so the baseline has to come
down with the fix. Where that happens depends on the stage:

  - At commit (quality-checks.sh --staged-only) an under count passes and asks for the
    baseline to be lowered, so a commit that removes violations never needs a baseline
    edit to land.
  - At the stages a push to main runs (pre-push's full sweep, make land-gate, CI) it
    fails, naming the baseline to lower and the value to lower it to.

scripts/quality-checks.sh decides which stage a run is and exports HELIX_RATCHET_STRICT=1
for the push stages. A checker run by hand is lenient unless the variable says otherwise.

From a checker:   return ratchet.under_baseline(gate, where, items, lenient_message)
From shell:       python3 scripts/ratchet.py GATE WHERE KEY BASELINE COUNT LENIENT
"""

import os
import sys

STRICT_ENV = 'HELIX_RATCHET_STRICT'
# A long list (a per-file baseline after a sweep) names this many and counts the rest.
SHOWN = 20


def strict() -> bool:
    """True at the push stages, where a count under its baseline fails."""
    return os.environ.get(STRICT_ENV) == '1'


def shown(path, root) -> str:
    """PATH relative to ROOT when it lies inside it, so a message names a repo path."""
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path)


def lowered(key, baseline, count) -> str:
    """One item of an under-baseline report: KEY, from BASELINE to COUNT."""
    return f'{key}: {baseline} -> {count}'


def under_baseline(gate, where, items, lenient) -> int:
    """Report counts under their baseline; return the gate's exit code.

    gate: the gate's name. where: the file holding the baseline (and how to rewrite it,
    when a command does). items: what came in under, one line each, from lowered() or a
    name that left a name-keyed baseline. lenient: the message a commit prints.
    """
    items = list(items)
    if not items:
        return 0
    if not strict():
        print(lenient)
        return 0
    print(f'❌ {gate}: under its baseline. A push holds a ratchet at its count, so lower '
          f'the baseline in {where}:')
    for item in items[:SHOWN]:
        print(f'     {item}')
    if len(items) > SHOWN:
        print(f'     ... and {len(items) - SHOWN} more')
    print(f'   A commit passes under the baseline; the push to main does not '
          f'({STRICT_ENV}=1).')
    return 1


def main(argv) -> int:
    if len(argv) != 7:
        print('usage: ratchet.py GATE WHERE KEY BASELINE COUNT LENIENT', file=sys.stderr)
        return 2
    gate, where, key, baseline, count, lenient = argv[1:]
    if int(count) >= int(baseline):
        return 0
    return under_baseline(gate, where, [lowered(key, baseline, count)], lenient)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
