#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_wifi_pii_logging.py — the network-PII log gate.
#
# The gate flags SSIDs, BSSIDs and MACs logged at debug level or above, because
# the log ring is captured at debug regardless of the user's verbosity and is
# uploaded by the debug bundle, the crash reporter, and the `ctl log` RPC.
#
# What these pin, beyond the obvious shapes, is that it reads CODE, not prose.
# The L081 gate shipped matching inside comments and string literals, which
# made the accurate comment the thing you had to delete — so the next person
# wrote a vaguer one. This gate has a worse version of that trap: every one of
# these log calls has the word "ssid" sitting in its format string, so a naive
# line-grep flags a correctly-redacted call. See tests/shell/test_l081_gate.bats.

GATE="scripts/check_wifi_pii_logging.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/wifi_pii_gate"
    mkdir -p "$FIXTURE_DIR"
}

run_gate() {
    printf '%s\n' "$1" > "$FIXTURE_DIR/case.cpp"
    run python3 "$GATE" "$FIXTURE_DIR/case.cpp"
}

# --- shapes that must fire ---------------------------------------------------

@test "flags a raw SSID at debug" {
    run_gate 'void f() {
    spdlog::debug("[WifiBackend] Status: ssid=@@", status.ssid);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"SSID"* ]]
}

@test "flags a raw SSID at info" {
    run_gate 'void f() {
    spdlog::info("[WiFiManager] Connecting to @@", ssid);
}'
    [ "$status" -eq 1 ]
}

@test "flags a raw SSID at warn and error" {
    run_gate 'void f() {
    spdlog::warn("[NM] Connection to @@ timed out", network.ssid);
}'
    [ "$status" -eq 1 ]

    run_gate 'void f() {
    spdlog::error("[NM] no such network: @@", net->ssid);
}'
    [ "$status" -eq 1 ]
}

@test "flags a BSSID" {
    run_gate 'void f() {
    spdlog::debug("[WifiBackend] AP @@", entry.bssid);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"SSID"* ]]
}

@test "flags a MAC address" {
    run_gate 'void f() {
    spdlog::debug("[wifi_ui] MAC for @@: @@", iface, mac_address);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"MAC"* ]]
}

@test "flags get_connected_ssid() and get_mac_address() accessor calls" {
    run_gate 'void f() {
    spdlog::info("[NetworkWidget] Detected WiFi (@@)", wifi_manager_->get_connected_ssid());
}'
    [ "$status" -eq 1 ]

    run_gate 'void f() {
    spdlog::debug("[Wizard] MAC @@", backend_->get_mac_address());
}'
    [ "$status" -eq 1 ]
}

@test "flags the LOG_ERROR_INTERNAL wrapper" {
    run_gate 'void f() {
    LOG_ERROR_INTERNAL("Failed to create item for SSID: @@", network.ssid);
}'
    [ "$status" -eq 1 ]
}

@test "flags a raw SSID in a NOTIFY_ERROR toast (also reaches spdlog::error)" {
    # NOTIFY_ERROR/_T/_MODAL and NOTIFY_WARNING/INFO/SUCCESS all expand to a
    # spdlog call under the hood (see core/error_reporting.h), so an unredacted
    # SSID passed to any of them leaks the same way a bare spdlog::error()
    # call would.
    run_gate 'void f() {
    spdlog::trace("init");
    NOTIFY_ERROR("Failed to connect to @@", ssid);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"SSID"* ]]
}

@test "flags every NOTIFY_* variant that carries a raw SSID" {
    for macro in NOTIFY_ERROR NOTIFY_WARNING NOTIFY_INFO NOTIFY_SUCCESS NOTIFY_ERROR_T \
                 NOTIFY_WARNING_T NOTIFY_INFO_T NOTIFY_SUCCESS_T NOTIFY_ERROR_MODAL; do
        run_gate "void f() {
    spdlog::trace(\"init\");
    ${macro}(\"title\", \"Failed: @@\", ssid);
}"
        [ "$status" -eq 1 ]
    done
}

@test "allows a redacted SSID in NOTIFY_ERROR" {
    run_gate 'void f() {
    spdlog::trace("init");
    NOTIFY_ERROR("Failed to connect to @@", helix::redact::ssid(ssid));
}'
    [ "$status" -eq 0 ]
}

