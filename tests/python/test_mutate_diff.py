# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/mutate_diff.py: the declaration-only skip, the stop
signals, the --max-minutes cap, the orphan stop and the proven-commit record.

The verdict logic has its meta-tests in tests/shell/test_mutate_diff_gate.bats.
These cover the bounds around it, which need what bats makes awkward: sending a
signal at a known instant mid-mutant and then asserting on bytes, process
groups and git notes.

Each integration test builds a throwaway repo with a stub `make test-build`
and a stub test binary, the same model the bats fixture uses: the stub build
leaves a different binary behind, and the stub suite prints a Catch2 failure
when the change's marker is gone.
"""

import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

import mutate_diff as md  # noqa: E402

# Orphan detection follows Linux reparenting, the jobserver handoff reopens its
# descriptors through /proc/self/fd, and parallel workers copy the tree with GNU
# cp --reflink. The test host runs these; a macOS checkout has none of the three.
linux_only = pytest.mark.skipif(sys.platform != "linux",
                                reason="needs /proc, GNU cp --reflink and Linux reparenting")


# ---------------------------------------------------------------------------
# The classifier, as a pure function of two file versions
# ---------------------------------------------------------------------------
HEADER = """#pragma once
#include <string>
namespace helix {
class Foo {
  public:
    Foo();
    int get() const;
  private:
    int n_;
};
}  // namespace helix
"""


def _reason(pre: str, post: str):
    """declaration_only_reason() over the changed lines difflib finds."""
    import difflib
    a, b = md.code_only_lines(pre, "c"), md.code_only_lines(post, "c")
    removed, added = [], []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=a, b=b).get_opcodes():
        if tag != "equal":
            removed += [i for i in range(i1, i2) if a[i].strip()]
            added += [j for j in range(j1, j2) if b[j].strip()]
    return md.declaration_only_reason(a, b, removed, added)


def _edit(old: str, new: str) -> str:
    assert old in HEADER
    return HEADER.replace(old, new)


DECLARATION_ONLY = {
    "member function": _edit("    int get() const;\n", "    int get() const;\n    void set(int v);\n"),
    "destructor": _edit("    Foo();\n", "    Foo();\n    ~Foo();\n"),
    "deleted copy": _edit("    Foo();\n", "    Foo();\n    Foo(const Foo&) = delete;\n"),
    "noexcept added": _edit("int get() const;", "int get() const noexcept;"),
    "const added": _edit("int get() const;", "int get();"),
    "pragma once": HEADER.replace("#pragma once\n", ""),
    "forward declaration": _edit("class Foo {", "class Bar;\nclass Foo {"),
    "opaque enum": _edit("class Foo {", "enum class E : int;\nclass Foo {"),
    "friend class": _edit("  private:\n", "    friend class Peer;\n  private:\n"),
    "access specifier": _edit("    Foo();\n", "  protected:\n    Foo();\n"),
    "static_assert": _edit("    int n_;\n", '    int n_;\n    static_assert(sizeof(int) == 4, "x");\n'),
    "signature change": _edit("int get() const;", "int get(int k) const;"),
    "new class, no base": _edit("class Foo {", "class Baz {\n  public:\n    void go();\n};\nclass Foo {"),
}

BEHAVIOURAL = {
    "include added": _edit("#include <string>\n", "#include <string>\n#include <cmath>\n"),
    "member initializer": _edit("    int n_;\n", "    int n_;\n    bool flag_ = false;\n"),
    "brace initializer": _edit("    int n_;\n", "    int n_;\n    int m_{3};\n"),
    "private plain member": _edit("    int n_;\n", "    int n_;\n    bool flag_;\n"),
    "static data member": _edit("    int n_;\n", "    int n_;\n    static int count_;\n"),
    "member type change": _edit("int n_;", "long n_;"),
    "member in a derived class": _edit("class Foo {", "class Foo : public Base {").replace("    int n_;\n", "    int n_;\n    int x_;\n"),
    "virtual added": _edit("int get() const;", "virtual int get() const;"),
    "standalone virtual line": _edit("    int get() const;\n", "    virtual\n    int get() const;\n"),
    "standalone static line": _edit("    int get() const;\n", "    static\n    int get();\n"),
    "new virtual function": _edit("    int get() const;\n", "    int get() const;\n    virtual void tick();\n"),
    "pure virtual": _edit("    int get() const;\n", "    int get() const;\n    virtual void tick() = 0;\n"),
    "class to struct": _edit("class Foo {", "struct Foo {"),
    "alignas": _edit("class Foo {", "class alignas(16) Foo {"),
    "alignas on a new class": _edit("class Foo {", "class alignas(16) Bar {\n  public:\n    void go();\n};\nclass Foo {"),
    "friend operator": _edit("  private:\n", "    friend bool operator==(const Foo&, const Foo&);\n  private:\n"),
    "explicit specialisation": _edit("    int get() const;\n", "    int get() const;\n    template <> void put<int>(int);\n"),
    "explicit instantiation": _edit("class Foo {", "template class Box<int>;\nclass Foo {"),
    "#define": _edit("#include <string>\n", "#include <string>\n#define X 1\n"),
    "inline body": _edit("    int get() const;\n", "    int get() const;\n    int twice() const { return 2 * n_; }\n"),
    "default argument": _edit("    int get() const;\n", "    int get() const;\n    void set(int v = 3);\n"),
    "macro at class scope": _edit("    Foo();\n", "    Foo();\n    HELIX_KIND(foo);\n"),
    "public aggregate field": _edit("class Foo {", "struct P {\n    int a;\n};\nclass Foo {"),
    "enumerator": _edit("class Foo {", "enum class E { A, B };\nclass Foo {"),
    "class with a base": _edit("class Foo {", "class Baz : public Foo2 {\n  public:\n    void go();\n};\nclass Foo {"),
    "namespace prototype": _edit("class Foo {", "void free_fn(int);\nclass Foo {"),
    "namespace alias": _edit("class Foo {", "using Id = int;\nclass Foo {"),
    "class alias": _edit("    int n_;\n", "    int n_;\n    using value_type = int;\n"),
    "extern": _edit("class Foo {", "extern int g_counter;\nclass Foo {"),
    "namespace variable": _edit("class Foo {", "int g_counter;\nclass Foo {"),
    "using-declaration": _edit("    int get() const;\n", "    int get() const;\n    using Base::get;\n"),
}


@pytest.mark.parametrize("name", sorted(DECLARATION_ONLY))
def test_declaration_only_shapes_are_skipped(name):
    assert _reason(HEADER, DECLARATION_ONLY[name]), name


@pytest.mark.parametrize("name", sorted(DECLARATION_ONLY))
def test_declaration_only_holds_in_reverse(name):
    # Removing the same declaration is the same question asked backwards.
    assert _reason(DECLARATION_ONLY[name], HEADER), name


@pytest.mark.parametrize("name", sorted(BEHAVIOURAL))
def test_behavioural_shapes_are_mutated(name):
    assert _reason(HEADER, BEHAVIOURAL[name]) is None, name


def test_explicit_added_to_an_existing_constructor_is_mutated():
    # Dropping `explicit` compiles and lets implicit conversions through.
    pre = _edit("    Foo();\n", "    Foo();\n    Foo(int);\n")
    assert _reason(pre, pre.replace("    Foo(int);", "    explicit Foo(int);")) is None


def test_a_whole_new_header_of_declarations_is_skipped():
    hdr = HEADER.replace("#include <string>\n", "").replace("  private:\n    int n_;\n", "")
    assert _reason("", hdr)


def test_a_declaration_beside_a_body_change_is_mutated():
    pre = "int f(int n) {\n    return n + 1;\n}\n"
    post = "class G;\nint f(int n) {\n    return n + 2;\n}\n"
    assert _reason(pre, post) is None


# ---------------------------------------------------------------------------
# Integration fixture
# ---------------------------------------------------------------------------
ORIGINAL = "int f(int n) {\n    return n + 1;\n}\n"
CHANGED = "int f(int n) {\n    return n + 2;   // NEW_BEHAVIOR\n}\n"

# The mutant build hangs (a sleeping grandchild included) when the marker is
# gone, recording its pids, so a test can stop the run at a known instant and
# then prove the whole group died.
SLOW_MUTANT_MAKEFILE = """test-build:
\t@if ! grep -q NEW_BEHAVIOR src/feature.cpp; then \\
\t    echo $$$$ > .mutant-build-started; \\
\t    sleep {secs} & echo $$! > .mutant-build-child; wait; \\
\tfi
\t@[ -e build/bin/helix-tests ] && printf '\\n# relinked\\n' >> build/bin/helix-tests || true
"""

FAST_MAKEFILE = """test-build:
\t@[ -e build/bin/helix-tests ] && printf '\\n# relinked\\n' >> build/bin/helix-tests || true
"""

DETECTING_SUITE = """#!/usr/bin/env bash
grep -q NEW_BEHAVIOR src/feature.cpp || {
    echo "tests/unit/test_feature.cpp:11: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
}
[ ! -e .mutant-expect-other ] || grep -q NEW_TOO src/other.cpp 2>/dev/null || {
    echo "tests/unit/test_other.cpp:3: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
}
exit 0
"""


# The host's jobpool shim stays out of a fixture's stub make.
ENV = {**os.environ, "JOBPOOL": "0"}


def _git(work, *args):
    return subprocess.run(["git", *args], cwd=work, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout


def _fixture(tmp_path, makefile=FAST_MAKEFILE, other=False):
    work = tmp_path / "repo"
    (work / "scripts").mkdir(parents=True)
    (work / "src").mkdir()
    (work / "build" / "bin").mkdir(parents=True)
    shutil.copy(SCRIPTS_DIR / "mutate_diff.py", work / "scripts")
    shutil.copy(SCRIPTS_DIR / "diff_base.py", work / "scripts")
    (work / "src" / "feature.cpp").write_text(ORIGINAL)
    if other:
        (work / "src" / "other.cpp").write_text("int g() { return 1; }\n")
    (work / ".gitignore").write_text("build/\n.mutant-*\n")
    _git(work, "init", "-q", "-b", "main")
    _git(work, "config", "user.email", "t@t")
    _git(work, "config", "user.name", "t")
    _git(work, "add", ".")
    _git(work, "commit", "-qm", "base")
    _git(work, "checkout", "-qb", "feature")
    (work / "src" / "feature.cpp").write_text(CHANGED)
    (work / "Makefile").write_text(makefile)
    suite = work / "build" / "bin" / "helix-tests"
    suite.write_text(DETECTING_SUITE)
    suite.chmod(0o755)
    return work


def _mutate(work, *args, timeout=120):
    return subprocess.run(
        [sys.executable, "scripts/mutate_diff.py", "--shards", "1",
         "--log", str(work / "mutate.log"), *args],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, env=ENV)


def _wait_for(path: Path, secs=60.0):
    deadline = time.monotonic() + secs
    while time.monotonic() < deadline:
        if path.exists() and path.read_text().strip():
            return int(path.read_text().split()[0])
        time.sleep(0.05)
    raise AssertionError(f"{path} never appeared")


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A zombie still answers kill(0) until its parent reaps it.
    try:
        return Path(f"/proc/{pid}/stat").read_text().split()[2] != "Z"
    except OSError:
        return False


# ---------------------------------------------------------------------------
# Stop signals
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("sig", [signal.SIGINT, signal.SIGTERM, signal.SIGHUP])
def test_a_stop_signal_mid_mutant_restores_the_file_byte_for_byte(tmp_path, sig):
    work = _fixture(tmp_path, SLOW_MUTANT_MAKEFILE.replace("{secs}", "60"))
    before = (work / "src" / "feature.cpp").read_bytes()
    proc = subprocess.Popen(
        [sys.executable, "scripts/mutate_diff.py", "--base", "main", "--shards", "1",
         "--log", str(work / "mutate.log")],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=True, env=ENV,
        # A run keeps an inherited SIGHUP ignore, so a nohup'd pytest would hand it one.
        preexec_fn=lambda: signal.signal(signal.SIGHUP, signal.SIG_DFL))
    try:
        _wait_for(work / ".mutant-build-started")
        child = _wait_for(work / ".mutant-build-child")
        # The mutant really is in the tree at this instant.
        assert (work / "src" / "feature.cpp").read_bytes() != before
        os.kill(proc.pid, sig)
        out, _ = proc.communicate(timeout=60)
    finally:
        if proc.poll() is None:
            proc.kill()
    assert proc.returncode == 128 + sig, out
    assert (work / "src" / "feature.cpp").read_bytes() == before, out
    assert f"STOPPED by {signal.Signals(sig).name}" in out
    deadline = time.monotonic() + 5
    while _alive(child) and time.monotonic() < deadline:
        time.sleep(0.05)
    assert not _alive(child), "the build's grandchild outlived the run"


def test_an_inherited_sighup_ignore_is_kept(tmp_path):
    # nohup is a deliberate detach: a hangup must not stop that run.
    work = _fixture(tmp_path, SLOW_MUTANT_MAKEFILE.replace("{secs}", "3"))
    proc = subprocess.Popen(
        [sys.executable, "scripts/mutate_diff.py", "--base", "main", "--shards", "1",
         "--log", str(work / "mutate.log")],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=True, env=ENV,
        preexec_fn=lambda: signal.signal(signal.SIGHUP, signal.SIG_IGN))
    try:
        _wait_for(work / ".mutant-build-started")
        os.kill(proc.pid, signal.SIGHUP)
        out, _ = proc.communicate(timeout=60)
    finally:
        if proc.poll() is None:
            proc.kill()
    assert "STOPPED" not in out
    assert "VERDICT: CLEAN" in out, out


# ---------------------------------------------------------------------------
# The wall-clock cap
# ---------------------------------------------------------------------------
def _two_hunk_fixture(tmp_path, makefile):
    """feature.cpp and other.cpp both changed, uncommitted; returns (work, base)."""
    work = _fixture(tmp_path, makefile)
    (work / "src" / "feature.cpp").write_text(ORIGINAL)
    (work / "src" / "other.cpp").write_text("int g() { return 1; }\n")
    _git(work, "add", "src")
    _git(work, "commit", "-qm", "other")
    base = _git(work, "rev-parse", "HEAD").strip()
    (work / "src" / "feature.cpp").write_text(CHANGED)
    (work / "src" / "other.cpp").write_text("int g() { return 2; }  // NEW_TOO\n")
    return work, base


@pytest.mark.parametrize("only", [["--only", "feature,third"],
                                  ["--only", "feature", "--only", "third"]])
def test_only_keeps_a_file_matching_any_pattern(tmp_path, only):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    (work / "src" / "third.cpp").write_text("int h() { return 3; }\n")
    _git(work, "add", "-N", "src/third.cpp")
    r = _mutate(work, "--base", base, *only, "--list-only")
    assert r.returncode == 0, r.stdout
    assert "src/feature.cpp:1" in r.stdout
    assert "src/third.cpp:1" in r.stdout
    assert "src/other.cpp:1" not in r.stdout


def test_max_minutes_stops_the_run_and_names_what_it_never_reached(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, SLOW_MUTANT_MAKEFILE.replace("{secs}", "60"))
    before = {p: (work / p).read_bytes() for p in ("src/feature.cpp", "src/other.cpp")}

    t0 = time.monotonic()
    r = _mutate(work, "--base", base, "--max-minutes", "0.05")
    took = time.monotonic() - t0
    child = _wait_for(work / ".mutant-build-child")

    assert r.returncode == 3, r.stdout
    assert took < 40, f"the cap did not bound the run ({took:.0f}s)"
    assert "--max-minutes 0.05 reached" in r.stdout
    assert "UNEXAMINED    src/feature.cpp:1" in r.stdout
    assert "UNEXAMINED    src/other.cpp:1" in r.stdout
    assert "VERDICT: INCOMPLETE" in r.stdout
    assert "hunk(s) UNEXAMINED" in r.stdout
    assert "VERDICT: CLEAN" not in r.stdout
    for p, want in before.items():
        assert (work / p).read_bytes() == want, p
    assert not _alive(child), "the capped build's grandchild outlived the run"


def test_max_minutes_zero_is_no_cap_for_a_release_run(tmp_path):
    work = _fixture(tmp_path)
    r = _mutate(work, "--base", "main", "--max-minutes", "0", "--release")
    assert r.returncode == 0, r.stdout
    assert "VERDICT: CLEAN" in r.stdout


# ---------------------------------------------------------------------------
# A run whose parent exits
# ---------------------------------------------------------------------------
# The first mutant's build ends only once the run's parent is gone, so the parent
# outlives the run's startup and dies before the first hunk finishes.
ORPHANED_BUILD_MAKEFILE = """test-build:
\t@if ! grep -q NEW_BEHAVIOR src/feature.cpp; then \\
\t    touch .mutant-build-started; \\
\t    while [ ! -e .parent-gone ]; do sleep 0.05; done; \\
\tfi
\t@[ -e build/bin/helix-tests ] && printf '\\n# relinked\\n' >> build/bin/helix-tests || true
"""


@linux_only
def test_an_orphaned_run_stops_before_its_next_hunk(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, ORPHANED_BUILD_MAKEFILE)
    out = work / "orphan.out"
    # The shell starts the run and exits during the first mutant's build, the
    # way a harness that reaps its shell but not the shell's children leaves it.
    subprocess.run(
        ["sh", "-c", f'{sys.executable} scripts/mutate_diff.py --base {base} --shards 1 '
                     f'--log mutate.log > {out} 2>&1 & echo $! > orphan.pid; '
                     'i=0; while [ ! -e .mutant-build-started ] && [ "$i" -lt 3600 ]; '
                     'do sleep 0.05; i=$((i + 1)); done'],
        cwd=work, check=True, timeout=240, env=ENV)
    pid = _wait_for(work / "orphan.pid")
    assert (work / ".mutant-build-started").exists(), out.read_text()
    (work / ".parent-gone").touch()
    deadline = time.monotonic() + 240
    while _alive(pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    text = out.read_text()
    assert not _alive(pid), text
    assert "stopping before the next hunk" in text, text
    assert "UNEXAMINED" in text
    assert "the parent process exited" in text
    assert (work / "src" / "feature.cpp").read_text() == CHANGED
    assert "NEW_TOO" in (work / "src" / "other.cpp").read_text()


# ---------------------------------------------------------------------------
# Declaration-only hunks in a real run
# ---------------------------------------------------------------------------
def test_declaration_only_hunks_are_reported_and_keep_the_run_clean(tmp_path):
    work = _fixture(tmp_path)
    (work / "include").mkdir()
    (work / "include" / "feature.h").write_text(HEADER)
    _git(work, "add", "include/feature.h")
    _git(work, "commit", "-qm", "header")
    _git(work, "branch", "-f", "main")
    (work / "include" / "feature.h").write_text(DECLARATION_ONLY["member function"])
    r = _mutate(work, "--base", "main")
    assert r.returncode == 0, r.stdout
    assert "skipped (declaration-only): member function declaration" in r.stdout
    assert "1 hunk(s) skipped (declaration-only)" in r.stdout
    assert "VERDICT: CLEAN (1 hunk(s) not built: declaration-only, unexamined)" in r.stdout
    assert "every other hunk was mutated" in r.stdout
    # Never counted as a kill.
    assert "1 hunk(s) mutated: killed=1" in r.stdout


def test_no_skip_declarations_mutates_them(tmp_path):
    work = _fixture(tmp_path)
    (work / "include").mkdir()
    (work / "include" / "feature.h").write_text(HEADER)
    _git(work, "add", "include/feature.h")
    _git(work, "commit", "-qm", "header")
    _git(work, "branch", "-f", "main")
    (work / "include" / "feature.h").write_text(DECLARATION_ONLY["member function"])
    r = _mutate(work, "--base", "main", "--no-skip-declarations", "--list-only")
    assert "declaration-only" not in r.stdout
    assert "include/feature.h:" in r.stdout


# ---------------------------------------------------------------------------
# The proven-commit record
# ---------------------------------------------------------------------------
def _proven(work):
    r = subprocess.run(["git", "notes", "--ref", md.PROOF_REF, "list"], cwd=work,
                       text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return {ln.split()[1] for ln in r.stdout.splitlines() if ln.strip()}


def test_a_clean_run_on_a_committed_tree_records_head(tmp_path):
    work = _fixture(tmp_path)
    _git(work, "commit", "-qam", "change")
    r = _mutate(work)
    assert r.returncode == 0, r.stdout
    head = _git(work, "rev-parse", "HEAD").strip()
    assert _proven(work) == {head}
    assert "recorded in refs/notes/mutate-proven" in r.stdout


def test_a_dirty_tree_is_not_recorded(tmp_path):
    work = _fixture(tmp_path)
    r = _mutate(work)
    assert r.returncode == 0, r.stdout
    assert _proven(work) == set()
    assert "not recorded as proven" in r.stdout


def test_a_survivor_is_not_recorded(tmp_path):
    work = _fixture(tmp_path)
    (work / "build" / "bin" / "helix-tests").write_text("#!/usr/bin/env bash\nexit 0\n")
    _git(work, "commit", "-qam", "change")
    r = _mutate(work)
    assert r.returncode == 1, r.stdout
    assert _proven(work) == set()


def test_no_record_writes_nothing(tmp_path):
    work = _fixture(tmp_path)
    _git(work, "commit", "-qam", "change")
    assert _mutate(work, "--no-record").returncode == 0
    assert _proven(work) == set()


def _commit_other_fix(work, msg="the fix"):
    """Commit a new src/other.cpp the stub suite checks for."""
    (work / ".mutant-expect-other").write_text("1\n")
    (work / "src" / "other.cpp").write_text("int g() { return 1; }\n")
    _git(work, "add", "src/other.cpp")
    _git(work, "commit", "--allow-empty", "-qm", f"{msg}: the file it edits")
    (work / "src" / "other.cpp").write_text("int g() { return 2; }  // NEW_TOO\n")
    _git(work, "add", "src/other.cpp")
    _git(work, "commit", "-qm", msg)


def _proven_then_new_change(tmp_path):
    work = _fixture(tmp_path, other=True)
    _git(work, "commit", "-qam", "change")
    assert _mutate(work).returncode == 0
    _commit_other_fix(work)
    return work


def test_the_next_run_mutates_only_the_delta_since_the_proven_commit(tmp_path):
    work = _proven_then_new_change(tmp_path)
    r = _mutate(work, "--list-only")
    assert "last commit proven by mutate-diff" in r.stdout
    assert "src/other.cpp:1" in r.stdout
    assert "src/feature.cpp" not in r.stdout


def test_whole_branch_ignores_the_record(tmp_path):
    work = _proven_then_new_change(tmp_path)
    r = _mutate(work, "--list-only", "--whole-branch")
    assert "merge-base with main" in r.stdout
    assert "src/feature.cpp:1" in r.stdout
    assert "src/other.cpp:1" in r.stdout


def test_since_names_the_proven_commit_by_hand(tmp_path):
    work = _proven_then_new_change(tmp_path)
    proven = _git(work, "rev-parse", "HEAD~1").strip()
    r = _mutate(work, "--list-only", "--since", proven)
    assert f"--since {proven}" in r.stdout
    assert "src/feature.cpp" not in r.stdout


def test_since_refuses_a_commit_that_is_not_an_ancestor(tmp_path):
    work = _proven_then_new_change(tmp_path)
    _git(work, "checkout", "-qb", "elsewhere", "main")
    (work / "x.txt").write_text("x\n")
    _git(work, "add", "x.txt")
    _git(work, "commit", "-qm", "elsewhere")
    other = _git(work, "rev-parse", "HEAD").strip()
    _git(work, "checkout", "-q", "feature")
    r = _mutate(work, "--list-only", "--since", other)
    assert r.returncode == 2, r.stdout
    assert "not an ancestor of HEAD" in r.stdout


def test_a_delta_that_edits_a_test_falls_back_to_the_fork_point(tmp_path):
    work = _fixture(tmp_path)
    (work / "tests").mkdir()
    (work / "tests" / "test_feature.cpp").write_text("CHECK(f(0) == 2);\n")
    _git(work, "add", ".")
    _git(work, "commit", "-qm", "change + test")
    assert _mutate(work).returncode in (0, 3)
    assert _proven(work), "the first run should have recorded HEAD"
    (work / "tests" / "test_feature.cpp").write_text("CHECK(true);\n")
    _git(work, "commit", "-qam", "weaken the test")
    r = _mutate(work, "--list-only")
    assert "not used: tests/ has lines removed or edited" in r.stdout
    assert "merge-base with main" in r.stdout
    assert "src/feature.cpp:1" in r.stdout


def test_since_a_commit_nobody_proved_records_nothing(tmp_path):
    work = _fixture(tmp_path)
    _git(work, "commit", "-qam", "change")
    _commit_other_fix(work, "more")
    r = _mutate(work, "--since", "HEAD~1")
    assert r.returncode == 0, r.stdout
    assert "is not a proven commit" in r.stdout
    assert _proven(work) == set()


def test_an_arbitrary_base_records_nothing(tmp_path):
    work = _fixture(tmp_path)
    _git(work, "commit", "-qam", "change")
    _commit_other_fix(work, "more")
    r = _mutate(work, "--base", "HEAD~1")
    assert "neither the fork point nor a proven commit" in r.stdout, r.stdout
    assert _proven(work) == set()


def test_a_base_naming_the_fork_point_records(tmp_path):
    work = _fixture(tmp_path)
    _git(work, "commit", "-qam", "change")
    assert _mutate(work, "--base", "main").returncode == 0
    assert _proven(work) == {_git(work, "rev-parse", "HEAD").strip()}


def test_since_a_proven_commit_records(tmp_path):
    work = _proven_then_new_change(tmp_path)
    (proven,) = _proven(work)
    r = _mutate(work, "--since", proven)
    assert r.returncode == 0, r.stdout
    assert _git(work, "rev-parse", "HEAD").strip() in _proven(work)


def test_head_moving_during_the_run_records_nothing(tmp_path):
    drift = FAST_MAKEFILE.replace(
        "test-build:\n", "test-build:\n\t@grep -q NEW_BEHAVIOR src/feature.cpp || "
                         "git commit -q --allow-empty -m drift\n")
    work = _fixture(tmp_path, drift)
    _git(work, "commit", "-qam", "change")
    r = _mutate(work)
    assert r.returncode == 0, r.stdout
    assert "HEAD moved during the run" in r.stdout
    assert _proven(work) == set()


def test_an_untracked_test_file_records_nothing(tmp_path):
    work = _fixture(tmp_path)
    _git(work, "commit", "-qam", "change")
    (work / "tests").mkdir()
    (work / "tests" / "test_local.cpp").write_text("CHECK(f(0) == 2);\n")
    r = _mutate(work)
    assert "untracked files under tests/, src/ or include/" in r.stdout, r.stdout
    assert _proven(work) == set()


def test_not_covered_paths_ride_along_with_the_proof(tmp_path):
    work = _fixture(tmp_path)
    (work / "tests").mkdir()
    (work / "tests" / "test_feature.cpp").write_text("CHECK(f(0) == 2);\n")
    _git(work, "add", ".")
    _git(work, "commit", "-qm", "change + test")
    assert _mutate(work).returncode == 3
    proven = _git(work, "rev-parse", "HEAD").strip()
    note = _git(work, "notes", "--ref", md.PROOF_REF, "show", proven)
    assert "not-covered: tests/test_feature.cpp" in note
    _commit_other_fix(work)
    r = _mutate(work)
    assert "last commit proven by mutate-diff" in r.stdout
    assert f"NOT COVERED tests/test_feature.cpp - not covered when {proven[:12]} was proven" in r.stdout
    assert r.returncode == 3, r.stdout
    assert "VERDICT: CLEAN" not in r.stdout


@linux_only
def test_a_tracked_child_does_not_start_with_stop_signals_blocked(tmp_path):
    rc, out = md.run_tracked(["grep", "^SigBlk", "/proc/self/status"], str(tmp_path))
    mask = int(out.split()[1], 16)
    for sig in md.STOP_SIGNALS:
        assert not mask & (1 << (sig - 1)), out


# ---------------------------------------------------------------------------
# Shards fit the filter: a shard that matches no case judges nothing, so a
# narrow --tests must not be split wider than the cases it selects.
# ---------------------------------------------------------------------------
@pytest.mark.parametrize('requested, matched, expected', [
    (8, 3, 3),
    (8, 50, 8),
    (8, 1, 1),
    (1, 50, 1),
])
def test_shards_never_exceed_the_matched_cases(requested, matched, expected):
    assert md.fit_shards(requested, matched) == expected


def test_a_filter_matching_nothing_is_refused():
    with pytest.raises(ValueError, match='matches no test case'):
        md.fit_shards(8, 0)


def _fake_catch2(tmp_path, footer):
    exe = tmp_path / 'helix-tests'
    exe.write_text(f'#!/bin/sh\necho "Matching test cases:"\necho "  a case"\n'
                   f'echo\necho "{footer}"\n')
    exe.chmod(0o755)
    return exe


@pytest.mark.parametrize('footer, expected', [
    ('3 matching test cases', 3),
    ('1 matching test case', 1),
    ('0 matching test cases', 0),
    ('no listing here', None),
])
def test_case_count_is_read_from_the_catch2_listing(tmp_path, footer, expected):
    exe = _fake_catch2(tmp_path, footer)
    assert md.count_catch2_cases(tmp_path, exe, '[tag]') == expected


# ---------------------------------------------------------------------------
# The job budget: every make the run starts is handed the jobserver it joined,
# and a slot beside the run's own is a token taken from it.
# ---------------------------------------------------------------------------
@linux_only
def test_a_child_is_handed_the_jobserver_descriptors(monkeypatch, tmp_path):
    r, w = os.pipe()
    os.set_inheritable(r, False)
    os.set_inheritable(w, False)
    env = {'MAKEFLAGS': f' -j --jobserver-auth={r},{w}'}
    budget = md.JobBudget()
    budget.join_inherited(env)
    monkeypatch.setattr(md, 'JOBS', budget)
    try:
        rc, out = md.run_tracked(['sh', '-c', f'[ -e /proc/self/fd/{r} ] && '
                                              f'[ -e /proc/self/fd/{w} ] && echo open'],
                                 str(tmp_path))
        assert out.strip() == 'open', 'a make started here would build at -j1'
    finally:
        os.close(r)
        os.close(w)


def test_a_jobserver_named_but_not_handed_down_is_dropped(tmp_path):
    env = {'MAKEFLAGS': ' -j --jobserver-auth=97,98'}
    budget = md.JobBudget()
    budget.join_inherited(env)
    assert '--jobserver-auth' not in env['MAKEFLAGS']
    assert not budget.pooled
    assert budget.pass_fds == ()


def _bats_cmd(monkeypatch, tmp_path, tokens):
    """The bats command line one Suites run builds when `tokens` slots are free."""
    class Budget:
        free = tokens
        def try_token(self):
            if self.free:
                self.free -= 1
                return object()
            return None
        def give(self, tok):
            pass
    monkeypatch.setattr(md, 'JOBS', Budget())
    monkeypatch.setattr(md.shutil, 'which', lambda name: '/usr/bin/' + name)
    seen = []
    args = type('A', (), {'jobs': 4, 'shards': 1, 'shell_tests': 'tests/shell'})()
    suites = md.Suites(tmp_path, args, open(os.devnull, 'w'))
    monkeypatch.setattr(suites, '_run_group',
                        lambda cmd, *a: (seen.append(cmd), (md.GREEN, ""))[1])
    suites._dispatch('bats', 1.0)
    return seen[0]


def test_bats_runs_serially_when_no_slot_beside_its_own_is_free(monkeypatch, tmp_path):
    cmd = _bats_cmd(monkeypatch, tmp_path, 0)
    assert '--jobs' not in cmd and '--no-parallelize-within-files' not in cmd, \
        'bats refuses --no-parallelize-within-files with --jobs 1'


def test_bats_takes_the_free_slots_as_jobs(monkeypatch, tmp_path):
    cmd = _bats_cmd(monkeypatch, tmp_path, 2)
    assert cmd[cmd.index('--jobs') + 1] == '3'


@linux_only
def test_slots_beside_the_runs_own_are_tokens_and_go_back():
    budget = md.JobBudget()
    env = {}
    assert budget.make_private(3, env)
    assert '--jobserver-auth=' in env['MAKEFLAGS']
    base = budget.take_base(lambda: False)
    assert base is md.OWN_SLOT
    toks = [budget.try_token() for _ in range(3)]
    assert toks[2] is None and all(t is not None for t in toks[:2]), \
        'a budget of 3 is the run\'s own slot and 2 tokens'
    for t in toks[:2]:
        budget.give(t)
    assert budget.try_token() is not None


@linux_only
def test_a_waiting_worker_gives_up_when_the_run_stops():
    budget = md.JobBudget()
    assert budget.make_private(1, {})
    budget.take_base(lambda: False)           # the only slot
    t0 = time.monotonic()
    with pytest.raises(md.OutOfTime):
        budget.take_base(lambda: time.monotonic() - t0 > 0.3)


# ---------------------------------------------------------------------------
# The C++ suite: small shards dispatched longest first, stopped at the first
# detection, each timed against its own baseline.
# ---------------------------------------------------------------------------
def _shard_bin(tmp_path, body):
    exe = tmp_path / 'helix-tests'
    exe.write_text('#!/usr/bin/env bash\n'
                   'i=0; while [ $# -gt 0 ]; do [ "$1" = --shard-index ] && i=$2; shift; done\n'
                   + body)
    exe.chmod(0o755)
    return exe


def test_a_detection_stops_the_shards_still_running(tmp_path):
    exe = _shard_bin(tmp_path, 'if [ "$i" = 0 ]; then echo "t.cpp:1: FAILED:"; exit 1; fi\n'
                               'sleep 30; echo "All tests passed (1 assertion in 1 test case)"\n')
    plan = md.Catch2Plan(4, 4)
    plan.limits = {i: 600.0 for i in range(4)}
    t0 = time.monotonic()
    state, why, _ = md.run_catch2(tmp_path, exe, '[x]', plan, open(os.devnull, 'w'))
    assert state == md.DETECTED and why == ''
    assert time.monotonic() - t0 < 10, 'the run waited on shards after a detection'


def test_a_hung_shard_is_measured_against_its_own_baseline(tmp_path):
    exe = _shard_bin(tmp_path, 'if [ "$i" = 2 ]; then sleep 30; fi\n'
                               'echo "All tests passed (1 assertion in 1 test case)"\n')
    plan = md.Catch2Plan(3, 3)
    plan.limits = {0: 60.0, 1: 60.0, 2: 0.5}
    state, why, who = md.run_catch2(tmp_path, exe, '[x]', plan, open(os.devnull, 'w'))
    assert (state, why) == (md.DETECTED, md.TIMED_OUT)
    assert 'catch2 shard 2' in who


def test_the_baseline_records_each_shard_and_orders_the_next_run(tmp_path):
    # Two seconds apart: on a loaded host a process can take over a second
    # just to start, which would reorder shards spaced closer.
    exe = _shard_bin(tmp_path, 'sleep "$((i * 2))"\n'
                               'echo "All tests passed (1 assertion in 1 test case)"\n')
    plan = md.Catch2Plan(3, 1)
    state, _, _ = md.run_catch2(tmp_path, exe, '[x]', plan, open(os.devnull, 'w'))
    assert state == md.GREEN
    assert sorted(plan.secs) == [0, 1, 2]
    assert plan.order() == [2, 1, 0], plan.secs


def test_every_shard_runs_and_none_twice(tmp_path):
    seen = tmp_path / 'seen'
    exe = _shard_bin(tmp_path, f'echo "$i" >> {seen}\n'
                               'echo "All tests passed (1 assertion in 1 test case)"\n')
    plan = md.Catch2Plan(7, 3)
    state, _, _ = md.run_catch2(tmp_path, exe, '[x]', plan, open(os.devnull, 'w'))
    assert state == md.GREEN
    assert sorted(seen.read_text().split()) == [str(i) for i in range(7)]


def _suites_timing_out_once(monkeypatch, entered):
    suites = object.__new__(md.Suites)
    runs = []

    def run_once(name, scale):
        runs.append((scale, list(entered)))
        return (md.DETECTED, md.TIMED_OUT, 'bats (1s)') if scale == 1.0 else (md.GREEN, '', '')
    monkeypatch.setattr(suites, '_run_once', run_once)
    return suites, runs


def test_a_timed_out_suite_reruns_at_normal_priority(monkeypatch):
    entered = []

    @md.contextlib.contextmanager
    def lifted():
        entered.append('lifted')
        try:
            yield
        finally:
            entered.pop()
    monkeypatch.setattr(md, 'normal_priority', lifted)
    suites, runs = _suites_timing_out_once(monkeypatch, entered)
    assert suites.run('bats') == (md.GREEN, '')
    assert runs == [(1.0, []), (2.0, ['lifted'])]


class FakeThreadSched:
    """The calling thread's scheduling class and niceness, in place of the
    kernel's: a desktop scheduler daemon can rewrite a real thread's class at
    any moment, and an unprivileged test can never exercise a successful lift."""

    def __init__(self, monkeypatch, policy, niceness, deny_lift):
        self.policy, self.niceness, self.deny_lift = policy, niceness, deny_lift
        for name in ('sched_getscheduler', 'sched_setscheduler', 'getpriority', 'setpriority'):
            monkeypatch.setattr(os, name, getattr(self, name), raising=False)

    def sched_getscheduler(self, pid):
        return self.policy

    def sched_setscheduler(self, pid, policy, param):
        if self.deny_lift and policy == os.SCHED_OTHER:
            raise PermissionError
        self.policy = policy

    def getpriority(self, which, who):
        return self.niceness

    def setpriority(self, which, who, niceness):
        self.niceness = niceness

    @property
    def state(self):
        return self.policy, self.niceness


@pytest.mark.skipif(not hasattr(os, 'SCHED_IDLE'), reason='scheduling classes are Linux-only')
@pytest.mark.parametrize('start,deny_lift,inside', [
    ((os.SCHED_IDLE, 19), False, (os.SCHED_OTHER, 0)),
    ((os.SCHED_IDLE, 0), False, (os.SCHED_OTHER, 0)),
    ((os.SCHED_OTHER, 19), False, (os.SCHED_OTHER, 0)),
    # Without the right to leave SCHED_IDLE the thread stays where it is, and
    # the rerun still happens.
    ((os.SCHED_IDLE, 19), True, (os.SCHED_IDLE, 19)),
    ((os.SCHED_OTHER, 0), False, (os.SCHED_OTHER, 0)),
] if hasattr(os, 'SCHED_IDLE') else [])
def test_normal_priority_lifts_a_lowered_thread_and_restores_it(monkeypatch, start, deny_lift, inside):
    sched = FakeThreadSched(monkeypatch, *start, deny_lift)
    with md.normal_priority():
        assert sched.state == inside
    assert sched.state == start


@pytest.mark.skipif(not hasattr(os, 'SCHED_IDLE'), reason='scheduling classes are Linux-only')
def test_normal_priority_restores_the_thread_when_the_body_raises(monkeypatch):
    sched = FakeThreadSched(monkeypatch, os.SCHED_IDLE, 19, False)
    with pytest.raises(ValueError):
        with md.normal_priority():
            raise ValueError
    assert sched.state == (os.SCHED_IDLE, 19)


# ---------------------------------------------------------------------------
# Parallel workers
# ---------------------------------------------------------------------------
# Hunks f0..f5. The suite detects f0, f2 and f4 and ignores the others, and
# records, for every run, its own directory and which files it found reverted
# there: a worker's tree must hold exactly one mutant at a time, and the tree
# the run started in none.
PARALLEL_FILES = 6
PARALLEL_SUITE = """#!/usr/bin/env bash
reverted=""
for f in src/f*.cpp; do grep -q NEW "$f" || reverted="$reverted $f"; done
echo "$PWD$reverted" >> {seen}
sleep 0.3
for f in $reverted; do
    case "$f" in src/f0.cpp|src/f2.cpp|src/f4.cpp)
        echo "tests/unit/test_f.cpp:1: FAILED:"; echo "assertions: 1 | 1 failed"; exit 1 ;;
    esac
