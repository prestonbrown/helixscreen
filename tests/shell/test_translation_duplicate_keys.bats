#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Duplicate keys in translations/*.yml.
#
# A locale file that defines a key twice is not a style problem. pyyaml.safe_load
# keeps the LAST value, so an empty placeholder sitting after a translated entry
# does not error — it DELETES the translation on the next write. That happened:
# a branch cut before the locales were filled ran translation-sync, which
# correctly added empty placeholders for keys that did not exist on it yet;
# merging brought main's translated entries in beside them, and because YAML
# merges textually both lines survived. "Print cancelled" and "Print did not
# start" shipped as untranslated English in all eight non-English locales, and
# the only symptom was a ruamel traceback at the bottom of one bats failure.

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    PY=".venv/bin/python"
    [ -x "$PY" ] || skip "translations venv not set up (run 'make venv-setup')"
    FIX="$BATS_TEST_TMPDIR"
}

# A locale file shaped like the real ones, with the duplicate this gate exists for.
write_dup_fixture() {
    cat > "$FIX/dup.yml" <<'YAML'
locale: ja
translations:
  Alpha: あるふぁ
  Print cancelled: 印刷がキャンセルされました
  Print cancelled: ''
  Zulu: ずーるー
YAML
}

write_clean_fixture() {
    cat > "$FIX/clean.yml" <<'YAML'
locale: ja
translations:
  Alpha: あるふぁ
  Print cancelled: 印刷がキャンセルされました
  Zulu: ずーるー
YAML
}

@test "a duplicate key is rejected, naming the key and the line" {
    write_dup_fixture
    run $PY -c "
import sys, pathlib
sys.path.insert(0, 'scripts')
from translations.yaml_manager import load_yaml_file, DuplicateTranslationKey
try:
    load_yaml_file(pathlib.Path('$FIX/dup.yml'))
    print('LOADED')
except DuplicateTranslationKey as e:
    print('REJECTED', e)
"
    [ "$status" -eq 0 ]
    [[ "$output" == REJECTED* ]]
    [[ "$output" == *"Print cancelled"* ]]
    # The line number is what makes the message actionable in a 2800-key file.
    [[ "$output" == *"line 5"* ]]
}

@test "the ruamel-less fallback rejects it too, instead of silently last-winning" {
    # The fallback is the DANGEROUS path: pyyaml.safe_load keeps the last value,
    # so without this the empty placeholder wins and the translation is dropped
    # with no error at all. The assert on RUAMEL_AVAILABLE is load-bearing — an
    # import blocker written against the removed find_module() API silently does
    # nothing on 3.12, and this test then re-runs the ruamel path and passes
    # while proving nothing.
    write_dup_fixture
    run $PY -c "
import sys, pathlib
class Block:
    def find_spec(self, name, path=None, target=None):
        if name.split('.')[0] == 'ruamel':
            raise ImportError('blocked for test')
        return None
sys.meta_path.insert(0, Block())
sys.path.insert(0, 'scripts')
from translations import yaml_manager as ym
assert ym.RUAMEL_AVAILABLE is False, 'blocker failed; still on the ruamel path'
try:
    ym.load_yaml_file(pathlib.Path('$FIX/dup.yml'))
    print('LOADED')
except ym.DuplicateTranslationKey as e:
    print('REJECTED', e)
"
    [ "$status" -eq 0 ]
    [[ "$output" == REJECTED* ]]
    [[ "$output" == *"Print cancelled"* ]]
}

@test "a clean file still loads on both paths" {
    write_clean_fixture
    for blocked in 0 1; do
        run $PY -c "
import sys, pathlib
if '$blocked' == '1':
    class Block:
        def find_spec(self, name, path=None, target=None):
            if name.split('.')[0] == 'ruamel':
                raise ImportError('blocked for test')
            return None
    sys.meta_path.insert(0, Block())
sys.path.insert(0, 'scripts')
from translations import yaml_manager as ym
d = ym.load_yaml_file(pathlib.Path('$FIX/clean.yml'))
print(len(d['translations']), d['translations']['Print cancelled'])
"
        [ "$status" -eq 0 ]
        [[ "$output" == "3 印刷がキャンセルされました" ]]
    done
}

@test "no locale file in the tree defines a key twice" {
    run $PY -c "
import sys, pathlib
sys.path.insert(0, 'scripts')
from translations.yaml_manager import load_yaml_file, DuplicateTranslationKey
bad = []
for f in sorted(pathlib.Path('translations').glob('*.yml')):
    try:
        load_yaml_file(f)
    except DuplicateTranslationKey as e:
        bad.append(str(e))
print('\n'.join(bad) if bad else 'CLEAN')
"
    [ "$status" -eq 0 ]
    [ "$output" = "CLEAN" ]
}

@test "every locale carries the same key set" {
    # A duplicate is one way a locale silently loses a key; a bare count mismatch
    # is the general form, and it is cheap to check while the files are open.
    run $PY -c "
import sys, pathlib
sys.path.insert(0, 'scripts')
from translations.yaml_manager import load_yaml_file
sizes = {}
for f in sorted(pathlib.Path('translations').glob('*.yml')):
    sizes[f.name] = len(load_yaml_file(f)['translations'])
print('MISMATCH' if len(set(sizes.values())) != 1 else 'UNIFORM', sizes)
"
    [ "$status" -eq 0 ]
    [[ "$output" == UNIFORM* ]]
}
