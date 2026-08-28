#!/usr/bin/env python3
"""Fail when test code serves an XML widget name that production owns.

The failure this exists to catch: a production widget object gets excluded from the
test link to resolve a single-symbol conflict, someone writes a replacement widget in
tests/ and registers it under the same XML name, and from then on every <name> any
test creates is the replacement. The copy drifts, and nothing goes red, because the
tests were never touching the shipped widget at all.

That has happened twice in this tree: ui_emergency_stop (the "unified recovery dialog"
tests asserted against a duplicate) and ui_text_input (31 ui_xml files use <text_input>;
the copy never grew bind_text two-way binding, the multiline pre-scan,
show_clear_button/clear_callback or KeyboardManager::register_textarea()).

The signal is narrow on purpose: registering a production widget NAME from tests is
fine when the callbacks are the shipped ones (test_xml_attribute_validator.cpp does
this deliberately). It is only a hijack when the callback is itself defined under
tests/, because then production owns the name and test code owns the behaviour.

Escape hatch: `// TEST_WIDGET_OK: <reason>` on or beside the registration.
"""

import argparse
import re
import sys
from pathlib import Path

REGISTER = re.compile(r'lv_xml_register_widget\(\s*"([a-zA-Z_][a-zA-Z0-9_]*)"\s*,\s*'
                      r'([A-Za-z_][A-Za-z0-9_:]*)\s*,\s*([A-Za-z_][A-Za-z0-9_:]*)')
OPT_OUT = re.compile(r'//\s*TEST_WIDGET_OK\s*:')


def defined_under(root_dir, symbol):
    """True if `symbol` looks like it has a function DEFINITION under root_dir.

    A definition line names the symbol followed by '(' and either opens a body on the
    same line or closes its parameter list there. A call site ends in ';' and a
    registration passes the symbol without '(' after it, so neither matches.
    """
    pat = re.compile(r'\b' + re.escape(symbol) + r'\s*\(')
    for path in root_dir.rglob('*'):
        if path.suffix not in ('.cpp', '.h', '.hpp') or not path.is_file():
            continue
        try:
            for line in path.read_text(errors='replace').split('\n'):
                if not pat.search(line) or 'lv_xml_register_widget' in line:
                    continue
                stripped = line.rstrip()
                if stripped.endswith('{') or stripped.endswith(')'):
                    return True
        except OSError:
            continue
    return False


def scan(root):
    src, tests = root / 'src', root / 'tests'
    if not src.is_dir() or not tests.is_dir():
        return []

    production_names = set()
    for path in src.rglob('*.cpp'):
        try:
            for m in REGISTER.finditer(path.read_text(errors='replace')):
                production_names.add(m.group(1))
        except OSError:
            continue

    findings = []
    for path in sorted(tests.rglob('*')):
        if path.suffix not in ('.cpp', '.h', '.hpp') or not path.is_file():
            continue
        try:
            lines = path.read_text(errors='replace').split('\n')
        except OSError:
            continue
        opt_out = {i for i, l in enumerate(lines, 1) if OPT_OUT.search(l)}
        for i, line in enumerate(lines, 1):
            m = REGISTER.search(line)
            if not m:
                continue
            name, create_cb, apply_cb = m.groups()
            if name not in production_names:
                continue  # a test-only widget name collides with nothing shipped
            if any(j in opt_out for j in range(i - 2, i + 3)):
                continue
            hijacked = [cb for cb in (create_cb, apply_cb) if defined_under(tests, cb)]
            if hijacked:
                findings.append((str(path.relative_to(root)), i, name, hijacked))
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', default='.')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()

    findings = scan(Path(args.root).resolve())
    if not findings:
        print('✅ Test widget registry: no production widget name is served by test code')
        return 0

    print(f'❌ Test code registers {len(findings)} production widget name(s):')
    for f, ln, name, cbs in findings:
        print(f'   {f}:{ln}: <{name}> is registered in src/, but {", ".join(cbs)} '
              f'is defined under tests/')
    print('\n   Every <' + findings[0][2] + '> a test creates through XML is the test copy,')
    print('   not the shipped widget. Link the production object instead of replacing it,')
    print('   or annotate with // TEST_WIDGET_OK: <reason> if the shadow is deliberate.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
