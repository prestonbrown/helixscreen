#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: HelixScreen declarations belong under helix::.
#
# docs/devel/DEVELOPMENT.md § Namespace organization: "all HelixScreen code lives
# under helix:: (UI helpers in helix::ui::, sensor managers in helix::sensors)".
# The rule was written and never gated, so a third of the tree drifted out from
# under it (prestonbrown/helixscreen#1370). The drift is not old code converging on
# its own: it runs along subsystem lines, so whole areas (every ams_backend_*, every
# ui_panel_*, every display/wifi/usb/sound backend) keep taking new global-scope
# declarations by local precedent.
#
# Flagged, per declaration site:
#   type        class/struct/union/enum declared at global scope
#   function    free function declared or defined at global scope
#   variable    global variable or constant at global scope
#   foreign-ns  declaration under a root namespace that is not helix
#
# NOT flagged:
#   - anything inside a helix-rooted namespace, at any depth
#   - extern "C" blocks and `extern "C"` declarations: C ABI symbols have no
#     namespace to live in. Structural.
#   - anonymous-namespace and file-scope `static` declarations in .cpp files:
#     internal linkage, so no cross-TU name is created. Still flagged in HEADERS,
#     where the name reaches every includer.
#   - forward declarations that mirror a third-party type (lv_*, hv_*, ...): the
#     foreign spelling is the correct one, same rule as CLAUDE.md's kCWSecurityWPA2
#     carve-out for foreign constant names.
#   - main()
#   - any line carrying a `// NAMESPACE_OK: <reason>` comment, or carrying it on
#     the line above (for multi-line declarations)
#
# This is a RATCHET, not a wall. The baseline freezes today's count so the debt can
# only shrink: a new global declaration fails the build, and moving one into helix::
# lowers the number. Lower the baseline in quality-checks.sh when you do.
#
# Usage:
#   check_namespace_compliance.py                    # fail on any violation
#   check_namespace_compliance.py --max-allowed 1234 # ratcheting baseline
#   check_namespace_compliance.py --summary          # counts only
#   check_namespace_compliance.py --list             # every site, file:line
#   check_namespace_compliance.py --files            # per-file counts, worst first
#   check_namespace_compliance.py --rule type        # one rule only

import argparse
import os
import re
import sys

SCAN_DIRS = ['src', 'include']
SCAN_EXTS = ('.cpp', '.cc', '.h', '.hpp')
HEADER_EXTS = ('.h', '.hpp')

OPT_OUT = 'NAMESPACE_OK'
ROOT_NS = 'helix'

RULES = ('type', 'function', 'variable', 'foreign-ns')

FIX = {
    'type':       'wrap in namespace helix { } (or a helix:: sub-namespace)',
    'function':   'wrap in namespace helix { }, or make it static if file-local',
    'variable':   'wrap in namespace helix { }, or make it static/constexpr-local',
    'foreign-ns': 'nest the namespace under helix::',
}

# Whole files that are structurally exempt.
#   main.cpp defines ::main, which the C++ standard requires at global scope.
#   include/compat/ shims third-party headers into the spelling the tree expects.
STRUCTURAL_FILES = {
    'src/main.cpp',
}
STRUCTURAL_DIRS = (
    'include/compat/',
    'src/lvgl-demo/',
)

# Forward declarations that mirror a foreign API keep the foreign spelling.
FOREIGN_PREFIXES = (
    'lv_', '_lv_', 'hv_', 'Hv', 'HV', 'json', 'nlohmann', 'cJSON', 'sqlite3',
    'DBus', 'GDBus', 'G', 'SDL_', 'drm', 'DRM', 'mbedtls', 'lws', 'ALSA',
    'snd_', 'png_', 'jpeg_', 'z_', 'Display', 'Window', 'CW', 'NS',
)

RAW_OPEN = re.compile(r'(?:u8|u|U|L)?R"([^()\\ ]{0,16})\(')

TYPE_RE = re.compile(
    r'^(?:template\s*<[^;{]*>\s*)?'
    r'(class|struct|union)\s+'
    r'(?:[A-Z_][A-Z0-9_]*_API\s+)?'
    r'([A-Za-z_]\w*)\b'
)
ENUM_RE = re.compile(r'^enum\s+(?:class\s+|struct\s+)?([A-Za-z_]\w*)\b')

