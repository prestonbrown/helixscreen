#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_lane_binding_reconcile.py: a backend that reads a
# firmware spool id must call reconcile_lane_binding() (prestonbrown/helixscreen#1645).

GATE="scripts/check_lane_binding_reconcile.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers
    FIXTURE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/lane_binding"
    mkdir -p "$FIXTURE/src/printer" "$FIXTURE/include"
}

backend_cpp() { printf '%s\n' "$2" > "$FIXTURE/src/printer/ams_backend_$1.cpp"; }
backend_h() { printf '%s\n' "$2" > "$FIXTURE/include/ams_backend_$1.h"; }

@test "flags a backend that parses a spool_id key with no reconcile call" {
    backend_cpp newfw '
void AmsBackendNewfw::parse(const json& lane) {
    slot.spoolman_id = json_util::safe_int(lane, "spool_id", 0);
}'
    run python3 "$GATE" --root "$FIXTURE"
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    [[ "$output" == *'ams_backend_newfw.cpp: reads "spool_id"'* ]] || fail "$output"
}

@test "flags a header that claims the printer reports spool ids" {
    backend_cpp newfw 'void AmsBackendNewfw::parse() {}'
    backend_h newfw '
class AmsBackendNewfw : public AmsBackend {
    static constexpr BackendTraits kTraits = [] {
        BackendTraits t;
        t.printer_reports_spool_ids = true;
        return t;
    }();
};'
    run python3 "$GATE" --root "$FIXTURE"
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    [[ "$output" == *"kTraits sets printer_reports_spool_ids"* ]] || fail "$output"
}

@test "passes a backend that reconciles beside the parse" {
    backend_cpp newfw '
void AmsBackendNewfw::parse(const json& lane) {
    const int id = json_util::safe_int(lane, "gate_spool_id", 0);
    reconcile_lane_binding(slot_index, id);
}'
    run python3 "$GATE" --root "$FIXTURE"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "ignores a spool id named only in a comment or a log line" {
    backend_cpp newfw '
// firmware has no "spool_id" field
void AmsBackendNewfw::parse() {
    spdlog::debug("no spool_id here");
}'
    run python3 "$GATE" --root "$FIXTURE"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "a key that only contains spool and id is not a spool id" {
    backend_cpp newfw '
void AmsBackendNewfw::parse(const json& lane) {
    width = json_util::safe_int(lane, "spool_width", 0);
    valid = json_util::safe_bool(lane, "spoolman_valid", false);
}'
    run python3 "$GATE" --root "$FIXTURE"
    [ "$status" -eq 0 ] || fail "$output"
}

@test "a camelCase spool id key is still a spool id" {
    backend_cpp newfw '
void AmsBackendNewfw::parse(const json& lane) {
    slot.spoolman_id = json_util::safe_int(lane, "spoolId", 0);
}'
    run python3 "$GATE" --root "$FIXTURE"
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
}

@test "a tree with no AMS backends is an error, not a pass" {
    run python3 "$GATE" --root "$FIXTURE"
    [ "$status" -eq 2 ] || fail "expected exit 2, got $status: $output"
    [[ "$output" == *"nothing was checked"* ]] || fail "$output"
}
