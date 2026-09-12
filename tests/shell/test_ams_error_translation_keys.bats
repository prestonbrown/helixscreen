#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every lv_tr() literal in include/ams_error.h must be a key in
# translations/en.yml (and so, by the same-key-set rule the duplicate-keys
# gate pins, in every locale). ams_error.h lives outside the trees
# `make translation-sync` scans (ui_xml/ + src/), so its keys reach the
# catalogues only through a manual --cpp-dir include pass — when one is
# missed nothing fails, and 8 of 9 locales show an English suggestion under
# a translated toast body. This guard kills that class for the file where
# it happened.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    PY=".venv/bin/python"
    [ -x "$PY" ] || skip "translations venv not set up (run 'make venv-setup')"
}

# Prints one line "count N", then one line per lv_tr() literal that is not
# an en.yml key. The extractor's exported LV_TR_RUN_RE joins adjacent
# string literals, so a key folded across two source lines still matches.
check_ams_error_keys() {
    "$PY" - <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, "scripts")
from translations.extractor import LV_TR_RUN_RE, _join_adjacent_literals
from translations.yaml_manager import load_yaml_file_readonly

content = Path("include/ams_error.h").read_text(encoding="utf-8")
literals = [_join_adjacent_literals(m) for m in LV_TR_RUN_RE.findall(content)]
keys = set(load_yaml_file_readonly(Path("translations/en.yml"))["translations"])
print(f"count {len(literals)}")
for lit in sorted(set(literals) - keys):
    print(lit)
PY
}

@test "every lv_tr() literal in ams_error.h is a translation key" {
    run check_ams_error_keys
    [ "$status" -eq 0 ]

    # The count floor proves the extraction ran: a broken regex or a moved
    # header would otherwise pass with zero literals examined.
    contains "count " "$output"
    lacks "count 0" "$output"

    local missing
    missing=$(printf '%s\n' "$output" | tail -n +2)
    [ -z "$missing" ] || {
        echo "missing en.yml keys:"
        echo "$missing"
        return 1
    }
}
