#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_gcode_error_ownership.py - the gcode error
# report-ownership gate.
#
# IMoonrakerAPI::execute_gcode's trailing caller_surfaces_errors means "my
# on_error actually SHOWS a human something". Claiming it falsely makes
# MoonrakerRequestTracker record the rejection for cross-channel dedup, and
# GcodeErrorRouter then suppresses its own report of Klipper's `!!` broadcast -
# so a rejected macro is reported by NOBODY. That is invisible in review: the
# call site looks fully handled, because there IS an error callback. It just
# writes to a log no user reads.
#
# What the gate demands is honesty about the callback, not a particular value:
# caller_surfaces_errors=true on a callback that toasts passes exactly as well
# as =false on one that logs. Only an unexamined log-only claim fails.
#
# These tests pin BOTH halves. The silent cases matter as much as the loud ones
# - "callback only logs" is a heuristic, and a gate that fires on a legitimate
# UI-surfacing callback is a gate somebody switches off, after which it protects
# nothing at all.

GATE="scripts/check_gcode_error_ownership.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/gcode_ownership"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into $FIXTURE_DIR/$1.cpp and run the gate over that one file.
run_gate() {
    local name="$1" body="$2"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$name.cpp"
    run python3 "$GATE" "$FIXTURE_DIR/$name.cpp" --list
    printf '%s\n' "$output" > "$FIXTURE_DIR/out.txt"
}

flagged() {
    grep -q "claims the report" "$FIXTURE_DIR/out.txt" \
        || fail "expected the gate to flag this site, got: $(cat "$FIXTURE_DIR/out.txt")"
}

quiet() {
    refute_grep "claims the report" "$FIXTURE_DIR/out.txt"
}

# ------------------------------------------------ shapes that must be CAUGHT