@test "flags a call whose arguments wrap onto later lines" {
    # wifi_backend_wpa_supplicant.cpp:1304 is a two-line call; a per-line
    # scanner misses the argument entirely.
    run_gate 'void f() {
    spdlog::debug("[WifiBackend] Status: connected=@@ ssid=@@ ip=@@ signal=@@",
                  status.connected, status.ssid, status.ip_address, status.signal);
}'
    [ "$status" -eq 1 ]
}

# --- shapes that must NOT fire -----------------------------------------------

@test "allows a raw SSID at trace" {
    # The ring is pinned to debug even when the logger is at trace, so trace
    # never leaves the machine. This is the escape hatch the fix relies on.
    run_gate 'void f() {
    spdlog::trace("[WifiBackend] Parsed network: @@ @@", ssid, bssid);
}'
    [ "$status" -eq 0 ]
}

@test "allows a redacted SSID at debug" {
    run_gate 'void f() {
    spdlog::debug("[WifiBackend] Status: ssid=@@", helix::redact::ssid(status.ssid));
}'
    [ "$status" -eq 0 ]
}

@test "allows a redacted SSID via the short namespace form" {
    run_gate 'void f() {
    spdlog::info("[WiFiManager] Connecting to @@", redact::ssid(target_ssid));
}'
    [ "$status" -eq 0 ]
}

@test "allows a redacted MAC" {
    run_gate 'void f() {
    spdlog::debug("[wifi_ui] MAC for @@: @@", iface, helix::redact::mac(mac_address));
}'
    [ "$status" -eq 0 ]
}

@test "does not fire on the word ssid inside the format string alone" {
    # THE central false positive: every one of these calls mentions "ssid" in
    # its format string. If the gate matched literals, a correctly-redacted
    # call would still fail and the fix would look impossible.
    run_gate 'void f() {
    spdlog::debug("[WifiBackend] ssid and bssid and mac_address are redacted below");
}'
    [ "$status" -eq 0 ]
}

@test "does not fire on a comment describing the anti-pattern" {
    run_gate 'void f() {
    // Never log status.ssid or entry.bssid here — see check_wifi_pii_logging.py
    /* mac_address must not reach spdlog::debug either. */
    spdlog::debug("[WifiBackend] status refreshed");
}'
    [ "$status" -eq 0 ]
}

@test "does not fire on a comment INSIDE the log call arguments" {
    # The comment sits within the paren span the gate actually scans, so this
    # is the case that genuinely exercises comment stripping. A comment
    # explaining what was removed is exactly what a good fix leaves behind.
    run_gate 'void f() {
    spdlog::debug("[WifiBackend] refreshed: @@",  // was status.ssid before redaction
                  count);
    spdlog::debug("[WifiBackend] refreshed: @@",
                  /* not entry.bssid / mac_address any more */ count);
}'
    [ "$status" -eq 0 ]
}

@test "does not fire on a commented-out violation" {
    run_gate 'void f() {
    // spdlog::debug("[WifiBackend] Status: ssid=@@", status.ssid);
    spdlog::debug("[WifiBackend] status refreshed");
}'
    [ "$status" -eq 0 ]
}

@test "does not fire on a raw string literal mentioning ssid" {
    run_gate 'void f() {
    static const std::regex re(R"(ssid=([^\s]+))");
    spdlog::debug("[WifiBackend] parsed");
}'
    [ "$status" -eq 0 ]
}

@test "does not fire on a raw string literal PASSED to a log call" {
    # Raw strings are how the wpa parsers spell their patterns; one landing in
    # a log call must not read as an SSID argument.
    run_gate 'void f() {
    spdlog::debug("[WifiBackend] pattern @@", R"(ssid=([^ ]+)\tbssid=(.*))");
}'
    [ "$status" -eq 0 ]
}

