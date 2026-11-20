#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The verdicts every count ratchet shares.

A ratchet's baseline may fall and never rise. A count under its baseline passes at every
stage and asks for the baseline to come down, so a change that removes violations never
needs a baseline edit to land, and the slack shows in the weekly report
(scripts/ratchet_report.py) until someone lowers it.

The count ratchets block nothing (prestonbrown/helixscreen#2004). A checker whose only
findings are ratchet warnings exits WARN_EXIT, which the qc wrappers print and pass
(qc_ratchet_verdict in scripts/qc/_lib.sh). The architecture ratchets reach a commit and
the land gate through scripts/design_ratchets.py, which names each new site.

From a checker:   return ratchet.under_baseline(gate, where, items, lenient_message)
From shell:       python3 scripts/ratchet.py GATE WHERE KEY BASELINE COUNT LENIENT
"""

import sys

# A checker's exit status when its only findings are ratchet warnings.
WARN_EXIT = 3


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
    """Report counts under their baseline; always 0, since an improvement never fails.

    gate: the gate's name. where: the file holding the baseline (and how to rewrite it,
    when a command does). items: what came in under, one line each, from lowered() or a
    name that left a name-keyed baseline. lenient: the message to print.
    """
    if list(items):
        print(lenient)
    return 0


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
