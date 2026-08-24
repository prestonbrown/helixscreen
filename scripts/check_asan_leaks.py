#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ratcheting gate on AddressSanitizer/LeakSanitizer at-exit leak reports.

`make test-asan` runs the suite under ASan with detect_leaks=1, so LeakSanitizer
prints every allocation still reachable-but-unfreed at exit. The suite leaves a
lot alive on purpose: panels, widgets, subjects and registries are process-scoped
and are never torn down, because tearing them down is close-time teardown work
(prestonbrown/helixscreen#1246), not a one-line free(). Failing on any leak would
mean failing forever, so LSAN_OPTIONS sets exitcode=0 and the verdict is made
here instead — against a baseline that may shrink and must not grow.

That split is deliberate. A non-zero exit from the ASan binary now means a
genuine failure (an ASan error, a crash, a failed assertion); "the suite passed
but leaked" is this script's business alone.

WHAT IS COUNTED
---------------
Only DIRECT leak blocks produce baseline keys. An indirect leak is a child of a
direct one — the vector's buffer under the leaked vector — so keying both counts
the same defect twice and makes the population move whenever an unrelated
container gains an element. The SUMMARY totals (which cover direct + indirect,
since that is what the sanitizer prints) are parsed separately and drive the
byte/object ceilings.

Each direct leak is attributed to its ORIGIN: the first stack frame that is our
code. Frame #0 is always the sanitizer's own allocator interceptor, and the
frames below the origin are libstdc++ plumbing and Catch2's runner, which are
identical for hundreds of unrelated leaks. Skipped as not-our-code:
libsanitizer, /usr/include/, /usr/lib/, tests/catch_amalgamated.*, and anything
whose path is absolute or escapes the tree with `../` (glibc's ../csu/,
../sysdeps/). Everything else — src/, include/, tests/unit/, lib/lvgl/,
lib/helix-xml/ — is ours; a leak that originates in vendored LVGL is still one
we have to account for, and lv_ll.c::lv_ll_ins_tail is as stable a key as any.

KEY STABILITY
-------------
The key is `<file>::<function>`, and every part of it that moves for reasons
unrelated to the leak is normalized away. A baseline that churns is a baseline
people regenerate blindly, which is the same as having no gate:

  * The LINE NUMBER is discarded. It moves on any edit anywhere above the
    allocation in that file, which would fail the build for a comment. The
    optional COLUMN clang prints after it (`file.cpp:339:17`) is likewise
    discarded, by a LAZY file match — a greedy one glued `:339` onto the file
    for every clang frame, which is how the 2026-08-16 nightly failed on all
    23 origins at once.
  * The file path is run through normpath, so the compiler's
    `tests/unit/../../src/ui/ui_button.cpp` and `src/ui/ui_button.cpp` are one
    key — and, importantly, `tests/unit/../catch_amalgamated.hpp` collapses onto
    tests/catch_amalgamated.hpp so the Catch2 skip actually fires on it. An
    ABSOLUTE checkout prefix is then stripped the same way: CI compiles with
    absolute source paths and a local build with relative ones, and the same
    file must be one key in both. Only a remainder that exists under this
    repository's tree roots is trusted, so `/usr/include/...` stays foreign.
  * The RETURN TYPE is dropped, and lambda closure types collapse to the token
    `lambda` (gcc `{lambda(Args)#1}`, clang `$_8`), and clang's `[abi:cxx11]`
    vendor tags go away. All three differ between gcc and clang symbolizers
    and between declaration orders; the function name that remains is what
    both compilers agree on.
  * TEMPLATE ARGUMENTS are truncated to the first one:
    `observe_int_sync<helix::PrintStatusWidget, helix::PrintStatusWidget::attach(
    lv_obj_t*, lv_obj_t*)::<lambda(helix::PrintStatusWidget*, int)> >` becomes
    `observe_int_sync<helix::PrintStatusWidget>`. The tail of that list is a
    lambda's synthesized name — it carries the enclosing function's full
    signature, so it changes when an unrelated parameter changes type, and gcc
    and clang do not spell it the same way. The first argument is the observing
    class, which is the part that identifies the site. The cost is real and
    accepted: several observe_* leaks in one class collapse into one key, and
    the ceilings are what notice if that bucket grows.
  * The PARAMETER LIST is dropped: `MoonrakerClientMock::gcode_script(std::__cxx11
    ::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)`
    is just `MoonrakerClientMock::gcode_script`. Demangled parameter spellings
    are libstdc++-version-dependent (`std::__cxx11::` prefixes, `long int` vs
    `long`) and enormous. Overloads stop being distinguishable, which is a
    non-issue: file + name is already finer than the ceilings need.
    `operator()` keeps its parens — stripping them leaves the word "operator".
  * `(anonymous namespace)::` is removed. It is a scope decoration, not
    identity, and it appears or vanishes depending on how a test file happens to
    wrap its helpers.
  * `CATCH2_INTERNAL_TEST_<n>` becomes `CATCH2_INTERNAL_TEST`. Catch2 numbers its
    generated test functions by ORDER WITHIN THE FILE, so adding a TEST_CASE
    renumbers every test below it — a one-line test addition would otherwise
    invalidate a dozen baseline keys in a file it never touched.

CEILINGS
--------
A key set alone cannot see an already-baselined origin start leaking MORE — the
same call site allocating twice as much is the same key. So the baseline also
carries `max-leaked-bytes:` and `max-leaked-objects:`, taken from the SUMMARY
line, mirroring `max-untagged-callbacks:` in scripts/update_queue_leak_baseline.txt.
Byte counts do move a little with allocator rounding and with how many tests
happened to touch a shared singleton, so set them at (or slightly above) an
observed run rather than hand-tightening them to the last byte.

MEASUREMENT
-----------
The baseline is pinned to ONE invocation, the one mk/tests.mk runs:

    make test-asan          # tees the run to /tmp/asan_output.txt

which is, underneath:

    ASAN_OPTIONS=detect_leaks=1:... LSAN_OPTIONS=exitcode=0 \
        ./build/bin/helix-tests-asan "~[.]" 2>&1 | tee /tmp/asan_output.txt

The `2>&1` is REQUIRED, not incidental: the sanitizer reports on stderr while
Catch2's run summary goes to stdout, and this gate refuses to believe a log that
has neither a leak report nor a Catch2 summary rather than reading the silence
as zero leaks. A truncated log — leak blocks but no SUMMARY — is rejected for
the same reason: it under-counts by however much got cut off.

Do not compare against a filtered or sharded run. A subset of the suite leaks a
different population (that is why test-asan-one does not call this gate), and a
baseline built from one is meaningless against the other.

Usage:
  check_asan_leaks.py --baseline scripts/asan_leak_baseline.txt /tmp/asan_output.txt
  check_asan_leaks.py --write-baseline scripts/asan_leak_baseline.txt run.log
  check_asan_leaks.py --list run.log            # every direct leak, by origin
  check_asan_leaks.py --by-file run.log         # group origins by file
  cat run.log | check_asan_leaks.py -           # or from stdin
"""

import argparse
import collections
import os.path
import re
import sys

# "Direct leak of 6944 byte(s) in 124 object(s) allocated from:"
LEAK_BLOCK_RE = re.compile(
    r'^(?P<kind>Direct|Indirect) leak of (?P<bytes>\d+) byte\(s\) in '
    r'(?P<objects>\d+) object\(s\) allocated from:'
)

# "    #1 0x614af140b0ff in ui_button_create tests/unit/../../src/ui/ui_button.cpp:568"
# The symbol may contain spaces (return types, parameter lists), so the file is
# taken as the last whitespace-separated token that ends in :<line>. The file
# part is LAZY and the column is a separate optional group because clang prints
# file:line:col while gcc prints file:line — with a greedy file, `a.cpp:562:26`
# parses as file "a.cpp:562", line 26, and the line number glues itself onto
# every key (the 2026-08-16 nightly: all 23 origins "new" for this alone).
# Frames with no source info ("in _start (/path/to/bin+0x446d2e4) (BuildId:
# ...)") match the frame prefix but yield no file, and are treated as
# unattributable.
FRAME_RE = re.compile(r'^\s+#(?P<n>\d+) 0x[0-9a-f]+ in (?P<rest>.*)$')
FRAME_LOC_RE = re.compile(r'^(?P<func>.*)\s+(?P<file>\S+?):(?P<line>\d+)(?::\d+)?$')

LEAK_HEADER_RE = re.compile(r'ERROR: LeakSanitizer: detected memory leaks')
SUMMARY_RE = re.compile(
    r'SUMMARY: AddressSanitizer: (?P<bytes>\d+) byte\(s\) leaked in '
    r'(?P<allocations>\d+) allocation\(s\)'
)

# Any OTHER AddressSanitizer finding — heap-use-after-free, stack-buffer-overflow.
# Those are real memory bugs, not accounted debt, and a leak ratchet is the wrong
# verdict to render on them. mk/tests.mk fails the recipe on these before this
# script runs; the check is here so a hand invocation cannot miss them either.
ASAN_ERROR_RE = re.compile(r'ERROR: AddressSanitizer: (?P<kind>[A-Za-z0-9_-]+)')

# Catch2's end-of-run summary. Its presence is what makes "no leak report"
# trustworthy rather than merely silent.
RUN_MARKER_RE = re.compile(r'All tests passed|test cases:\s*\d+|assertions:\s*\d+')

# Frames that are never an origin: the sanitizer's own interceptors, the standard
# library, and Catch2's runner. See the module docstring.
SKIP_PATH_PREFIXES = ('/usr/include/', '/usr/lib/')
SKIP_PATH_SUBSTRINGS = ('libsanitizer/',)
CATCH2_BASENAME = 'catch_amalgamated.'

CEILING_BYTES = 'max-leaked-bytes'
CEILING_OBJECTS = 'max-leaked-objects'

# How many "did not leak this run" origins to name before summarizing. Absences
# are informational, and a clean-but-unrelated run should not bury the verdict.
GONE_LIST_LIMIT = 25


def _match_angle(s, start):
    """Index of the '>' closing the '<' at s[start], or None."""
    depth = 0
    for i in range(start, len(s)):
        if s[i] == '<':
            depth += 1
        elif s[i] == '>':
            depth -= 1
            if depth == 0:
                return i
    return None


def _split_top_level(s):
    """Split on commas that are not nested inside <>, () or []."""
    parts = []
    depth = 0
    current = []
    for c in s:
        if c in '<([':
            depth += 1
        elif c in '>)]':
            depth -= 1
        if c == ',' and depth == 0:
            parts.append(''.join(current))
            current = []
            continue
        current.append(c)
    parts.append(''.join(current))
    return parts


def truncate_template_args(s):
    """Reduce every <...> group to its FIRST argument, recursively.

    The tail of a template argument list is where the compiler-formatted lambda
    names live, and they encode the enclosing function's whole signature. The
    first argument is the class the site belongs to, which is the identity worth
    keying on.
    """
    out = []
    i = 0
    while i < len(s):
        if s[i] == '<':
            close = _match_angle(s, i)
            if close is None:  # unbalanced; leave the rest alone
                out.append(s[i:])
                break
            first = _split_top_level(s[i + 1 : close])[0]
            out.append('<' + truncate_template_args(first).strip() + '>')
            i = close + 1
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


TRAILING_QUALIFIER_RE = re.compile(r'\s+(const|volatile|noexcept|&&|&)$')


def strip_parameter_list(s):
    """Drop a trailing top-level (...) parameter list and its cv-qualifiers.

    Demangled parameter spellings depend on the libstdc++ version and are often
    longer than the rest of the line. `operator()` is left intact — without its
    parens it is just the word "operator".
    """
    s = s.strip()
    while True:
        stripped = TRAILING_QUALIFIER_RE.sub('', s)
        if stripped == s:
            break
        s = stripped
    if not s.endswith(')'):
        return s
    depth = 0
    open_idx = None
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ')':
            depth += 1
        elif s[i] == '(':
            depth -= 1
            if depth == 0:
                open_idx = i
                break
    if open_idx is None:
        return s
    # Only strip a list that is at the top level — the parens inside
    # `<lambda(TestPanel*, int)>` belong to a template argument, not to us.
    head = s[:open_idx]
    if head.count('<') != head.count('>'):
        return s
    name = head.rstrip()
    if not name or name.endswith('operator'):
        return s
    return name


ANON_NS_RE = re.compile(r'\(anonymous namespace\)::')
CATCH2_TEST_RE = re.compile(r'CATCH2_INTERNAL_TEST_\d+')
# Clang spells the TEST_CASE-method class with a parameter list
# (`CATCH2_INTERNAL_TEST_22()::`) where gcc spells it bare. Drop clang's parens
# so both compilers key the same frame.
CATCH2_TEST_PARENS_RE = re.compile(r'CATCH2_INTERNAL_TEST\(\)')
# Clang appends vendor tags like `[abi:cxx11]` to operator() symbols.
ABI_TAG_RE = re.compile(r'\[abi:[^\]]*\]')
# The two compilers spell a lambda's closure type differently, and BOTH number
# them by declaration order within the enclosing scope — adding an unrelated
# lambda above the site renumbers it and would fork the key. Collapse either
# spelling to the literal token `lambda`.
LAMBDA_CLANG_RE = re.compile(r'\$_\d+')
LAMBDA_GCC_RE = re.compile(r'\{lambda\([^()]*\)(?:#[0-9]+)?\}')
# Leading namespace qualifiers of the FUNCTION name: gcc's libbacktrace
# reconstructs template function names from DWARF without their enclosing
# namespaces, while clang prints them fully qualified. Lowercase segments are
# namespaces by repo convention; CamelCase qualifiers are classes and survive.
NAMESPACE_PREFIX_RE = re.compile(r'^(?:[a-z_][a-z0-9_]*::)+')
WHITESPACE_RE = re.compile(r'\s+')

# A bare trailing word that is an operator name rather than a function: taking
# the last whitespace token of `Foo::operator new` would key on just `new`.
OPERATOR_WORDS = {'new', 'delete', 'co_await'}


def _split_top_level_tokens(s):
    """Split on whitespace that is not nested inside <>, (), [] or {}."""
    tokens = []
    depth = 0
    current = []
    for c in s:
        if c in '<([{':
            depth += 1
        elif c in '>)]}':
            depth -= 1
        if c.isspace() and depth == 0:
            if current:
                tokens.append(''.join(current))
                current = []
        else:
            current.append(c)
    if current:
        tokens.append(''.join(current))
    return tokens


def strip_return_type(s):
    """Drop a leading return type from a demangled symbol.

    Clang's symbolizer prints `RetType ns::func<Args>(params)`; gcc prints the
    same shape for template free functions (`void ns::func<...>(...)`) but no
    return type at all for plain methods. The function name is the LAST
    top-level token once the parameter list is gone, so keying on it unifies
    both spellings. The parameter list must already be stripped — `operator()`
    and friends carry meaning in their tail.
    """
    tokens = _split_top_level_tokens(s)
    if len(tokens) <= 1:
        return s
    if tokens[-1] in OPERATOR_WORDS:
        return ' '.join(tokens[-2:]) if len(tokens) >= 2 else s
    return tokens[-1]


def normalize_function(func):
    """Normalize a demangled symbol into a stable key component."""
    func = ANON_NS_RE.sub('', func)
    func = CATCH2_TEST_RE.sub('CATCH2_INTERNAL_TEST', func)
    func = CATCH2_TEST_PARENS_RE.sub('CATCH2_INTERNAL_TEST', func)
    func = ABI_TAG_RE.sub('', func)
    func = LAMBDA_CLANG_RE.sub('lambda', func)
    func = LAMBDA_GCC_RE.sub('lambda', func)
    func = truncate_template_args(func)
    func = strip_parameter_list(func)
    func = strip_return_type(func)
    func = NAMESPACE_PREFIX_RE.sub('', func)
    return WHITESPACE_RE.sub(' ', func).strip()


# Repository tree roots, in the spelling they have as the FIRST path segment of
# a repo-relative source path. Used to strip an absolute checkout prefix.
TREE_ROOTS = ('src', 'include', 'lib', 'tests', 'mk', 'scripts')

# The repo this script lives in. Existence checks for relativization are made
# against it, so the gate behaves the same run from any cwd.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def normalize_path(path):
    """Collapse `..` segments so one source file has one spelling.

    Also strips an absolute checkout prefix: CI compiles with absolute source
    paths (`/home/runner/work/<repo>/<repo>/src/...`), a local build may use
    either spelling, and the same file must be one key. A tree-root segment is
    only trusted when the remainder actually exists in this repository, so
    `/usr/src/linux/...` and `/usr/include/...` stay foreign (they contain
    `src`/`include` segments too, but no such repo files exist).
    """
    path = os.path.normpath(path)
    if os.path.isabs(path):
        parts = [p for p in path.split(os.sep) if p]
        for i, seg in enumerate(parts):
            if seg in TREE_ROOTS:
                rel = os.path.join(*parts[i:])
                if os.path.isfile(os.path.join(REPO_ROOT, rel)):
                    return rel
    return path


def is_our_code(path):
    """True if a frame in this file can serve as a leak origin."""
    if path.startswith(SKIP_PATH_PREFIXES):
        return False
    if path.startswith('/'):
        # Still absolute after normalize_path's relativization attempt: either
        # toolchain/system code (/usr/..., /lib/...) or a repo file that no
        # longer exists under any tree root. Neither can match a baseline key,
        # so it is not ours.
        return False
    if path.startswith('..'):
        # ../csu/, ../sysdeps/, ../../../../src/libsanitizer/ — out of tree.
        return False
    if any(sub in path for sub in SKIP_PATH_SUBSTRINGS):
        return False
    if os.path.basename(path).startswith(CATCH2_BASENAME):
        return False
    return True


def is_sanitizer_frame(path):
    """Frame #0 territory: the allocator interceptor itself."""
    return any(sub in path for sub in SKIP_PATH_SUBSTRINGS)


def parse_frame(line):
    """Return (func, file) for a stack frame line, or None. file may be None."""
    m = FRAME_RE.match(line)
    if not m:
        return None
    rest = m.group('rest').strip()
    loc = FRAME_LOC_RE.match(rest)
    if not loc:
        return (rest, None)
    return (loc.group('func').strip(), normalize_path(loc.group('file')))


NO_STACK_KEY = '<no-stack>'


def origin_key(frames):
    """Key a leak on the first frame that is our code.

    Falls back to the first non-sanitizer frame when nothing in the stack is
    ours — dropping such a leak would mask it, and a normalized libstdc++ frame
    is still deterministic. Falls back again to a marker when there is no usable
    frame at all, so the leak is still counted somewhere visible.
    """
    fallback = None
    for func, path in frames:
        if path is None:
            continue
        if is_our_code(path):
            return f'{path}::{normalize_function(func)}'
        if fallback is None and not is_sanitizer_frame(path):
            fallback = f'{path}::{normalize_function(func)}'
    return fallback or NO_STACK_KEY


class Report:
    """Everything one log says about leaks."""

    def __init__(self):
        self.direct = []  # [(bytes, objects, key)]
        self.indirect_bytes = 0
        self.indirect_objects = 0
        self.summary_bytes = None
        self.summary_allocations = None
        self.saw_leak_block = False
        self.saw_run = False
        self.asan_errors = []

    @property
    def direct_bytes(self):
        return sum(b for b, _, _ in self.direct)

    @property
    def direct_objects(self):
        return sum(o for _, o, _ in self.direct)

    def keys(self):
        return {k for _, _, k in self.direct}

    def totals(self):
        """(bytes, objects) for the ceilings.

        Prefers the sanitizer's own SUMMARY — it is the number a human reads off
        the run — and falls back to the parsed blocks if a log somehow lacks it.
        """
        if self.summary_bytes is not None:
            return self.summary_bytes, self.summary_allocations
        return (
            self.direct_bytes + self.indirect_bytes,
            self.direct_objects + self.indirect_objects,
        )

    def by_key(self):
        """{key: (bytes, objects, blocks)} over direct leaks."""
        agg = collections.defaultdict(lambda: [0, 0, 0])
        for nbytes, nobjects, key in self.direct:
            entry = agg[key]
            entry[0] += nbytes
            entry[1] += nobjects
            entry[2] += 1
        return {k: tuple(v) for k, v in agg.items()}


def parse(streams):
    """Parse ASan run log(s) into a Report."""
    report = Report()
    pending = None  # (kind, bytes, objects, frames)
    for stream in streams:
        for line in stream:
            if pending is not None:
                frame = parse_frame(line)
                if frame is not None:
                    pending[3].append(frame)
                    continue
                flush(report, pending)
                pending = None
            if not report.saw_run and RUN_MARKER_RE.search(line):
                report.saw_run = True
            if LEAK_HEADER_RE.search(line):
                report.saw_leak_block = True
                continue
            m = ASAN_ERROR_RE.search(line)
            if m:
                report.asan_errors.append(m.group('kind'))
                continue
            m = SUMMARY_RE.search(line)
            if m:
                report.saw_leak_block = True
                report.summary_bytes = int(m.group('bytes'))
                report.summary_allocations = int(m.group('allocations'))
                continue
            m = LEAK_BLOCK_RE.match(line)
            if m:
                pending = (
                    m.group('kind'),
                    int(m.group('bytes')),
                    int(m.group('objects')),
                    [],
                )
        if pending is not None:
            flush(report, pending)
            pending = None
    return report


def flush(report, pending):
    """Record a finished leak block."""
    kind, nbytes, nobjects, frames = pending
    if kind == 'Direct':
        report.direct.append((nbytes, nobjects, origin_key(frames)))
    else:
        # Indirect leaks are children of a direct one. Their bytes are counted
        # toward the totals; they never produce a key.
        report.indirect_bytes += nbytes
        report.indirect_objects += nobjects


BASELINE_HEADER = (
    '# Known at-exit leaks under AddressSanitizer (#1279). Generated by:\n'
    '#   make test-asan          # tees the run to /tmp/asan_output.txt\n'
    '#   scripts/check_asan_leaks.py --write-baseline <this file> /tmp/asan_output.txt\n'
    '#\n'
    '# One line per leak ORIGIN, keyed <file>::<function> from the first stack\n'
    '# frame that is our code. Only DIRECT leaks are keyed — an indirect leak is a\n'
    '# child of a direct one, and counting both double-counts the same defect.\n'
    '#\n'
    '# Line numbers (and clang\'s trailing columns) are deliberately absent, template\n'
    '# argument lists are truncated to their first argument, parameter lists and\n'
    '# return types are dropped, absolute checkout prefixes are relativized, lambda\n'
    '# closure spellings collapse to `lambda`, and Catch2 test numbering is\n'
    '# normalized away — the key is the same whether gcc or clang symbolized the\n'
    '# run. All of these move for reasons that have nothing to do with the leak,\n'
    '# and a key that moves is a baseline nobody trusts. See the\n'
    '# rationale in scripts/check_asan_leaks.py.\n'
    '#\n'
    '# A key NOT listed here fails the build. The list may SHRINK as leaks are\n'
    '# fixed; it must not grow. Regenerate rather than hand-editing.\n'
    '#\n'
    '# The ceilings below catch an already-listed origin leaking MORE, which the key\n'
    '# set alone cannot see. They come from the SUMMARY line, so they cover direct\n'
    '# and indirect bytes together.\n'
)


def read_baseline(path):
    """Return (key set, {ceiling name: value})."""
    keys = set()
    ceilings = {}
    try:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                name, sep, value = line.partition(':')
                if sep and name.strip() in (CEILING_BYTES, CEILING_OBJECTS):
                    ceilings[name.strip()] = int(value.strip())
                    continue
                keys.add(line)
    except OSError as e:
        print(f'❌ ASan leaks: cannot read baseline {path}: {e}')
        sys.exit(2)
    except ValueError:
        print(f'❌ ASan leaks: malformed ceiling value in {path}')
        sys.exit(2)
    return keys, ceilings


def write_baseline(path, keys, total_bytes, total_objects):
    with open(path, 'w') as fh:
        fh.write(BASELINE_HEADER)
        fh.write(f'{CEILING_BYTES}: {total_bytes}\n')
        fh.write(f'{CEILING_OBJECTS}: {total_objects}\n')
        for key in sorted(keys):
            fh.write(key + '\n')


def open_streams(paths):
    if not paths or paths == ['-']:
        return [sys.stdin]
    handles = []
    for p in paths:
        try:
            handles.append(open(p, 'r', errors='replace'))
        except OSError as e:
            print(f'❌ ASan leaks: cannot read {p}: {e}')
            sys.exit(2)
    return handles


def check_usable(report):
    """Return an exit code if the log cannot be trusted, else None.

    Absence of evidence is not evidence of absence. A log that was truncated,
    captured without stderr, or pointed at the wrong file has no leak report in
    it — and reading that as "zero leaks" is a gate that passes forever.
    """
    if report.asan_errors:
        kinds = collections.Counter(report.asan_errors)
        detail = ', '.join(f'{k} x{n}' for k, n in kinds.most_common())
        print(f'❌ ASan leaks: the log records AddressSanitizer error(s): {detail}')
        print('   Those are memory bugs, not accounted leak debt. Fix them first —')
        print('   the leak ratchet is not the right verdict to render on this run.')
        return 2

    if not report.saw_leak_block:
        if report.saw_run:
            return None  # a genuinely clean run; main() reports the pass
        print('❌ ASan leaks: no LeakSanitizer report and no Catch2 run summary.')
        print('   This does not look like a completed ASan run. Capture both streams:')
        print('     make test-asan          # tees the run to /tmp/asan_output.txt')
        return 2

    if report.summary_bytes is None:
        print('❌ ASan leaks: leak blocks present but no "SUMMARY: AddressSanitizer"')
        print('   line. The log is truncated, so every count in it is a lower bound.')
        print('   Re-run and capture the whole thing: make test-asan')
        return 2

    if report.summary_bytes > 0 and not report.direct:
        print('❌ ASan leaks: SUMMARY reports '
              f'{report.summary_bytes} leaked byte(s) but no "Direct leak" block')
        print('   could be parsed. The report format changed, and this gate would')
        print('   otherwise pass a run it cannot actually read.')
        return 2

    return None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument('logs', nargs='*', help='ASan run logs (or - for stdin)')
    ap.add_argument(
        '--baseline',
        metavar='FILE',
        help='Known-leak key set (<file>::<function>) plus max-leaked-bytes / '
        'max-leaked-objects ceilings. Fail on a key NOT listed, or on a ceiling '
        'being exceeded.',
    )
    ap.add_argument(
        '--write-baseline',
        metavar='FILE',
        help='Write the leak keys and ceilings in these logs to FILE and exit.',
    )
    ap.add_argument(
        '--list', action='store_true', help='Print every leak origin with its size'
    )
    ap.add_argument(
        '--by-file', action='store_true', help='Group leaked bytes by source file'
    )
    args = ap.parse_args()

    streams = open_streams(args.logs)
    report = parse(streams)
    for s in streams:
        if s is not sys.stdin:
            s.close()

    unusable = check_usable(report)
    if unusable is not None:
        return unusable

    by_key = report.by_key()
    total_bytes, total_objects = report.totals()
    keys = report.keys()

    if args.list:
        for key, (nbytes, nobjects, blocks) in sorted(
            by_key.items(), key=lambda kv: (-kv[1][0], kv[0])
        ):
            print(f'{nbytes:9d} B  {nobjects:6d} obj  {blocks:4d} block(s)  {key}')
        print()

    if args.by_file:
        per_file = collections.Counter()
        for key, (nbytes, _, _) in by_key.items():
            per_file[key.split('::', 1)[0]] += nbytes
        for path, nbytes in per_file.most_common():
            print(f'{nbytes:9d} B  {path}')
        print()

    detail = (
        f'{len(report.direct)} direct leak(s) from {len(keys)} origin(s); '
        f'{total_bytes} byte(s) in {total_objects} allocation(s) total'
    )

    if args.write_baseline:
        # Regenerating from several runs is good practice for the key set, but a
        # ceiling is a PER-RUN quantity: summing it across logs would write one
        # several times the real total and neuter it. Union the keys, take the
        # highest ceiling seen in any single run.
        if len(args.logs) > 1:
            keys = set()
            total_bytes = total_objects = 0
            for path in args.logs:
                with open(path, 'r', errors='replace') as fh:
                    per_log = parse([fh])
                keys |= per_log.keys()
                log_bytes, log_objects = per_log.totals()
                total_bytes = max(total_bytes, log_bytes)
                total_objects = max(total_objects, log_objects)
        write_baseline(args.write_baseline, keys, total_bytes, total_objects)
        print(
            f'✅ Wrote {len(keys)} leak origin(s) and ceilings of {total_bytes} '
            f'byte(s) / {total_objects} allocation(s) to {args.write_baseline}'
        )
        return 0

    if not args.baseline:
        if report.direct:
            print(f'❌ ASan leaks: {detail}.')
            print('   Run with --baseline scripts/asan_leak_baseline.txt to check')
            print('   against known debt, or --list to see the origins.')
            return 1
        print('✅ ASan leaks: none.')
        return 0

    known, ceilings = read_baseline(args.baseline)
    new = sorted(keys - known, key=lambda k: (-by_key[k][0], k))
    gone = sorted(known - keys)
    failed = False

    if new:
        print(f'❌ ASan leaks: {len(new)} leak origin(s) not in the baseline.')
        for key in new:
            nbytes, nobjects, blocks = by_key[key]
            print(f'     {nbytes:9d} B  {nobjects:6d} obj  {blocks:4d} block(s)  {key}')
        print('   Each is an allocation still live at exit whose origin is new.')
        print('   Free it at the owning scope, or — if it is process-scoped state')
        print('   that only close-time teardown can release (#1246) — regenerate:')
        print(f'     scripts/check_asan_leaks.py --write-baseline {args.baseline} '
              f'{" ".join(args.logs) or "<log>"}')
        failed = True

    ceiling_bytes = ceilings.get(CEILING_BYTES)
    if ceiling_bytes is not None and total_bytes > ceiling_bytes:
        print(f'❌ ASan leaks: {total_bytes} leaked byte(s) exceeds the baseline '
              f'ceiling ({ceiling_bytes}).')
        print('   An already-baselined origin is leaking more than it was — the key')
        print('   set cannot see that. Find the growth with:')
        print(f'     scripts/check_asan_leaks.py --list {" ".join(args.logs) or "<log>"}')
        failed = True

    ceiling_objects = ceilings.get(CEILING_OBJECTS)
    if ceiling_objects is not None and total_objects > ceiling_objects:
        print(f'❌ ASan leaks: {total_objects} leaked allocation(s) exceeds the '
              f'baseline ceiling ({ceiling_objects}).')
        print('   An already-baselined origin is leaking more often than it was.')
        failed = True

    if failed:
        return 1

    print(f'✅ ASan leaks: {detail}; no new origins ({len(known)} known).')
    if ceiling_bytes is not None or ceiling_objects is not None:
        print(f'   Ceilings: {total_bytes}/{ceiling_bytes} byte(s), '
              f'{total_objects}/{ceiling_objects} allocation(s).')
    if gone:
        # Reported, never failed on: a shrinking baseline is progress, and the
        # population moves a little with which tests touched a shared singleton.
        print(f'   {len(gone)} baseline origin(s) did not leak this run:')
        for key in gone[:GONE_LIST_LIMIT]:
            print(f'     {key}')
        if len(gone) > GONE_LIST_LIMIT:
            print(f'     ... and {len(gone) - GONE_LIST_LIMIT} more')
        print('   If that holds across runs, regenerate with --write-baseline.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
