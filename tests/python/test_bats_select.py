# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/bats_select.py - which shell test files a change runs.

Each case builds a throwaway repo with a tiny tests/shell/ and trigger table and
runs the real script against it, so the git half (what counts as changed) is
exercised along with the selection.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SELECT = REPO_ROOT / "scripts" / "bats_select.py"

PRODUCT = "tests/shell/test_installer.bats"
TOOL_A = "tests/shell/test_gate_a.bats"
TOOL_B = "tests/shell/test_gate_b.bats"


def _git(root, *args):
    subprocess.run(["git", *args], cwd=root, check=True, capture_output=True)


def _write(root, rel, content="x\n"):
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content)


def _repo(tmp_path, table=None):
    """A committed repo: one product file, two tooling files, their scripts."""
    root = tmp_path / "repo"
    root.mkdir()
    _git(root, "init", "-q", "-b", "main")
    _git(root, "config", "user.email", "t@example.com")
    _git(root, "config", "user.name", "T")
    for f in (PRODUCT, TOOL_A, TOOL_B, "tests/shell/helpers.bash",
              "scripts/gate_a.py", "scripts/qc/b.sh", "scripts/lib/installer/main.sh",
              "src/app.cpp"):
        _write(root, f)
    _write(root, "tests/shell/tooling-triggers.tsv", table if table is not None else
           "# comment\n"
           f"{TOOL_A}\tscripts/gate_a.py\n"
           f"{TOOL_B}\tscripts/qc/*.sh mk/\n")
    _write(root, "mk/rules.mk")
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "base")
    return root


def _run(root, *args, env=None):
    e = {k: v for k, v in os.environ.items() if k != "BATS_ALL"}
    e.update(env or {})
    r = subprocess.run([sys.executable, str(SELECT), "--root", str(root), *args],
                       capture_output=True, text=True, env=e)
    return r.returncode, r.stdout.split(), r.stderr


def test_product_only_change_runs_no_tooling_file(tmp_path):
    root = _repo(tmp_path)
    _write(root, "src/app.cpp", "changed\n")
    rc, files, _ = _run(root, "--base", "HEAD")
    assert rc == 0
    assert files == [PRODUCT]


def test_a_tooling_path_runs_its_file_and_the_product(tmp_path):
    root = _repo(tmp_path)
    _write(root, "scripts/qc/b.sh", "changed\n")
    rc, files, _ = _run(root, "--base", "HEAD")
    assert rc == 0
    assert files == [TOOL_B, PRODUCT]


def test_a_directory_trigger_covers_everything_under_it(tmp_path):
    root = _repo(tmp_path)
    _write(root, "mk/rules.mk", "changed\n")
    rc, files, _ = _run(root, "--base", "HEAD")
    assert files == [TOOL_B, PRODUCT]


def test_a_committed_change_since_the_base_counts(tmp_path):
    root = _repo(tmp_path)
    _write(root, "scripts/gate_a.py", "changed\n")
    _git(root, "commit", "-q", "-am", "edit")
    rc, files, _ = _run(root, "--base", "HEAD~1")
    assert files == [TOOL_A, PRODUCT]


def test_editing_a_tooling_test_itself_runs_it(tmp_path):
    root = _repo(tmp_path)
    _write(root, TOOL_A, "changed\n")
    rc, files, _ = _run(root, "--base", "HEAD")
    assert files == [TOOL_A, PRODUCT]


def test_a_helpers_change_runs_everything(tmp_path):
    root = _repo(tmp_path)
    _write(root, "tests/shell/helpers.bash", "changed\n")
    rc, files, err = _run(root, "--base", "HEAD")
    assert rc == 0
    assert files == [TOOL_A, TOOL_B, PRODUCT]
    assert "helpers.bash" in err


def test_bats_all_runs_everything(tmp_path):
    root = _repo(tmp_path)
    rc, files, _ = _run(root, "--base", "HEAD", env={"BATS_ALL": "1"})
    assert files == [TOOL_A, TOOL_B, PRODUCT]


def test_an_unlisted_new_file_always_runs(tmp_path):
    root = _repo(tmp_path)
    _git(root, "commit", "-q", "--allow-empty", "-m", "noop")
    new = "tests/shell/test_new_thing.bats"
    _write(root, new)
    _git(root, "add", new)
    _git(root, "commit", "-q", "-m", "new test")
    # Measured from the commit that added it, the new file is not part of the
    # change, and runs anyway because nothing lists it.
    rc, files, _ = _run(root, "--base", "HEAD")
    assert rc == 0
    assert new in files
    assert TOOL_A not in files


def test_a_listed_file_that_does_not_exist_fails(tmp_path):
    root = _repo(tmp_path, table=f"{TOOL_A}\tscripts/gate_a.py\n"
                                 "tests/shell/test_gone.bats\tscripts/gate_a.py\n")
    rc, files, err = _run(root, "--check")
    assert rc == 2
    assert "test_gone.bats" in err and "does not exist" in err
    rc, files, _ = _run(root, "--base", "HEAD")
    assert rc == 2 and files == []


def test_a_trigger_matching_nothing_fails(tmp_path):
    root = _repo(tmp_path, table=f"{TOOL_A}\tscripts/renamed_away.py\n")
    rc, _, err = _run(root, "--check")
    assert rc == 2
    assert "scripts/renamed_away.py matches no tracked path" in err


def test_a_file_listed_twice_fails(tmp_path):
    root = _repo(tmp_path, table=f"{TOOL_A}\tscripts/gate_a.py\n{TOOL_A}\tmk/\n")
    rc, _, err = _run(root, "--check")
    assert rc == 2
    assert "listed twice" in err


def test_the_real_table_resolves():
    rc, _, err = _run(REPO_ROOT, "--check")
    assert rc == 0, err


def test_a_tree_without_git_runs_everything(tmp_path):
    root = _repo(tmp_path)
    shutil.rmtree(root / ".git")
    rc, files, err = _run(root)
    assert rc == 0
    assert files == [TOOL_A, TOOL_B, PRODUCT]
    assert "not a git checkout" in err