done
exit 0
"""


def _parallel_fixture(tmp_path, makefile=FAST_MAKEFILE):
    work = _fixture(tmp_path, makefile)
    for i in range(PARALLEL_FILES):
        (work / "src" / f"f{i}.cpp").write_text(f"int f{i}() {{ return 1; }}\n")
    _git(work, "add", "src")
    _git(work, "commit", "-qm", "files")
    base = _git(work, "rev-parse", "HEAD").strip()
    for i in range(PARALLEL_FILES):
        (work / "src" / f"f{i}.cpp").write_text(f"int f{i}() {{ return 2; }}  // NEW\n")
    seen = tmp_path / "seen.log"
    suite = work / "build" / "bin" / "helix-tests"
    suite.write_text(PARALLEL_SUITE.replace("{seen}", str(seen)))
    return work, base, seen


PARALLEL_ENV = {**ENV, "MUTATE_DIFF_PLAIN_COPY": "1"}

# A mutant build (some f*.cpp without its marker) hangs, recording its pids.
PARALLEL_SLOW_MAKEFILE = """test-build:
\t@if grep -L NEW src/f*.cpp | grep -q .; then \\
\t    echo $$$$ > .mutant-build-started; \\
\t    sleep 60 & echo $$! > .mutant-build-child; wait; \\
\tfi
\t@[ -e build/bin/helix-tests ] && printf '\\n# relinked\\n' >> build/bin/helix-tests || true
"""


def _verdicts(out):
    """The report's verdict lines, in report order."""
    tail = out.split("=" * 68, 1)[1]
    return [ln.split() for ln in tail.splitlines()
            if ln.startswith("  ") and ":" in ln and "src/" in ln]


