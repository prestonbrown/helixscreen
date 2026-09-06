#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_thumbnail_cache_guard.py — the gate that keeps
# src/ off ThumbnailCache's unguarded legacy overloads.
#
# The gate exists because the legacy overloads take no ThumbnailLoadContext.
# The context is what lets fetch() drop a superseded on_success; without it an
# in-flight download that has already been outdated still lands and overwrites
# a NEWER thumbnail. The overloads stay public only because 7 test call sites
# exercise them deliberately, so the compiler cannot enforce this.
#
# Both halves of the contract are pinned here. The positive half is every
# spelling of the legacy call, including the multi-line form the real call
# sites use and the alias receiver (`auto& cache = get_thumbnail_cache()`) that
# a receiver-blind grep would miss. The negative half is the guarded idiom
# actually in the tree, plus the three places the legacy calls are correct
# code: thumbnail_cache.cpp, tests/, and comments. A gate that fires on those
# is a gate that gets switched off.

GATE="scripts/check_thumbnail_cache_guard.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/thumb_guard"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into a fixture .cpp and run the gate over that file alone.
run_gate() {
    local name="$1" body="$2"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$name.cpp"
    run python3 "$GATE" "$FIXTURE_DIR/$name.cpp"
}

# Write $2 into a fixture at relative path $1 (dirs created) and run the gate.
run_gate_at() {
    local rel="$1" body="$2"
    mkdir -p "$FIXTURE_DIR/$(dirname "$rel")"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$rel"
    run python3 "$GATE" "$FIXTURE_DIR/$rel"
}

# --- the shapes that reintroduce the stale-write bug ------------------------

@test "flags the legacy fetch(api, path, ...) overload" {
    run_gate legacy_fetch '
void Panel::load() {
    get_thumbnail_cache().fetch(api_, "thumbs/a.png",
                                [this](const std::string& p, bool) { set(p); },
                                nullptr);
}'
    [ "$status" -eq 1 ]
    contains ':3:' "$output"
    [[ "$output" == *"unguarded ThumbnailCache::fetch()"* ]]
}

@test "flags the legacy fetch even when the arguments are on later lines" {
    # Every real call site in src/ wraps like this, so a single-line matcher
    # would report the tree clean while the bug sat in it.
    run_gate legacy_fetch_wrapped '
void Panel::load() {
    get_thumbnail_cache().fetch(
        api_,
        relative_path,
        on_ok,
        on_err);
}'
    [ "$status" -eq 1 ]
    contains ':3:' "$output"
    [[ "$output" == *"unguarded ThumbnailCache::fetch()"* ]]
}

@test "flags legacy get_if_cached in both its 1-arg and 2-arg forms" {
    run_gate legacy_gic '
void Panel::a() {
    auto x = get_thumbnail_cache().get_if_cached(relative_path);
}
void Panel::b() {
    auto y = get_thumbnail_cache().get_if_cached(relative_path, source_modified);
}'
    [ "$status" -eq 1 ]
    contains ':3:' "$output"
    contains ':6:' "$output"
    [[ "$output" == *"2 site(s)"* ]]
}

@test "flags a legacy call reached through an aliased receiver" {
    # `auto& cache = get_thumbnail_cache();` is how two src/ files spell it, so
    # matching only the literal singleton call would miss them.
    run_gate alias_receiver '
void Overlay::load() {
    auto& cache = get_thumbnail_cache();
    std::string cached = cache.get_if_cached(thumb_rel_path);
}'
    [ "$status" -eq 1 ]
    contains ':4:' "$output"
    [[ "$output" == *"unguarded ThumbnailCache::get_if_cached()"* ]]
}

@test "flags a legacy call on a ThumbnailCache reference parameter" {
    run_gate ref_param '
void warm(ThumbnailCache& cache, const std::string& path) {
    cache.fetch(nullptr, path, on_ok, on_err);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *':3:'* ]]
}

@test "flags a request-shaped first argument paired with a non-context second" {
    # The half-migrated shape: someone builds the request but drops the guard.
    # fetch(req, path, ...) still binds the legacy overload.
    run_gate half_migrated '
void Panel::load() {
    ThumbnailRequest req;
    get_thumbnail_cache().fetch(req, "thumbs/a.png", on_ok, on_err);
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"is not a ThumbnailLoadContext"* ]]
}

@test "names the file, the line and the fix in its diagnostic" {
    run_gate diagnostic '
void Panel::load() {
    get_thumbnail_cache().get_if_cached(relative_path, mtime);
}'
    [ "$status" -eq 1 ]
    contains "diagnostic.cpp:3:" "$output"
    contains "ThumbnailLoadContext" "$output"
    [[ "$output" == *"THUMB_LEGACY_OK"* ]]
}

# --- the idioms it must stay quiet about ------------------------------------

