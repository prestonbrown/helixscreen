#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The verdicts every count ratchet shares.

A ratchet's baseline may fall and never rise. When a count comes in under its baseline,
the slack would absorb the next violation without a word, so the baseline should come
down with the fix. Under the baseline is bookkeeping, never a bug, so it warns at every
stage (commit, push, make land-gate, CI) and names the baseline to lower. The weekly
ratchet report (scripts/ratchet_report.py) shows the slack that builds up. Set
HELIX_RATCHET_STRICT=1 by hand to make it fail, for a run that lowers baselines in bulk.

A checker whose only findings are bookkeeping ratchets (the test-line budget, TestAccess
and friend counts, file size, hardcoded pixels, unreferenced callbacks) exits WARN_EXIT,
which the qc wrappers print as a warning and pass (qc_run_ratchet in scripts/qc/_lib.sh).

From a checker:   return ratchet.under_baseline(gate, where, items, lenient_message)
From shell:       python3 scripts/ratchet.py GATE WHERE KEY BASELINE COUNT LENIENT
"""

import os
import sys

STRICT_ENV = 'HELIX_RATCHET_STRICT'
# A checker's exit status when its only findings are bookkeeping ratchet warnings.
WARN_EXIT = 3
# A long list (a per-file baseline after a sweep) names this many and counts the rest.
SHOWN = 20


def strict() -> bool:
    """True when a run asks for a count under its baseline to fail."""
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
    name that left a name-keyed baseline. lenient: the message a run prints.
    """
    items = list(items)
    if not items:
        return 0
    if not strict():
        print(lenient)
        return 0
    print(f'❌ {gate}: under its baseline. Lower the baseline in {where}:')
    for item in items[:SHOWN]:
        print(f'     {item}')
    if len(items) > SHOWN:
        print(f'     ... and {len(items) - SHOWN} more')
    print(f'   Under the baseline fails only when {STRICT_ENV}=1 asks it to.')
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