@linux_only
def test_parallel_verdicts_match_serial(tmp_path):
    work, base, seen = _parallel_fixture(tmp_path)
    before = {p: p.read_bytes() for p in (work / "src").iterdir()}
    serial = subprocess.run(
        [sys.executable, "scripts/mutate_diff.py", "--base", base, "--shards", "1",
         "--log", str(work / "serial.log"), "--only", "src/f"],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=120, env=PARALLEL_ENV)
    parallel = subprocess.run(
        [sys.executable, "scripts/mutate_diff.py", "--base", base, "--shards", "1",
         "--workers", "3", "--log", str(work / "parallel.log"), "--only", "src/f"],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=120, env=PARALLEL_ENV)
    assert "3 workers" in parallel.stdout, parallel.stdout
    assert serial.returncode == parallel.returncode == 1, parallel.stdout
    assert _verdicts(serial.stdout) == _verdicts(parallel.stdout)
    assert [v[0] for v in _verdicts(parallel.stdout)].count("killed") == 3
    for p, want in before.items():
        assert p.read_bytes() == want, p
    assert not list(tmp_path.glob(".repo.mutate-w*")), "a worker copy was left behind"


@linux_only
def test_a_parallel_run_makes_no_more_copies_than_it_has_hunks(tmp_path):
    work = _fixture(tmp_path)
    r = subprocess.run(
        [sys.executable, "scripts/mutate_diff.py", "--shards", "1", "--workers", "4",
         "--worker-dir", str(tmp_path / "copies"), "--log", str(work / "mutate.log"),
         "--base", "main", "--no-record"],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=120, env=PARALLEL_ENV)
    assert r.returncode == 0, r.stdout
    assert "1 workers, each in a copy of the tree" in r.stdout


@linux_only
def test_no_two_mutants_share_a_tree_and_the_original_holds_none(tmp_path):
    work, base, seen = _parallel_fixture(tmp_path)
    r = subprocess.run(
        [sys.executable, "scripts/mutate_diff.py", "--base", base, "--shards", "1",
         "--workers", "3", "--log", str(work / "mutate.log"), "--only", "src/f"],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=120, env=PARALLEL_ENV)
    assert r.returncode == 1, r.stdout
    runs = [ln.split() for ln in seen.read_text().splitlines()]
    # A detection repeats once before it counts, so a mutant can run twice.
    mutant_runs = sorted({tuple(ln) for ln in runs if len(ln) > 1})
    assert len(mutant_runs) == PARALLEL_FILES, runs
    trees = {ln[0] for ln in mutant_runs}
    assert str(work) not in trees, "a mutant ran in the tree the run started in"
    assert len(trees) == 3, trees
    for ln in mutant_runs:
        assert len(ln) == 2, f"{ln[0]} held {len(ln) - 1} mutants at once"
    # The baseline ran in the original, with nothing reverted.
    assert [str(work)] in runs


