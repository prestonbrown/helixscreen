#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_subscription_null_safety.py rule 2 — the
# uncatchable const nlohmann::json operator[] gate.
#
# Rule 2 exists because NDEBUG is never defined in this build, so nlohmann's
# JSON_ASSERT is a live assert(): reading a missing key through the CONST
# operator[] overload is a SIGABRT (exit 134), not a catchable throw. The
# canonical shape is a non-mutable lambda capturing the response by value —
# which makes the capture const — followed by an unguarded subscript chain.
# Five such sites were fixed in 530fcea39.
#
# These tests pin the gate's behaviour on both the shape it must catch and the
# guard idioms already used throughout the codebase, which it must NOT flag. A
# gate that fires on the negative cases below would be noise on every commit.

GATE="scripts/check_subscription_null_safety.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/const_subscript"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into a fixture .cpp and run rule 2 over it alone.
run_gate() {
    local name="$1" body="$2"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$name.cpp"
    run python3 "$GATE" --rule 2 "$FIXTURE_DIR/$name.cpp"
}

# --- the shape that actually kills the app ---------------------------------

@test "rule2 flags an unguarded chain in a non-mutable lambda" {
    run_gate abort_chain '
void f() {
    token.defer("tag", [this, response = std::move(response)]() {
        const auto& settings = response["result"]["status"]["configfile"]["settings"];
        apply(settings);
    });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'response["result"]'* ]]
}

@test "rule2 flags an unguarded chain behind a const json& parameter" {
    run_gate const_param '
void f(const nlohmann::json& response) {
    const auto& settings = response["result"]["status"];
    use(settings);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'response["result"]'* ]]
}

@test "rule2 flags a partially-guarded chain at the first unproven level" {
    run_gate partial '
void f(const nlohmann::json& response) {
    if (!response.contains("result"))
        return;
    const auto& s = response["result"]["status"]["configfile"];
    use(s);
}'
    [ "$status" -eq 1 ]
    # "result" is proven; the gate must point at the next level, not the first.
    [[ "$output" == *'response.result["status"]'* ]]
}

@test "rule2 flags an unbounded numeric index on a const json" {
    run_gate array_oob '
void f(const nlohmann::json& msg) {
    if (!msg.contains("params"))
        return;
    const auto& p = msg["params"][0];
    use(p);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'msg["params"][0]'* ]]
}

# --- guard idioms already used in the tree, which must stay silent ---------

@test "rule2 accepts the short-circuit early-return guard chain" {
    run_gate guarded_early '
void f(const nlohmann::json& response) {
    if (!response.contains("result") || !response["result"].contains("status") ||
        !response["result"]["status"].contains("configfile")) {
        return;
    }
    const auto& s = response["result"]["status"]["configfile"];
    use(s);
}'
    [ "$status" -eq 0 ]
}

@test "rule2 accepts the conjunction guard idiom" {
    run_gate guarded_and '
void f(const nlohmann::json& response) {
    if (response.contains("result") && response["result"].contains("status")) {
        const auto& status = response["result"]["status"];
        use(status);
    }
}'
    [ "$status" -eq 0 ]
}

@test "rule2 accepts the contains + is_array + empty guard before params[0]" {
    run_gate guarded_params '
void f(const nlohmann::json& msg) {
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty())
        return;
    const auto& p = msg["params"][0];
    use(p);
}'
    [ "$status" -eq 0 ]
}

@test "rule2 accepts a dynamic key proven by contains(key)" {
    run_gate guarded_dynamic '
void f(const nlohmann::json& event, const char* key) {
    if (event.contains("app") && event["app"].contains(key) &&
        event["app"][key].is_string()) {
        use(event["app"][key].get<std::string>());
    }
}'
    [ "$status" -eq 0 ]
}

@test "rule2 ignores a mutable lambda capture (operator[] is the inserting overload)" {
    run_gate mutable_lambda '
void f() {
    token.defer("tag", [this, response = std::move(response)]() mutable {
        auto& settings = response["result"]["status"];
        use(settings);
    });
}'
    [ "$status" -eq 0 ]
}

@test "rule2 flags a by-value capture in a file with no const keyword" {
    # The canonical shape acquires its constness from the non-mutable lambda's
    # operator(), not from a `const` in the source. Any pre-filter that keys on
    # the token `const` silently skips exactly the defect this rule exists for.
    run_gate no_const_token '
void f() {
    tok.defer("t", [this, resp = std::move(resp)]() {
        auto& s = resp["result"]["status"];
        use(s);
    });
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *'resp["result"]'* ]]
}

@test "rule2 ignores a by-reference capture" {
    run_gate ref_capture '
void f(nlohmann::json& response) {
    call([&response]() {
        auto& s = response["result"]["status"];
        use(s);
    });
}'
    [ "$status" -eq 0 ]
}