# A free function: <type tokens> <name>(...) followed by ; { or a trailing
# specifier. Requires at least one type token before the name, so a bare macro
# invocation like LV_IMG_DECLARE(foo); does not match.
FUNC_RE = re.compile(
    r'^(?:template\s*<[^;{]*>\s*)?'
    r'(?:(?:inline|constexpr|consteval|static|extern|virtual|explicit)\s+)*'
    r'(?:[A-Za-z_]\w*(?:\s*::\s*\w+)*(?:\s*<[^;{]*>)?[\s*&]+)+'
    r'([A-Za-z_]\w*|operator\s*\S+)\s*\('
)

VAR_RE = re.compile(
    r'^(?:(?:inline|constexpr|const|extern|static|thread_local)\s+)+'
    r'(?:[A-Za-z_]\w*(?:\s*::\s*\w+)*(?:\s*<[^;{]*>)?[\s*&]+)+'
    r'([A-Za-z_]\w*)\s*(?:=|\[|;|\{)'
)

NS_OPEN_RE = re.compile(r'^\s*(?:inline\s+)?namespace\s*([A-Za-z_][\w:]*)?\s*\{')
EXTERN_C_RE = re.compile(r'^\s*extern\s*"C"\s*\{')

SKIP_PREFIXES = (
    '}', ')', '#', 'using ', 'typedef ', 'namespace', 'friend ', 'return ',
    'public:', 'private:', 'protected:', 'else', 'template class',
    'template struct', 'extern "C"', '__attribute__', 'static_assert',
)


def strip_noise(src):
    """Blank comments, string/char literals and preprocessor lines, preserving
    line numbers and every brace that is real code.

    Single left-to-right scan on purpose. Stripping `//` before string literals
    truncates a line like `if (s.rfind("// ", 0) == 0) {` at the quoted slashes
    and loses its opening brace, which silently drifts brace depth for the rest
    of the file and reports function-local declarations as global ones.
    """
    out_lines = []
    in_block = False
    in_raw = None
    pp_cont = False
    for line in src.split('\n'):
        n = len(line)
        if not in_block and in_raw is None and (pp_cont or line.lstrip().startswith('#')):
            pp_cont = line.rstrip().endswith('\\')
            out_lines.append(' ' * n)
            continue
        pp_cont = False
        res = []
        i = 0
        while i < n:
            if in_raw is not None:
                idx = line.find(in_raw, i)
                if idx == -1:
                    res.append(' ' * (n - i)); i = n
                else:
                    res.append(' ' * (idx + len(in_raw) - i))
                    i = idx + len(in_raw); in_raw = None
                continue
            if in_block:
                idx = line.find('*/', i)
                if idx == -1:
                    res.append(' ' * (n - i)); i = n
                else:
                    res.append(' ' * (idx + 2 - i)); i = idx + 2; in_block = False
                continue
            if line.startswith('//', i):
                res.append(' ' * (n - i)); i = n
                continue
            if line.startswith('/*', i):
                in_block = True; res.append('  '); i += 2
                continue
            m = RAW_OPEN.match(line, i)
            if m:
                in_raw = ')' + m.group(1) + '"'
                res.append(' ' * (m.end() - i)); i = m.end()
                continue
            c = line[i]
            if c in '"\'':
                j = i + 1
                while j < n:
                    if line[j] == '\\':
                        j += 2; continue
                    if line[j] == c:
                        j += 1; break
                    j += 1
                res.append(' ' * (j - i)); i = j
                continue
            res.append(c); i += 1
        out_lines.append(''.join(res))
    return '\n'.join(out_lines)


def is_foreign(name):
    return name.startswith(FOREIGN_PREFIXES)


