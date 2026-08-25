#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for KIAUH extension structural correctness
# Prevents GitHub issue #3: malformed metadata.json causing NoneType errors

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
KIAUH_DIR="$WORKTREE_ROOT/scripts/kiauh/helixscreen"

setup() {
    load helpers
}

# --- metadata.json structural tests ---

@test "metadata.json is valid JSON" {
    python3 -c "import json; json.load(open('$KIAUH_DIR/metadata.json'))"
}

@test "metadata.json has 'metadata' top-level key (issue #3 fix)" {
    python3 -c "
import json
data = json.load(open('$KIAUH_DIR/metadata.json'))
assert 'metadata' in data, f'Missing metadata key. Got keys: {list(data.keys())}'
"
}

@test "metadata.json has all required fields" {
    python3 -c "
import json
data = json.load(open('$KIAUH_DIR/metadata.json'))['metadata']
required = ['index', 'module', 'maintained_by', 'display_name', 'description', 'repo', 'updates']
missing = [f for f in required if f not in data]
assert not missing, f'Missing required fields: {missing}'
"
}

@test "metadata.json module field matches a .py filename" {
    local module
    module=$(python3 -c "
import json
data = json.load(open('$KIAUH_DIR/metadata.json'))['metadata']
print(data['module'])
")
    [ -f "$KIAUH_DIR/${module}.py" ]
}

@test "metadata.json index is an integer" {
    python3 -c "
import json
data = json.load(open('$KIAUH_DIR/metadata.json'))['metadata']
assert isinstance(data['index'], int), f'index should be int, got {type(data[\"index\"])}'
"
}

@test "metadata.json updates is a boolean" {
    python3 -c "
import json
data = json.load(open('$KIAUH_DIR/metadata.json'))['metadata']
assert isinstance(data['updates'], bool), f'updates should be bool, got {type(data[\"updates\"])}'
"
}

# --- Python file structural tests ---

@test "helixscreen_extension.py has valid Python syntax" {
    python3 -c "import ast; ast.parse(open('$KIAUH_DIR/helixscreen_extension.py').read())"
}

@test "helixscreen_extension.py defines HelixscreenExtension class" {
    grep -q 'class HelixscreenExtension' "$KIAUH_DIR/helixscreen_extension.py"
}

@test "helixscreen_extension.py inherits from BaseExtension" {
    grep -q 'BaseExtension' "$KIAUH_DIR/helixscreen_extension.py"
}

@test "helixscreen_extension.py implements install_extension" {
    grep -q 'def install_extension' "$KIAUH_DIR/helixscreen_extension.py"
}

@test "helixscreen_extension.py implements update_extension" {
    grep -q 'def update_extension' "$KIAUH_DIR/helixscreen_extension.py"
}

@test "helixscreen_extension.py implements remove_extension" {
    grep -q 'def remove_extension' "$KIAUH_DIR/helixscreen_extension.py"
}

# --- __init__.py structural tests ---

@test "__init__.py has valid Python syntax" {
    python3 -c "import ast; ast.parse(open('$KIAUH_DIR/__init__.py').read())"
}

@test "__init__.py uses future annotations (Python 3.9 compat, issue #28)" {
    grep -q 'from __future__ import annotations' "$KIAUH_DIR/__init__.py"
}

@test "helixscreen_extension.py uses future annotations (Python 3.9 compat)" {
    grep -q 'from __future__ import annotations' "$KIAUH_DIR/helixscreen_extension.py"
}

@test "__init__.py exports find_install_dir" {
    grep -q 'def find_install_dir' "$KIAUH_DIR/__init__.py"
}

@test "__init__.py exports HELIXSCREEN_REPO" {
    grep -q 'HELIXSCREEN_REPO' "$KIAUH_DIR/__init__.py"
}

# --- Import path tests (issue #30: dual-import identity mismatch) ---

@test "helixscreen_extension.py imports BaseExtension without kiauh. prefix" {
    # KIAUH uses 'from extensions.base_extension import BaseExtension'
    # Using 'from kiauh.extensions...' creates a dual-import identity mismatch
    # where issubclass() fails because Python sees two different classes
    ! grep -q 'from kiauh\.' "$KIAUH_DIR/helixscreen_extension.py"
}

@test "helixscreen_extension.py uses KIAUH-style imports" {
    # All KIAUH extensions use 'from extensions.*' and 'from core.*' paths
    grep -q 'from extensions.base_extension import BaseExtension' "$KIAUH_DIR/helixscreen_extension.py"
    grep -q 'from core.logger import Logger' "$KIAUH_DIR/helixscreen_extension.py"
}
