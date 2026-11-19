#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_esp32_stub_coverage.py: a class the firmware stubs
# has its vtable in the image, and that vtable names every virtual the class has,
# so a virtual only an excluded file defines fails the ELF link with no firmware
# caller in sight. The fixture is a tiny repo: one interface-backed class, its
# excluded implementation, a listed source and a firmware stub file.

load helpers

GATE="scripts/check_esp32_stub_coverage.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    APP="$ROOT/fw/components/helixapp"
    mkdir -p "$ROOT/src/api" "$ROOT/src/ui" "$ROOT/include" "$APP"
    cat > "$ROOT/include/api.h" <<'EOF'
class IApi {
  public:
    virtual ~IApi() = default;
    virtual void fetch() = 0;
    virtual void currency() = 0;
};
class Api : public IApi {
  public:
    Api();
    void fetch() override;
    void currency() override;
    void helper();
    static Api& instance();
    int inline_size() const { return 1; }
};
EOF
    cat > "$ROOT/src/api/api.cpp" <<'EOF'
#include "api.h"
Api::Api() {}
void Api::fetch() {}
void Api::currency() {}
void Api::helper() {}
Api& Api::instance() { static Api a; return a; }
EOF
    printf '#include "api.h"\nvoid use(IApi& a) { a.currency(); }\n' > "$ROOT/src/ui/user.cpp"
    printf 'src/ui/user.cpp\n' > "$APP/app_srcs.txt"
    printf 'src/api/api.cpp  # stubbed\n' > "$APP/app_srcs_excluded.txt"
    : > "$APP/CMakeLists.txt"
}

stub() {
    printf '#include "api.h"\n%s\n' "$1" > "$APP/stubs.cpp"
}

run_gate() {
    run python3 "$GATE" --manifest "$APP/app_srcs.txt" \
        --exclusions "$APP/app_srcs_excluded.txt" \
        --src-root "$ROOT/src" --firmware-root "$ROOT/fw" "$@"
}

# ----------------------------------------------------------- the catch half

@test "flags a virtual the stub's vtable names and only the excluded file defines" {
    stub 'Api::Api() {}
void Api::fetch() {}'
    run_gate
    [ "$status" -eq 1 ]
    contains "src/api/api.cpp  Api::currency  (virtual; its vtable is emitted in $APP/stubs.cpp)" "$output"
    lacks "Api::helper" "$output"
}

@test "a stubbed constructor alone emits the vtable" {
    stub 'Api::Api() {}'
    run_gate
    [ "$status" -eq 1 ]
    contains "Api::fetch" "$output"
    contains "Api::currency" "$output"
}

@test "the key function in a listed source emits the vtable too" {
    printf 'src/ui/user.cpp\nsrc/ui/api_part.cpp\n' > "$APP/app_srcs.txt"
    printf '#include "api.h"\nvoid Api::fetch() {}\n' > "$ROOT/src/ui/api_part.cpp"
    stub ''
    run_gate
    [ "$status" -eq 1 ]
    contains "Api::currency  (virtual; its vtable is emitted in src/ui/api_part.cpp)" "$output"
}

@test "flags a constructor a firmware-tree source calls" {
    stub 'void boot() { auto a = std::make_unique<Api>(); }'
    run_gate
    [ "$status" -eq 1 ]
    contains "Api::Api  (constructed in" "$output"
}

# ----------------------------------------------------------- the quiet half

@test "passes once every virtual has a stub" {
    stub 'Api::Api() {}
void Api::fetch() {}
void Api::currency() {}'
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == OK:* ]]
}

@test "a stub of a non-virtual member emits no vtable" {
    stub 'void Api::helper() {}'
    run_gate
    [ "$status" -eq 0 ]
}

@test "a class the excluded file defines itself is local to it" {
    stub 'struct Local { Local(); }; void boot() { Local l; }'
    printf 'struct Local { Local(); };\nLocal::Local() {}\n' >> "$ROOT/src/api/api.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

@test "ESP32_STUB_OK on the definition waives it" {
    stub 'Api::Api() {}
void Api::fetch() {}'
    sed -i 's|^void Api::currency() {}|// ESP32_STUB_OK: never reached on the panel\nvoid Api::currency() {}|' \
        "$ROOT/src/api/api.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

@test "--if-touched skips when no staged path can change the answer" {
    stub 'Api::Api() {}'
    run_gate --if-touched src/ui/user.cpp docs/README.md
    [ "$status" -eq 0 ]
    contains "no staged header" "$output"
}

@test "--if-touched checks for a header, an excluded source or a firmware file" {
    stub 'Api::Api() {}'
    for p in include/api.h src/api/api.cpp "$ROOT/fw/components/helixapp/stubs.cpp"; do
        run_gate --if-touched "$p"
        [ "$status" -eq 1 ] || fail "$p skipped the check"
    done
}

# ----------------------------------------------------------- the wiring

@test "the commit trigger takes headers, sources and the firmware tree, not docs" {
    re="$(qc_trigger esp32_stub_coverage)"
    for p in include/api.h src/api/moonraker_spoolman_api.cpp \
             firmware/helixscreen-esp32/components/helixapp/task10_pending_stubs.cpp \
             scripts/check_esp32_stub_coverage.py; do
        printf '%s\n' "$p" | grep -qE "$re" || fail "$p does not trigger"
    done
    run grep -qE "$re" <<< "docs/devel/ESP32_PORT.md"
    [ "$status" -eq 1 ]
}

@test "at commit the gate hands the staged paths to --if-touched" {
    run bash -c '. scripts/qc/_lib.sh && . scripts/qc/esp32_stub_coverage.sh &&
        STAGED_ONLY=true QC_STAGED_ALL=$'"'"'docs/a.md\nsrc/ui/x.cpp'"'"' QC_TMP="$1" \
        qc_esp32_stub_coverage' _ "$BATS_TEST_TMPDIR"
    [ "$status" -eq 0 ]
    contains "no staged header" "$output"
}