@test "does not fire on non-log code touching an SSID" {
    run_gate 'void f() {
    std::string s = status.ssid;
    net.bssid = parse_bssid(line);
    connect(s, mac_address);
}'
    [ "$status" -eq 0 ]
}

@test "honors a // PII_OK opt-out" {
    run_gate 'void f() {
    spdlog::debug("[Mock] Connected to @@", ssid); // PII_OK: mock backend, fixtures only
}'
    [ "$status" -eq 0 ]
}

@test "honors // PII_OK on any line of a wrapped call" {
    run_gate 'void f() {
    spdlog::debug("[Mock] Status: @@ @@",
                  ssid,            // PII_OK: mock backend, fixtures only
                  signal);
}'
    [ "$status" -eq 0 ]
}

@test "clean wifi code passes" {
    run_gate 'void WifiBackend::refresh() {
    auto status = query_status();
    spdlog::debug("[WifiBackend] Status: connected=@@ ssid=@@ signal=@@%",
                  status.connected, helix::redact::ssid(status.ssid), status.signal);
    spdlog::trace("[WifiBackend] raw ssid=@@ bssid=@@", status.ssid, status.bssid);
}'
    [ "$status" -eq 0 ]
}

# --- plumbing ----------------------------------------------------------------

@test "gate is executable and has a usage message" {
    run python3 "$GATE" --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"--staged-only"* ]]
}

@test "gate reports file and line number" {
    run_gate 'void f() {
    spdlog::debug("[WifiBackend] ssid=@@", status.ssid);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"case.cpp:2"* ]]
}

# --- identifier-shape blind spots (regression: the gate shipped missing these) --

@test "flags an SSID buried in a member variable name" {
    # `\bssid\b` does not match here: '_' is a word character, so there is no
    # word boundary. Two live sites in wifi_backend_macos.mm passed the gate
    # this way.
    run_gate 'void f() {
    spdlog::error("[WiFiMacOS] Network not found: @@", connecting_ssid_);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"SSID"* ]]
}

@test "flags SSID-bearing locals and parameters of every shape" {
    for ident in clean_ssid wifi_ssid current_ssid_ target_ssid ssid_to_join; do
        run_gate "void f() {
    spdlog::info(\"[X] connecting @@\", $ident);
}"
        [ "$status" -eq 1 ] || {
            echo "missed identifier: $ident"
            return 1
        }
    done
}

@test "flags a MAC buried in a member or prefixed name" {
    for ident in bt_mac real_mac_ device_mac mac_addr; do
        run_gate "void f() {
    spdlog::debug(\"[X] addr @@\", $ident);
}"
        [ "$status" -eq 1 ] || {
            echo "missed identifier: $ident"
            return 1
        }
    done
}

@test "does not fire on identifiers that merely contain the letters mac" {
    # The reason 'mac' cannot be matched as a loose substring the way 'ssid' is.
    for ident in macro_name machine_id format_macro mac_os_version macros_; do
        run_gate "void f() {
    spdlog::debug(\"[X] value @@\", $ident);
}"
        [ "$status" -eq 0 ] || {
            echo "false positive on: $ident"
            return 1
        }
    done
}

# --- false-positive shapes found by the first real sweep ----------------------

@test "does not fire on an ssid-adjacent identifier holding a command result" {
    # `ssid_result` is the wpa_cli reply ("OK\n" / "FAIL"), not a network name.
    # The loose "ssid" substring match flags it without this carve-out.
    run_gate 'void f() {
    std::string ssid_result = send_command(set_ssid_cmd);
    LOG_ERROR_INTERNAL("Failed to set SSID: @@", ssid_result);
}'
    [ "$status" -eq 0 ]
}

@test "does not fire on other ssid-prefixed non-value identifiers" {
    for ident in ssid_len ssid_cmd ssid_status ssid_reply ssid_count; do
        run_gate "void f() {
    spdlog::debug(\"[X] @@\", $ident);
}"
        [ "$status" -eq 0 ] || {
            echo "false positive on: $ident"
            return 1
        }
    done
}