@test "rule2 ignores writes through a non-const json" {
    run_gate write_target '
nlohmann::json build(const nlohmann::json& other) {
    nlohmann::json j;
    j["lane"] = 1;
    j["color"] = "red";
    return j;
}'
    [ "$status" -eq 0 ]
}

@test "rule2 does not leak a const json& parameter scope into sibling functions" {
    # Regression: matching a bare `const json& x` as a local binding gave it a
    # scope that ran to the next closing brace, marking a later, unrelated
    # local `json x;` const and flagging every write to it.
    run_gate scope_leak '
int parse(const nlohmann::json& json) {
    return json.value("v", 0);
}

nlohmann::json build() {
    nlohmann::json json;
    json["version"] = 1;
    json["noise"] = 2;
    return json;
}'
    [ "$status" -eq 0 ]
}

@test "rule2 honours the per-line opt-out comment" {
    run_gate opt_out '
void f(const nlohmann::json& response) {
    const auto& s = response["result"]["status"]; // JSON_CONST_OK: built above
    use(s);
}'
    [ "$status" -eq 0 ]
}

@test "rule2 ignores string literals containing brackets and braces" {
    run_gate literal_noise '
void f(const nlohmann::json& response) {
    if (!response.contains("result"))
        return;
    log("{ [not code] }", response["result"]);
}'
    [ "$status" -eq 0 ]
}

@test "rule2 ignores commented-out violations" {
    run_gate commented '
void f(const nlohmann::json& response) {
    // const auto& s = response["result"]["status"]["configfile"];
    /* const auto& t = response["a"]["b"]; */
    use(response);
}'
    [ "$status" -eq 0 ]
}

# --- gate wiring ------------------------------------------------------------

@test "rule 1 behaviour is unchanged by the rule-2 addition" {
    run python3 "$GATE" --rule 1 --max-allowed 0 --summary
    [ "$status" -eq 0 ]
    [[ "$output" == *"Subscription null-safety: 0"* ]]
}

# ------------------------------------------------------- --staged-only content
#
# --staged-only picks its file SET from `git diff --cached`, but must read
# each file's STAGED content (the index blob), not the working tree. Stage a
# violation and then revert the working file to something clean, and a gate
# that reads the working tree reports nothing while the bad blob commits.
# Both rules share one collect_files() implementation, so one repo covers both.

GATE_ABS="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/check_subscription_null_safety.py"

setup_tmp_repo() {
    TMP_REPO="$(mktemp -d "${BATS_TEST_TMPDIR:-${BATS_TMPDIR:-/tmp}}/null-safety-XXXXXX")"
    cd "$TMP_REPO" || return 1
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test"
    mkdir -p src/printer
    printf 'void PrinterState::update_from_status(const json& j) {\n    int x = 0;\n}\n' \
        > src/printer/foo.cpp
    git add -A && git commit -qm base
}

@test "rule 1 --staged-only catches a violation in the STAGED blob, not a reverted working file" {
    setup_tmp_repo
    printf 'void PrinterState::update_from_status(const json& j) {\n    int x = j["temp"].get<int>();\n}\n' \
        > src/printer/foo.cpp
    git add src/printer/foo.cpp
    printf 'void PrinterState::update_from_status(const json& j) {\n    int x = 0;\n}\n' \
        > src/printer/foo.cpp
    run python3 "$GATE_ABS" --staged-only --rule 1 --max-allowed 0
    [ "$status" -eq 1 ]
    [[ "$output" == *'exceeds baseline'* ]]
}

@test "rule 1 --staged-only stays silent on a clean staged file with a dirty violation on disk" {
    setup_tmp_repo
    # Genuinely different from the base commit (a second clean statement), so
    # the file shows up as staged at all - content byte-identical to HEAD
    # stages nothing for git to report, which would pass this test for free.
    printf 'void PrinterState::update_from_status(const json& j) {\n    int x = 0;\n    int y = 1;\n}\n' \
        > src/printer/foo.cpp
    git add src/printer/foo.cpp
    printf 'void PrinterState::update_from_status(const json& j) {\n    int x = j["temp"].get<int>();\n}\n' \
        > src/printer/foo.cpp
    run python3 "$GATE_ABS" --staged-only --rule 1 --max-allowed 0
    [ "$status" -eq 0 ]
}

@test "rule 2 --staged-only skips files outside the roots its baseline covers" {
    setup_tmp_repo
    mkdir -p tests/unit
    printf 'void f() {\n    token.defer("t", [response]() { use(response["result"]["x"]); });\n}\n' \
        > tests/unit/foo.cpp
    git add tests/unit/foo.cpp
    run python3 "$GATE_ABS" --staged-only --rule 2
    [ "$status" -eq 0 ]

    mv tests/unit/foo.cpp src/printer/bar.cpp
    git add -A
    run python3 "$GATE_ABS" --staged-only --rule 2
    [ "$status" -eq 1 ]
}