@test "stays quiet on the guarded request+context fetch" {
    run_gate guarded_fetch '
void Panel::load() {
    ThumbnailLoadContext ctx = ThumbnailLoadContext::create(lifetime_, &gen_);
    ThumbnailRequest req;
    req.cache_key = key;
    get_thumbnail_cache().fetch(
        req, ctx,
        [this, ctx](const std::string& lvgl_path, bool) { apply(lvgl_path); },
        nullptr);
}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "stays quiet on the guarded get_if_cached(req)" {
    run_gate guarded_gic '
void Panel::load() {
    ThumbnailRequest req;
    req.cache_key = key;
    std::string cached = get_thumbnail_cache().get_if_cached(req);
}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "stays quiet when the context is bound with auto" {
    # print_status_widget.cpp spells it `auto ctx = ThumbnailLoadContext::create(...)`.
    run_gate auto_ctx '
void Widget::load() {
    auto ctx = ThumbnailLoadContext::create(lifetime_, &idle_thumb_generation_);
    ThumbnailRequest req;
    get_thumbnail_cache().fetch(req, ctx, on_ok, nullptr);
}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "stays quiet on an inline-constructed request and context" {
    run_gate inline_ctor '
void Panel::load() {
    get_thumbnail_cache().fetch(ThumbnailRequest{key},
                                ThumbnailLoadContext::create(lifetime_, &gen_),
                                on_ok, nullptr);
}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "stays quiet on a comment that merely shows the legacy call" {
    # thumbnail_cache.h documents the old API in its header comment. A gate that
    # cannot tell code from prose fires on documentation forever.
    # `cache` is a REAL receiver in this fixture, so the commented-out calls are
    # matchable text — only comment stripping keeps them quiet. Without that the
    # fixture would pass for the wrong reason.
    run_gate comment_only '
void Panel::load() {
    auto& cache = get_thumbnail_cache();
    // Legacy usage was:
    //   cache.fetch(api_, relative_path, on_ok, on_err);
    //   cache.get_if_cached(relative_path, mtime);
    /* cache.get_if_cached(path); */
    ThumbnailRequest req;
    (void)cache.get_if_cached(req);
}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "stays quiet on an unrelated fetch() that is not a ThumbnailCache call" {
    # PrintHistoryManager::fetch() is called ~25 times in tests and once in src.
    # A method-name-only matcher would drown the gate in noise.
    run_gate unrelated '
void Panel::refresh() {
    history_->fetch();
    manager_->fetch(api_, "path", cb, err);
    other_cache.get_if_cached(path, mtime);
}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "stays quiet on thumbnail_cache.cpp, which implements the request forms" {
    # The request overloads delegate to the legacy ones. That IS the correct
    # code there, so the implementation file is excluded by path.
    run_gate_at print/thumbnail_cache.cpp '
void ThumbnailCache::fetch(const ThumbnailRequest& req, ThumbnailLoadContext ctx,
                           SuccessCallback on_success, ErrorCallback on_error) {
    ThumbnailCache& cache = *this;
    cache.fetch(req.api, req.cache_key, on_success, on_error);
}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "stays quiet on tests, which exercise the legacy overloads on purpose" {
    run_gate_at tests/unit/test_thumbnail_cache.cpp '
TEST_CASE("legacy overload still behaves") {
    ThumbnailCache cache;
    cache.fetch(nullptr, "does/not/exist/thumb.png", nullptr, on_err);
    CHECK(cache.get_if_cached(test_path, old_time).empty());
}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "honours a THUMB_LEGACY_OK annotation with a reason" {
    run_gate opt_out '
void Panel::load() {
    auto x = get_thumbnail_cache().get_if_cached(path); // THUMB_LEGACY_OK: one-shot probe
}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# --- the tree itself --------------------------------------------------------

@test "src/ is clean under the gate today" {
    # The ratchet. src/ was migrated off the legacy overloads entirely; this
    # pins that state, and is what fails the moment a new consumer reaches for
    # the unguarded form.
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the gate is wired into quality-checks.sh" {
    # A gate nothing runs is not a gate.
    run grep -c 'check_thumbnail_cache_guard.py' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
    [ "$output" -ge 1 ]
}

# --- the gate can actually fail ---------------------------------------------

@test "the gate reports nothing when its matcher is broken" {
    # Meta-meta: prove the positive tests above are load-bearing. Neuter the
    # receiver matcher on a copy and confirm the legacy fixture stops being
    # caught — if this passes silently with the real gate too, the positive
    # tests were proving nothing.
    local broken="${FIXTURE_DIR}/broken_gate.py"
    sed 's@^SINGLETON = "get_thumbnail_cache"@SINGLETON = "never_matches_anything"@' \
        "$GATE" > "$broken"
    refute_grep 'SINGLETON = "get_thumbnail_cache"' "$broken"

    printf '%s\n' '
void Panel::load() {
    get_thumbnail_cache().fetch(api_, "thumbs/a.png", on_ok, on_err);
}' > "$FIXTURE_DIR/mutation.cpp"

    # Real gate catches it...
    run python3 "$GATE" "$FIXTURE_DIR/mutation.cpp"
    [ "$status" -eq 1 ]

    # ...broken gate does not. That difference is what the suite is pinning.
    run python3 "$broken" "$FIXTURE_DIR/mutation.cpp"
    [ "$status" -eq 0 ]
}