@linux_only
def test_a_stop_signal_mid_parallel_run_restores_every_tree(tmp_path):
    work, base, seen = _parallel_fixture(tmp_path, PARALLEL_SLOW_MAKEFILE)
    before = {p: p.read_bytes() for p in (work / "src").iterdir()}
    proc = subprocess.Popen(
        [sys.executable, "scripts/mutate_diff.py", "--base", base, "--shards", "1",
         "--workers", "2", "--log", str(work / "mutate.log"), "--only", "src/f"],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=True, env=PARALLEL_ENV)
    try:
        deadline = time.monotonic() + 60
        started = []
        while time.monotonic() < deadline and len(started) < 2:
            started = list(tmp_path.glob(".repo.mutate-w*/.mutant-build-child"))
            time.sleep(0.05)
        assert len(started) == 2, "both workers should be mid-build"
        children = [int(p.read_text().split()[0]) for p in started]
        os.kill(proc.pid, signal.SIGTERM)
        out, _ = proc.communicate(timeout=60)
    finally:
        if proc.poll() is None:
            proc.kill()
    assert proc.returncode == 128 + signal.SIGTERM, out
    assert "STOPPED by SIGTERM" in out
    for p, want in before.items():
        assert p.read_bytes() == want, p
    for tree in tmp_path.glob(".repo.mutate-w*"):
        for p, want in before.items():
            assert (tree / "src" / p.name).read_bytes() == want, tree / "src" / p.name
    for child in children:
        deadline = time.monotonic() + 5
        while _alive(child) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert not _alive(child), "a worker's build outlived the run"


def test_max_minutes_in_a_parallel_run_names_every_hunk_it_did_not_finish(tmp_path):
    work, base, seen = _parallel_fixture(tmp_path, PARALLEL_SLOW_MAKEFILE)
    before = {p: p.read_bytes() for p in (work / "src").iterdir()}
    t0 = time.monotonic()
    r = subprocess.run(
        [sys.executable, "scripts/mutate_diff.py", "--base", base, "--shards", "1",
         "--workers", "2", "--max-minutes", "0.1", "--log", str(work / "mutate.log"),
         "--only", "src/f"],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=120, env=PARALLEL_ENV)
    assert r.returncode == 3, r.stdout
    assert time.monotonic() - t0 < 60
    for i in range(PARALLEL_FILES):
        assert f"UNEXAMINED    src/f{i}.cpp:1" in r.stdout, r.stdout
    assert "VERDICT: INCOMPLETE" in r.stdout
    for p, want in before.items():
        assert p.read_bytes() == want, p


def test_without_reflink_a_parallel_request_runs_serial_and_says_why(tmp_path):
    probe = tmp_path / "probe"
    probe.write_text("x")
    if subprocess.run(["cp", "--reflink=always", str(probe), str(tmp_path / "probe2")],
                      capture_output=True).returncode == 0:
        pytest.skip("this filesystem takes reflink copies")
    work, base, seen = _parallel_fixture(tmp_path)
    r = subprocess.run(
        [sys.executable, "scripts/mutate_diff.py", "--base", base, "--shards", "1",
         "--workers", "3", "--log", str(work / "mutate.log"), "--only", "src/f"],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=120, env=ENV)
    assert "workers: 1 (" in r.stdout and "reflink" in r.stdout, r.stdout
    assert r.returncode == 1


@linux_only
def test_a_worker_copy_points_its_build_files_at_itself(tmp_path):
    src = tmp_path / "tree"
    (src / "build" / "obj").mkdir(parents=True)
    (src / "build" / "obj-asan").mkdir()
    (src / ".git").mkdir()
    dep = src / "build" / "obj" / "a.d"
    dep.write_text(f"{src}/build/obj/a.o: {src}/src/a.cpp {src}-other/x.h\n")
    os.utime(dep, (1_000_000, 1_000_000))
    (src / "build" / "obj-asan" / "big.o").write_text("x")
    (src / "compile_commands.json").write_text(f'[{{"directory": "{src}"}}]')
    dst = tmp_path / ".tree.mutate-w1"
    env_before = os.environ.get("MUTATE_DIFF_PLAIN_COPY")
    os.environ["MUTATE_DIFF_PLAIN_COPY"] = "1"
    try:
        md._copy_tree(src, dst)
    finally:
        if env_before is None:
            os.environ.pop("MUTATE_DIFF_PLAIN_COPY")
    got = (dst / "build" / "obj" / "a.d").read_text()
    assert got == f"{dst}/build/obj/a.o: {dst}/src/a.cpp {src}-other/x.h\n"
    assert (dst / "build" / "obj" / "a.d").stat().st_mtime == 1_000_000, \
        "a rewritten dependency file must keep its mtime or make rebuilds everything"
    assert str(dst) in (dst / "compile_commands.json").read_text()
    assert not (dst / "build" / "obj-asan").exists()
    # A directory that is not a worker copy is never deleted to make room.
    other = tmp_path / ".tree.mutate-w2"
    other.mkdir()
    (other / "precious").write_text("x")
    with pytest.raises(RuntimeError, match="not a worker copy"):
        md._copy_tree(src, other)
    assert (other / "precious").exists()


# ---------------------------------------------------------------------------
# Per-file filters, the merge-time cap, generated files
# ---------------------------------------------------------------------------
def test_tests_for_parses_glob_and_filter():
    assert md.parse_tests_for(["src/ui/*=[ui]", " ui_xml/x.xml = [x],[y] "]) == [
        ("src/ui/*", "[ui]"), ("ui_xml/x.xml", "[x],[y]")]
    assert md.parse_tests_for(None) == []


@pytest.mark.parametrize("spec", ["src/*", "=[x]", "src/*=", "   =  "])
def test_tests_for_refuses_a_spec_without_both_halves(spec):
    with pytest.raises(ValueError):
        md.parse_tests_for([spec])


SOURCES = {
    "test_network_validation": '#include "utils/network_validation.h"\n',
    "test_moonraker_connect_failure": "ConnectFailureKind k = classify(network_validation(x));\n",
    "test_other": "network_validation_extra and networkvalidation\n",
}


def test_auto_filter_is_the_test_files_naming_the_stem_as_a_word():
    filt = md.auto_tests_filter("src/utils/network_validation.cpp", SOURCES)
    assert filt == ("[#test_moonraker_connect_failure]~[.]~[slow],"
                    "[#test_network_validation]~[.]~[slow]")


def test_auto_filter_ignores_a_short_stem_and_a_stem_nothing_names():
    assert md.auto_tests_filter("src/a/io.cpp", {"test_io": "io io io"}) == ""
    assert md.auto_tests_filter("src/ui/unmentioned_thing.cpp", SOURCES) == ""


def test_auto_filter_gives_up_on_a_stem_too_many_files_name():
    many = {f"test_{i}": "widget_base here" for i in range(md.AUTO_TESTS_MAX_FILES + 1)}
    assert md.auto_tests_filter("src/ui/widget_base.cpp", many) == ""
    some = dict(list(many.items())[:md.AUTO_TESTS_MAX_FILES])
    assert md.auto_tests_filter("src/ui/widget_base.cpp", some) != ""


SPLIT_TU = (
    '#include "gizmo_bar_internal.h"\n'
    '#include "gizmo.h"\n'
    '#include "gizmo_bar_zap.h"\n'
    'void Gizmo::discover() {}\n'
)
SPLIT_SOURCES = {
    "test_gizmo": '#include "gizmo.h"\nTEST_CASE("x") { auto g = make_one(); g->connect(); }\n',
    "test_unrelated": '#include "bar.h"\ngizmo_bar_zap and gizmobar\n',
}


def test_auto_filter_selects_the_tests_of_the_header_a_split_tu_implements():
    # gizmo_bar.cpp implements gizmo.h: a test including gizmo.h reaches it, though
    # it never names gizmo_bar. gizmo_bar_internal.h is not a prefix of the stem.
    filt = md.auto_tests_filter("src/gizmo_bar.cpp", SPLIT_SOURCES, source_text=SPLIT_TU)
    assert filt == "[#test_gizmo]~[.]~[slow]"


def test_auto_filter_selects_tests_naming_the_class_a_tu_defines():
    tu = '#include "printer_print_state.h"\nvoid PrinterPrintState::reset() {}\n'
    sources = {"test_reach": "PrinterPrintState& s = get();\n",
               "test_file": '#include "printer_print_state.h"\n',
               "test_other": "PrinterState and PrinterPrintStateX\n"}
    filt = md.auto_tests_filter("src/printer/printer_print_state.cpp", sources, source_text=tu)
    assert filt == "[#test_file]~[.]~[slow],[#test_reach]~[.]~[slow]"
    # A class the TU defines nothing of is no evidence: only the stem counts.
    assert md.auto_tests_filter("src/printer/printer_print_state.cpp", sources,
                                source_text='#include "printer_print_state.h"\n') \
        == "[#test_file]~[.]~[slow]"


def test_auto_filter_lets_header_reached_tests_past_the_stem_cap():
    # A narrow filter that misses the header's tests reads as false survivors,
    # so the header's includers widen it up to AUTO_TESTS_MAX_REACHED files.
    sources = {f"test_{i}": '#include "gizmo.h"\n' for i in range(md.AUTO_TESTS_MAX_FILES)}
    sources["test_named"] = "gizmo_bar\n"
    filt = md.auto_tests_filter("src/gizmo_bar.cpp", sources, source_text=SPLIT_TU)
    assert filt.count("[#test_") == md.AUTO_TESTS_MAX_FILES + 1
    assert md.auto_tests_filter("src/gizmo_bar.cpp", sources, source_text="") \
        == "[#test_named]~[.]~[slow]"


def test_auto_filter_abstains_when_the_implemented_header_is_too_common():
    sources = {f"test_{i}": '#include "gizmo.h"\n' for i in range(md.AUTO_TESTS_MAX_REACHED)}
    sources["test_named"] = "gizmo_bar\n"
    assert md.auto_tests_filter("src/gizmo_bar.cpp", sources, source_text=SPLIT_TU) == ""
    # --auto-tests-max past the reached cap raises it too.
    assert md.auto_tests_filter("src/gizmo_bar.cpp", sources, source_text=SPLIT_TU,
                                max_files=md.AUTO_TESTS_MAX_REACHED + 1).count("[#test_") \
        == md.AUTO_TESTS_MAX_REACHED + 1


def test_auto_filter_still_gives_up_on_a_common_stem_whatever_its_header():
    sources = {f"test_{i}": "gizmo_bar\n" for i in range(md.AUTO_TESTS_MAX_FILES + 1)}
    assert md.auto_tests_filter("src/gizmo_bar.cpp", sources, source_text=SPLIT_TU) == ""


GIZMO_LOADER = (
    '#include "gizmo_loader.h"\n'
    'json GizmoLoader::load() { return read(find_readable("gizmo_db.json")); }\n'
)
DATA_SOURCES = {
    "test_loader_user": "auto rules = GizmoLoader::load();\n",
    "test_names_db": 'REQUIRE(path_of("gizmo_db") != "");\n',
    "test_unrelated": "GizmoLoaderX and gizmo_dbx\n",
}


def test_a_data_file_is_judged_by_the_tests_reaching_its_declared_consumers():
    filt = md.auto_tests_filter("assets/config/gizmo_db.json", DATA_SOURCES,
                                consumers=[("src/gizmo_loader.cpp", GIZMO_LOADER)])
    assert filt == "[#test_loader_user]~[.]~[slow],[#test_names_db]~[.]~[slow]"
    assert md.auto_tests_filter("assets/config/gizmo_db.json", DATA_SOURCES) \
        == "[#test_names_db]~[.]~[slow]"


def test_data_consumers_follow_the_map_globs():
    rows = md.parse_data_consumers(
        "# comment\n\n"
        "assets/config/gizmo_db.json\tsrc/gizmo_loader.cpp\tgizmo_db.json\n"
        "assets/config/presets/*.json\tsrc/system/config.cpp\tpresets/\n"
        "assets/config/gizmo_db.json\tsrc/gizmo_rules.cpp\tgizmo_rules(\n")
    assert md.data_consumers("assets/config/gizmo_db.json", rows) \
        == ["src/gizmo_loader.cpp", "src/gizmo_rules.cpp"]
    assert md.data_consumers("assets/config/presets/k1.json", rows) == ["src/system/config.cpp"]
    assert md.data_consumers("assets/config/other.json", rows) == []


def test_a_row_naming_its_reach_replaces_the_sources_own():
    rows = md.parse_data_consumers(
        "assets/config/presets/*.json\tsrc/system/config.cpp\t\"presets/\"\t"
        "apply_preset_file, config/presets\n"
        "assets/config/presets/*.json\tsrc/other.cpp\tpresets\n")
    path = "assets/config/presets/k1.json"
    assert md.data_consumers(path, rows) == ["src/other.cpp"]
    assert md.data_names(path, rows) == ["apply_preset_file", "config/presets"]
    sources = {"test_apply": "cfg.apply_preset_file(\"k1\");\n",
               "test_links": 'const char* d = "assets/config/presets";\n',
               "test_config": "Config c; c.load();\n"}
    assert md.auto_tests_filter(path, sources, extra_names=md.data_names(path, rows)) \
        == "[#test_apply]~[.]~[slow],[#test_links]~[.]~[slow]"


def test_a_declared_reach_stands_in_for_a_data_stem_too_many_tests_name():
    # A preset named for a printer (ad5x) shares its stem with that printer's
    # every test; the loader the map declares is the evidence instead.
    sources = {f"test_{i}": "ad5x persona\n" for i in range(md.AUTO_TESTS_MAX_FILES + 1)}
    sources["test_apply"] = "cfg.apply_preset_file(name);\n"
    path = "assets/config/presets/ad5x.json"
    assert md.auto_tests_filter(path, sources) == ""
    assert md.auto_tests_filter(path, sources, extra_names=["apply_preset_file"]) \
        == "[#test_apply]~[.]~[slow]"


def test_parse_data_consumers_refuses_a_row_without_three_fields():
    with pytest.raises(ValueError):
        md.parse_data_consumers("assets/config/x.json\tsrc/x.cpp\n")


def test_every_declared_data_consumer_still_reads_its_data():
    # A consumer that no longer loads the file would silently narrow its filter.
    rows = md.parse_data_consumers((REPO_ROOT / md.DATA_CONSUMERS_MAP).read_text())
    assert rows
    tracked = subprocess.run(["git", "ls-files", "assets"], cwd=REPO_ROOT, text=True,
                             stdout=subprocess.PIPE).stdout.split()
    for glob, source, evidence, names in rows:
        assert any(md.fnmatch(t, glob) for t in tracked), f"{glob} matches no tracked file"
        text = (REPO_ROOT / source).read_text()
        assert evidence in text, f"{source} no longer contains {evidence!r} for {glob}"
        assert md.classify(source, False) == ("mutate", "cxx"), source
        # A named reach is the source's own loader, or the data's own directory.
        for name in names:
            assert name in text or name in glob, f"{source} no longer has {name!r}"


def test_the_printer_database_reaches_the_tests_reading_it_through_code():
    rows = md.parse_data_consumers((REPO_ROOT / md.DATA_CONSUMERS_MAP).read_text())
    path = "assets/config/printer_database.json"
    consumers = [(c, (REPO_ROOT / c).read_text()) for c in md.data_consumers(path, rows)]
    filt = md.auto_tests_filter(path, md._test_sources(REPO_ROOT), consumers=consumers)
    # The whole --tests ('') also runs them; a narrow filter must name both.
    assert filt == "" or ("[#test_probe_preparation]" in filt
                          and "[#test_probe_sensor_manager]" in filt), filt


