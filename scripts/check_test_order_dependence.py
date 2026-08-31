#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a test must pass on its own, not because of what ran before it.
#
# THE FAILURE MODE
#
# A test that reads state some other file's test established. It asserts real
# computed values, executes the code it names, and would be reported "killed" by
# mutation testing -- so none of the other four gates can see it. It is simply
# not testing what it claims: the state it checks was set up by a stranger.
#
# It stays invisible until an unrelated change perturbs test ordering, which is
# the worst possible moment to find it. The specimen this gate was built
# against: tests/unit/test_grid_edit_mode.cpp:233 "build_default_grid only sets
# positions for anchor widgets" passes in the full suite and fails 5/5 alone
# (col==0 got 2, row>=2 got 0). Adding nine unrelated test cases elsewhere
# changed the case count, which changed shard composition, which moved its
# accidental prerequisite into another shard, and a green suite went red with
# nothing wrong in the changed code.
#
# THE METHOD
#
# Two result sets, compared. The full-suite report says how each case did in
# context; one run per source file says how its cases do alone. A case that
# passes in the suite and fails alone is order-dependent. No analysis, no
# heuristics, and no false positives that are not themselves real findings.
#
# The reverse -- fails in the suite, passes alone -- is also reported, as
# `pollution`: some earlier test left state that BREAKS this one. Same root
# cause, opposite sign, and worth knowing separately because the fix differs
# (the polluter needs cleanup; the dependent needs its own setup).
#
# Usage:
#   ./build/bin/helix-tests "~[.]~[slow]" --reporter xml --success --out full.xml
#   python3 scripts/check_test_order_dependence.py full.xml
#   python3 scripts/check_test_order_dependence.py full.xml --only test_grid_edit_mode

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ILLEGAL_XML = re.compile(rb'[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]')


class _Clean:
    """Test stdout can inject ANSI bytes into a report written to stdout."""

    def __init__(self, path):
        self._f = open(path, 'rb')

    def read(self, n=-1):
        return ILLEGAL_XML.sub(b'', self._f.read(n))

    def close(self):
        self._f.close()


def parse_report(path, strict=True):
    """{case name: (source file, passed)} from a Catch2 XML report.

    strict=False returns whatever parsed before the error instead of exiting.
    An isolated run is one file out of ~950: a report that goes bad part way
    through must cost that file's verdict, not the entire scan. (It did once --
    a single malformed report killed a run that had already done 900 files.)
    """
    out, src = {}, _Clean(path)
    try:
        for _, elem in ET.iterparse(src, events=('end',)):
            if elem.tag != 'TestCase':
                continue
            name = elem.get('name')
            res = elem.find('OverallResult')
            if name and res is not None:
                out[name] = (elem.get('filename', '?'),
                             res.get('success') == 'true')
            elem.clear()
    except ET.ParseError as e:
        if strict:
            sys.exit(f'{path}: malformed report ({e}). Regenerate with --out FILE.')
        sys.stderr.write(f'note: isolated report truncated or malformed ({e}); '
                         f'judging only the {len(out)} case(s) that parsed\n')
    finally:
        src.close()
    return out


# Catch2 test specs give , [ ] * ~ \ their own meaning, so a case named
# "Drag end uses snap preview position, not release point" parses as two specs
# and the run dies with `Invalid Filter`. Worse, it dies for the WHOLE file: the
# report comes back with nothing but an XML header, which reads as "no findings"
# unless something checks. Escape first, and verify cases actually ran.
SPEC_META = re.compile(r'([,\[\]\\*~])')


def escape_spec(name):
    return SPEC_META.sub(r'\\\1', name)


def evidence_root_for(report_path):
    """Directory beside the suite report that holds all retained evidence."""
    return Path(report_path).resolve().parent / 'order-dependence-evidence'


