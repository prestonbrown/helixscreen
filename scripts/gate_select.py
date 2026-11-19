#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which land-gate stages, and which C++ test cases, a change can reach.

make land-gate runs only what a change can reach. The full C++ suite and every
bats file run nightly (nightly.yml), on every push to main (build.yml), at a
release (.claude/commands/release.md runs the gate with HELIX_GATE_FULL=1), and
on a release/* branch, so what a selection leaves out is still run within a
day. The change is everything between the base and the working tree:
committed since the base, staged, unstaged and untracked.

Subcommands:

  plan     one line per gate stage, "<stage> TAB <run|skip|select|full> TAB <why>"
  changed  the changed paths, one per line (what plan and cxx read)
  cxx      after the test build: the Catch2 input file (-f) of the test cases
           the change reaches, for one filter (the fast suite or the [slow] tier)

THE C++ RULE

A test translation unit (a tests/unit/**.cpp) is reached when:

  1. it changed itself;
  2. its dependency file (the build's .d) lists a changed file: a header, or a
     .cpp it includes;
  3. its .d lists a header a changed production .cpp implements: the headers
     in that .cpp's own .d whose stem is the .cpp's stem, or failing that the
     longest stem that prefixes it at a '_' (printer_state_x.cpp implements
     printer_state.h);
  4. a changed file is data the tests read at run time (ui_xml/, assets/,
     config/, translations/, tests/fixtures/, anything that is not C++ source),
     and the unit's text, or a tests/ header it includes, names the file (its
     basename as a word, or for ui_xml/ its component name; any
     translations/ file reaches units that say "translations"). ui_xml/globals.xml reaches
     every unit that builds UI from XML (one that includes an LVGL test fixture).

The whole suite runs instead (a "valve") when any of these hold, because a
dependency file cannot say what such a change reaches:

  - HELIX_GATE_FULL=1, or the branch is release/*;
  - more than MAX_CHANGED paths changed;
  - the build system or a submodule changed: Makefile, mk/, lv_conf.h, patches/,
    lib/, .gitmodules, or a build step whose output the test link compiles
    (CXX_FULL names them);
  - the test harness changed: a tests/*.cpp (the Catch2 main and amalgamation,
    the shared fixtures and UI test utilities, linked into every test), or this
    selector;
  - the reached units are more than SELECTED_FRACTION of all of them, where a
    selection saves nothing worth its blind spot. A core header lands here: a
    header half the units include selects them all.

Changes that reach no C++ test: docs/ and any *.md, scripts/ (other than those build
steps), tests/shell/, tests/python/, tests/ui/, moonraker-plugin/, .github/,
android/, firmware/, server/, .claude/, .githooks/.

What the rule cannot see: a production .cpp whose behaviour a test reaches
only through another class (bar.cpp calls foo.cpp, the test includes bar.h
alone). Rule 3 covers the class the .cpp implements, not its callers, and the
selector does not follow callers: one hop would select most of the suite for
any change to a class many others use. The backstop is CI: build.yml runs the
whole C++ suite on every push to main, so such a break reds main within the
hour, and the nightly runs everything again.
"""

import argparse
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

MAX_CHANGED = 200
# A path the gate can hand to make (and the test host's shell) as one word of
# a comma-separated list.
SAFE_PATH = re.compile(r'[A-Za-z0-9_./@+=-]+')
SELECTED_FRACTION = 0.5

# Paths whose change makes the whole C++ suite run. A trailing / is a directory.
CXX_FULL = (
    'Makefile', 'mk/', 'lv_conf.h', 'patches/', 'lib/', '.gitmodules',
    'scripts/gate_select.py',
    # The build steps whose output the test link compiles. A generator whose
    # output is committed (unit art, icon constants, translations) is not one:
    # its output changes in the same commit, or a freshness check fails.
    'scripts/gen-git-hash.sh', 'scripts/gen-contributors.sh',
    'scripts/gpu_lib_return_kinds.py', 'scripts/apply_submodule_patch.sh',
    'scripts/check_patch_markers.py', 'scripts/gen_patch_markers.py',
    'scripts/pick-fast-linker.sh',
)
# The test harness: every tests/*.cpp is linked into every test.
HARNESS_RE = re.compile(r'tests/[^/]+\.(cpp|c)$')

# Paths whose change reaches no C++ test.
CXX_NONE = (
    'docs/', 'scripts/', 'tests/shell/', 'tests/python/', 'tests/ui/',
    'moonraker-plugin/', '.github/', 'android/', 'firmware/', 'server/',
    '.claude/', '.githooks/', 'tools/screensaver-perf/',
)
SOURCE_EXT = ('.cpp', '.c', '.cc', '.h', '.hpp', '.inc', '.mm')
HEADER_EXT = ('.h', '.hpp', '.inc')

# A unit that includes one of these builds UI from the XML components.
XML_FIXTURES = ('tests/lvgl_test_fixture.h', 'tests/lvgl_ui_test_fixture.h')

DOC_FILES = ('LICENSE', 'COPYRIGHT', 'THANKS.md', 'CONTRIBUTING.md')

# --------------------------------------------------------------------------
# Stage triggers. A stage runs when a changed path matches one of its
# patterns (a segment glob over the repo-relative path, see glob_re; a trailing / is a directory).
# --------------------------------------------------------------------------

# The build inputs of the desktop app: what tests/ui drives and no-mocks scans.
APP_INPUTS = (
    'src/', 'include/', 'ui_xml/', 'assets/', 'config/', 'translations/', 'lib/',
    'patches/', 'Makefile', 'mk/', 'lv_conf.h', '.gitmodules', 'tools/',
    'CONTRIBUTORS.txt', 'VERSION.txt', 'requirements.txt',
)

STAGE_TRIGGERS = {
    # tests/ui boots the built app and drives it with ctl.
    'ui': APP_INPUTS + ('tests/ui/', 'scripts/screenshot-recipes.sh'),
    # The plugin suite imports moonraker-plugin/ alone, plus the two repo files
    # its tests read.
    'plugin': ('moonraker-plugin/', 'tests/fixtures/journal/',
               'assets/config/helix_macros.cfg', 'requirements.txt'),
    # check_no_mocks.py compiles the branch's packaged units with mocks off.
    'no-mocks': ('src/', 'include/', 'Makefile', 'mk/', 'lv_conf.h',
                 'scripts/check_no_mocks.py'),
    # esp32_syntax_check.py compiles the firmware units the branch reaches.
    'esp32': ('src/', 'include/', 'ui_xml/', 'firmware/', 'lib/', 'patches/',
              'Makefile', 'mk/', 'lv_conf.h', 'scripts/esp32_syntax_check.py',
              'scripts/compile_command.py'),
}

# tests/python reads scripts/ and the repo data the scripts process. A change
# to C++ source alone runs only the files that read C++ source.
PYTHON_ALL = (
    'scripts/', 'tests/python/', 'tests/fixtures/', 'config/', 'ui_xml/',
    'translations/', 'assets/', 'firmware/', 'requirements.txt', 'lib/lvgl',
    'docs/images/doc-screenshots.yml',
)
PYTHON_CXX_READERS = (
    'tests/python/test_cpp_escape_decoding.py',
    'tests/python/test_cpp_translation_coverage.py',
    'tests/python/test_doc_screenshots.py',
    'tests/python/test_toolhead_art.py',
    'tests/python/test_unit_art_kit.py',
)

# Stages each step of the gate has, in the gate's order.
STAGES = ('app-build', 'ci-shards', 'slow', 'bats', 'ui', 'plugin', 'python',
          'theme-tokens', 'xml-schema', 'lint-xml', 'lvgl-event-codes', 'esp32',
          'no-mocks', 'quality')

# The quality gates a docs-only change runs.
DOC_QC_GATES = 'qc_doc_refs qc_doc_anchors qc_doc_images'


def git(root, *args, check=True):
    r = subprocess.run(['git', *args], cwd=root, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True, errors='replace')
    if check and r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


_GLOBS = {}


def glob_re(pattern):
    """A pattern as a regex over repo-relative paths: * and ? stay within one
    path segment, as in tests/shell/tooling-triggers.tsv."""
    if pattern not in _GLOBS:
        _GLOBS[pattern] = re.compile(''.join(
            '[^/]*' if ch == '*' else '[^/]' if ch == '?' else re.escape(ch) for ch in pattern))
    return _GLOBS[pattern]


def matches(path, patterns):
    """Does path fall under any pattern (dir/, exact path, or a segment glob)?"""
    for p in patterns:
        if p.endswith('/'):
            if path.startswith(p):
                return True
        elif path == p or glob_re(p).fullmatch(path):
            return True
    return False


def is_doc(path):
    return path.startswith('docs/') or path.endswith('.md') or path in DOC_FILES


def is_test(path):
    """A test or test data: a tests-only change runs the light test gate."""
    return path.startswith('tests/') or path.startswith('moonraker-plugin/tests/')


def changed_paths(root, base):
    """Paths that differ between base and the working tree, untracked included."""
    paths = set(git(root, 'diff', '--name-only', '--no-renames', base, '--').splitlines())
    paths |= set(git(root, 'ls-files', '--others', '--exclude-standard').splitlines())
    return sorted(p for p in paths if p)


def forced_full(root):
    """Why everything runs regardless of the change, or None."""
    if os.environ.get('HELIX_GATE_FULL') == '1':
        return 'HELIX_GATE_FULL=1'
    branch = git(root, 'symbolic-ref', '--quiet', '--short', 'HEAD', check=False).strip()
    if branch.startswith('release/'):
        return f'the branch is {branch}'
    return None



def cxx_path_verdict(changed):
    """('full', why) | ('none', why) | ('select', why) from the paths alone."""
    if len(changed) > MAX_CHANGED:
        return 'full', f'{len(changed)} changed paths, over {MAX_CHANGED}'
    odd = next((c for c in changed if not SAFE_PATH.fullmatch(c)), None)
    if odd:
        return 'full', f'{odd!r} cannot travel in TEST_SELECT_CHANGED'
    for c in changed:
        if matches(c, CXX_FULL):
            return 'full', f'{c} changed (build system or submodule)'
        if HARNESS_RE.fullmatch(c):
            return 'full', f'{c} changed (test harness, linked into every test)'
    relevant = [c for c in changed if not matches(c, CXX_NONE) and not is_doc(c)]
    if not relevant:
        return 'none', 'no changed path reaches a C++ test'
    return 'select', f'{len(relevant)} changed path(s) reach C++ tests'


# --------------------------------------------------------------------------
# Dependency files
# --------------------------------------------------------------------------

def norm(p):
    return os.path.normpath(p)


def parse_depfile(text):
    """(target source, [prerequisites]) from one make .d file."""
    first = text.split('\n\n', 1)[0]
    body = first.replace('\\\n', ' ')
    _, _, rest = body.partition(':')
    deps = [norm(d) for d in rest.split() if d]
    return (deps[0] if deps else None), deps


def load_depfiles(obj_dir):
    """{source path: set(prerequisites)} for every .d under obj_dir."""
    out = {}
    for d in Path(obj_dir).rglob('*.d'):
        try:
            src, deps = parse_depfile(d.read_text(errors='replace'))
        except OSError:
            continue
        if src:
            out[src] = set(deps)
    return out


def implemented_headers(cpp, deps):
    """The headers a .cpp implements, from its own prerequisites."""
    stem = Path(cpp).stem
    headers = [d for d in deps if d.endswith(HEADER_EXT)]
    same = [h for h in headers if Path(h).stem == stem]
    if same:
        return same
    prefix = [h for h in headers if stem.startswith(Path(h).stem + '_')]
    if not prefix:
        return []
    longest = max(len(Path(h).stem) for h in prefix)
    return [h for h in prefix if len(Path(h).stem) == longest]


class DepIndex:
    """The test units' dependency files, and which units list each file."""

    def __init__(self, deps):
        self.tests = {s: d for s, d in deps.items() if s.startswith('tests/unit/')}
        self.deps = deps
        self._texts = {}
        self._stems = None
        self.rev = {}
        for u, d in self.tests.items():
            for f in d:
                self.rev.setdefault(f, []).append(u)

    def includers(self, f):
        return self.rev.get(f, [])

    def texts(self, unit):
        """The unit and the tests/ headers it includes: where it names its data."""
        if unit not in self._texts:
            self._texts[unit] = [unit] + sorted(
                h for h in self.tests[unit] if h.startswith('tests/') and h.endswith(HEADER_EXT))
        return self._texts[unit]

    def headers_by_stem(self, root, cpp):
        """Fallback for a .cpp with no .d (not built here): same-stem headers in the tree."""
        if self._stems is None:
            self._stems = {}
            for top in ('include', 'src', 'tests'):
                for h in (root / top).rglob('*.h'):
                    self._stems.setdefault(h.stem, []).append(str(h.relative_to(root)))
        return self._stems.get(Path(cpp).stem, [])


class CxxSelection:
    def __init__(self, verdict, why, units=(), reasons=None):
        self.verdict = verdict          # full | none | select
        self.why = why
        self.units = sorted(units)
        self.reasons = reasons or {}    # unit -> first reason it was reached


def select_units(root, changed, index):
    """The test units a change reaches, by the module docstring's rule."""
    verdict, why = cxx_path_verdict(changed)
    if verdict != 'select':
        return CxxSelection(verdict, why)
    if not isinstance(index, DepIndex):
        index = DepIndex(index)
    tests, deps, includers = index.tests, index.deps, index.includers
    if not tests:
        return CxxSelection('full', 'no test dependency files to read (tests not built)')
    total = len(tests)
    reasons = {}

    def reach(unit, why):
        reasons.setdefault(unit, why)

    relevant = [c for c in changed if not matches(c, CXX_NONE) and not is_doc(c)]
    needles = set()
    for c in relevant:
        if c.startswith('tests/unit/') and c.endswith(('.cpp', '.cc')):
            if (root / c).exists():
                reach(c, 'changed')
            continue
        if c.endswith(SOURCE_EXT):
            for u in includers(c):
                reach(u, f'includes {c}')
            if c.endswith(('.cpp', '.c', '.cc', '.mm')):
                own = deps.get(c)
                hdrs = implemented_headers(c, own) if own else index.headers_by_stem(root, c)
                for h in hdrs:
                    for u in includers(h):
                        reach(u, f'includes {h}, which {c} implements')
            continue
        # Data the tests read at run time.
        if c == 'ui_xml/globals.xml':
            for u, d in tests.items():
                if any(f in d for f in XML_FIXTURES):
                    reach(u, 'builds UI from XML, and ui_xml/globals.xml changed')
            continue
        needles.add(Path(c).name)
        if c.startswith('ui_xml/') and c.endswith('.xml'):
            needles.add(Path(c).stem)
        if c.startswith(('translations/', 'ui_xml/translations/')):
            needles.add('translations')

    if needles:
        for u in tests:
            for t in index.texts(u):
                hit = needles & text_words(root, t)
                if hit:
                    reach(u, f'names {min(hit)}' + (f' (in {t})' if t != u else ''))
                    break

    if not reasons:
        return CxxSelection('none', f'{len(relevant)} changed path(s), none reached by a test unit')
    if len(reasons) > SELECTED_FRACTION * total:
        return CxxSelection('full', f'{len(reasons)} of {total} test units reached, '
                                    f'over {int(SELECTED_FRACTION * 100)}%')
    return CxxSelection('select', f'{len(reasons)} of {total} test units reached',
                        reasons.keys(), reasons)


_WORDS = {}
WORD_RE = re.compile(r'[A-Za-z0-9_.-]+')


def text_words(root, rel):
    """The words of a file: runs of name characters, so a path's every segment
    and a file name with its extension are each one word, as is each dotted
    part (a sentence's closing period is not part of the word before it)."""
    if rel not in _WORDS:
        try:
            runs = WORD_RE.findall((root / rel).read_text(errors='replace'))
        except OSError:
            runs = []
        words = set()
        for r in runs:
            r = r.strip('.-')
            words.add(r)
            words.update(r.split('.'))
        _WORDS[rel] = words
    return _WORDS[rel]


# --------------------------------------------------------------------------
# Test cases
# --------------------------------------------------------------------------

def list_cases(test_bin, spec):
    """[(name, source file)] for the cases matching a Catch2 test spec."""
    r = subprocess.run([test_bin, *spec.split(), '--list-tests', '--reporter', 'xml'],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                       errors='replace')
    if r.returncode not in (0, 2):  # 2: no test matched the spec
        raise RuntimeError(f'{test_bin} --list-tests: exit {r.returncode}: {r.stderr.strip()[:200]}')
    out = []
    root = ET.fromstring(r.stdout) if r.stdout.strip() else None
    if root is None:
        return out
    for tc in root.iter('TestCase'):
        out.append((tc.findtext('Name') or '', norm(tc.findtext('SourceInfo/File') or '')))
    return out


def catch2_quote(name):
    """One --input-file line naming exactly this case."""
    return '"' + re.sub(r'([\\,\[\]"])', r'\\\1', name) + '"'


# --------------------------------------------------------------------------
# tests/ui: which pytest files a change reaches
# --------------------------------------------------------------------------
#
# Every tests/ui file boots the built app and drives it through ctl. When the
# ui step runs at all (an app input changed), these run:
#
#   - UI_SMOKE, always: a boot, every XML expression compiling, navigation
#     between the main panels, reading text and state. A few seconds, and what
#     any change to the app can break.
#   - a file that changed, or one of whose UI_TRIGGERS paths changed: the
#     components, sources and data behind the screens it drives (a changed
#     header counts as the same-stem sources it declares);
#   - a file that names a changed ui_xml/ component (its file stem as a word).
#
# Every file runs (a valve) for UI_FULL: the shared harness, ui_xml/globals.xml,
# the UI framework at the top of src/ui/ (and its headers) and the theme, the app's startup,
# navigation and ctl server, the mock printer every file drives (any source or
# header named for a mock: moonraker_client_mock*, ams_backend_mock), fonts, and the
# build. So does a change to a building block, an XML component or widget more
# than UI_BLOCK_USERS other components use (icon, header_bar, ui_button), and a
# selection of more than UI_SELECTED_FRACTION of the files.

UI_EXCLUDED = ('tests/ui/test_screens.py',)  # the goldens: never in CI's slice
UI_SMOKE = (
    'tests/ui/test_navigation.py',
    'tests/ui/test_xml_expressions_boot.py',
    'tests/ui/test_detection_sources_boot.py',
    'tests/ui/test_text.py',
    'tests/ui/test_state.py',
)
UI_FULL = (
    'ui_xml/globals.xml', 'tests/ui/conftest.py', 'tests/ui/helix/',
    'src/ui/*.cpp', 'src/ui/theme/', 'src/ui/components/',
    'src/main.cpp', 'src/xml_registration.cpp', 'src/application/application*',
    'src/application/panel_factory.cpp', 'src/application/subject_initializer.cpp',
    'src/application/display_manager.cpp', 'src/application/main_loop_handler.cpp',
    'src/application/asset_manager.cpp', 'src/application/session_wiring.cpp',
    'include/application*.h', 'include/panel_factory.h', 'include/subject_initializer.h',
    'include/display_manager.h', 'src/remote/', 'include/remote_control_server.h',
    'src/mock/', 'src/*mock*', 'src/*/*mock*', 'include/*mock*',
    'assets/fonts/', 'config/themes/',
    'Makefile', 'mk/', 'lib/', 'patches/', 'lv_conf.h', '.gitmodules', 'requirements.txt',
)
# The screens each file drives beyond the smoke set: their components, the
# sources that build them and the data they show. The harness files (reset,
# wait_idle, freeze, screenshot, diagnostics, goldens) drive the ctl server and
# tests/ui/helix/, which UI_FULL already names.
UI_TRIGGERS = {
    'tests/ui/test_active_spool_geometry.py': (
        'ui_xml/panel_widget_active_spool.xml', 'ui_xml/ams_loaded_card.xml',
        'src/ui/panel_widgets/active_spool_widget.*', 'src/ui/home/', 'src/printer/spoolman*',
        'include/spoolman*', 'ui_xml/home_panel.xml'),
    'tests/ui/test_callouts_recipe.py': (
        'scripts/screenshot-recipes.sh', 'ui_xml/home_panel.xml', 'src/ui/home/',
        'src/ui/panel_widgets/'),
    'tests/ui/test_filament_grade_warning.py': (
        'ui_xml/print_select*', 'src/ui/print/ui_panel_print_select*', 'src/ui/print/print_select*',
        'src/printer/print_start_checks.cpp', 'src/printer/filament_variants.cpp',
        'include/print_start_checks.h', 'include/filament_variants.h',
        'src/printer/ams_backend.cpp', 'assets/test_gcodes/'),
    'tests/ui/test_history_navigation.py': (
        'ui_xml/history_*', 'src/ui/print/ui_panel_history*', 'src/ui/print/ui_history*',
        'include/*history*', 'src/ui/settings/ui_panel_advanced*', 'ui_xml/advanced_panel.xml'),
    'tests/ui/test_modal_geometry.py': (
        'ui_xml/ams_*', 'ui_xml/klipper_recovery_dialog.xml', 'ui_xml/recovery_fault_modal.xml',
        'ui_xml/fault_path*', 'src/ui/filament/', 'src/ui/modals/', 'src/ui/ui_emergency_stop.cpp',
        'include/ams_*'),
    'tests/ui/test_motion_small_screen_geometry.py': (
        'ui_xml/motion_*', 'src/ui/controls/', 'ui_xml/controls_panel.xml'),
    'tests/ui/test_reset.py': ('ui_xml/runout_guidance_modal.xml',),
    'tests/ui/test_screenshot.py': ('src/system/screenshot*', 'include/screenshot*'),
}
UI_SELECTED_FRACTION = 0.75
UI_BLOCK_USERS = 8

_COMPONENT_TAG = re.compile(r'<([A-Za-z_][A-Za-z0-9_]*)')
_WIDGET_REG = re.compile(r'lv_xml_register_widget\(\s*"([A-Za-z0-9_]+)"')


def ui_blocks(root):
    """The building blocks: components and XML widgets more than UI_BLOCK_USERS
    other components use, and the sources that register such a widget."""
    stems, uses = set(), {}
    for f in sorted((root / 'ui_xml').rglob('*.xml')):
        if 'translations' in f.parts:
            continue
        stems.add(f.stem)
        uses.setdefault(f.stem, set()).update(_COMPONENT_TAG.findall(f.read_text(errors='replace')))
    registers = {}
    for f in sorted((root / 'src').rglob('*.cpp')):
        tags = set(_WIDGET_REG.findall(f.read_text(errors='replace')))
        if tags:
            registers[str(f.relative_to(root))] = tags
    nodes = stems.union(*registers.values()) if registers else stems
    users = {}
    for comp, tags in uses.items():
        for t in (tags & nodes) - {comp}:
            users[t] = users.get(t, 0) + 1
    blocks = {n for n, k in users.items() if k > UI_BLOCK_USERS}
    block_sources = {src for src, tags in registers.items() if tags & blocks}
    return blocks, block_sources


def ui_files(root):
    return sorted(str(f.relative_to(root)) for f in (root / 'tests/ui').glob('test_*.py')
                  if str(f.relative_to(root)) not in UI_EXCLUDED)


def select_ui(root, changed):
    """(verdict, why, files, reasons) for the ui step: skip, select or full."""
    code = [c for c in changed if not is_doc(c)]
    hit = next((c for c in code if matches(c, STAGE_TRIGGERS['ui'])), None)
    if not hit:
        return 'skip', 'nothing it reads changed', [], {}
    if len(changed) > MAX_CHANGED:
        return 'full', f'{len(changed)} changed paths, over {MAX_CHANGED}', [], {}
    broad = next((c for c in code if matches(c, UI_FULL)), None)
    # The header of a framework source at the top of src/ui/ is framework too.
    broad = broad or next((c for c in code if c.startswith('include/') and c.endswith('.h')
                           and (root / 'src/ui' / (Path(c).stem + '.cpp')).exists()), None)
    if broad:
        return 'full', f'{broad} changed (every file boots or drives it)', [], {}
    blocks, block_sources = ui_blocks(root)
    for c in code:
        if (c.startswith('ui_xml/') and Path(c).stem in blocks) or c in block_sources:
            return 'full', f'{c} changed (a building block most screens use)', [], {}
    files = ui_files(root)
    reasons = {f: 'smoke: runs on every app change' for f in UI_SMOKE if f in files}
    components = {Path(c).stem for c in code if c.startswith('ui_xml/') and c.endswith('.xml')}
    # A header stands for the sources it declares: include/ui_panel_motion.h
    # reaches what src/ui/controls/ui_panel_motion.cpp's trigger reaches.
    as_sources = list(code)
    headers = [c for c in code if c.startswith('include/') and c.endswith(HEADER_EXT)]
    if headers:
        stems = {Path(h).stem for h in headers}
        as_sources += [str(f.relative_to(root)) for f in (root / 'src').rglob('*.cpp') if f.stem in stems]
    for f in files:
        if f in reasons:
            continue
        if f in changed:
            reasons[f] = 'changed'
            continue
        trig = next((c for c in as_sources if matches(c, UI_TRIGGERS.get(f, ()))), None)
        if trig:
            reasons[f] = f'{trig} changed'
            continue
        named = sorted(text_words(root, f) & components)
        if named:
            reasons[f] = f'names {named[0]}'
    chosen = sorted(reasons)
    if len(chosen) > UI_SELECTED_FRACTION * len(files):
        return 'full', f'{len(chosen)} of {len(files)} files reached', [], {}
    return 'select', f'{len(chosen)} of {len(files)} files: smoke and what the change reaches', chosen, reasons

# --------------------------------------------------------------------------
# Stage plan
# --------------------------------------------------------------------------

def plan(root, changed):
    """(mode, [(stage, verdict, why, arg)]) for the gate.

    verdict: run, skip, select (a narrower run: arg says what) or full. mode:
    full, docs (every path is documentation), tests (every path is a test or
    documentation) or selected."""
    full = forced_full(root)
    if not full and not changed:
        full = 'no change to select from'
    if full:
        return 'full', [(s, 'full' if s in ('ci-shards', 'slow', 'bats') else 'run', full, '')
                        for s in STAGES]
    if all(is_doc(c) for c in changed):
        mode = 'docs'
    elif all(is_test(c) or is_doc(c) for c in changed):
        mode = 'tests'
    else:
        mode = 'selected'
    rows = []
    # Documentation reaches no test step, wherever it lives (scripts/CLAUDE.md).
    code = [c for c in changed if not is_doc(c)]
    cxx, cxx_why = cxx_path_verdict(changed)
    cxx_verdict = {'full': 'full', 'none': 'skip', 'select': 'select'}[cxx]
    for s in STAGES:
        if s == 'app-build':
            if mode == 'docs':
                rows.append((s, 'skip', 'docs-only change: nothing is built', ''))
            else:
                rows.append((s, 'run', 'bats, ui and quality test the binaries it links', ''))
        elif s in ('ci-shards', 'slow'):
            rows.append((s, cxx_verdict, cxx_why, ''))
        elif s == 'bats':
            rows.append((s, 'select', 'tooling files whose paths changed, product files the change reaches', ''))
        elif s == 'python':
            hit = next((c for c in code if matches(c, PYTHON_ALL)), None)
            if hit:
                rows.append((s, 'run', f'{hit} changed', ''))
            elif any(c.startswith(('src/', 'include/')) for c in code):
                rows.append((s, 'select', 'C++ source changed: only the files that read it',
                             ' '.join(PYTHON_CXX_READERS)))
            else:
                rows.append((s, 'skip', 'nothing it reads changed', ''))
        elif s == 'ui':
            v, why, files, _ = select_ui(root, changed)
            rows.append((s, v, why, ' '.join(files)))
        elif s in STAGE_TRIGGERS:
            hit = next((c for c in code if matches(c, STAGE_TRIGGERS[s])), None)
            if hit:
                rows.append((s, 'run', f'{hit} changed', ''))
            else:
                rows.append((s, 'skip', 'nothing it reads changed', ''))
        elif s == 'quality':
            if mode == 'docs':
                rows.append((s, 'select', 'docs-only change: the doc gates alone', DOC_QC_GATES))
            else:
                rows.append((s, 'run', 'every change', ''))
        elif mode == 'docs':
            rows.append((s, 'skip', 'docs-only change', ''))
        else:
            # The freshness checks take a second or two each.
            rows.append((s, 'run', 'every change: a second or two', ''))
    return mode, rows


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def read_changed(root, args):
    if args.changed_from:
        text = Path(args.changed_from).read_text()
        return sorted({l.strip() for l in text.splitlines() if l.strip()})
    if args.changed is not None:
        return sorted({p for p in args.changed.split(',') if p})
    return changed_paths(root, args.base or 'HEAD')


def cmd_cxx(root, args, changed):
    full = forced_full(root)
    if full:
        sel = CxxSelection('full', full)
    else:
        sel = select_units(root, changed, load_depfiles(args.obj_dir))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if sel.verdict != 'select':
        out.write_text('')
        print(f'{sel.verdict.upper()} {sel.why}')
        return 0
    units = set(sel.units)
    cases = [(n, f) for n, f in list_cases(args.bin, args.filter) if f in units]
    if not cases:
        out.write_text('')
        print(f'NONE {sel.why}, but none has a case matching {args.filter}')
        return 0
    out.write_text(''.join(catch2_quote(n) + '\n' for n, _ in cases))
    files = sorted({f for _, f in cases})
    print(f'SELECT {len(cases)} case(s) in {len(files)} file(s): {sel.why}')
    for f in files:
        print(f'  {f}: {sel.reasons.get(f, "")}')
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('command', choices=('plan', 'changed', 'cxx'))
    ap.add_argument('--root', default='.')
    ap.add_argument('--base', help='diff the working tree against this commit')
    ap.add_argument('--changed', help='comma-separated changed paths instead of a git diff')
    ap.add_argument('--changed-from', help='a file of changed paths, one per line')
    ap.add_argument('--obj-dir', default='build/obj')
    ap.add_argument('--bin', default='build/bin/helix-tests')
    ap.add_argument('--filter', default='~[.] ~[slow]', help='Catch2 test spec the cases must match')
    ap.add_argument('--out', default='build/test-select/cases.txt')
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()
    try:
        changed = read_changed(root, args)
        if args.command == 'changed':
            print('\n'.join(changed))
            return 0
        if args.command == 'cxx':
            return cmd_cxx(root, args, changed)
        mode, rows = plan(root, changed)
        print(f'# mode\t{mode}\t{len(changed)} changed path(s)\t')
        for s, v, why, arg in rows:
            print(f'{s}\t{v}\t{why}\t{arg}')
        return 0
    except (RuntimeError, OSError, ET.ParseError) as e:
        print(f'gate_select: {e}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