@pytest.mark.parametrize("path,expected", [
    ("assets/config/presets/k1.json",
     ("test_config_preset", "test_preset_database_links")),
    # Tests opening a preset by its quoted path segment ("presets/ad5x.json",
    # "config" / "presets") read it without naming the loader.
    ("assets/config/presets/ad5x.json",
     ("test_config_preset", "test_preset_database_links", "test_display_manager",
      "test_display_metrics", "test_hardware_validator")),
    ("assets/config/default_layout.json",
     ("test_default_layout", "test_default_layout_resolver", "test_panel_widget_live_default")),
])
def test_presets_and_the_default_layout_get_their_loaders_tests_not_the_suite(path, expected):
    rows = md.parse_data_consumers((REPO_ROOT / md.DATA_CONSUMERS_MAP).read_text())
    consumers = [(c, (REPO_ROOT / c).read_text()) for c in md.data_consumers(path, rows)]
    filt = md.auto_tests_filter(path, md._test_sources(REPO_ROOT), consumers=consumers,
                                extra_names=md.data_names(path, rows))
    assert filt, "fell back to the whole suite"
    for name in expected:
        assert f"[#{name}]" in filt, filt


PARSER_CPP = (
    '#include "ifs_parser.h"\n'
    'namespace ifs {\n'
    'SlotMap parse_slots(const json& j) {\n'
    '    SlotMap out;\n'
    '    return out;\n'
    '}\n'
    'class LaneReader {};\n'
    'struct SensorFrame {\n'
    '    std::optional<bool> motion_detected;\n'
    '    std::array<int, 4> port_counts{};\n'
    '};\n'
    '}\n'
)
BACKEND_PRE = 'void Backend::update(const json& j) {\n    for (auto& s : j["slots"]) {}\n}\n'


def _hunk(path, body, new_file=False):
    head = "--- /dev/null\n" if new_file else f"--- a/{path}\n"
    return {"file": path, "line": 1, "old_line": 1,
            "body": ["@@ -1,2 +1,2 @@\n", *body],
            "patch": f"diff --git a/{path} b/{path}\n{head}+++ b/{path}\n"}


def test_an_added_file_is_one_whose_patch_starts_from_nothing():
    hunks = [_hunk("src/ifs_parser.cpp", ["+x\n"], new_file=True),
             _hunk("src/backend.cpp", ["-a\n", "+b\n"])]
    assert md.added_files(hunks) == {"src/ifs_parser.cpp"}


def test_a_survivor_calling_into_an_added_file_is_an_extraction():
    swap = _hunk("src/backend.cpp", ['-    for (auto& s : j["slots"]) {}\n',
                                     "+    slots_ = ifs::parse_slots(j);\n"])
    plain = _hunk("src/backend.cpp", ["-    return 1;\n", "+    return 2;\n"])
    # A name the pre-image already has (update, json) is no evidence of a move.
    reuse = _hunk("src/backend.cpp", ["-    x();\n", "+    update(j);\n"])
    # A member of a struct the added file declares is a moved name too.
    field = _hunk("src/backend.cpp", ["-    if (j.contains(\"m\")) {\n",
                                      "+    if (frame.motion_detected) {\n"])
    field["line"] = 9
    # Only adding a call is new behaviour, whose survival is a real gap.
    added_call = _hunk("src/backend.cpp", ["+    ifs::parse_slots(j);\n"])
    added_call["line"] = 20
    found = md.extraction_targets([swap, plain, reuse, field, added_call], {"src/ifs_parser.cpp": PARSER_CPP},
                                  {"src/backend.cpp": BACKEND_PRE})
    assert found == {"src/backend.cpp:1": "src/ifs_parser.cpp",
                     "src/backend.cpp:9": "src/ifs_parser.cpp"}
    assert md.extraction_targets([swap], {}, {"src/backend.cpp": BACKEND_PRE}) == {}


def test_a_name_an_added_header_declares_is_credited_to_its_source():
    swap = _hunk("src/backend.cpp", ["-    int k = 0;\n", "+    LaneFrame k = read();\n"])
    found = md.extraction_targets(
        [swap], {"include/ifs_parser.h": "struct LaneFrame { int n; };\n",
                 "src/ifs_parser.cpp": PARSER_CPP}, {"src/backend.cpp": BACKEND_PRE})
    assert found == {"src/backend.cpp:1": "src/ifs_parser.cpp"}


def test_declared_names_cover_functions_types_and_members():
    names = md.declared_names(PARSER_CPP)
    assert {"parse_slots", "LaneReader", "ifs", "SensorFrame", "motion_detected",
            "port_counts", "out"} <= names
    assert "SlotMap" not in names and "return" not in names


def test_the_summary_groups_extraction_survivors_by_the_file_they_moved_into():
    lines = md.extraction_report({"src/backend.cpp:12": "src/ifs_parser.cpp",
                                  "src/backend.cpp:40": "src/ifs_parser.cpp"},
                                 ["src/ifs_parser.cpp:1", "src/ifs_parser.cpp:30",
                                  "src/other.cpp:5"])
    text = "\n".join(lines)
    assert "2 survivor(s) replace code with a call into a file this diff adds" in text
    assert "src/ifs_parser.cpp <- src/backend.cpp:12, src/backend.cpp:40" in text
    assert "2 hunk(s) inside src/ifs_parser.cpp cannot be judged by a revert" in text
    assert "--inside N --only src/ifs_parser.cpp" in text
    assert "other.cpp" not in text
    assert md.extraction_report({}, ["src/other.cpp:5"]) == []


EXTRACTION_MAKEFILE = """test-build:
\t@if grep -q parse_gizmo src/feature.cpp && ! grep -qs parse_gizmo src/gizmo_parser.cpp; \\
\t    then echo 'undefined reference to parse_gizmo'; exit 1; fi
\t@[ -e build/bin/helix-tests ] && printf '\\n# relinked\\n' >> build/bin/helix-tests || true
"""


def test_a_run_over_an_extraction_names_the_shape_instead_of_listing_survivors(tmp_path):
    work = _fixture(tmp_path, EXTRACTION_MAKEFILE)
    (work / "src" / "feature.cpp").write_text("int f(int n) {\n    return parse_gizmo(n);\n}\n")
    (work / "src" / "gizmo_parser.cpp").write_text(
        "int parse_gizmo(int n) {\n    return n + 1;\n}\n")
    _git(work, "add", "-N", "src/gizmo_parser.cpp")
    suite = work / "build" / "bin" / "helix-tests"
    suite.write_text("#!/usr/bin/env bash\nexit 0\n")
    r = _mutate(work, "--base", "main", "--no-record", "--no-auto-tests")
    assert r.returncode == 1, r.stdout
    assert "EXTRACTION: 1 survivor(s) replace code with a call" in r.stdout, r.stdout
    assert "src/gizmo_parser.cpp <- src/feature.cpp:1" in r.stdout
    assert "1 hunk(s) inside src/gizmo_parser.cpp cannot be judged by a revert" in r.stdout
    summary = r.stdout.split("hunk(s) survived reversion", 1)[1].split("EXTRACTION:", 1)[0]
    assert "src/feature.cpp:1" not in summary, r.stdout


def test_reach_names_are_the_stem_the_implemented_header_and_its_class():
    assert md.reach_names("src/gizmo_bar.cpp", SPLIT_TU) == ["gizmo_bar", "gizmo", "Gizmo"]
    assert md.reach_names("src/gizmo_bar.cpp", "") == ["gizmo_bar"]
    # A header's own includes are its dependencies, not what it implements.
    assert md.reach_names("include/gizmo_bar.h", SPLIT_TU) == ["gizmo_bar"]
    # The longest prefix header wins; an unrelated include is ignored.
    tu = '#include "gizmo.h"\n#include "gizmo_bar.h"\n#include "zap.h"\n'
    assert md.reach_names("src/gizmo_bar_baz.cpp", tu) == ["gizmo_bar_baz", "gizmo_bar"]


def test_filter_for_reads_the_split_tu_it_is_given():
    assert md.filter_for("src/gizmo_bar.cpp", [], True, SPLIT_SOURCES, "[all]",
                         source_text=SPLIT_TU) == ("[#test_gizmo]~[.]~[slow]",
                                                   "test files reaching it")
    assert md.filter_for("src/gizmo_bar.cpp", [], True, SPLIT_SOURCES, "[all]")[0] == "[all]"


def test_filter_for_prefers_explicit_then_auto_then_default():
    tests_for = [("src/utils/*", "[explicit]")]
    assert md.filter_for("src/utils/network_validation.cpp", tests_for, True, SOURCES,
                         "[all]")[0] == "[explicit]"
    assert md.filter_for("src/net/network_validation.cpp", tests_for, True, SOURCES,
                         "[all]")[0].startswith("[#test_")
    assert md.filter_for("src/net/network_validation.cpp", tests_for, False, SOURCES,
                         "[all]")[0] == "[all]"
    assert md.filter_for("src/ui/unmentioned_thing.cpp", [], True, SOURCES,
                         "[all]")[0] == "[all]"


def test_files_sharing_a_filter_run_as_one_group():
    filt = {"a.cpp": "[x]", "b.cpp": "[y]", "c.cpp": "[x]"}
    assert md.group_by_filter(["a.cpp", "b.cpp", "c.cpp", "a.cpp"], filt) == [
        ("[x]", ["a.cpp", "c.cpp"]), ("[y]", ["b.cpp"])]


@pytest.mark.parametrize("path", ["translations/de.yml", "assets/images/tour/preview.png"])
def test_content_with_no_logic_does_not_make_a_run_incomplete(path):
    assert md.classify(path, False)[0] == "inert"


def test_an_unclaimed_asset_stays_visible():
    assert md.classify("assets/sounds/beep.wav", False)[0] == "uncovered"


@pytest.mark.parametrize("codes, worst", [
    ([0, 0], 0), ([0, 3], 3), ([3, 1, 0], 1), ([0, 4, 3], 4), ([143, 3], 143), ([], 0)])
def test_the_worst_child_exit_is_the_run_exit(codes, worst):
    assert md.combine_exits(codes) == worst


def test_file_tag_filters_turn_on_catch2_file_tags():
    assert md.catch2_argv("bin", "[#test_foo]") == ["bin", "[#test_foo]", "-#"]
    assert md.catch2_argv("bin", "[ams]") == ["bin", "[ams]"]


def test_generated_output_is_not_mutated_as_code():
    assert md.classify("src/generated/theme_token_table.cpp", False)[0] == "inert"
    assert md.classify("ui_xml/translations/de.xml", False)[0] == "inert"
    assert md.classify("ui_xml/print_status_panel.xml", False) == ("mutate", "data")
    assert md.classify("src/ui/ui_panel.cpp", False) == ("mutate", "cxx")


@pytest.mark.parametrize("minutes", ["46", "0", "240"])
def test_a_merge_time_run_over_the_cap_is_refused_without_release(minutes):
    p = subprocess.run([sys.executable, str(SCRIPTS_DIR / "mutate_diff.py"), "--list-only",
                        "--max-minutes", minutes], cwd=REPO_ROOT, capture_output=True, text=True)
    assert p.returncode == 2
    assert "--release" in p.stderr


def test_release_lifts_the_cap(tmp_path):
    # A fixture repo, not this checkout: the pre-push sweep runs in a throwaway
    # checkout whose submodules are symlinks, and git diff refuses those.
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    r = _mutate(work, "--list-only", "--max-minutes", "240", "--release", "--base", base)
    assert r.returncode == 0, r.stdout


def test_first_puts_named_files_ahead_and_leaves_nothing_out():
    assert md.first_rank("src/printer/notice.cpp", ["ams_backend", "notice"]) == 1
    assert md.first_rank("src/printer/ams_backend.cpp", ["ams_backend", "notice"]) == 0
    assert md.first_rank("src/other.cpp", ["ams_backend", "notice"]) == 2
    assert md.split_patterns(["a,b", " c "]) == ["a", "b", "c"]


def test_a_filter_is_ranked_by_how_much_it_runs():
    defaults = ("~[.]", "tests/shell", "tests/python")
    assert md.filter_scope(("[#a]", "", ""), defaults, True) == md.SCOPE_NARROW
    assert md.filter_scope(("~[.]", "", ""), defaults, True) == md.SCOPE_WHOLE_CATCH2
    assert md.filter_scope(("~[.]", "tests/shell/a.bats", ""), defaults, False) == md.SCOPE_NARROW
    assert md.filter_scope(("~[.]", "tests/shell", "tests/python"), defaults, False) \
        == md.SCOPE_WHOLE_SCRIPTS