def evidence_dir_for(report_path, source_file):
    """Per-file directory a non-green isolated run's artifacts are kept in.

    Lives under the evidence root beside the suite report so every entry point
    (make target, CI job) finds it without a new flag, and is resolved absolute
    so a finding printed into a log can be pasted without reconstructing the
    working directory. The source path flattened to one name keeps each file's
    evidence self-describing: the finding names the file, the directory names
    it identically.
    """
    return evidence_root_for(report_path) / source_file.replace('/', '_')


def _retain_evidence(dest, tmp):
    """Copy an isolated run's artifacts out of the tempdir before it evaporates."""
    dest.mkdir(parents=True, exist_ok=True)
    for name in ('r.xml', 'output.txt', 'names.txt'):
        src = Path(tmp) / name
        if src.is_file():
            shutil.copy2(src, dest / name)


def run_isolated(binary, names, workdir, timeout, evidence=None, suite_failed=()):
    """Run exactly these cases in one fresh process; {name: passed}.

    A timeout is not optional. Some files deadlock when run as an isolated set
    while passing in the full suite -- test_gcode_vase_streaming.cpp sat in
    futex_do_wait for 30 minutes and blocked the entire scan, because one stuck
    worker starves the pool and nothing else ever reports. A file that times out
    is un-judgeable, which is a finding to surface, not a reason to hang.

    Evidence retention: a run that is not green for every requested case copies
    its XML report, captured stdout/stderr, and the escaped spec into
    @p evidence before the tempdir is deleted. The verdict names the case, but
    the only artifacts that could say WHY it failed -- the expanded assertion in
    the report, the child's last words on stderr -- were discarded with the
    tempdir, and DEVNULL meant there were no last words at all. The 2026-08-31
    nightly fired a finding that three environments of local reruns could not
    reproduce because of exactly that gap. @p suite_failed (the names this
    file's report shows failing in the suite) widens the trigger to cover
    pollution too: that finding's isolated run is GREEN, and its report is the
    proof the case passes alone. The caller drops the copy again when the run
    turns out to agree with the suite (a consistently red file is test-all's
    story to tell, not evidence of ordering), so a red suite does not
    accumulate directories.
    """
    with tempfile.TemporaryDirectory() as tmp:
        spec = Path(tmp) / 'names.txt'
        spec.write_text('\n'.join(escape_spec(n) for n in names) + '\n')
        report = Path(tmp) / 'r.xml'
        output = Path(tmp) / 'output.txt'
        timed_out = False
        with output.open('wb') as sink:
            try:
                subprocess.run(
                    [str(binary), '-f', str(spec), '--reporter', 'xml',
                     '--success', '--out', str(report)],
                    cwd=workdir, stdout=sink, stderr=subprocess.STDOUT,
                    timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
        results = {}
        if not timed_out and report.is_file():
            results = {n: ok for n, (_, ok)
                       in parse_report(report, strict=False).items()}
        green = (not timed_out and report.is_file()
                 and all(results.get(n) for n in names))
        # A suite failure that passed alone is a pollution finding in the
        # making: retain even a green run when one of those is in play.
        pollution_candidate = any(results.get(n) for n in suite_failed)
        if (not green or pollution_candidate) and evidence is not None:
            _retain_evidence(evidence, tmp)
        return results


def report_ran_nothing(names, alone):
    """An empty isolated report is a broken run, not a clean one."""
    return bool(names) and not alone


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('report', help='full-suite Catch2 XML report (--success --out)')
    ap.add_argument('--binary', default='build/bin/helix-tests')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--timeout', type=int, default=300,
                    help='seconds before an isolated run is abandoned as hung')
    ap.add_argument('--only', default=None,
                    help='restrict to source files whose name contains this')
    # One process per test file over ~950 files is too long for a single CI
    # runner, and the work is embarrassingly parallel across files. Sharding is
    # by file so a case is always judged in the same company it keeps locally.
    ap.add_argument('--shard-count', type=int, default=1)
    ap.add_argument('--shard-index', type=int, default=0)
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--max-allowed', type=int, default=None)
    args = ap.parse_args()

    root = Path(subprocess.run(['git', 'rev-parse', '--show-toplevel'],
                               capture_output=True, text=True).stdout.strip())
    binary = root / args.binary
    if not binary.is_file():
        sys.exit(f'{binary} missing - run `make test` first')

    full = parse_report(Path(args.report))
    by_file = defaultdict(list)
    for name, (f, _) in full.items():
        if args.only and args.only not in f:
            continue
        by_file[f].append(name)
    if not by_file:
        sys.exit('no test cases matched')

    if args.shard_count > 1:
        if not 0 <= args.shard_index < args.shard_count:
            sys.exit(f'--shard-index must be in [0, {args.shard_count})')
        ordered = sorted(by_file)
        by_file = {f: by_file[f] for i, f in enumerate(ordered)
                   if i % args.shard_count == args.shard_index}
        if not by_file:
            print(f'shard {args.shard_index}/{args.shard_count}: no files')
            return 0

    shard = (f' [shard {args.shard_index}/{args.shard_count}]'
             if args.shard_count > 1 else '')
    print(f'{len(full)} case(s) in the report; running {len(by_file)} file(s) '
          f'in isolation with {args.jobs} job(s){shard}...')

    findings = []
    def check(item):
        f, names = item
        ev = evidence_dir_for(args.report, f)
        failed_in_suite = {n for n in names if not full[n][1]}
        alone = run_isolated(binary, names, root, args.timeout, ev, failed_in_suite)
        return f, names, alone, ev

    skipped = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for f, names, alone, ev in ex.map(check, sorted(by_file.items())):
            if report_ran_nothing(names, alone):
                skipped.append(f)
                continue  # un-judgeable: keep the evidence, it is all we have
            fired = 0
            for n in names:
                in_suite = full[n][1]
                # A case absent from the isolated report did not run (a name
                # Catch2's spec parser could not round-trip); not a finding.
                if n not in alone:
                    continue
                if in_suite and not alone[n]:
                    findings.append(('order-dependent', f, n))
                    fired += 1
                elif not in_suite and alone[n]:
                    findings.append(('pollution', f, n))
                    fired += 1
            if not fired:
                # Agrees with the suite: nothing here is about ordering, and
                # the suite log already tells this file's story.
                shutil.rmtree(ev, ignore_errors=True)

    for kind, f, n in (findings if args.list else findings[:40]):
        why = ('passes in the suite, FAILS alone - it reads state another '
               'test established' if kind == 'order-dependent' else
               'fails in the suite, passes alone - an earlier test leaves '
               'state that breaks it')
        print(f'{f}: [{kind}] {n}\n    {why}')
        print(f'    evidence: {evidence_dir_for(args.report, f)} '
              f'(report + captured output + escaped spec)')

    for f in skipped:
        print(f'{f}: [not-run] isolated run produced no results (hung past '
              f'--timeout {args.timeout}s, crashed, or wrote an unreadable '
              f'report) - the gate could not judge this file', file=sys.stderr)
        print(f'evidence retained at {evidence_dir_for(args.report, f)}',
              file=sys.stderr)

    # A file whose evidence was kept and then dropped leaves an empty root.
    # Prune it here, after the pool has joined: doing it per file would race a
    # sibling worker that is mid-mkdir of its own evidence directory.
    try:
        evidence_root_for(args.report).rmdir()
    except OSError:
        pass

    kinds = {}
    for k, _, _ in findings:
        kinds[k] = kinds.get(k, 0) + 1
    print(f'\norder-dependence findings: {len(findings)} '
          f'({", ".join(f"{k}={v}" for k, v in sorted(kinds.items())) or "none"})')

    if args.max_allowed is not None and len(findings) > args.max_allowed:
        print(f'FAIL: exceeds --max-allowed {args.max_allowed}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