@test "still fires on ssid-prefixed identifiers that DO carry a value" {
    # The carve-out above is a suffix denylist, not a blanket 'ssid_*' pass.
    for ident in ssid_to_join ssid_from_config; do
        run_gate "void f() {
    spdlog::info(\"[X] joining @@\", $ident);
}"
        [ "$status" -eq 1 ] || {
            echo "missed identifier: $ident"
            return 1
        }
    done
}

@test "does not fire on length or emptiness checks of a redacted value" {
    # Both shapes are live: the value is redacted on the same line while the
    # raw variable is still consulted for its size. Neither discloses anything.
    run_gate 'void f() {
    spdlog::debug("[Wizard] WiFi MAC: @@ (len=@@)", helix::redact::mac(mac), mac.size());
    spdlog::info("[Scanner] Selected @@", bt_mac.empty() ? "" : helix::redact::mac(bt_mac));
}'
    [ "$status" -eq 0 ]
}

@test "still fires when the raw value is logged beside a length check" {
    # Guards the carve-out above: stripping `.size()` must not blind the gate to
    # the raw value sitting next to it.
    run_gate 'void f() {
    spdlog::debug("[Wizard] WiFi MAC: @@ (len=@@)", mac, mac.size());
}'
    [ "$status" -eq 1 ]
}

# ------------------------------------------------------- --staged-only content
#
# --staged-only picks its file SET from `git diff --cached`, but must read
# each file's STAGED content (the index blob), not the working tree. Stage a
# violation and then revert the working file to something clean, and a gate
# that reads the working tree reports nothing while the bad blob commits.

GATE_ABS="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/check_wifi_pii_logging.py"

setup_tmp_repo() {
    TMP_REPO="$(mktemp -d "${BATS_TEST_TMPDIR:-${BATS_TMPDIR:-/tmp}}/wifi-pii-XXXXXX")"
    cd "$TMP_REPO" || return 1
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test"
    mkdir -p src/network
    printf 'void f() { spdlog::info("status: {}", 1); }\n' > src/network/foo.cpp
    git add -A && git commit -qm base
}

@test "--staged-only catches a violation in the STAGED blob, not a reverted working file" {
    setup_tmp_repo
    printf 'void f() { spdlog::info("ssid: {}", wifi_ssid); }\n' > src/network/foo.cpp
    git add src/network/foo.cpp
    printf 'void f() { spdlog::info("status: {}", 1); }\n' > src/network/foo.cpp
    run python3 "$GATE_ABS" --staged-only
    [ "$status" -eq 1 ]
    contains "SSID logged" "$output"
}

@test "--staged-only stays silent on a clean staged file with a dirty violation on disk" {
    setup_tmp_repo
    # Genuinely different from the base commit (a second clean log call), so
    # the file shows up as staged at all - content byte-identical to HEAD
    # stages nothing for git to report, which would pass this test for free.
    printf 'void f() { spdlog::info("status: {}", 1); spdlog::info("status2: {}", 2); }\n' \
        > src/network/foo.cpp
    git add src/network/foo.cpp
    printf 'void f() { spdlog::info("ssid: {}", wifi_ssid); }\n' > src/network/foo.cpp
    run python3 "$GATE_ABS" --staged-only
    [ "$status" -eq 0 ]
}

@test "--staged-only still catches a violation staged then deleted from disk" {
    # `rm` without staging the deletion leaves the index (and so the commit)
    # holding the violating content, with `git status` showing MD. A gate
    # that drops staged paths on Path.is_file() sees no file at all and
    # reports clean, even though `git show :path` proves the commit will
    # carry exactly the content it should have flagged.
    setup_tmp_repo
    printf 'void f() { spdlog::info("ssid: {}", wifi_ssid); }\n' > src/network/foo.cpp
    git add src/network/foo.cpp
    rm src/network/foo.cpp
    run git status --short
    contains "MD src/network/foo.cpp" "$output"
    run python3 "$GATE_ABS" --staged-only
    [ "$status" -eq 1 ]
    contains "SSID logged" "$output"
}
