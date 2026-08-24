#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_pytest_asyncio_deps.py — the gate on async
# pytest support being declared, not just installed.
#
# The shape it exists to catch: moonraker-plugin/tests/test_helix_print.py
# landed with 27 `async def` cases and a CI step to run them, but nothing
# added pytest-asyncio to requirements.txt. The developer's .venv had the
# plugin installed by hand, so the suite was green locally; CI builds its
# environment from requirements.txt alone and every one of those 27 cases
# failed with "async def functions are not natively supported". Build stayed
# red for a day. Nothing in that failure mode says "missing dependency" — it
# reads as 27 broken tests, which is why a gate is worth having.
#
# The second rule covers the mirror case. pytest-asyncio's default is strict
# mode, which SKIPS an unmarked async test rather than failing it, so the
# suite goes green having run nothing. That is the more dangerous half, and
# it only shows up once the plugin IS installed.
#
# The quiet half matters as much. This gate must stay silent on a repo with
# no async tests, on the legitimate alternatives to a per-test marker
# (module-level pytestmark, asyncio_mode = auto), and on async helpers that
# pytest would never collect — otherwise it fires on correct code and gets
# switched off.

GATE="scripts/check_pytest_asyncio_deps.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/py"
    mkdir -p "$ROOT/suite"
    # Baseline: requirements declares the plugin, and the one async case is marked.
    printf 'pytest>=8.0\npytest-asyncio>=0.23\n' > "$ROOT/requirements.txt"
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
import pytest

def test_sync_case():
    assert True

@pytest.mark.asyncio
async def test_async_case():
    assert True
EOF
}

run_gate() {
    run python3 "$GATE" --root "$ROOT" --test-dir suite
}

# ----------------------------------------------------------- the catch half

@test "flags async tests when requirements.txt does not declare pytest-asyncio" {
    printf 'pytest>=8.0\n' > "$ROOT/requirements.txt"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"pytest-asyncio"* ]]
    [[ "$output" == *"async def functions are not natively supported"* ]]
    [[ "$output" == *"test_async_case"* ]]
}

@test "an empty requirements.txt is a missing declaration, not a pass" {
    : > "$ROOT/requirements.txt"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"does not declare it"* ]]
}

@test "a missing requirements.txt is a missing declaration, not a pass" {
    rm -f "$ROOT/requirements.txt"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"does not declare it"* ]]
}

@test "flags an unmarked async test (strict mode would silently SKIP it)" {
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
async def test_unmarked():
    assert True
EOF
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"lack @pytest.mark.asyncio"* ]]
    [[ "$output" == *"SKIPS"* ]]
    [[ "$output" == *"test_unmarked"* ]]
}

@test "flags an unmarked async test inside a class, with the class in the name" {
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
class TestGroup:
    async def test_inside(self):
        assert True
EOF
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"TestGroup::test_inside"* ]]
}

@test "reports both rules at once when both are broken" {
    printf 'pytest>=8.0\n' > "$ROOT/requirements.txt"
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
async def test_unmarked():
    assert True
EOF
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"does not declare it"* ]]
    [[ "$output" == *"lack @pytest.mark.asyncio"* ]]
}

# ----------------------------------------------------------- the quiet half

@test "passes when the plugin is declared and every async case is marked" {
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "silent on a suite with no async tests at all" {
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
def test_sync_only():
    assert True
EOF
    printf 'pytest>=8.0\n' > "$ROOT/requirements.txt"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"no async tests"* ]]
}

@test "a module-level pytestmark satisfies the marker rule" {
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
import pytest

pytestmark = pytest.mark.asyncio

async def test_covered_by_module():
    assert True
EOF
    run_gate
    [ "$status" -eq 0 ]
}

@test "a pytestmark LIST containing the asyncio mark also satisfies the rule" {
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
import pytest

pytestmark = [pytest.mark.slow, pytest.mark.asyncio]

async def test_covered_by_module():
    assert True
EOF
    run_gate
    [ "$status" -eq 0 ]
}

@test "asyncio_mode = auto in pytest.ini drops the marker rule" {
    printf '[pytest]\nasyncio_mode = auto\n' > "$ROOT/pytest.ini"
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
async def test_unmarked_but_auto():
    assert True
EOF
    run_gate
    [ "$status" -eq 0 ]
}

@test "asyncio_mode = auto does NOT excuse the missing dependency" {
    # auto mode is a pytest-asyncio setting; without the plugin it does nothing.
    printf '[pytest]\nasyncio_mode = auto\n' > "$ROOT/pytest.ini"
    printf 'pytest>=8.0\n' > "$ROOT/requirements.txt"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"does not declare it"* ]]
}

@test "asyncio_mode = auto in pyproject.toml is honoured too" {
    printf '[tool.pytest.ini_options]\nasyncio_mode = "auto"\n' > "$ROOT/pyproject.toml"
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
async def test_unmarked_but_auto():
    assert True
EOF
    run_gate
    [ "$status" -eq 0 ]
}

@test "asyncio_mode = strict does NOT drop the marker rule" {
    printf '[pytest]\nasyncio_mode = strict\n' > "$ROOT/pytest.ini"
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
async def test_unmarked():
    assert True
EOF
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"lack @pytest.mark.asyncio"* ]]
}

@test "a parameterised @pytest.mark.asyncio(...) still counts as marked" {
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
import pytest

@pytest.mark.asyncio(loop_scope="session")
async def test_marked_with_args():
    assert True
EOF
    run_gate
    [ "$status" -eq 0 ]
}

@test "a version pin or extra on the requirements line still reads as declared" {
    printf 'pytest>=8.0\npytest-asyncio[docs]==0.24.0 ; python_version >= "3.9"\n' \
        > "$ROOT/requirements.txt"
    run_gate
    [ "$status" -eq 0 ]
}

@test "an async def that is not a test_ function is not collected as a case" {
    # An async fixture or helper is never collected by pytest, so demanding a
    # marker on it would be noise on every async suite.
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
import pytest

@pytest.fixture
async def some_async_fixture():
    return 1

async def helper_that_is_not_a_test():
    return 2
EOF
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"no async tests"* ]]
}

@test "a nested async def inside a test body is a helper, not a second case" {
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
import pytest

@pytest.mark.asyncio
async def test_outer():
    async def inner_helper():
        return 1
    assert await inner_helper() == 1
EOF
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"1 async test"* ]]
}

@test "a non-test_ .py file in the suite dir is not scanned" {
    cat > "$ROOT/suite/conftest.py" <<'EOF'
async def test_looks_like_one_but_is_in_conftest():
    assert True
EOF
    printf 'pytest>=8.0\n' > "$ROOT/requirements.txt"
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
def test_sync_only():
    assert True
EOF
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"no async tests"* ]]
}

@test "a test dir that does not exist is skipped rather than erroring" {
    run python3 "$GATE" --root "$ROOT" --test-dir does_not_exist
    [ "$status" -eq 0 ]
    [[ "$output" == *"no async tests"* ]]
}

# ----------------------------------------------------------- the listing mode

@test "--list names every async case and its marker state" {
    cat > "$ROOT/suite/test_thing.py" <<'EOF'
import pytest

@pytest.mark.asyncio
async def test_marked():
    assert True

async def test_bare():
    assert True
EOF
    run python3 "$GATE" --root "$ROOT" --test-dir suite --list
    [[ "$output" == *"test_marked (marked)"* ]]
    [[ "$output" == *"test_bare (UNMARKED)"* ]]
}

# ------------------------------------------------- the real tree (the ratchet)

@test "the repository's own Python suites satisfy the gate" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}