def scan_file(path):
    rel = path.replace(os.sep, '/')
    if rel in STRUCTURAL_FILES or rel.startswith(STRUCTURAL_DIRS):
        return []
    try:
        with open(path, encoding='utf-8', errors='replace') as fh:
            raw = fh.read()
    except OSError:
        return []

    is_header = path.endswith(HEADER_EXTS)
    src = strip_noise(raw)
    raw_lines = raw.split('\n')

    hits = []
    depth = 0
    prev_code = ''
    scopes = []  # list of dicts: {'kind': 'ns'|'anon'|'externc', 'root': str|None}

    for idx, line in enumerate(src.split('\n')):
        stripped = line.strip()

        # --- scope openers -------------------------------------------------
        m = NS_OPEN_RE.match(line)
        if m:
            name = m.group(1)
            if name is None:
                scopes.append({'kind': 'anon', 'root': None})
            else:
                scopes.append({'kind': 'ns', 'root': name.split('::')[0]})
            depth += line.count('{') - line.count('}')
            while scopes and depth < len(scopes):
                scopes.pop()
            continue
        if EXTERN_C_RE.match(line):
            scopes.append({'kind': 'externc', 'root': None})
            depth += line.count('{') - line.count('}')
            while scopes and depth < len(scopes):
                scopes.pop()
            continue

        # --- are we directly inside the innermost scope? -------------------
        directly = (depth == len(scopes))
        rule = None
        if directly:
            if any(s['kind'] in ('externc', 'anon') for s in scopes):
                rule = None  # C ABI or internal linkage: structural
            elif not scopes:
                rule = 'global'
            elif scopes[0]['root'] != ROOT_NS:
                rule = 'foreign-ns'

        # A line continuing a multi-line parameter list is not a declaration.
        continuation = prev_code.endswith((',', '(')) or prev_code.endswith('&&')
        if rule and stripped and not continuation \
                and not stripped.startswith(SKIP_PREFIXES):
            hit = classify(stripped, is_header)
            if hit:
                kind, name = hit
                if not is_foreign(name) and name != 'main':
                    line_txt = raw_lines[idx] if idx < len(raw_lines) else ''
                    prev_txt = raw_lines[idx - 1] if idx >= 1 else ''
                    if OPT_OUT not in line_txt and OPT_OUT not in prev_txt:
                        hits.append((rel, idx + 1,
                                     'foreign-ns' if rule == 'foreign-ns' else kind,
                                     line_txt.strip()[:100]))

        if stripped:
            prev_code = stripped
        depth += line.count('{') - line.count('}')
        if depth < 0:
            depth = 0
        while scopes and depth < len(scopes):
            scopes.pop()

    return hits


def classify(stripped, is_header):
    """Return (rule, name) for a declaration line, or None."""
    m = TYPE_RE.match(stripped)
    if m:
        # 'struct Foo;' forward decls count: the name is still global.
        return ('type', m.group(2))
    m = ENUM_RE.match(stripped)
    if m:
        return ('type', m.group(1))

    # In a .cpp, `static` at file scope is internal linkage: no global name.
    file_local = stripped.startswith('static ')
    if file_local and not is_header:
        return None

    m = FUNC_RE.match(stripped)
    if m:
        return ('function', m.group(1).replace(' ', ''))
    m = VAR_RE.match(stripped)
    if m:
        return ('variable', m.group(1))
    return None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--max-allowed', type=int, default=None,
                    help='Pass if total violations <= N (ratcheting baseline). '
                         'Default: fail on any.')
    ap.add_argument('--summary', action='store_true', help='Per-rule counts only')
    ap.add_argument('--list', action='store_true', help='Print every site')
    ap.add_argument('--files', action='store_true',
                    help='Per-file counts, worst first')
    ap.add_argument('--rule', choices=sorted(RULES), help='Restrict to one rule')
    ap.add_argument('paths', nargs='*',
                    help='Files to scan (default: src/ and include/)')
    args = ap.parse_args()

    if args.paths:
        targets = [p for p in args.paths
                   if p.endswith(SCAN_EXTS) and os.path.isfile(p)]
    else:
        targets = []
        for d in SCAN_DIRS:
            for root, _, files in os.walk(d):
                targets += [os.path.join(root, f)
                            for f in files if f.endswith(SCAN_EXTS)]

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

    if args.files:
        per = {}
        for path, _, _, _ in hits:
            per[path] = per.get(path, 0) + 1
        for path, n in sorted(per.items(), key=lambda kv: (-kv[1], kv[0])):
            print(f'  {n:>4}  {path}')
        print(f'\n  {len(per)} files')

    if args.summary or args.list or args.files:
        for rule in RULES:
            n = by_rule.get(rule, 0)
            if not args.rule or args.rule == rule:
                print(f'  {rule:<11} {n:>5}   → {FIX[rule]}')
        print(f'  {"TOTAL":<11} {total:>5}')

    limit = args.max_allowed
    if limit is None:
        if total:
            print(f'❌ Namespace: {total} declarations outside helix::.')
            return 1
        print('✅ Namespace: every declaration lives under helix::.')
        return 0

    if total > limit:
        print(f'❌ Namespace: {total} declarations outside helix:: '
              f'exceeds baseline ({limit}).')
        print('   New code declares types under helix:: (or a helix:: sub-namespace).')
        print('   Run: python3 scripts/check_namespace_compliance.py --list')
        return 1
    if total < limit:
        print(f'✅ Namespace: {total} (baseline {limit} — ratchet the baseline down)')
        return 0
    print(f'✅ Namespace: {total} == baseline ({limit})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