def test_first_orders_the_run_and_a_whole_suite_waits_behind_a_scoped_filter(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    r = _mutate(work, "--base", base, "--list-only", "--tests-for", "src/other.cpp=[oth]")
    listed = [ln.split()[0] for ln in r.stdout.splitlines() if ln.endswith("[cxx]")]
    assert [p.split(":")[0] for p in listed] == ["src/other.cpp", "src/feature.cpp"], r.stdout
    r = _mutate(work, "--base", base, "--list-only", "--tests-for", "src/other.cpp=[oth]",
                "--first", "feature")
    listed = [ln.split()[0] for ln in r.stdout.splitlines() if ln.endswith("[cxx]")]
    assert [p.split(":")[0] for p in listed] == ["src/feature.cpp", "src/other.cpp"], r.stdout


def _pool(work, *args):
    return subprocess.run(
        [sys.executable, "scripts/mutate_diff.py", "--shards", "1", "--workers", "2", "--pool",
         "--log", str(work / "mutate.log"), *args],
        cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=120, env=PARALLEL_ENV)


@linux_only
def test_a_pool_judges_each_hunk_by_its_own_filter_with_one_build_and_one_set_of_copies(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    (work / ".mutant-expect-other").write_text("")
    r = _pool(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]",
              "--tests-for", "src/other.cpp=[oth]", "--no-record")
    assert r.returncode == 0, r.stdout
    assert "=== group" not in r.stdout
    assert r.stdout.count("workers, each in a copy of the tree") == 1, r.stdout
    assert "catch2 green" in r.stdout and r.stdout.count("catch2 green") == 2, r.stdout
    assert [v[0] for v in _verdicts(r.stdout)] == ["killed", "killed"], r.stdout
    assert "VERDICT: CLEAN" in r.stdout


@linux_only
def test_a_pool_names_the_filter_a_hunk_survived(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    r = _pool(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]",
              "--tests-for", "src/other.cpp=[oth]", "--no-record")
    assert r.returncode == 1, r.stdout
    assert re.search(r"src/other.cpp:\d+  \[oth\]", r.stdout), r.stdout



@linux_only
def test_a_survivor_judged_by_the_tests_reaching_it_says_how_to_run_more(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    (work / "tests" / "unit").mkdir(parents=True, exist_ok=True)
    (work / "tests" / "unit" / "test_other.cpp").write_text('#include "other.h"\n')
    r = _pool(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]", "--no-record")
    assert r.returncode == 1, r.stdout
    assert re.search(r"src/other.cpp:\d+  \[#test_other\]", r.stdout), r.stdout
    assert "ran only the tests that name them (src/other.cpp)" in r.stdout, r.stdout
    assert "--tests-for 'PATH=[tag]'" in r.stdout
    explicit = _pool(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]",
                     "--tests-for", "src/other.cpp=[oth]", "--no-record")
    assert explicit.returncode == 1, explicit.stdout
    assert "ran only the tests that name them" not in explicit.stdout

NARROW_GREEN_SUITE = """#!/usr/bin/env bash
case "$1" in *feat*) exit 0 ;; esac
grep -q NEW_BEHAVIOR src/feature.cpp || {
    echo "tests/unit/test_wide.cpp:3: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
}
exit 0
"""


@linux_only
def test_confirm_survivors_asks_the_whole_suite_before_a_narrow_filter_reads_survived(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    (work / "build" / "bin" / "helix-tests").write_text(NARROW_GREEN_SUITE)
    flags = ("--base", base, "--tests-for", "src/feature.cpp=[feat]",
             "--only", "feature", "--no-record")
    plain = _pool(work, *flags)
    assert re.search(r"SURVIVED\s+src/feature.cpp", plain.stdout), plain.stdout
    r = _pool(work, *flags, "--confirm-survivors")
    assert "only the whole suite detected it" in r.stdout, r.stdout
    assert re.search(r"killed\s+src/feature.cpp", r.stdout), r.stdout


@linux_only
def test_a_red_baseline_leaves_its_filter_unjudged_and_the_others_judged(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    suite = work / "build" / "bin" / "helix-tests"
    suite.write_text(DETECTING_SUITE.replace(
        "#!/usr/bin/env bash\n",
        '#!/usr/bin/env bash\n[ "$1" != "[oth]" ] || '
        '{ echo "tests/unit/test_o.cpp:1: FAILED:"; exit 1; }\n', 1))
    r = _pool(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]",
              "--tests-for", "src/other.cpp=[oth]", "--no-record")
    assert r.returncode == 3, r.stdout
    assert "baseline catch2 suite is RED" in r.stdout
    verdicts = {v[1]: v[0] for v in _verdicts(r.stdout)}
    assert any(k.startswith("src/feature.cpp") and v == "killed" for k, v in verdicts.items()), r.stdout
    assert any(k.startswith("src/other.cpp") and v == "INCONCLUSIVE" for k, v in verdicts.items()), r.stdout
    assert "whose suite exited without a verdict" in r.stdout


def test_auto_tests_max_widens_what_a_stem_may_be_named_by():
    sources = {f"test_{i}": "foo_stem" for i in range(20)}
    assert md.auto_tests_filter("src/foo_stem.cpp", sources) == ""
    assert md.auto_tests_filter("src/foo_stem.cpp", sources, max_files=20).count("[#test_") == 20


@pytest.mark.parametrize("line,expected", [
    ("    if (a == b) {\n", ["    if (!(a == b)) {\n", "    if (a != b) {\n"]),
    ("    helix::publish(x);\n", ["    ;\n"]),
    ("    obj->call(a).then(b);\n", ["    ;\n"]),
    ("    return true;\n", ["    return false;\n"]),
    ("    auto x = (a < b);\n", ["    auto x = (a >= b);\n"]),
    ('    s = "a && b";\n', []),
    ("    x = 1;  // true && false\n", []),
    ('    spdlog::info("x", y == z);\n', []),
    ("#if X == 1\n", []),
    ("    Foo bar(1);\n", []),
    ("    /* a == b */ c = d;\n", []),
    ('    t = R"(a == b)";\n', []),
])
def test_inside_edits_touch_code_and_never_a_literal_a_comment_or_a_log(line, expected):
    assert [text for _, text in md.inside_edits(line)] == expected


def test_inside_mutants_cover_added_lines_only_and_the_most_telling_first():
    lines = ["int f() {\n", "    bool a = true;\n", "    if (b) {\n", "    if (old) {\n", "}\n"]
    h = {"line": 1, "body": ["@@\n", " int f() {\n", "+    bool a = true;\n", "+    if (b) {\n",
                             "-    gone();\n", "     if (old) {\n", " }\n"]}
    assert [(m[0], m[1]) for m in md.hunk_inside_mutants(h, lines)] == [
        (3, "negate if"), (2, "true to false")]


def test_inside_drops_a_call_written_over_several_lines():
    lines = ["void f() {\n", "    helix::publish_ui_request(\n", "        {Kind::Offer, slot});\n",
             "    int y = g(\n", "        1);\n", "    spdlog::info(\n", "        \"x\");\n", "}\n"]
    h = {"line": 1, "body": ["@@\n", " void f() {\n"] + ["+" + ln for ln in lines[1:7]] + [" }\n"]}
    mutants = md.hunk_inside_mutants(h, lines)
    assert [(m[0], m[1], m[2], m[3]) for m in mutants] == [(2, "drop call", "    ;\n", 2)]


def test_inside_mutants_spread_across_a_long_hunk():
    mutants = [(line, "negate if", "x", 1) for line in range(1, 41)]
    assert [m[0] for m in md.spread_mutants(mutants, 4)[:4]] == [1, 11, 21, 31]
    assert md.spread_mutants(mutants, 1) == mutants
    assert sorted(md.spread_mutants(mutants, 4)) == mutants


INSIDE_CHANGED = "int f(int n) {\n    if (n == 2) { return 1; }  // NEW_BEHAVIOR\n    return n;\n}\n"
# The revert removes a marker the build needs, so it can never build; the suite
# reds on the original condition being edited.
INSIDE_MAKEFILE = """test-build:
\t@grep -q NEW_BEHAVIOR src/feature.cpp
\t@printf '\\n# relinked\\n' >> build/bin/helix-tests
"""
INSIDE_SUITE = """#!/usr/bin/env bash
grep -q 'if (n == 2) {' src/feature.cpp || {
    echo "tests/unit/test_feature.cpp:11: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
}
exit 0
"""


def _inside_fixture(tmp_path, suite):
    work = _fixture(tmp_path, INSIDE_MAKEFILE)
    (work / "src" / "feature.cpp").write_text(INSIDE_CHANGED)
    (work / "build" / "bin" / "helix-tests").write_text(suite)
    return work


def test_an_inside_mutant_judges_a_hunk_whose_revert_does_not_build(tmp_path):
    work = _inside_fixture(tmp_path, INSIDE_SUITE)
    without = _mutate(work, "--base", "main", "--no-record")
    assert without.returncode == 3 and "inside mutants" not in without.stdout, without.stdout
    r = _mutate(work, "--base", "main", "--no-record", "--inside", "1")
    assert re.search(r"uncompilable\s+src/feature.cpp:\d+", r.stdout), r.stdout
    assert re.search(r"killed\s+src/feature.cpp:2  negate if", r.stdout), r.stdout
    # The revert's verdict stands: one edit is not the question a revert asks.
    assert r.returncode == 3, r.stdout
    assert "VERDICT: INCOMPLETE" in r.stdout
    assert (work / "src" / "feature.cpp").read_text() == INSIDE_CHANGED


def test_a_surviving_inside_mutant_fails_the_run(tmp_path):
    work = _inside_fixture(tmp_path, "#!/usr/bin/env bash\nexit 0\n")
    r = _mutate(work, "--base", "main", "--no-record", "--inside", "1")
    assert r.returncode == 1, r.stdout
    assert "1 inside mutant(s) survived" in r.stdout
    assert "src/feature.cpp:2  negate if" in r.stdout


def test_an_inside_mutant_judges_a_revert_that_left_the_old_binary_in_place(tmp_path):
    work = _inside_fixture(tmp_path, INSIDE_SUITE.replace(
        "if (n == 2) {' src/feature.cpp", "if (n == 2) { return 1; }  // NEW_BEHAVIOR' src/feature.cpp"))
    (work / "Makefile").write_text(
        "test-build:\n\t@! grep -q NEW_BEHAVIOR src/feature.cpp || "
        "printf '\\n# relinked\\n' >> build/bin/helix-tests\n")
    r = _mutate(work, "--base", "main", "--no-record", "--inside", "1")
    assert re.search(r"INCONCLUSIVE\s+src/feature.cpp", r.stdout), r.stdout
    assert re.search(r"killed\s+src/feature.cpp:2  negate if", r.stdout), r.stdout


def test_an_inside_mutant_that_does_not_build_is_passed_over(tmp_path):
    work = _inside_fixture(tmp_path, INSIDE_SUITE)
    (work / "Makefile").write_text(INSIDE_MAKEFILE.replace(
        "\t@printf", "\t@! grep -q 'if (!(' src/feature.cpp\n\t@printf", 1))
    r = _mutate(work, "--base", "main", "--no-record", "--inside", "1")
    assert re.search(r"killed\s+src/feature.cpp:2  == to !=", r.stdout), r.stdout


@pytest.mark.parametrize("rc,printed,rejected", [
    (1, "src/feature.cpp: FAILED (1.0s)\nsummary: 1 compiled, 0 skipped, 1 failed", True),
    (0, "src/feature.cpp: ok (1.0s)", False),
    (1, "src/feature.cpp: no such file\nsummary: 0 compiled, 0 skipped, 1 failed", False),
    (2, "make compile-prereqs failed, so no make-derived command can run", False),
])
def test_only_a_compile_that_ran_and_failed_is_a_syntax_rejection(tmp_path, rc, printed, rejected):
    (tmp_path / "scripts").mkdir()
    (tmp_path / "scripts" / "syntax_check.py").write_text(
        f"import sys\nprint({printed!r})\nsys.exit({rc})\n")
    import io
    assert md.syntax_rejects(tmp_path, "src/feature.cpp", io.StringIO()) is rejected


def _flaky_suite(tmp_path):
    """Red the first time its baseline runs, then green unless a mutant is in."""
    return f"""#!/usr/bin/env bash
case "$*" in *--list-tests*) exit 0 ;; esac
if [ ! -e {tmp_path}/flaked ]; then
    touch {tmp_path}/flaked
    echo "tests/unit/test_budget.cpp:9: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
fi
grep -q NEW_BEHAVIOR src/feature.cpp || {{
    echo "tests/unit/test_feature.cpp:11: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
}}
exit 0
"""


def test_a_catch2_baseline_red_once_is_retried_and_its_flake_named(tmp_path):
    work = _fixture(tmp_path)
    (work / "build" / "bin" / "helix-tests").write_text(_flaky_suite(tmp_path))
    r = _mutate(work, "--base", "main", "--no-record")
    assert "red once and green on a re-run" in r.stdout, r.stdout
    assert "tests/unit/test_budget.cpp:9" in r.stdout
    assert re.search(r"killed\s+src/feature.cpp", r.stdout), r.stdout


def test_a_failure_that_does_not_repeat_kills_nothing(tmp_path):
    work = _fixture(tmp_path)
    # Green at baseline; the first run against a mutant fails an unrelated budget.
    (work / "build" / "bin" / "helix-tests").write_text(f"""#!/usr/bin/env bash
case "$*" in *--list-tests*) exit 0 ;; esac
if ! grep -q NEW_BEHAVIOR src/feature.cpp && [ ! -e {tmp_path}/flaked ]; then
    touch {tmp_path}/flaked
    echo "tests/unit/test_budget.cpp:9: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
fi
exit 0
""")
    r = _mutate(work, "--base", "main", "--no-record")
    assert "tests/unit/test_budget.cpp:9 failed once and not again" in r.stdout, r.stdout
    assert re.search(r"SURVIVED\s+src/feature.cpp", r.stdout), r.stdout
    assert r.returncode == 1, r.stdout


def test_a_reverted_new_source_is_inconclusive_with_no_build(tmp_path):
    work = _fixture(tmp_path)
    (work / "src" / "feature.cpp").write_text(ORIGINAL)
    (work / "src" / "added.cpp").write_text("int a() {\n    if (x) { return 1; }\n    return 0;\n}\n")
    _git(work, "add", "-N", "src/added.cpp")
    (work / "Makefile").write_text(
        "test-build:\n\t@[ -e src/added.cpp ] || touch .mutant-built\n"
        "\t@printf '\\n# relinked\\n' >> build/bin/helix-tests\n")
    (work / "build" / "bin" / "helix-tests").write_text("#!/usr/bin/env bash\nexit 0\n")
    r = _mutate(work, "--base", "main", "--no-record", "--only", "added", "--inside", "1")
    assert re.search(r"INCONCLUSIVE\s+src/added.cpp:1", r.stdout), r.stdout
    assert "deletes a new source file" in r.stdout
    assert not (work / ".mutant-built").exists(), "a deleted source reached a build"
    assert re.search(r"SURVIVED\s+src/added.cpp:2  negate if", r.stdout), r.stdout
    assert (work / "src" / "added.cpp").exists()


def test_a_catch2_baseline_red_twice_still_stops_the_run(tmp_path):
    work = _fixture(tmp_path)
    (work / "build" / "bin" / "helix-tests").write_text(
        '#!/usr/bin/env bash\necho "t.cpp:1: FAILED:"\nexit 1\n')
    r = _mutate(work, "--base", "main", "--no-record")
    assert r.returncode == 2, r.stdout
    assert "baseline catch2 suite is RED" in r.stdout


def test_a_failure_only_at_a_flaky_site_judges_nothing(tmp_path):
    binary = tmp_path / "suite"
    binary.write_text('#!/usr/bin/env bash\necho "t.cpp:1: FAILED:"\nexit 1\n')
    binary.chmod(0o755)
    plan = md.Catch2Plan(1, 1)
    plan.limits = {0: 60}
    import io
    assert md.run_catch2(tmp_path, binary, "[x]", plan, io.StringIO())[0] == md.DETECTED
    plan.flaky = {"t.cpp:1"}
    assert md.run_catch2(tmp_path, binary, "[x]", plan, io.StringIO())[0] == md.GREEN


def test_a_header_revert_is_syntax_checked_through_the_source_of_its_stem(tmp_path):
    (tmp_path / "scripts").mkdir()
    (tmp_path / "scripts" / "syntax_check.py").write_text(
        "import sys\nprint(sys.argv[1] + ': FAILED (1.0s)')\nsys.exit(1)\n")
    (tmp_path / "src" / "printer").mkdir(parents=True)
    (tmp_path / "src" / "printer" / "foo.cpp").write_text("")
    import io
    log = io.StringIO()
    assert md.syntax_rejects(tmp_path, "include/foo.h", log) is True
    assert "src/printer/foo.cpp: FAILED" in log.getvalue()
    assert md.syntax_rejects(tmp_path, "include/nothing_named_this.h", io.StringIO()) is False


def test_header_fanout_counts_the_units_naming_a_header(tmp_path):
    objs = tmp_path / "build" / "obj"
    objs.mkdir(parents=True)
    (objs / "a.d").write_text(f"a.o: src/a.cpp include/x.h {tmp_path}/include/y.h \\\n include/x.h\n")
    (objs / "b.d").write_text("b.o: src/b.cpp include/x.h /usr/include/z.h\n")
    (objs / "b.o").write_text("include/x.h\n")
    assert md.header_fanout(tmp_path) == {"include/x.h": 2, "include/y.h": 1}


def test_a_pool_leaves_a_header_with_many_includers_until_last(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    (work / "include").mkdir()
    (work / "include" / "wide.h").write_text("inline int w() { return 1; }\n")
    (work / "include" / "thin.h").write_text("inline int t() { return 1; }\n")
    _git(work, "add", "include", "src")
    _git(work, "commit", "-qm", "headers")
    base = _git(work, "rev-parse", "HEAD~1").strip()
    (work / "include" / "wide.h").write_text("inline int w() { return 2; }\n")
    (work / "include" / "thin.h").write_text("inline int t() { return 2; }\n")
    objs = work / "build" / "obj"
    objs.mkdir(parents=True)
    for i in range(md.HEADER_COSTLY_INCLUDERS + 1):
        (objs / f"u{i}.d").write_text("u.o: u.cpp include/wide.h\n")
    r = _mutate(work, "--base", base, "--list-only", "--pool", "--first", "wide")
    listed = [ln.split()[0].split(":")[0] for ln in r.stdout.splitlines() if ln.endswith("[cxx]")]
    assert listed[-1] == "include/wide.h", r.stdout
    assert "include/thin.h" in listed[:-1], r.stdout


# The build records in the binary whether the source it built held the change, so
# the suite judges what the binary holds, whatever the source says now.
HOLDS_MAKEFILE = """test-build:
\t@[ ! -e .mutant-refuse-rebuild ] || ! grep -q NEW_BEHAVIOR src/feature.cpp
\t@grep -q NEW_BEHAVIOR src/feature.cpp && echo clean > build/bin/holds || echo mutant > build/bin/holds
\t@printf '\\n# relinked\\n' >> build/bin/helix-tests
"""
HOLDS_SUITE = """#!/usr/bin/env bash
case "$*" in *--list-tests*) exit 0 ;; esac
[ "$(cat build/bin/holds 2>/dev/null)" != mutant ] || {
    echo "tests/unit/test_feature.cpp:11: FAILED:"
    echo "assertions: 1 | 1 failed"
    exit 1
}
exit 0
"""


def _compiled_then_data_fixture(tmp_path):
    work = _fixture(tmp_path, HOLDS_MAKEFILE)
    (work / "src" / "feature.cpp").write_text(ORIGINAL)
    (work / "ui_xml").mkdir()
    (work / "ui_xml" / "panel.xml").write_text("<view>old</view>\n")
    _git(work, "add", "ui_xml", "src")
    _git(work, "commit", "-qm", "panel")
    base = _git(work, "rev-parse", "HEAD").strip()
    (work / "src" / "feature.cpp").write_text(CHANGED)
    (work / "ui_xml" / "panel.xml").write_text("<view>new</view>\n")
    (work / "build" / "bin" / "helix-tests").write_text(HOLDS_SUITE)
    return work, base


def test_a_data_hunk_after_a_compiled_one_is_judged_on_a_clean_binary(tmp_path):
    work, base = _compiled_then_data_fixture(tmp_path)
    r = _mutate(work, "--base", base, "--no-record")
    assert re.search(r"killed\s+src/feature.cpp", r.stdout), r.stdout
    assert re.search(r"SURVIVED\s+ui_xml/panel.xml", r.stdout), r.stdout


def test_a_clean_binary_that_will_not_build_leaves_the_data_hunk_unjudged(tmp_path):
    work, base = _compiled_then_data_fixture(tmp_path)
    suite = work / "build" / "bin" / "helix-tests"
    suite.write_text(HOLDS_SUITE.replace('echo "tests/unit', 'touch .mutant-refuse-rebuild; echo "tests/unit', 1))
    r = _mutate(work, "--base", base, "--no-record")
    assert re.search(r"INCONCLUSIVE\s+ui_xml/panel.xml", r.stdout), r.stdout
    assert "uncompilable" not in r.stdout


def test_an_inside_mutant_cut_off_by_the_deadline_is_not_recorded(tmp_path, monkeypatch):
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "f.cpp").write_text("    if (a) {\n")
    h = {"file": "src/f.cpp", "line": 1, "body": ["@@\n", "+    if (a) {\n"]}
    guard = md.TreeGuard(tmp_path)
    guard.capture("src/f.cpp")

    def cut_off(*_a, **_k):
        monkeypatch.setattr(md.RUN_STATE, "out_of_time", True)
        return "survived", "", True
    monkeypatch.setattr(md, "build_and_judge", cut_off)
    args = type("A", (), {"inside": 1})()
    md.mutate_inside(tmp_path, guard, None, h, {}, args, None)
    assert h["inside"] == []
    assert (tmp_path / "src" / "f.cpp").read_text() == "    if (a) {\n"


def _suites_on(tmp_path, script):
    binary = tmp_path / "build" / "bin" / "helix-tests"
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.write_text(script)
    binary.chmod(0o755)
    args = type("A", (), {"jobs": 1, "shards": 1, "timeout": 0, "tests": "[x]", "log": "x",
                          "shell_tests": "", "python_tests": ""})()
    import io
    suites = md.Suites(tmp_path, args, io.StringIO())
    suites.catch2.limits = {0: 60}
    return suites


def _counted_script(tmp_path, *runs):
    """A suite whose Nth run is the Nth of `runs`: a site that fails, or '' for green."""
    cases = "\n".join(
        f'{i}) ' + (f'echo "{site}: FAILED:"; echo "assertions: 1 | 1 failed"; exit 1'
                    if site else 'exit 0') + ' ;;'
        for i, site in enumerate(runs, 1))
    return f"""#!/usr/bin/env bash
n=$(( $(cat {tmp_path}/n 2>/dev/null || echo 0) + 1 )); echo $n > {tmp_path}/n
case $n in
{cases}
*) exit 0 ;;
esac
"""


def test_a_second_flake_on_the_rerun_has_to_repeat_too(tmp_path):
    # Run, its repeat, the re-run failing elsewhere, that repeat, the last re-run.
    suites = _suites_on(tmp_path, _counted_script(tmp_path, "a.cpp:1", "", "b.cpp:2", "", ""))
    assert suites.run("catch2")[0] == md.GREEN
    assert suites.catch2.flaky == {"a.cpp:1", "b.cpp:2"}


def test_a_failure_elsewhere_on_the_repeat_is_not_a_repeat(tmp_path):
    suites = _suites_on(tmp_path, _counted_script(tmp_path, "a.cpp:1", "b.cpp:2", "", ""))
    assert suites.run("catch2")[0] == md.GREEN
    assert "a.cpp:1" in suites.catch2.flaky


def test_a_failure_that_repeats_at_its_site_is_a_detection(tmp_path):
    suites = _suites_on(tmp_path, _counted_script(tmp_path, "a.cpp:1", "a.cpp:1"))
    assert suites.run("catch2")[0] == md.DETECTED
    assert suites.catch2.flaky == set()


def test_a_repeat_that_crashes_naming_nothing_is_unjudged_and_flags_no_site(tmp_path):
    suites = _suites_on(tmp_path, f"""#!/usr/bin/env bash
if [ ! -e {tmp_path}/once ]; then
    touch {tmp_path}/once; echo "a.cpp:1: FAILED:"; exit 1
fi
exit 139
""")
    state, why = suites.run("catch2")
    assert state == md.INCONCLUSIVE, why
    assert suites.catch2.flaky == set()


def test_a_repeat_that_stalls_is_unjudged(tmp_path):
    suites = _suites_on(tmp_path, f"""#!/usr/bin/env bash
if [ ! -e {tmp_path}/once ]; then
    touch {tmp_path}/once; echo "a.cpp:1: FAILED:"; exit 1
fi
exec sleep 30
""")
    suites.catch2.limits = {0: 3}
    state, why = suites.run("catch2")
    assert state == md.INCONCLUSIVE, why
    assert suites.catch2.flaky == set()


def test_every_flaky_assertion_is_named_at_the_end_and_on_the_clean_line(tmp_path):
    work = _fixture(tmp_path)
    (work / "build" / "bin" / "helix-tests").write_text(_flaky_suite(tmp_path))
    r = _mutate(work, "--base", "main", "--no-record")
    tail = r.stdout.split("=" * 68, 1)[1]
    assert "set aside as flaky" in tail and "tests/unit/test_budget.cpp:9" in tail, r.stdout
    clean = next(ln for ln in tail.splitlines() if ln.startswith("VERDICT: CLEAN"))
    assert "1 flaky assertion" in clean, r.stdout


def test_a_group_run_names_its_childrens_flaky_assertions(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    (work / ".mutant-expect-other").write_text("")
    (work / "build" / "bin" / "helix-tests").write_text(_flaky_suite(tmp_path).replace(
        'grep -q NEW_BEHAVIOR src/feature.cpp ||',
        'grep -q NEW_BEHAVIOR src/feature.cpp && grep -q NEW_TOO src/other.cpp ||'))
    r = _mutate(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]",
                "--tests-for", "src/other.cpp=[oth]")
    tail = r.stdout.rsplit("=" * 68, 1)[1]
    assert "tests/unit/test_budget.cpp:9" in tail, r.stdout
    assert re.search(r"VERDICT: CLEAN.*1 flaky assertion", tail), r.stdout


@pytest.mark.skipif(shutil.which("bats") is None, reason="a script hunk is listed only when bats can run it")
def test_a_script_waits_behind_every_compiled_hunk(tmp_path):
    work = _fixture(tmp_path)
    (work / "tests" / "shell").mkdir(parents=True)
    (work / "scripts" / "tool.sh").write_text("#!/bin/sh\necho old\n")
    (work / "tests" / "shell" / "test_tool.bats").write_text(
        '@test "t" {\n  grep -q tool.sh "$BATS_TEST_FILENAME"\n}\n')
    _git(work, "add", "scripts/tool.sh", "tests/shell")
    _git(work, "commit", "-qm", "tool")
    base = _git(work, "rev-parse", "HEAD").strip()
    (work / "scripts" / "tool.sh").write_text("#!/bin/sh\necho NEW\n")
    r = _mutate(work, "--base", base, "--list-only", "--python-tests", "", "--first", "tool")
    listed = [ln.split()[0] for ln in r.stdout.splitlines()
              if ln.endswith(("[cxx]", "[tooling]"))]
    assert [p.split(":")[0] for p in listed] == ["src/feature.cpp", "scripts/tool.sh"], r.stdout


def test_confirm_survivors_without_a_pool_says_it_does_nothing(tmp_path):
    work = _fixture(tmp_path)
    r = _mutate(work, "--base", "main", "--no-record", "--confirm-survivors")
    assert "--confirm-survivors needs --pool" in r.stdout, r.stdout


def test_an_inside_mutant_of_a_file_the_change_deleted_is_passed_over(tmp_path):
    h = {"file": "src/gone.cpp", "line": 1, "body": ["@@\n"]}
    args = type("A", (), {"inside": 1})()
    guard = md.TreeGuard(tmp_path)
    assert md.mutate_inside(tmp_path, guard, None, h, {}, args, None) is False
    assert h["inside"] == []


def test_each_file_runs_against_its_own_filter_as_its_own_group(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    (work / ".mutant-expect-other").write_text("")
    suite = work / "build" / "bin" / "helix-tests"
    suite.write_text(DETECTING_SUITE.replace(
        "#!/usr/bin/env bash\n", '#!/usr/bin/env bash\necho "$1" >> .filters\n', 1))
    r = _mutate(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]",
                "--tests-for", "src/other.cpp=[oth]")
    assert r.returncode == 0, r.stdout
    assert "group 1/2: [feat]" in r.stdout
    assert "group 2/2: [oth]" in r.stdout
    assert "VERDICT: CLEAN - every group" in r.stdout
    filters = set((work / ".filters").read_text().split())
    assert filters == {"[feat]", "[oth]"}, filters


def test_a_surviving_group_makes_the_whole_run_fail(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    # No .mutant-expect-other: nothing detects other.cpp's mutant.
    r = _mutate(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]",
                "--tests-for", "src/other.cpp=[oth]")
    assert r.returncode == 1, r.stdout
    assert "exit 1  [oth]" in r.stdout
    assert "exit 0  [feat]" in r.stdout


@pytest.mark.parametrize("other_detected, code, says", [
    (False, 1, "Hunks SURVIVED in the groups above"),
    (True, 3, "Nothing survived"),
])
def test_an_incomplete_group_run_tells_the_truth_about_survivors(
        tmp_path, other_detected, code, says):
    work, base = _two_hunk_fixture(tmp_path, FAST_MAKEFILE)
    if other_detected:
        (work / ".mutant-expect-other").write_text("")
    (work / "notes").mkdir()
    (work / "notes" / "plan.txt").write_text("plan\n")
    _git(work, "add", "-N", "notes/plan.txt")
    r = _mutate(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]",
                "--tests-for", "src/other.cpp=[oth]")
    assert r.returncode == code, r.stdout
    assert "NOT COVERED notes/plan.txt" in r.stdout
    verdict = r.stdout[r.stdout.index("VERDICT: INCOMPLETE - this run"):]
    assert says in verdict


# The mutant's build SIGKILLs the group child that started it (make's parent),
# so the child never runs its own restore.
KILL_CHILD_MAKEFILE = """test-build:
\t@if ! grep -q NEW_BEHAVIOR src/feature.cpp; then kill -9 $$(ps -o ppid= -p $$PPID); fi
\t@[ -e build/bin/helix-tests ] && printf '\\n# relinked\\n' >> build/bin/helix-tests || true
"""


def test_a_group_child_killed_mid_mutant_has_its_file_put_back(tmp_path):
    work, base = _two_hunk_fixture(tmp_path, KILL_CHILD_MAKEFILE)
    (work / ".mutant-expect-other").write_text("")
    before = (work / "src" / "feature.cpp").read_bytes()
    r = _mutate(work, "--base", base, "--tests-for", "src/feature.cpp=[feat]",
                "--tests-for", "src/other.cpp=[oth]")
    assert (work / "src" / "feature.cpp").read_bytes() == before, r.stdout
    assert "RESTORED      src/feature.cpp" in r.stdout
    assert r.returncode == 137, r.stdout


# ---------------------------------------------------------------------------
# Scripts run against the bats and pytest files that name them
# ---------------------------------------------------------------------------
def test_suite_paths_splits_a_comma_list_and_drops_blanks():
    assert md.suite_paths("tests/shell/a.bats, tests/shell/b.bats,") == [
        "tests/shell/a.bats", "tests/shell/b.bats"]
    assert md.suite_paths("") == []
    assert md.suite_paths("tests/shell") == ["tests/shell"]


SHELL_SOURCES = {
    "tests/shell/test_phase2.bats": 'run scripts/qc/phase2.sh\n',
    "tests/shell/test_jobs.bats": "myjobs.sh and jobs.shx\n",
    "tests/shell/test_mutate.bats": "python3 scripts/mutate_diff.py --list-only\n",
}
PY_SOURCES = {"tests/python/test_mutate_diff.py": "import mutate_diff as md\n"}


def test_a_script_is_found_by_its_file_name_as_a_whole_word():
    assert md.auto_script_tests("scripts/qc/phase2.sh", SHELL_SOURCES) == [
        "tests/shell/test_phase2.bats"]
    assert md.auto_script_tests("scripts/jobs.sh", SHELL_SOURCES) == []


def test_a_python_script_is_found_by_its_import_stem_too():
    assert md.auto_script_tests("scripts/mutate_diff.py", PY_SOURCES) == [
        "tests/python/test_mutate_diff.py"]
    assert md.auto_script_tests("scripts/mutate_diff.py", SHELL_SOURCES) == [
        "tests/shell/test_mutate.bats"]


def test_a_script_file_name_is_never_too_common():
    many = {f"tests/shell/t{i}.bats": "run quality-checks.sh"
            for i in range(md.AUTO_TESTS_MAX_FILES + 5)}
    assert len(md.auto_script_tests("scripts/quality-checks.sh", many)) == md.AUTO_TESTS_MAX_FILES + 5


def test_an_import_stem_too_many_files_mention_is_ignored():
    many = {f"tests/python/test_{i}.py": "from common_util import x"
            for i in range(md.AUTO_TESTS_MAX_FILES + 1)}
    assert md.auto_script_tests("scripts/common_util.py", many) == []
    many["tests/python/test_x.py"] = "subprocess.run(['scripts/common_util.py'])"
    assert md.auto_script_tests("scripts/common_util.py", many) == ["tests/python/test_x.py"]


def test_a_script_nothing_names_keeps_both_whole_suites():
    shell, python, why = md.script_tests_for(
        "scripts/orphan.sh", True, SHELL_SOURCES, PY_SOURCES, "tests/shell", "tests/python")
    assert (shell, python) == ("tests/shell", "tests/python")
    shell, python, _ = md.script_tests_for(
        "scripts/qc/phase2.sh", True, SHELL_SOURCES, PY_SOURCES, "tests/shell", "tests/python")
    assert (shell, python) == ("tests/shell/test_phase2.bats", "")
    shell, python, _ = md.script_tests_for(
        "scripts/qc/phase2.sh", False, SHELL_SOURCES, PY_SOURCES, "tests/shell", "tests/python")
    assert (shell, python) == ("tests/shell", "tests/python")


def test_a_group_is_described_by_everything_that_judges_it():
    assert md.describe_key(("[x]", "", "")) == "[x]"
    assert md.describe_key(("[x]", "tests/shell/a.bats", "")) == "[x] | tests/shell/a.bats"


needs_bats = pytest.mark.skipif(shutil.which("bats") is None, reason="needs bats")


@needs_bats
def test_a_cxx_file_with_no_tests_pays_no_baseline_for_a_script_with_none(tmp_path):
    work = _fixture(tmp_path)
    (work / "tests" / "shell").mkdir(parents=True)
    (work / "scripts" / "tool.sh").write_text("#!/bin/sh\necho old\n")
    (work / "tests" / "shell" / "test_other.bats").write_text('@test "other" {\n  true\n}\n')
    _git(work, "add", "scripts/tool.sh", "tests/shell")
    _git(work, "commit", "-qm", "tool")
    base = _git(work, "rev-parse", "HEAD").strip()
    (work / "scripts" / "tool.sh").write_text("#!/bin/sh\necho NEW\n")
    r = _mutate(work, "--base", base, "--python-tests", "")
    groups = [ln for ln in r.stdout.splitlines() if ln.startswith("=== group ")]
    assert len(groups) == 2, r.stdout
    cxx = next(g for g in groups if "| tests/shell" not in g)
    tool = next(g for g in groups if "| tests/shell" in g)
    assert "~[.]~[slow]  (1 file(s))" in cxx and "~[.]~[slow] | tests/shell  (1 file(s))" in tool
    # The C++ group's own section runs no bats at all.
    section = r.stdout.split(cxx, 1)[1].split("=== group ", 1)[0]
    assert "bats green" not in section, section


@needs_bats
def test_each_script_runs_only_the_bats_file_that_names_it(tmp_path):
    work = _fixture(tmp_path)
    (work / "tests" / "shell").mkdir(parents=True)
    for name in ("alpha", "beta"):
        (work / "scripts" / f"{name}.sh").write_text("#!/bin/sh\necho old\n")
        (work / "tests" / "shell" / f"test_{name}.bats").write_text(
            f'@test "{name}" {{\n'
            f'  echo test_{name} >> "$BATS_TEST_DIRNAME/../../.ran"\n'
            f'  grep -q NEW "$BATS_TEST_DIRNAME/../../scripts/{name}.sh"\n'
            f'}}\n')
    _git(work, "add", ".")
    _git(work, "commit", "-qm", "scripts")
    base = _git(work, "rev-parse", "HEAD").strip()
    for name in ("alpha", "beta"):
        (work / "scripts" / f"{name}.sh").write_text("#!/bin/sh\necho NEW\n")
    r = _mutate(work, "--base", base)
    assert r.returncode == 0, r.stdout
    assert "scripts/alpha.sh  tests/shell/test_alpha.bats" in r.stdout
    assert "scripts/beta.sh  tests/shell/test_beta.bats" in r.stdout
    assert "VERDICT: CLEAN - every group" in r.stdout
    ran = (work / ".ran").read_text().split()
    # Baseline plus mutant for each, and never the other script's file.
    assert sorted(set(ran)) == ["test_alpha", "test_beta"]
    assert ran.count("test_alpha") == 2 and ran.count("test_beta") == 2, ran


# ---------------------------------------------------------------------------
# Script cases red at baseline are set aside, not a reason to refuse
# ---------------------------------------------------------------------------
def test_failing_tests_names_bats_and_pytest_failures():
    bats = "ok 1 fine\nnot ok 2 needs a jobpool\nnot ok 3 skipped thing # TODO later\nok 4 x\n"
    assert md.failing_tests("bats", bats) == {"needs a jobpool", "skipped thing"}
    py = "FAILED tests/python/test_a.py::test_x - AssertionError\n1 failed, 3 passed\n"
    assert md.failing_tests("pytest", py) == {"tests/python/test_a.py::test_x"}
    assert md.failing_tests("catch2", "t.cpp:1: FAILED:\n") == set()


def _script_with_bats(tmp_path, body):
    work = _fixture(tmp_path)
    (work / "tests" / "shell").mkdir(parents=True)
    (work / "scripts" / "tool.sh").write_text("#!/bin/sh\necho old\n")
    (work / "tests" / "shell" / "test_tool.bats").write_text(body)
    _git(work, "add", ".")
    _git(work, "commit", "-qm", "tool")
    base = _git(work, "rev-parse", "HEAD").strip()
    (work / "scripts" / "tool.sh").write_text("#!/bin/sh\necho NEW\n")
    return work, base


ENV_RED = '@test "needs the environment" {\n  false\n}\n'


@needs_bats
def test_a_case_red_at_baseline_is_set_aside_and_the_rest_still_judge(tmp_path):
    work, base = _script_with_bats(tmp_path, ENV_RED + (
        '@test "tool is new" {\n'
        '  grep -q NEW "$BATS_TEST_DIRNAME/../../scripts/tool.sh"\n'
        '}\n'))
    r = _mutate(work, "--base", base, "--only", "scripts/tool.sh", "--no-auto-tests",
                "--shell-tests", "tests/shell/test_tool.bats", "--python-tests", "")
    assert r.returncode == 0, r.stdout
    assert "1 case(s) red at baseline here, set aside" in r.stdout
    assert "needs the environment" in r.stdout
    assert "VERDICT: CLEAN" in r.stdout


@needs_bats
def test_a_mutant_only_a_baseline_red_case_notices_survives(tmp_path):
    work, base = _script_with_bats(tmp_path, ENV_RED + '@test "unrelated" {\n  true\n}\n')
    r = _mutate(work, "--base", base, "--only", "scripts/tool.sh", "--no-auto-tests",
                "--shell-tests", "tests/shell/test_tool.bats", "--python-tests", "")
    assert r.returncode == 1, r.stdout
    assert "SURVIVED" in r.stdout


def test_the_catch2_runner_turns_on_file_tags_for_a_file_tag_filter(tmp_path):
    seen = tmp_path / "argv"
    exe = tmp_path / "helix-tests"
    exe.write_text(f'#!/usr/bin/env bash\necho "$*" > {seen}\n'
                   'echo "All tests passed (1 assertion in 1 test case)"\n')
    exe.chmod(0o755)
    plan = md.Catch2Plan(1, 1)
    state, _, _ = md.run_catch2(tmp_path, exe, "[#test_foo]~[.]~[slow]", plan,
                                open(os.devnull, "w"))
    assert state == md.GREEN
    assert "-#" in seen.read_text().split()


@needs_bats
def test_a_script_named_by_two_bats_files_runs_both(tmp_path):
    work = _fixture(tmp_path)
    (work / "tests" / "shell").mkdir(parents=True)
    (work / "scripts" / "tool.sh").write_text("#!/bin/sh\necho old\n")
    for name in ("one", "two"):
        (work / "tests" / "shell" / f"test_{name}.bats").write_text(
            f'@test "{name}" {{\n'
            f'  echo {name} >> "$BATS_TEST_DIRNAME/../../.ran"\n'
            f'  grep -q NEW "$BATS_TEST_DIRNAME/../../scripts/tool.sh"  # tool.sh\n'
            f'}}\n')
    _git(work, "add", ".")
    _git(work, "commit", "-qm", "tool")
    base = _git(work, "rev-parse", "HEAD").strip()
    (work / "scripts" / "tool.sh").write_text("#!/bin/sh\necho NEW\n")
    r = _mutate(work, "--base", base, "--python-tests", "")
    assert r.returncode == 0, r.stdout
    assert "tests/shell/test_one.bats,tests/shell/test_two.bats" in r.stdout
    # The baseline runs both files; the mutant stops at whichever fails first.
    assert {"one", "two"} <= set((work / ".ran").read_text().split())


def test_a_pytest_case_red_at_baseline_is_deselected_and_the_rest_judge(tmp_path):
    work = _fixture(tmp_path)
    (work / "tests" / "python").mkdir(parents=True)
    (work / "scripts" / "tool.py").write_text("VALUE = 'old'\n")
    (work / "tests" / "python" / "test_tool.py").write_text(
        "import pathlib\n"
        "SRC = pathlib.Path(__file__).resolve().parents[2] / 'scripts' / 'tool.py'\n"
        "def test_env_red():\n    assert False\n"
        "def test_new():\n    assert 'NEW' in SRC.read_text()\n")
    _git(work, "add", ".")
    _git(work, "commit", "-qm", "tool")
    base = _git(work, "rev-parse", "HEAD").strip()
    (work / "scripts" / "tool.py").write_text("VALUE = 'NEW'\n")
    # The run judges with the repo's own venv when it has one; this interpreter has
    # pytest, where a bare python3 on PATH (a CI runner's) may not. A wrapper, not a
    # symlink: a venv's python started through a link elsewhere finds no pyvenv.cfg
    # beside it and runs as the bare base interpreter.
    venv_python = work / ".venv" / "bin" / "python3"
    venv_python.parent.mkdir(parents=True)
    venv_python.write_text(f'#!/bin/sh\nexec "{sys.executable}" "$@"\n')
    venv_python.chmod(0o755)
    r = _mutate(work, "--base", base, "--no-auto-tests", "--shell-tests", "",
                "--python-tests", "tests/python/test_tool.py")
    assert r.returncode == 0, r.stdout
    assert "pytest: 1 case(s) red at baseline here, set aside" in r.stdout
    assert "test_env_red" in r.stdout
    assert "VERDICT: CLEAN" in r.stdout


# ---------------------------------------------------------------------------
# A group child reports only its own files
# ---------------------------------------------------------------------------
def _group_fixture(tmp_path):
    work = _fixture(tmp_path)
    (work / "include").mkdir()
    (work / "include" / "feature.h").write_text(HEADER)
    (work / "notes").mkdir()
    (work / "notes" / "plan.txt").write_text("old\n")
    _git(work, "add", "include/feature.h", "notes/plan.txt")
    _git(work, "commit", "-qm", "header")
    _git(work, "branch", "-f", "main")
    (work / "include" / "feature.h").write_text(DECLARATION_ONLY["member function"])
    (work / "notes" / "plan.txt").write_text("new\n")
    return work


def test_a_group_child_counts_and_judges_only_its_own_files(tmp_path):
    work = _group_fixture(tmp_path)
    r = _mutate(work, "--base", "main", "--group-child", "--no-record",
                "--group-files", "src/feature.cpp")
    assert r.returncode == 0, r.stdout
    assert "declaration-only" not in r.stdout
    assert "VERDICT: CLEAN - every changed hunk was mutated" in r.stdout
    # The parent, not the child, reports what lies outside the group.
    p = _mutate(work, "--base", "main", "--no-auto-tests")
    assert p.returncode == 3, p.stdout
    assert "NOT COVERED notes/plan.txt" in p.stdout
    assert "declaration-only" in p.stdout


def test_a_failed_setup_file_is_named_after_its_file():
    out = ("not ok 1 setup_file failed\n"
           "# (from function `setup_file' in test file /w1/tests/shell/test_one.bats, line 2)\n"
           "#   `false' failed\n"
           "not ok 2 teardown_file failed\n"
           "# (from function `teardown_file' in test file /w1/tests/shell/test_two.bats, line 9)\n"
           "not ok 3 an ordinary case # skip no jobpool\n"
           "# (in test file /w1/tests/shell/test_two.bats, line 4)\n")
    assert md.failing_tests("bats", out) == {
        "setup_file failed in test_one.bats", "teardown_file failed in test_two.bats",
        "an ordinary case"}


@needs_bats
def test_a_mutant_breaking_another_files_setup_is_killed_past_a_baseline_red_one(tmp_path):
    # test_env.bats fails its setup_file at baseline; test_tool.bats's setup_file
    # fails only under the mutant. Both read "setup_file failed".
    work, base = _script_with_bats(tmp_path, '@test "unrelated" {\n  true\n}\n')
    (work / "tests" / "shell" / "test_env.bats").write_text(
        'setup_file() {\n  false\n}\n@test "needs env" {\n  true\n}\n')
    (work / "tests" / "shell" / "test_tool.bats").write_text(
        'setup_file() {\n  grep -q NEW "$BATS_TEST_DIRNAME/../../scripts/tool.sh"\n}\n'
        '@test "uses tool" {\n  true\n}\n')
    _git(work, "add", "tests/shell")
    _git(work, "commit", "-qm", "suites")
    base = _git(work, "rev-parse", "HEAD~1").strip()
    r = _mutate(work, "--base", base, "--only", "scripts/tool.sh", "--no-auto-tests",
                "--shell-tests", "tests/shell/test_env.bats,tests/shell/test_tool.bats",
                "--python-tests", "")
    assert "setup_file failed in test_env.bats" in r.stdout
    assert "killed" in r.stdout and "SURVIVED" not in r.stdout, r.stdout
