#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if the Python test suites' async support is declared but not installed.

Both halves of async pytest fail SILENTLY-ish, in the specific sense that
matters: neither one looks like a missing dependency at the point you'd look.

  E1  An `async def test_*` exists but requirements.txt does not declare
      pytest-asyncio. Plain pytest does not skip such a test and does not
      error out during collection — it collects it, runs it as an ordinary
      function, gets a coroutine back, and fails it with

          Failed: async def functions are not natively supported.

      So the suite reads as N genuine test failures rather than "your
      environment is missing a plugin". That is exactly how CI went red for a
      day on a suite that was green on the developer's machine: the local
      .venv had pytest-asyncio installed by hand, requirements.txt did not
      list it, and CI builds its environment only from requirements.txt.

  E2  An `async def test_*` with no @pytest.mark.asyncio. pytest-asyncio's
      DEFAULT mode is strict, which SKIPS an unmarked async test. That is the
      worse failure: the suite is green and the test never ran. This rule is
      dropped when a pytest config sets asyncio_mode = auto, which makes the
      markers unnecessary by design.

Neither rule fires on a repo with no async tests, so a suite that never grows
one never sees this gate.
"""

import argparse
import ast
import re
import sys
from pathlib import Path

# Test trees scanned by default. These are the ones CI runs with pytest.
DEFAULT_TEST_DIRS = ("moonraker-plugin/tests", "tests/ui")

# Files a pytest asyncio_mode can be configured in.
PYTEST_CONFIG_FILES = ("pytest.ini", "pyproject.toml", "setup.cfg", "tox.ini")

# Matches the requirement NAME only, so a version pin, an extra, or an
# environment marker on the line does not hide the declaration.
ASYNCIO_REQ = re.compile(r"^\s*pytest[-_]asyncio\b", re.IGNORECASE)


def declares_pytest_asyncio(requirements: Path) -> bool:
    if not requirements.is_file():
        return False
    return any(ASYNCIO_REQ.match(line) for line in requirements.read_text().splitlines())


def asyncio_mode_is_auto(root: Path) -> bool:
    """True if a pytest config turns on auto mode, which makes markers optional."""
    for name in PYTEST_CONFIG_FILES:
        path = root / name
        if not path.is_file():
            continue
        text = path.read_text()
        # Deliberately loose: any of the four file formats spells the setting
        # `asyncio_mode = auto` / `asyncio_mode = "auto"` on one line.
        if re.search(r"asyncio_mode\s*[=:]\s*[\"']?auto\b", text):
            return True
    return False


def marks_asyncio(decorator: ast.expr) -> bool:
    """True for @pytest.mark.asyncio and @pytest.mark.asyncio(...)."""
    if isinstance(decorator, ast.Call):
        decorator = decorator.func
    return isinstance(decorator, ast.Attribute) and decorator.attr == "asyncio"


def module_marks_asyncio(tree: ast.Module) -> bool:
    """True for a module-level `pytestmark = pytest.mark.asyncio` (or a list of them)."""
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(t, ast.Name) and t.id == "pytestmark" for t in node.targets):
            continue
        value = node.value
        marks = value.elts if isinstance(value, (ast.List, ast.Tuple)) else [value]
        if any(marks_asyncio(m) for m in marks):
            return True
    return False


def async_tests(path: Path):
    """Yield (lineno, qualname, is_marked) for every async test function in a file.

    Only module-level and class-level defs are considered — a nested `async def`
    inside a test body is a helper, not a case pytest would collect.
    """
    try:
        tree = ast.parse(path.read_text(), filename=str(path))
    except SyntaxError as exc:
        print(f"ERROR: cannot parse {path}: {exc}", file=sys.stderr)
        return

    covered_by_module = module_marks_asyncio(tree)

    def scan(body, prefix=""):
        for node in body:
            if isinstance(node, ast.ClassDef):
                yield from scan(node.body, f"{prefix}{node.name}::")
            elif isinstance(node, ast.AsyncFunctionDef) and node.name.startswith("test_"):
                marked = covered_by_module or any(
                    marks_asyncio(d) for d in node.decorator_list
                )
                yield node.lineno, f"{prefix}{node.name}", marked

    yield from scan(tree.body)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=root)
    parser.add_argument("--requirements", type=Path)
    parser.add_argument("--test-dir", action="append", dest="test_dirs")
    parser.add_argument("--list", action="store_true", help="print every async test found")
    args = parser.parse_args()

    root = args.root.resolve()
    requirements = args.requirements or (root / "requirements.txt")
    test_dirs = args.test_dirs or [root / d for d in DEFAULT_TEST_DIRS]

    found = []  # (relpath, lineno, qualname, marked)
    for d in test_dirs:
        d = Path(d)
        if not d.is_absolute():
            d = root / d
        if not d.is_dir():
            continue
        for path in sorted(d.rglob("test_*.py")):
            for lineno, qualname, marked in async_tests(path):
                rel = path.relative_to(root) if path.is_relative_to(root) else path
                found.append((rel, lineno, qualname, marked))

    if args.list:
        for rel, lineno, qualname, marked in found:
            flag = "marked" if marked else "UNMARKED"
            print(f"{rel}:{lineno}: {qualname} ({flag})")

    if not found:
        print("OK: no async tests in the Python suites — pytest-asyncio not required")
        return 0

    failed = False

    # E1 — the dependency itself.
    if not declares_pytest_asyncio(requirements):
        failed = True
        print(
            f"ERROR: {len(found)} async test(s) need pytest-asyncio, but "
            f"{requirements.name} does not declare it."
        )
        print(
            "Without the plugin pytest still COLLECTS them and fails each one with"
        )
        print('  "async def functions are not natively supported."')
        print(f"Add a pytest-asyncio line to {requirements.name}.")
        for rel, lineno, qualname, _ in found[:5]:
            print(f"  {rel}:{lineno}: {qualname}")
        if len(found) > 5:
            print(f"  ... and {len(found) - 5} more")

    # E2 — the marker, unless auto mode makes it unnecessary.
    if not asyncio_mode_is_auto(root):
        unmarked = [f for f in found if not f[3]]
        if unmarked:
            failed = True
            print(
                f"ERROR: {len(unmarked)} async test(s) lack @pytest.mark.asyncio."
            )
            print(
                "pytest-asyncio defaults to strict mode, which SKIPS an unmarked "
                "async test — the suite stays green and the test never runs."
            )
            print("Add the marker, or set asyncio_mode = auto in a pytest config.")
            for rel, lineno, qualname, _ in unmarked:
                print(f"  {rel}:{lineno}: {qualname}")

    if failed:
        return 1

    print(f"OK: {len(found)} async test(s), pytest-asyncio declared and every case marked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