@test "flags an error callback that only calls spdlog" {
    run_gate logs_only '
void Thing::go() {
    api_->execute_gcode(
        "AFC_UNLOAD LANE=1", []() {},
        [](const MoonrakerError& err) {
            spdlog::error("[Thing] unload failed: {}", err.message);
        },
        IMoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
}'
    flagged
}

@test "flags a completely empty error callback" {
    run_gate empty_cb '
void Thing::go() {
    api_->execute_gcode("BOX_RESTORE_FAN", []() {}, [](const MoonrakerError&) {}, 5000);
}'
    flagged
}

@test "flags a log-only callback that also resets internal state" {
    # Bookkeeping is not a user-visible report: clearing a spinner flag tells
    # nobody WHY the operation stopped.
    run_gate logs_and_state '
void Thing::go() {
    api_->execute_gcode(
        "ACE_CHANGE_TOOL TOOL=-1", []() {},
        [this](const MoonrakerError& err) {
            spdlog::error("[ACE] unload failed: {}", err.message);
            system_info_.action = AmsAction::IDLE;
        },
        5000);
}'
    flagged
}

# ------------------------------------------------ shapes that must stay QUIET

@test "silent about a callback that logs AND raises a notification" {
    # The log line is deliberate: a callback with no spdlog at all is classified
    # as surfacing by falling through the log-only check, which would let this
    # pass without ever exercising the notification-marker path.
    run_gate notifies '
void Thing::go() {
    api_->execute_gcode(
        "LOAD_FILAMENT", []() {},
        [](const MoonrakerError& err) {
            spdlog::error("[Thing] load failed: {}", err.message);
            NOTIFY_ERROR(lv_tr("Failed to load filament: {}"), err.user_message());
        },
        5000);
}'
    quiet
}

@test "silent about a callback that shows a modal" {
    run_gate modal '
void Thing::go() {
    api_->execute_gcode(
        "CALIBRATE", []() {},
        [](const MoonrakerError& err) {
            spdlog::warn("calibrate failed: {}", err.message);
            ui_notification_error("Calibration", err.user_message().c_str(), true);
        },
        5000);
}'
    quiet
}

@test "silent once caller_surfaces_errors is stated explicitly as false" {
    run_gate explicit_false '
void Thing::go() {
    api_->execute_gcode(
        "AFC_UNLOAD LANE=1", []() {},
        [](const MoonrakerError& err) {
            spdlog::error("[Thing] unload failed: {}", err.message);
        },
        5000, /*silent=*/false, /*on_queued=*/nullptr, /*caller_surfaces_errors=*/false);
}'
    quiet
}

@test "silent once caller_surfaces_errors is stated explicitly as true" {
    # The gate polices unexamined claims, not a particular answer. An author who
    # wrote the parameter has made the decision consciously.
    run_gate explicit_true '
void Thing::go() {
    api_->execute_gcode(
        "AFC_UNLOAD LANE=1", []() {},
        [](const MoonrakerError& err) {
            spdlog::error("[Thing] unload failed: {}", err.message);
        },
        5000, /*silent=*/false, /*on_queued=*/nullptr, /*caller_surfaces_errors=*/true);
}'
    quiet
}

@test "silent about a null error callback" {
    # Nothing to claim: intent derives to false, so the router stays free.
    run_gate null_cb '
void Thing::go() {
    api_->execute_gcode("M107", nullptr, nullptr, 5000);
}'
    quiet
}

@test "silent about a plain JSON-RPC send" {
    # No `!!` broadcast behind server.files.list, so the generic fallback is the
    # only surface and this rule does not apply.
    run_gate plain_rpc '
void Thing::go() {
    client_.send_jsonrpc(
        "server.files.list", params, [](const json&) {},
        [](const MoonrakerError& err) { spdlog::warn("list failed: {}", err.message); });
}'
    quiet
}

@test "honours the ERROR_OWNERSHIP_OK escape hatch" {
    run_gate annotated '
void Thing::go() {
    // ERROR_OWNERSHIP_OK: best-effort unwind; a failure here is expected by
    // design and must not be surfaced to the user at all.
    api_->execute_gcode(
        "RESTORE_GCODE_STATE NAME=x", []() {},
        [](const MoonrakerError&) {}, 5000);
}'
    quiet
}

@test "an escaped string earlier in the file does not shift the scanner" {
    # Blanking must be length-preserving: offsets found in the blanked copy are
    # used to slice the ORIGINAL source. Emitting one space for a two-character
    # escape shifted every later offset, so the scanner read a span starting
    # ~20 chars early and missed a caller_surfaces_errors that was plainly
    # there -- reporting a correctly-annotated site as a violation.
    run_gate escaped_string_before '
void Thing::first() {
    spdlog::debug("led target \"{}\" with a \\ backslash and a \n newline", name);
}

void Thing::second() {
    api_->execute_gcode(
        "RESTORE_GCODE_STATE NAME=x", []() {},
        [](const MoonrakerError&) {}, 5000,
        /*silent=*/false, /*on_queued=*/nullptr, /*caller_surfaces_errors=*/false);
}'
    quiet
}

@test "skips mock implementations" {
    run_gate some_client_mock '
void Mock::go() {
    api_.execute_gcode(
        "BED_MESH_CALIBRATE", []() {},
        [](const MoonrakerError& err) {
            spdlog::error("[Mock] regen failed: {}", err.message);
        },
        5000);
}'
    quiet
}

# ------------------------------------------------------------------ ratchet

@test "--max-allowed fails when the count exceeds the baseline" {
    printf '%s\n' '
void Thing::go() {
    api_->execute_gcode(
        "AFC_UNLOAD LANE=1", []() {},
        [](const MoonrakerError& err) { spdlog::error("failed: {}", err.message); }, 5000);
}' > "$FIXTURE_DIR/ratchet.cpp"

    run python3 "$GATE" "$FIXTURE_DIR/ratchet.cpp" --max-allowed 0
    [ "$status" -eq 1 ] || fail "ratchet must fail when exceeded, got status $status"

    run python3 "$GATE" "$FIXTURE_DIR/ratchet.cpp" --max-allowed 1
    [ "$status" -eq 0 ] || fail "ratchet must pass at the baseline, got status $status"
}

@test "the real tree stays at or under its recorded baseline" {
    # Guards the gate against its own bugs as much as the tree: if a refactor
    # makes the scanner stop seeing call sites, this notices the count moving.
    run python3 "$GATE" --summary
    [ "$status" -eq 0 ] || fail "gate errored on the real tree: $output"
    printf '%s\n' "$output" | grep -qE "gcode error-ownership: [0-9]+ site" \
        || fail "unexpected summary format: $output"
}
