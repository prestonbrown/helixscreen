#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_json_dump_utf8.py — the bare-.dump() save-path
# gate (prestonbrown/helixscreen#1493).
#
# nlohmann dumps with error_handler_t::strict by default, so a string holding
# bytes UTF-8 cannot decode makes serialization throw. On a save path that is
# either a terminated process or, behind a catch, a change the user made that
# quietly never reached disk. helix::json_util::safe_dump() replaces the
# offending bytes instead.
#
# Both halves of the contract are pinned here. The flagged shapes are the two
# ways a document leaves this process — a stream write and an HTTP body. The
# quiet ones matter just as much: a gate that fires on the correct form, on a
# log line, or on an annotated site is a gate somebody switches off.

GATE="scripts/check_json_dump_utf8.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/json_dump_utf8"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into $FIXTURE_DIR/$1.cpp and run the gate over that one file.
run_gate() {
    local name="$1" body="$2"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$name.cpp"
    run python3 "$GATE" "$FIXTURE_DIR/$name.cpp"
    printf '%s\n' "$output" > "$FIXTURE_DIR/out.txt"
}

flagged() {
    grep -q "Bare .dump() on a save path" "$FIXTURE_DIR/out.txt" \
        || fail "expected the gate to flag this site, got: $(cat "$FIXTURE_DIR/out.txt")"
    [ "$status" -eq 1 ] || fail "expected exit 1 on a finding, got $status"
}

quiet() {
    refute_grep "Bare .dump() on a save path" "$FIXTURE_DIR/out.txt"
    [ "$status" -eq 0 ] || fail "expected exit 0 with no findings, got $status"
}

# ------------------------------------------------ shapes that must be CAUGHT

@test "flags a document written straight to an ofstream" {
    run_gate ofstream '
void save(const json& doc) {
    std::ofstream out(path);
    out << doc.dump(2);
}'
    flagged
}

@test "flags a compact dump inserted into a stream" {
    run_gate compact '
void save(const json& j) {
    ofs << j.dump();
}'
    flagged
}

@test "flags a dump built inline from a call result" {
    # The document need not be a named variable — entries_to_json() and
    # json(queue_) are both real save-path spellings.
    run_gate inline_call '
void save() {
    file << entries_to_json().dump(2);
}'
    flagged
}

@test "flags a dump assigned into an HTTP request body" {
    run_gate request_body '
void send(const json& batch) {
    auto req = std::make_shared<HttpRequest>();
    req->body = batch.dump();
}'
    flagged
}

@test "flags a dump assigned to a local named body" {
    run_gate local_body '
bool post(const json& payload) {
    std::string body = payload.dump();
    return upload(body);
}'
    flagged
}

# -------------------------------------------- shapes that must stay SILENT

@test "does not flag the safe_dump helper itself" {
    run_gate safe_form '
void save(const json& doc) {
    ofs << helix::json_util::safe_dump(doc, 2);
    req->body = helix::json_util::safe_dump(doc);
}'
    quiet
}

@test "does not flag a dump used as a log argument" {
    # A log line is formatted and discarded; the loss a save path takes is
    # permanent, which is the only place this rule is worth its noise.
    run_gate logging '
void trace(const json& status) {
    spdlog::trace("[Fan] status: {}", status.dump());
    spdlog::debug("[LED] {}", status["led"].dump().substr(0, 200));
}'
    quiet
}

@test "does not flag a commented-out save" {
    run_gate commented '
void save(const json& doc) {
    // out << doc.dump(2);
    out << helix::json_util::safe_dump(doc, 2);
}'
    quiet
}

@test "honors an opt-out annotation on the same line" {
    run_gate optout_inline '
void save(const json& flag) {
    ofs << flag.dump();  // JSON_DUMP_OK: fixed-shape flag, no free-form text
}'
    quiet
}

@test "honors an opt-out annotation on the line above" {
    run_gate optout_above '
void save(const json& flag) {
    // JSON_DUMP_OK: fixed-shape flag, no free-form text
    ofs << flag.dump();
}'
    quiet
}

@test "an unrelated opt-out token further up the file does not silence a site" {
    # The annotation covers its own site, not everything below it.
    run_gate optout_scope '
void a(const json& flag) {
    ofs << flag.dump();  // JSON_DUMP_OK: fixed-shape flag
}

void b(const json& doc) {
    ofs << doc.dump(2);
}'
    flagged
}

# ------------------------------------------------------- the live tree ----

@test "src/ and include/ carry no bare save-path dumps" {
    run python3 "$GATE"
    [ "$status" -eq 0 ] || fail "gate is red on the tree: $output"
}
