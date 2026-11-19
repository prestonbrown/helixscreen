#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_translation_files.py, the gate that keeps
# translations/*.yml in the shape Weblate writes (TRANSLATION_SYSTEM.md,
# "Weblate"). A file in another shape is reflowed wholesale by Weblate's next
# commit; a wrong locale: field misroutes translation-sync. The plural-collapse
# rule needs a git base and is covered in
# tests/python/test_check_translation_files.py.

load helpers

GATE="scripts/check_translation_files.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    PY=".venv/bin/python"
    [ -x "$PY" ] || skip "translations venv not set up (run 'make venv-setup')"
    DIR="$BATS_TEST_TMPDIR/translations"
    mkdir -p "$DIR"
}

run_gate() {
    run "$PY" "$GATE" --dir "$DIR" --base none "$@"
}

@test "flags a value folded across lines, naming the file and line" {
    printf '%s\n' 'locale: de' 'translations:' \
        "  Cancel: Abbrechen" \
        "  Connected: 'Stellen Sie sicher, dass der ADXL345 angeschlossen" \
        "    ist'" > "$DIR/de.yml"
    run_gate
    [ "$status" -eq 1 ]
    contains "de.yml:4" "$output"
}

@test "--fix rewrites the file into the shape the gate then accepts" {
    printf '%s\n' 'locale: de' 'translations:' \
        "  Connected: 'Stellen Sie sicher, dass der ADXL345 angeschlossen" \
        "    ist'" > "$DIR/de.yml"
    run_gate --fix
    [ "$status" -eq 0 ]
    run_gate
    [ "$status" -eq 0 ]
    grep -qx "  Connected: 'Stellen Sie sicher, dass der ADXL345 angeschlossen ist'" "$DIR/de.yml"
}

@test "flags a locale field that does not match the file name" {
    printf '%s\n' 'locale: en' 'translations:' '  Cancel: Abbrechen' > "$DIR/de.yml"
    run_gate
    [ "$status" -eq 1 ]
    contains "locale is 'en'" "$output"
}

@test "accepts canonical files, blanks, comments and plural mappings" {
    printf '%s\n' '# British English.' 'locale: en-GB' 'translations:' \
        "  Cancel: ''" '  Colour: Colour' > "$DIR/en-GB.yml"
    printf '%s\n' 'locale: ru' 'translations:' "  '%d gates':" \
        "    one: '%d шлюз'" "    few: '%d шлюза'" "    other: '%d шлюзов'" > "$DIR/ru.yml"
    run_gate
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "quality-checks runs it when a locale file or the writer changes" {
    grep -q 'QC_ALL=.*qc_translation_files' scripts/quality-checks.sh
    trigger="$(qc_trigger translation_files)"
    echo translations/de.yml | grep -qE "$trigger"
    echo scripts/translations/yaml_manager.py | grep -qE "$trigger"
    [[ ! src/main.cpp =~ $trigger ]]
}

@test "--list-plural-keys names each key some locale writes as a mapping, once" {
    printf '%s\n' 'locale: ru' 'translations:' "  '%d gates':" \
        "    one: '%d шлюз'" "    other: '%d шлюзов'" "  Cancel: Отмена" > "$DIR/ru.yml"
    printf '%s\n' 'locale: fr' 'translations:' "  '%d gates':" \
        "    one: '%d Gate'" "    other: '%d Gates'" "  Cancel: Annuler" > "$DIR/fr.yml"
    run "$PY" "$GATE" --dir "$DIR" --list-plural-keys
    [ "$status" -eq 0 ]
    [ "$output" = "%d gates" ]
}
