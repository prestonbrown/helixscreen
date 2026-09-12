# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for merge_compile_commands.entry_argv - the one tokenizer every
compile-command replay path uses (the clang divergence gate, syntax_check.py).

A fragment is rewritten only when its object is, and make rebuilds an object
when a prerequisite is newer - not when the command line changes. So the
database on disk mixes entries recorded by builds with different quoting
conventions for quoted defines, all of them stamped with the current version
and therefore trusted. The tokenizer must hand the compiler the same argv for
both shapes, or the replay manufactures diagnostics about the recording, not
about the code.
"""

import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from merge_compile_commands import entry_argv  # noqa: E402

# The shell-quoted token emit-compile-command writes today, the bare
# double-quoted token older fragments carry, and the argv both must produce.
SHELL_QUOTED = (
    "g++ -std=c++17 -DHELIX_VERSION='\"1.1.0-beta.1\"' -DHELIX_BUILD_TYPE='\"dev\"' "
    "-c src/system/telemetry_manager.cpp -o build/obj/system/telemetry_manager.o"
)
DOUBLE_QUOTED = (
    "g++ -std=c++17 -DHELIX_VERSION=\"1.1.0-beta.1\" -DHELIX_BUILD_TYPE=\"dev\" "
    "-c src/system/telemetry_manager.cpp -o build/obj/system/telemetry_manager.o"
)
EXPECTED_DEFINE = "-DHELIX_VERSION=\"1.1.0-beta.1\""


def _defining_entry(command: str) -> dict:
    return {
        "directory": str(REPO_ROOT),
        "file": os.path.join(REPO_ROOT, "src/system/telemetry_manager.cpp"),
        "command": command,
    }


def test_shell_quoted_define_keeps_its_quotes():
    argv = entry_argv(_defining_entry(SHELL_QUOTED))
    assert EXPECTED_DEFINE in argv


def test_double_quoted_define_keeps_its_quotes():
    # A POSIX split would strip the double quotes here and leave the macro a
    # bare 1.1.0-beta.1, which no compiler can parse as a string.
    argv = entry_argv(_defining_entry(DOUBLE_QUOTED))
    assert EXPECTED_DEFINE in argv


def test_both_recording_shapes_yield_the_same_argv():
    assert entry_argv(_defining_entry(SHELL_QUOTED)) == entry_argv(
        _defining_entry(DOUBLE_QUOTED)
    )


def test_arguments_array_passes_through_untouched():
    argv = ["clang++", "-DHELIX_VERSION=\"1.1.0-beta.1\"", "-c", "a.cpp"]
    assert entry_argv({"arguments": argv}) == argv


def test_token_quoted_end_to_end_is_unwrapped():
    # Genuine shell quoting of a whole argument (a Bear/cmake-style database,
    # e.g. a path with spaces) is one layer and comes off.
    entry = {
        "directory": str(REPO_ROOT),
        "file": "a b.cpp",
        "command": "g++ -std=c++17 -c 'a b.cpp' -o 'a b.o'",
    }
    argv = entry_argv(entry)
    assert "a b.cpp" in argv
    assert "a b.o" in argv


def test_unquoted_command_splits_on_whitespace():
    entry = {"directory": str(REPO_ROOT), "file": "a.cpp", "command": "g++ -c a.cpp"}
    assert entry_argv(entry) == ["g++", "-c", "a.cpp"]
