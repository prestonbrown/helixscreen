#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fixture tests of scripts/code_lint.sh, the tree-wide code lints. Each case
# pins one lint's behaviour on code it writes itself, or on a mutated copy of
# the file the lint guards: the shape it must catch and the idioms it must stay
# quiet about. The verdict over the real tree is quality-checks.sh's
# (qc_code_lint, qc_test_lint, qc_ui_lint), and each lint's reason lives beside
# it in the script.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    # shellcheck source=scripts/code_lint.sh
    . scripts/code_lint.sh
}

# A scratch tree holding the files a lint reads, so a lint_<name> function
# runs over it exactly as it runs over the repository. Leaves the test there.
fixture_root() {
    FIXTURE_ROOT="$BATS_TEST_TMPDIR/root"
    mkdir -p "$FIXTURE_ROOT"
    cd "$FIXTURE_ROOT" || return 1
}

# Writes stdin to $1 under the current directory, making its parent.
put() {
    mkdir -p "$(dirname "$1")"
    cat > "$1"
}

@test "locate_by_symbol names the one file a definition lives in" {
    run locate_by_symbol '^bool PrinterSwitchFlow::switch_printer\('
    [ "$status" -eq 0 ]
    [ "$output" = "src/application/printer_switch_flow.cpp" ]
}

@test "locate_by_symbol fails on a symbol nothing defines" {
    run locate_by_symbol '^void NoSuchClass::no_such_method\('
    [ "$status" -eq 1 ]
    [[ "$output" == *"matches 0 file(s)"* ]]
}

@test "locate_by_symbol fails on a pattern several files match" {
    run locate_by_symbol '^#pragma once'
    [ "$status" -eq 1 ]
    [[ "$output" == *"expected 1"* ]]
}

@test "the switch_printer cache-invalidation gate fails when the call is removed" {
    # Meta-test: a gate that cannot fail is not a gate. Strip the call from a
    # copy and confirm the check reports the #804 regression.
    local mutated="${BATS_TEST_TMPDIR}/application_no_clear.cpp"
    grep -v 'PrinterCacheRegistry::instance().invalidate_all()' src/application/printer_switch_flow.cpp > "$mutated"

    run check_switch_printer_clears_caches "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"#804"* ]]
}

@test "the switch_printer cache-invalidation gate fails when the call moves after teardown" {
    # The ordering half: invalidating after teardown re-reads the OLD printer's
    # values on the way down, so position matters as much as presence.
    local mutated="${BATS_TEST_TMPDIR}/application_late_clear.cpp"
    sed -e 's@^    PrinterCacheRegistry::instance().invalidate_all();@@' \
        -e 's@^    tear_down();@    tear_down();\n    PrinterCacheRegistry::instance().invalidate_all();@' \
        src/application/printer_switch_flow.cpp > "$mutated"

    run check_switch_printer_clears_caches "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"must precede"* ]]
}

@test "the switch_printer cache-invalidation gate fails when the function cannot be located" {
    # Fail-closed: a rename or signature change must break the gate loudly rather
    # than silently pass on an empty body.
    local mutated="${BATS_TEST_TMPDIR}/application_no_fn.cpp"
    sed -e 's@^bool PrinterSwitchFlow::switch_printer(@bool PrinterSwitchFlow::switch_printer_renamed(@' \
        src/application/printer_switch_flow.cpp > "$mutated"

    run check_switch_printer_clears_caches "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not locate"* ]]
}

@test "the switch_printer cache-invalidation gate fails when teardown is missing" {
    local mutated="${BATS_TEST_TMPDIR}/application_no_teardown.cpp"
    grep -v '^    tear_down();' src/application/printer_switch_flow.cpp > "$mutated"

    run check_switch_printer_clears_caches "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"does not call tear_down()"* ]]
}

@test "the RTTI gate catches each forbidden shape" {
    # Meta-test: a gate that cannot fail is not a gate.
    local f="${BATS_TEST_TMPDIR}/offender.cpp"
    cat > "$f" <<'EOF'
void a(Base* b) { auto* d = dynamic_cast<Derived*>(b); }
void b(const E& e) { log(typeid(e).name()); }
std::map<std::type_index, int> registry;
void c(const std::function<void()>& f) { f.target_type(); }
EOF
    run rtti_offenders "$(rtti_forbidden_pattern)" "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 4 ]
    contains "dynamic_cast" "$output"
    contains "typeid" "$output"
    contains "type_index" "$output"
    [[ "$output" == *"target_type"* ]]
}

@test "the RTTI gate stays quiet on prose and RTTI_OK opt-outs" {
    # The silent half matters as much as the loud half: every occurrence in the
    # tree today is a comment describing the RTTI-free replacement, and a gate
    # that fires on those gets switched off.
    local f="${BATS_TEST_TMPDIR}/quiet.cpp"
    cat > "$f" <<'EOF'
// Replaces typeid(e).name(), which needs RTTI - the firmware builds -fno-rtti.
/// Replacement for std::type_index map keys under -fno-rtti.
/**
 * stand-in for the `dynamic_cast<AmsBackendMock*>` it used to do - the
 * RTTI-free stand-in for `typeid(*this)`.
 */
void ok() { auto* p = plain_static_cast<T>(q); }
void hatch() { auto* d = dynamic_cast<Derived*>(b); } // RTTI_OK: desktop tool, never in the firmware slice
EOF
    run rtti_offenders "$(rtti_forbidden_pattern)" "$f"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the std::any gate catches type() comparisons but not ordinary type() getters" {
    local f="${BATS_TEST_TMPDIR}/any_shapes.cpp"
    cat > "$f" <<'EOF'
if (value.type() == typeid(bool)) {}
if (value.type() != typeid(float)) {}
log(value.type().name());
const bool portrait = is_portrait_layout(LayoutManager::instance().type());
switch (lm.type()) { default: break; }
if (const bool* held = std::any_cast<bool>(&value)) {}
EOF
    run rtti_offenders "$(rtti_any_pattern)" "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 3 ]
    lacks "LayoutManager" "$output"
    [[ "$output" != *"any_cast"* ]]
}

@test "the std::regex gate catches includes and uses, and stays quiet on prose and opt-outs" {
    local f="${BATS_TEST_TMPDIR}/regex_shapes.cpp"
    cat > "$f" <<'SHAPES'
#include <regex>
static const std::regex re("a+");
std::smatch m;
auto it = std::sregex_iterator(s.begin(), s.end(), re);
// Plain string walking rather than std::regex, which overflows the stack.
auto hatch = std::regex("x"); // STD_REGEX_OK: test fixture
helix::Regex fine("a+");
SHAPES
    run code_offenders "$(std_regex_pattern)" STD_REGEX_OK "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 4 ]
    lacks "Plain string" "$output"
    lacks "STD_REGEX_OK" "$output"
    lacks "helix::Regex" "$output"
}

@test "the event-sweep gate catches removal by index and stays quiet on named removal and opt-outs" {
    local f="${BATS_TEST_TMPDIR}/sweep_shapes.cpp"
    cat > "$f" <<'SHAPES'
lv_obj_remove_event(back_button, i - 1);
if (ok) ::lv_obj_remove_event(obj, 0);
// Never lv_obj_remove_event(obj, 0): it takes the observer hooks too.
lv_obj_remove_event(obj, 1); // REMOVE_EVENT_OK: test fixture
lv_obj_remove_event_cb(obj, on_clicked);
lv_obj_remove_event_cb_with_user_data(obj, on_deleted, this);
my_lv_obj_remove_event(obj, 0);
SHAPES
    run code_offenders "$(remove_event_sweep_pattern)" REMOVE_EVENT_OK "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 2 ]
    contains "back_button" "$output"
    contains "::lv_obj_remove_event" "$output"
    lacks "REMOVE_EVENT_OK" "$output"
    lacks "_cb" "$output"
    lacks "my_lv" "$output"
}

@test "the raw-thread gate catches spawns and stays quiet on members, ids and opt-outs" {
    local f="${BATS_TEST_TMPDIR}/thread_shapes.cpp"
    cat > "$f" <<'SHAPES'
worker_ = std::thread([this] { run(); });
std::thread t(&Foo::run, this);
std::thread{fn}.detach();
auto fut = std::async(std::launch::async, fn);
std::jthread j([] {});
pthread_create(&tid, nullptr, run, this);
std::thread worker_;
std::jthread jworker_;
std::thread::id owner_;
std::vector<std::thread> pool_;
// Joined in the destructor: std::thread(...) must not outlive us.
auto hatch = std::async(std::launch::async, fn); // THREAD_OK: test fixture
worker_ = helix::make_thread([this] { run(); });
SHAPES
    run code_offenders "$(raw_thread_pattern)" THREAD_OK "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 6 ]
    lacks "std::thread worker_;" "$output"
    lacks "jworker_" "$output"
    lacks "THREAD_OK" "$output"
    lacks "make_thread" "$output"
}

@test "the WiFiManager owner gate catches constructions and stays quiet on uses and opt-outs" {
    local f="${BATS_TEST_TMPDIR}/wifi_owner_shapes.cpp"
    cat > "$f" <<'SHAPES'
auto wm = std::make_shared<WiFiManager>(true);
auto wm = std::make_unique<helix::WiFiManager>(std::move(b));
auto* wm = new WiFiManager(true);
WiFiManager wm(std::move(b));
helix::WiFiManager wm;
std::shared_ptr<WiFiManager> get_wifi_manager();
explicit WiFiManager(bool silent = false);
WiFiManager::~WiFiManager() {
friend class WiFiManagerTestAccess;
WiFiManager* manager = static_cast<WiFiManager*>(p);
// std::make_shared<WiFiManager>() in a comment
auto wm = std::make_shared<WiFiManager>(true); // WIFI_MANAGER_OWNER_OK: the one owner
SHAPES
    run code_offenders "$(wifi_manager_owner_pattern)" WIFI_MANAGER_OWNER_OK "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 5 ]
    lacks "get_wifi_manager" "$output"
    lacks "explicit" "$output"
    lacks "TestAccess" "$output"
    lacks "static_cast" "$output"
    lacks "OWNER_OK" "$output"
}

@test "the subject-init gate catches bare declarations, and stays quiet on initialized, static and pointer forms" {
    local f="${BATS_TEST_TMPDIR}/subject_shapes.h"
    cat > "$f" <<'SHAPES'
    lv_subject_t bare_;
    lv_subject_t slots_[kMaxSlots];
    std::array<lv_subject_t, 4> rows_;
    lv_subject_t
        split_bare_; ///< clang-format moved the name
    mutable lv_subject_t mutable_bare_;
    ::lv_subject_t qualified_bare_;
    lv_subject_t first_bare_, second_bare_;
    lv_subject_t init_first_{}, trailing_bare_;
    lv_subject_t grid_[2][3];
    std::array<lv_subject_t, static_cast<size_t>(Kind::Count)> cast_rows_;
    lv_subject_t braced_{};
    lv_subject_t braced_slots_[kMaxSlots]{};
    std::array<lv_subject_t, 4> braced_rows_{};
    std::array<lv_subject_t, static_cast<size_t>(Kind::Count)> braced_cast_rows_{};
    lv_subject_t
        braced_split_{}; ///< clang-format moved the name
    mutable lv_subject_t braced_mutable_{};
    lv_subject_t braced_pair_a_{}, braced_pair_b_{};
    lv_subject_t braced_grid_[2][3]{};
    static lv_subject_t zeroed_by_storage;
    lv_subject_t* pointer_ = nullptr;
    extern lv_subject_t elsewhere;
    lv_subject_t hatch_; // SUBJECT_INIT_OK: test fixture
    // lv_subject_t in_a_comment_;
SHAPES
    run subject_offenders "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 10 ]
    contains "subject_shapes.h:4: " "$output"
    contains "split_bare_" "$output"
    lacks "braced" "$output"
    lacks "zeroed_by_storage" "$output"
    lacks "pointer_" "$output"
    lacks "elsewhere" "$output"
    lacks "SUBJECT_INIT_OK" "$output"
}

@test "the deferral gate catches raw idioms, and stays quiet on prose, cancel and opt-outs" {
    local f="${BATS_TEST_TMPDIR}/defer_shapes.cpp"
    cat > "$f" <<'SHAPES'
lv_async_call(cb, this);
helix::ui::async_call([](void*) {}, nullptr);
helix::async::call_method(this, &T::m);
// lv_async_call(cb, this) in a comment
lv_async_call_cancel(cb, this);
simulate_ui_async_call([] {});
lv_async_call(cb, &slot); // LV_ASYNC_OK: test fixture
helix::ui::run_next_tick(tok, [] {});
SHAPES
    run code_offenders "$(defer_idiom_pattern)" LV_ASYNC_OK "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 3 ]
    lacks "in a comment" "$output"
    lacks "_cancel" "$output"
    lacks "simulate_ui" "$output"
    lacks "LV_ASYNC_OK" "$output"
    lacks "run_next_tick" "$output"
}

@test "the queue_update tag gate catches tagless and wrapped calls, and skips tags and opt-outs" {
    local f="${BATS_TEST_TMPDIR}/queue_shapes.cpp"
    cat > "$f" <<'SHAPES'
helix::ui::queue_update([this]() {});
ui::queue_update(std::move(cb));
helix::ui::queue_update(
    [x]() {});
helix::ui::queue_update("Owner::method", [this]() {});
helix::ui::queue_update(
    "Owner::wrapped", [x]() {});
// helix::ui::queue_update([]() {}) in a comment
ui_queue_update([] {});
helix::ui::queue_update(tag, [o]() {}); // QUEUE_TAG_OK: forwarded literal
SHAPES
    CODE_LINT_JOIN="$(queue_split_pattern)" run code_offenders \
        "$(queue_untagged_pattern)" QUEUE_TAG_OK "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 3 ]
    contains "queue_shapes.cpp:1: " "$output"
    contains "queue_shapes.cpp:3: " "$output"
    lacks "Owner::" "$output"
    lacks "in a comment" "$output"
    lacks "ui_queue_update" "$output"
    lacks "QUEUE_TAG_OK" "$output"
}

@test "the observer-name gate catches the retired spellings, and stays quiet on the new API and prose" {
    local f="${BATS_TEST_TMPDIR}/observer_shapes.cpp"
    cat > "$f" <<'SHAPES'
g = observe_int_sync<Panel>(s, this, h, lt);
g = helix::ui::observe_string<Panel>(s, this, h, lt);
g = observe_int_immediate<Panel>(s, this, h, lt);
g = observe_string_immediate(s, this, h, lt);
g = observe_int_async<Panel>(s, this, h, u, lt);
g = observe_print_state_immediate<Panel>(s, this, h, lt);
g = observe<int>(s, this, h, lt);
g = observe<const char*>(s, this, h, lt, Dispatch::Immediate);
g = observe_print_state<Panel>(s, this, h, lt);
// observe_int_sync<Panel>(s, this, h, lt) in a comment
auto observe_string_view = 3;
SHAPES
    run code_offenders "$(old_observer_pattern)" "" "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 6 ]
    lacks "in a comment" "$output"
    lacks "observe<int>" "$output"
    lacks "observe_print_state<Panel>" "$output"
    lacks "observe_string_view" "$output"
}

# --- Every extractable UI string is already in the translation catalogs ---
#
# `translation_sync.py sync` extracts from XML *and* C++, including the static
# tables that the UI translates through a variable (lv_tr(def.display_name) and
# friends -- see scripts/translations/cpp_tables.py). If a dry run would still
# add keys, someone shipped a user-facing string that no locale can translate.
# That is silent at runtime: lv_translation_get() falls back to the tag, so the
# string renders in English in all nine languages.
#
# The verdict over the tree is qc_translation_coverage in quality-checks.sh,
# which the pre-push hook, land-gate and the Code Quality workflow run. These
# cases pin that wiring.

@test "the translation coverage gate is wired into quality-checks.sh" {
  run grep -q 'QC_ALL=.*qc_translation_coverage' scripts/quality-checks.sh
  [ "$status" -eq 0 ]
  run grep -q '^qc_translation_coverage() {' scripts/qc/translation_coverage.sh
  [ "$status" -eq 0 ]
}

@test "quality-checks.sh runs the coverage gate as a dry run" {
  # A bare `sync` REWRITES all nine catalogs. A gate that edits the tree it is
  # inspecting would stage catalog churn behind the committer's back, and would
  # then report green on the very drift it just introduced.
  run grep -n 'translation_sync.py sync' scripts/qc/translation_coverage.sh
  [ "$status" -eq 0 ]
  while IFS= read -r line; do
    contains "--dry-run" "$line"
  done <<< "$output"
}

@test "the coverage gate wakes on src and ui_xml, not just translations" {
  # A new lv_tr() in src/ or a label_tag in ui_xml/ is what ADDS an untranslated
  # string; gating the check on ^translations/ alone would sleep through exactly
  # the commit that introduces one.
  run qc_trigger translation_coverage
  [ "$status" -eq 0 ]
  contains "^ui_xml/" "$output"
  [[ "$output" == *"^src/"* ]]
}

@test "the coverage gate fails on empty locale values via CI's pytest" {
  # The dry run proves a KEY exists; `make translation-sync` writes a new key
  # as an EMPTY placeholder, and an empty value renders as empty text in that
  # locale (the English-tag fallback only fires on a MISSING key). The gate
  # must run the same scan CI's Code Quality job runs -
  # tests/python/test_cpp_translation_coverage.py - so a tree the hook passes
  # cannot fail CI on the same rule.
  if [ ! -x .venv/bin/python ]; then
    skip "translations venv not set up (run 'make venv-setup')"
  fi
  run bash -c "sed -n '/^qc_translation_coverage() {/,/^}/p' scripts/qc/translation_coverage.sh"
  [ "$status" -eq 0 ]
  contains "tests/python/test_cpp_translation_coverage.py" "$output"
  contains "-m pytest" "$output"
}

# A missing .venv used to turn this gate into a warning in every mode. The full
# sweep is what pre-push and CI run, and a fresh clone (a cloud session, a new
# box) has no .venv until someone runs `make venv-setup`, so the last gate before
# main read as green over an untranslated lv_tr() key (prestonbrown/helixscreen#1507).
# Staged mode keeps the skip: pre-commit on a box without the venv must stay usable.

run_coverage_gate_without_venv() {
  # $1 = STAGED_ONLY value. Runs the real function body with the venv pointed
  # at a path that does not exist, and the timing helper stubbed out.
  bash -c '
    section_time() { :; }
    STAGED_ONLY="$1"
    VENV_PYTHON="/nonexistent/helix-venv/bin/python"
    . scripts/qc/translation_coverage.sh
    qc_translation_coverage
  ' _ "$1"
}

@test "a missing .venv fails the full translation coverage sweep" {
  run run_coverage_gate_without_venv false
  [ "$status" -eq 1 ]
  contains "make venv-setup" "$output"
  lacks "skipping" "$output"
}

@test "a missing .venv is still only a skip in staged mode" {
  run run_coverage_gate_without_venv true
  [ "$status" -eq 0 ]
  contains "skipping" "$output"
}

@test "the test_mode gate fires on a file that sets but never restores" {
    # Meta-test: a gate that cannot fail is not a gate.
    local d="${BATS_TEST_TMPDIR}/leak"
    mkdir -p "$d"
    cat > "$d/offender.cpp" <<'EOF'
TEST_CASE("leaks the global flag") {
    get_runtime_config()->test_mode = true;
}
EOF
    run test_mode_unrestored_files "$d" --include='*.cpp'
    [ "$status" -eq 0 ]
    [[ "$output" == *"offender.cpp"* ]]
}

@test "the test_mode gate stays quiet on a scoped or manually restored setter" {
    local d="${BATS_TEST_TMPDIR}/ok"
    mkdir -p "$d"
    cat > "$d/scoped.cpp" <<'EOF'
TEST_CASE("uses the shared guard") {
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->test_mode = true;
}
EOF
    cat > "$d/manual.cpp" <<'EOF'
struct Guard {
    bool prev = get_runtime_config()->test_mode;
    Guard() { get_runtime_config()->test_mode = true; }
    ~Guard() { get_runtime_config()->test_mode = prev; }
};
EOF
    cat > "$d/local_instance.cpp" <<'EOF'
TEST_CASE("a local RuntimeConfig is not global state") {
    RuntimeConfig config;
    config.test_mode = true;
}
EOF
    run test_mode_unrestored_files "$d" --include='*.cpp'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the display gate fires on a kept display and stays quiet on a deleted one" {
    local d="${BATS_TEST_TMPDIR}/displays"
    mkdir -p "$d/tests"
    cat > "$d/kept.cpp" <<'EOF'
struct Init {
    Init() { lv_display_t* disp = lv_display_create(800, 480); (void)disp; }
};
static Init init;
EOF
    cat > "$d/prose.cpp" <<'EOF'
// The fixture builds its display with a bare lv_display_create(), no driver.
 * lv_display_create() is never called here.
EOF
    cat > "$d/scoped.cpp" <<'EOF'
TEST_CASE("a second display for one body") {
    lv_display_t* disp = lv_display_create(64, 32);
    lv_display_delete(disp);
}
EOF
    cat > "$d/tests/lvgl_test_fixture.cpp" <<'EOF'
void ensure() { s_display = lv_display_create(800, 480); }
EOF
    run unreleased_display_files "$d" --include='*.cpp'
    [ "$status" -eq 0 ]
    [[ "$output" == *"kept.cpp"* ]] || fail "kept display not reported: $output"
    lacks "scoped.cpp" "$output"
    lacks "prose.cpp" "$output"
    lacks "lvgl_test_fixture.cpp" "$output"
}

@test "the AmsState init gate fires on a false register_xml" {
    # Meta-test: a gate that cannot fail is not a gate.
    local d="${BATS_TEST_TMPDIR}/ams_unpublished"
    mkdir -p "$d"
    cat > "$d/offender.cpp" <<'EOF'
TEST_CASE("binds through the C++ accessor only") {
    AmsState::instance().init_subjects(false);
}
EOF
    run ams_state_unpublished_init_files "$d" --include='*.cpp'
    [ "$status" -eq 0 ]
    [[ "$output" == *"offender.cpp"* ]]
}

@test "the AmsState init gate stays quiet on a published init" {
    local d="${BATS_TEST_TMPDIR}/ams_published"
    mkdir -p "$d"
    cat > "$d/ok.cpp" <<'EOF'
TEST_CASE("publishes the names") {
    AmsState::instance().init_subjects(true);
    AmsState::instance().init_subjects();
}
EOF
    run ams_state_unpublished_init_files "$d" --include='*.cpp'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the history lazy-load gate fires on a reintroduced fetch()" {
    # Meta-test: a gate that cannot fail is not a gate. This is the exact shape
    # the four converted call sites had.
    local d="${BATS_TEST_TMPDIR}/history_bad"
    mkdir -p "$d"
    cat > "$d/offender.cpp" <<'EOF'
    if (auto* hm = get_print_history_manager()) {
        hm->add_observer(&history_cb_);
        if (!hm->is_loaded()) {
            hm->fetch();
        }
    }
EOF
    cat > "$d/deferred.cpp" <<'EOF'
    auto* history = get_print_history_manager();
    if (history && !history->is_loaded()) {
        history->fetch();
    }
EOF
    run history_lazy_fetch_offenders "$d"
    [ "$status" -eq 0 ]
    contains "hm->fetch(" "$output"
    [[ "$output" == *"history->fetch("* ]]
}

@test "the history lazy-load gate stays quiet on ensure_loaded and other fetchers" {
    local d="${BATS_TEST_TMPDIR}/history_ok"
    mkdir -p "$d"
    cat > "$d/converted.cpp" <<'EOF'
    if (auto* hm = get_print_history_manager()) {
        hm->add_observer(&history_cb_);
        hm->ensure_loaded();
    }
EOF
    cat > "$d/job_queue.cpp" <<'EOF'
    if (auto* jqs = get_job_queue_state()) {
        jqs->fetch();
    }
    get_thumbnail_cache().fetch(path, cb);
EOF
    run history_lazy_fetch_offenders "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the deinit_all logging gate fires when a log call is reintroduced" {
    local mutated="${BATS_TEST_TMPDIR}/subject_managed_panel_logs.h"
    sed -e 's@^        subjects_.clear();@        spdlog::trace("clearing");\n        subjects_.clear();@' \
        include/subject_managed_panel.h > "$mutated"

    run check_deinit_all_does_not_log "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"static destruction"* ]]
}

@test "the deinit_all logging gate reads the whole body, not a prefix" {
    # Extraction has to end on the brace that closes deinit_all(), not on the
    # first four-space "}" it meets. A nested block whose closing brace lands in
    # that column - a lambda, or a clang-format reflow - would otherwise end the
    # extraction early, and every line after it goes unchecked while the gate
    # reports green.
    local mutated="${BATS_TEST_TMPDIR}/subject_managed_panel_prefix.h"
    awk '
        /^        for \(size_t i = 0; i < subjects_.size\(\); \+\+i\) \{/ && !done {
            print "        if (subjects_.empty()) {"
            print "    }"
            print "        spdlog::trace(\"after the reflowed brace\");"
            done = 1
        }
        { print }
    ' include/subject_managed_panel.h > "$mutated"

    # The mutation has to actually be there, or this test passes vacuously.
    contains "after the reflowed brace" "$(cat "$mutated")"

    run check_deinit_all_does_not_log "$mutated"
    [ "$status" -eq 1 ]
    contains "static destruction" "$output"
}

@test "the deinit_all logging gate fails closed when the function is renamed" {
    local mutated="${BATS_TEST_TMPDIR}/subject_managed_panel_renamed.h"
    sed -e 's@^    void deinit_all() {@    void deinit_everything() {@' \
        include/subject_managed_panel.h > "$mutated"

    run check_deinit_all_does_not_log "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not locate"* ]]
}

# --- the vacuous-test ceiling has one home ---
# The nightly and mk/tests.mk both need the number. Written twice they drift,
# and the copy that drifts is the enforcing one, so the tree silently stops
# being held to the value it claims.

@test "the nightly reads the vacuous ceiling from the makefile, not a literal" {
    run grep -A2 'check_vacuous_tests.py' .github/workflows/nightly.yml
    [ "$status" -eq 0 ]
    contains "print-vacuous-max" "$output"
    # A bare --max-allowed <number> is the shape that drifts.
    [[ ! "$output" =~ --max-allowed[[:space:]]+[0-9]+ ]]
}

@test "print-vacuous-max resolves to a bare integer" {
    local out='' err=''
    # `make -C <dir>` implies --print-directory and exports it through MAKEFLAGS,
    # so an inner make launched under one writes "Entering/Leaving directory" to
    # STDOUT around the echoed value. scratch_make's --no-print-directory cancels
    # that inherited -w; -s alone does not. make's stderr is kept for the failure message: a red
    # here must say what make said.
    out="$(scratch_make -s print-vacuous-max \
        2>"$BATS_TEST_TMPDIR/vacuous-max.err")" || true
    [[ "$out" =~ ^[0-9]+$ ]] && return 0
    err="$(cat "$BATS_TEST_TMPDIR/vacuous-max.err")" || true
    echo "print-vacuous-max stdout: [$out]"
    echo "print-vacuous-max stderr: [$err]"
    false
}

# --- the vacuous baseline can express a test name that starts with '#' ---
# Entries are keyed by exact test-case name, and names carry issue references.
# A parser that treats any leading '#' as a comment drops those entries in
# silence: the case reappears as a finding and the ratchet reads one too high.

@test "the vacuous baseline honours a test name beginning with a hash" {
    local base="${BATS_TEST_TMPDIR}/baseline.txt"
    cat > "$base" <<'EOF'
# a real comment
#1127 seeding state costs no extra bytes per layer entry  # static_assert on sizeof
EOF
    run python3 -c "
import sys; sys.path.insert(0, 'scripts')
from pathlib import Path
import importlib.util
spec = importlib.util.spec_from_file_location('cvt', 'scripts/check_vacuous_tests.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
b = m.load_baseline(Path('$base'))
print(sorted(b))
"
    [ "$status" -eq 0 ]
    contains "#1127 seeding state costs no extra bytes per layer entry" "$output"
    [[ "$output" != *"a real comment"* ]]
}

@test "the ObserverGuard move gate fires when std::move comes back" {
    local mutated="${BATS_TEST_TMPDIR}/ui_observer_guard_moved.cpp"
    sed -e 's@std::exchange(other\.cleanup_, nullptr)@std::move(other.cleanup_)@' \
        src/ui/ui_observer_guard.cpp > "$mutated"

    run check_observer_guard_move_clears_cleanup "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"libc++"* ]]
}

@test "the ObserverGuard move gate fails closed when a move path disappears" {
    local mutated="${BATS_TEST_TMPDIR}/ui_observer_guard_dropped.cpp"
    awk '/cleanup_.*other\.cleanup_/ && !seen { seen = 1; next } { print }' \
        src/ui/ui_observer_guard.cpp > "$mutated"

    run check_observer_guard_move_clears_cleanup "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"expected exactly 2"* ]]
}

@test "the backend friend gate fires when a second friend is added" {
    local mutated="${BATS_TEST_TMPDIR}/afc_two_friends.h"
    sed -e 's@^\([[:space:]]*\)friend class AfcTestAccess;@\1friend class AfcTestAccess;\n\1friend class AfcSomeNewHelper;@' \
        include/ams_backend_afc.h > "$mutated"

    run check_friend_list "$mutated" "friend class AfcTestAccess;"
    [ "$status" -eq 1 ]
    [[ "$output" == *"expected exactly 1"* ]]
}

@test "the backend friend gate fails closed when the shim friend disappears" {
    local mutated="${BATS_TEST_TMPDIR}/afc_no_friend.h"
    grep -v 'friend class AfcTestAccess;' include/ams_backend_afc.h > "$mutated"

    run check_friend_list "$mutated" "friend class AfcTestAccess;"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no friend declaration"* ]]
}

@test "the on_deactivate gate fires on a reintroduced override" {
    local dir="${BATS_TEST_TMPDIR}/reintroduced"
    mkdir -p "$dir"
    cat > "$dir/ui_overlay_thing.h" <<'EOF'
class ThingOverlay : public OverlayBase {
    void on_deactivate() override;
};
EOF

    run check_no_on_deactivate_overrides "$dir"
    [ "$status" -eq 1 ]
    [[ "$output" == *"on_deactivating(DeactivateReason)"* ]]
}

@test "the on_deactivate gate fires on an out-of-line definition" {
    local dir="${BATS_TEST_TMPDIR}/outofline"
    mkdir -p "$dir"
    cat > "$dir/ui_overlay_thing.cpp" <<'EOF'
void ThingOverlay::on_deactivate() {
    stop_scanning();
}
EOF

    run check_no_on_deactivate_overrides "$dir"
    [ "$status" -eq 1 ]
    [[ "$output" == *"ui_overlay_thing.cpp"* ]]
}

@test "the on_deactivate gate stays quiet on the hook, PanelWidget and prose" {
    local dir="${BATS_TEST_TMPDIR}/quiet"
    mkdir -p "$dir"
    cat > "$dir/ui_overlay_migrated.h" <<'EOF'
/// Fires when the overlay is popped; on_deactivate() reaches this via the base.
class MigratedOverlay : public OverlayBase {
  protected:
    void on_deactivating(DeactivateReason reason) override;
};
EOF
    cat > "$dir/clock_widget.h" <<'EOF'
class ClockWidget : public PanelWidget {
    void on_deactivate() override;
};
EOF
    cat > "$dir/panel_lifecycle.h" <<'EOF'
class ViewLifecycleBase : public IPanelLifecycle {
  public:
    void on_deactivate(DeactivateReason reason) final;
};
EOF

    run check_no_on_deactivate_overrides "$dir"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the cleanup gate fires on a log-only override" {
    local dir="${BATS_TEST_TMPDIR}/logonly"
    mkdir -p "$dir"
    cat > "$dir/ui_overlay_thing.h" <<'EOF'
class ThingOverlay : public OverlayBase {
  public:
    void cleanup() override;
};
EOF
    cat > "$dir/ui_overlay_thing.cpp" <<'EOF'
void ThingOverlay::cleanup() {
    spdlog::trace("[Thing] Cleanup");
}
EOF

    run check_overlay_cleanup_calls_base "$dir"
    [ "$status" -eq 1 ]
    [[ "$output" == *"ui_overlay_thing.cpp:1"* ]]
}

@test "the cleanup gate fires on an inline header override" {
    local dir="${BATS_TEST_TMPDIR}/inline"
    mkdir -p "$dir"
    cat > "$dir/ui_overlay_thing.h" <<'EOF'
class ThingOverlay : public OverlayBase {
  public:
    void cleanup() override {
        stop_polling();
    }
};
EOF

    run check_overlay_cleanup_calls_base "$dir"
    [ "$status" -eq 1 ]
    [[ "$output" == *"ui_overlay_thing.h:3"* ]]
}

@test "the cleanup gate reads each override separately, not the file as a whole" {
    # One correct override in a file does not vouch for a second one beside it.
    local dir="${BATS_TEST_TMPDIR}/twobodies"
    mkdir -p "$dir"
    cat > "$dir/ui_overlay_pair.h" <<'EOF'
class FirstOverlay : public OverlayBase {
    void cleanup() override;
};
class SecondOverlay : public OverlayBase {
    void cleanup() override;
};
EOF
    cat > "$dir/ui_overlay_pair.cpp" <<'EOF'
void FirstOverlay::cleanup() {
    stop_polling();
    OverlayBase::cleanup();
}

void SecondOverlay::cleanup() {
    stop_polling();
}
EOF

    run check_overlay_cleanup_calls_base "$dir"
    [ "$status" -eq 1 ]
    [[ "$output" == *"ui_overlay_pair.cpp:6"* ]]
}

@test "the cleanup gate stays quiet on a base call and on the other hierarchies" {
    local dir="${BATS_TEST_TMPDIR}/quietcleanup"
    mkdir -p "$dir"
    cat > "$dir/ui_overlay_good.h" <<'EOF'
class GoodOverlay : public OverlayBase {
  public:
    void cleanup() override;
};
EOF
    cat > "$dir/ui_overlay_good.cpp" <<'EOF'
void GoodOverlay::cleanup() {
    if (timer_) {
        lv_timer_cancel_safe(&timer_);
    }
    OverlayBase::cleanup();
}
EOF
    cat > "$dir/ui_wizard_step_thing.cpp" <<'EOF'
void WizardThingStep::cleanup() {
    spdlog::trace("[WizardThing] Cleanup");
}
EOF
    cat > "$dir/ui_thing_list_view.h" <<'EOF'
class ThingListView : public ContainerDeleteNet {
  public:
    void cleanup() {
        rows_.clear();
    }
};
EOF

    run check_overlay_cleanup_calls_base "$dir"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the Z-offset publisher gate fires when the call is missing or too early" {
    local d="${BATS_TEST_TMPDIR}/zoffset_wiring"
    mkdir -p "$d"
    printf '%s\n' 'void f() {' '    init_ams_subjects();' '}' > "$d/missing.cpp"
    run zoffset_publisher_wiring "$d/missing.cpp"
    [ "$status" -eq 0 ]
    contains "never called" "$output"

    printf '%s\n' 'void f() {' '    helix::zoffset::init_save_available_subject();' \
        '    init_ams_subjects();' '}' > "$d/early.cpp"
    run zoffset_publisher_wiring "$d/early.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"before init_ams_subjects"* ]]
}

@test "the Z-offset save-rule gate fires on a reintroduced inline condition" {
    local d="${BATS_TEST_TMPDIR}/zoffset_rule_copy"
    mkdir -p "$d"
    cat > "$d/offender.xml" <<'EOF'
<ui_button name="btn_save_z_offset">
  <bind_flag_if cond="z_offset_can_save and (gcode_z_offset ne 0 or any_tool_z_dirty)" flag="hidden" invert="true"/>
</ui_button>
EOF
    run zoffset_save_rule_copies "$d" --include='*.xml'
    [ "$status" -eq 0 ]
    [[ "$output" == *"offender.xml"* ]]
}

@test "the Z-offset save-rule gate stays quiet on the published binding" {
    local d="${BATS_TEST_TMPDIR}/zoffset_rule_ok"
    mkdir -p "$d"
    cat > "$d/ok.xml" <<'EOF'
<ui_button name="btn_save_z_offset">
  <bind_flag_if_eq subject="z_offset_save_available" flag="hidden" ref_value="0"/>
</ui_button>
EOF
    run zoffset_save_rule_copies "$d" --include='*.xml'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the Z-offset save-surface gate fires on a button that binds nothing" {
    local d="${BATS_TEST_TMPDIR}/zoffset_unbound"
    mkdir -p "$d"
    printf '%s\n' '<ui_button name="btn_save_z_offset" hidden="true"/>' > "$d/offender.xml"

    run zoffset_save_buttons_unbound "$d/offender.xml"
    [ "$status" -eq 0 ]
    [[ "$output" == *"offender.xml"* ]]
}

@test "the weight-poll gate fires when the poll writes a whole SlotInfo" {
    # Meta-test: a gate that cannot fail is not a gate.
    local mutated="${BATS_TEST_TMPDIR}/spoolman_manager_whole_struct.cpp"
    local filing='helix::ams::ingest(lane, stated);'
    # A mutation whose target has moved changes nothing and passes vacuously.
    grep -qF "$filing" src/printer/spoolman_manager.cpp
    sed -e 's@helix::ams::ingest(lane, stated);@owner.sync_external_identity(slot_index, slot);@' \
        src/printer/spoolman_manager.cpp > "$mutated"

    run check_weight_poll_is_weight_only "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"helix::ams::ingest"* ]]
}

@test "the weight-poll gate fails closed when the file is missing" {
    run check_weight_poll_is_weight_only "${BATS_TEST_TMPDIR}/does_not_exist.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not locate"* ]]
}

@test "the lane store friend gate fires when a third writer is added" {
    local mutated="${BATS_TEST_TMPDIR}/lane_store_three_friends.h"
    sed -e 's@^\([[:space:]]*\)friend void ingest(LaneId, const Observation\&);@\1friend void ingest(LaneId, const Observation\&);\n\1friend void sync_from_backend(LaneId, const Observation\&);@' \
        include/lane_source_store.h > "$mutated"

    run check_friend_list "$mutated" "$LANE_STORE_FRIENDS_EXPECTED"
    [ "$status" -eq 1 ]
    [[ "$output" == *"expected exactly 2"* ]]
}

@test "the lane store friend gate fires when a funnel is swapped for another writer" {
    # A count is not the invariant; which two names hold the grant is. A
    # substitution keeps the list at two and hands write() to code the funnels
    # do not cover.
    local mutated="${BATS_TEST_TMPDIR}/lane_store_swapped_friend.h"
    sed -e 's@^\([[:space:]]*\)friend void commit_slot_edit(LaneId, const Observation\&);@\1friend void sync_from_backend(LaneId, const Observation\&);@' \
        include/lane_source_store.h > "$mutated"

    [ "$(grep -cE '^[[:space:]]*friend ' "$mutated")" -eq 2 ]

    run check_friend_list "$mutated" "$LANE_STORE_FRIENDS_EXPECTED"
    [ "$status" -eq 1 ]
    [[ "$output" == *"sync_from_backend"* ]]
}

@test "the lane store write-declaration gate fails closed when write() is renamed" {
    local mutated="${BATS_TEST_TMPDIR}/lane_store_renamed_write.h"
    sed -e 's@void write(LaneId lane, const Observation\& obs, bool amend);@void set(LaneId lane, const Observation\& obs, bool amend);@' \
        include/lane_source_store.h > "$mutated"

    run check_lane_store_write_declared "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"not declared where the friend list guards it"* ]]
}

@test "the lane store write-declaration gate fails closed when the file does not exist" {
    run check_lane_store_write_declared "${BATS_TEST_TMPDIR}/does_not_exist.h"
    [ "$status" -eq 1 ]
    [[ "$output" == *"not declared where the friend list guards it"* ]]
}

@test "the lane store singleton gate fires on a new caller" {
    local dir="${BATS_TEST_TMPDIR}/fake_src"
    mkdir -p "$dir"
    cat > "$dir/ams_backend_example.cpp" <<'EOF'
void refresh() {
    helix::ams::LaneSourceStore::instance().get(0);
}
EOF

    run check_no_lane_store_instance_callers "$dir"
    [ "$status" -eq 1 ]
    [[ "$output" == *"reachable only from the funnels"* ]]
}

@test "the lane store singleton gate fails closed when given no directory to search" {
    run check_no_lane_store_instance_callers "${BATS_TEST_TMPDIR}/does_not_exist"
    [ "$status" -eq 1 ]
    [[ "$output" == *"does not exist"* ]]
}

@test "the friend-list gate ignores harmless reordering of the same friends" {
    local mutated="${BATS_TEST_TMPDIR}/lane_store_reordered_friends.h"
    sed -e 's@^\([[:space:]]*\)friend void ingest(LaneId, const Observation\&);@\1PLACEHOLDER_SWAP@' \
        -e 's@^\([[:space:]]*\)friend void commit_slot_edit(LaneId, const Observation\&);@\1friend void ingest(LaneId, const Observation\&);@' \
        -e 's@^\([[:space:]]*\)PLACEHOLDER_SWAP@\1friend void commit_slot_edit(LaneId, const Observation\&);@' \
        include/lane_source_store.h > "$mutated"

    run check_friend_list "$mutated" "$LANE_STORE_FRIENDS_EXPECTED"
    [ "$status" -eq 0 ]
}

@test "the write-privacy gate fires when write() moves to public with the friend list untouched" {
    local mutated="${BATS_TEST_TMPDIR}/lane_store_public_write.h"
    sed '/^[[:space:]]*void write(LaneId lane, const Observation& obs, bool amend);$/d' \
        include/lane_source_store.h \
        | sed 's/^  public:$/  public:\n    void write(LaneId lane, const Observation\& obs, bool amend);/' \
        > "$mutated"

    # The friend list itself is unwidened by this mutation, so both gates that
    # read declaration text only stay quiet on it.
    run check_friend_list "$mutated" "$LANE_STORE_FRIENDS_EXPECTED"
    [ "$status" -eq 0 ]
    run check_lane_store_write_declared "$mutated"
    [ "$status" -eq 0 ]

    run check_lane_store_write_is_private "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"declared public"* ]]
}

@test "the write-privacy gate fails closed when class LaneSourceStore is not found" {
    local mutated="${BATS_TEST_TMPDIR}/lane_store_no_class.h"
    printf '%s\n' 'namespace helix::ams { void ingest(int, int); }' > "$mutated"

    run check_lane_store_write_is_private "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"was not found"* ]]
}

@test "the write-privacy gate fails closed when write() is missing from the class" {
    local mutated="${BATS_TEST_TMPDIR}/lane_store_write_gone.h"
    grep -v '^[[:space:]]*void write(LaneId lane, const Observation& obs, bool amend);$' \
        include/lane_source_store.h > "$mutated"

    run check_lane_store_write_is_private "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"was not found"* ]]
}

@test "the mock publish gate fires when a second call site is added" {
    local mutated="${BATS_TEST_TMPDIR}/mock_two_publishes.cpp"
    sed -e 's@^\([[:space:]]*\)publish_lane_observations();@\1publish_lane_observations();\n\1publish_lane_observations();@' \
        src/printer/ams_backend_mock.cpp > "$mutated"

    run check_mock_publishes_lane_readings_once "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"found 2"* ]]
}

@test "the mock publish gate fails closed when the method is renamed" {
    local mutated="${BATS_TEST_TMPDIR}/mock_renamed_publish.cpp"
    sed -e 's@publish_lane_observations@refile_lane_readings@g' \
        src/printer/ams_backend_mock.cpp > "$mutated"

    run check_mock_publishes_lane_readings_once "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"is not defined in"* ]]
}

@test "the mock publish gate fails closed when the file is missing" {
    run check_mock_publishes_lane_readings_once "${BATS_TEST_TMPDIR}/does_not_exist.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not locate"* ]]
}

# --- Hand-built tool labels and lane/slot offsets must route through display_numbering.h ---
# include/display_numbering.h keeps the storage-index -> display-number `+ 1`
# in exactly one function (helix::ui::lane_number()) and spells a gcode tool's
# label through exactly one function (helix::ui::tool_label()). Both are one
# line to rebuild by hand, which is exactly how they drift: one call site
# gets fixed and a sibling built the old way does not.
#
# scripts/check_tool_labels.sh is the gate. It runs as its own process here,
# the same way it runs from a shell prompt or CI, rather than sourcing its
# internals - SCAN_ROOT redirects it at a fixture directory instead of the
# real tree.

run_tool_label_gate() {
    SCAN_ROOT="$1" bash "$BATS_TEST_DIRNAME/../../scripts/check_tool_labels.sh"
}

@test "the tool label gate catches each forbidden shape" {
    # Meta-test: a gate that cannot fail is not a gate.
    local d="${BATS_TEST_TMPDIR}/offenders"
    mkdir -p "$d"
    cat > "$d/a.cpp" <<'EOF'
void a(char* b, int i) { snprintf(b, 8, "T%d", i); }
void c(char* b, int i) { snprintf(b, 8, "T{}", i); }
void e(int i) { auto s = "T" + std::to_string(i); }
void f(int slot_index) { auto s = fmt::format("Slot {}", slot_index + 1); }
void g(int backup) { auto s = fmt::format("Slot {} matches.", backup + 1); }
EOF
    run run_tool_label_gate "$d"
    [ "$status" -eq 1 ]
    contains "T%d" "$output"
    contains "T{}" "$output"
    contains "std::to_string(i)" "$output"
    contains "slot_index + 1" "$output"
    contains "backup + 1" "$output"
}

@test "the tool label gate catches the spellings the tree actually uses" {
    # sizeof(buf) and lv_tr(...) each put a close paren between the formatting
    # call's open paren and the index; a pattern that bridges the gap with
    # [^)]* stops dead at that paren. These are the spellings of nearly every
    # real snprintf and fmt::format site, so a fixture without them certifies
    # nothing about the tree.
    local d="${BATS_TEST_TMPDIR}/real_spellings"
    mkdir -p "$d"
    cat > "$d/a.cpp" <<'EOF'
void h(char* buf, int slot_index) { snprintf(buf, sizeof(buf), "Slot %d", slot_index + 1); }
void i(int lane) { auto s = fmt::format(lv_tr("Lane {}"), lane + 1); }
void j(int gate) { lv_label_set_text_fmt(label, "Gate %d", gate + 1); }
EOF
    run run_tool_label_gate "$d"
    [ "$status" -eq 1 ]
    contains "slot_index + 1" "$output"
    contains "lane + 1" "$output"
    contains "gate + 1" "$output"
}

@test "the tool label gate fails closed when the scan root is missing" {
    run run_tool_label_gate "${BATS_TEST_TMPDIR}/does_not_exist"
    [ "$status" -eq 1 ]
}

@test "the tool label gate catches an assignment that feeds a formatting call a few lines later" {
    # Shape 3: shape 2 only sees the arithmetic and the formatting call
    # together on one physical line. Splitting the assignment out reaches the
    # same display text and is the same violation.
    local d="${BATS_TEST_TMPDIR}/split_offenders"
    mkdir -p "$d"
    cat > "$d/a.cpp" <<'EOF'
void f(int slot_index) {
    int display_slot = slot_index + 1;
    std::string label = "unit";
    do_other_work();
    snprintf(tmp, sizeof(tmp), lv_tr("Current: Slot %d"), display_slot);
}
EOF
    run run_tool_label_gate "$d"
    [ "$status" -eq 1 ]
    contains "int display_slot = slot_index + 1;" "$output"
    contains "display_slot);" "$output"
}

@test "the tool label gate stays quiet on a protocol field id with no formatting call" {
    # The silent half of shapes 2 and 3: the arithmetic alone is not the
    # violation, and neither is assigning it to a variable that a formatting
    # call never reads. Only building display text from it without
    # lane_number() is - matching the real allowlisted wire-format shape in
    # filament_slot_override_store.cpp, which is 1-based on the wire by spec.
    local d="${BATS_TEST_TMPDIR}/plain_offset"
    mkdir -p "$d"
    cat > "$d/quiet.cpp" <<'EOF'
void f(int slot_index) {
    wire_fields[slot_index + 1] = 0;
    unit_addrs.push_back(unit_index + 1);
}
void g(int slot_index) {
    int idx = slot_index + 1;
    registers_[idx] = 0;
}
EOF
    run run_tool_label_gate "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the tool label gate stays quiet on an spdlog diagnostic containing + 1" {
    local d="${BATS_TEST_TMPDIR}/spdlog_diag"
    mkdir -p "$d"
    cat > "$d/quiet.cpp" <<'EOF'
void f(int slot_index) {
    spdlog::debug("[Thing] resolved lane {}", slot_index + 1);
}
EOF
    run run_tool_label_gate "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the tool label gate stays quiet on filament_slot_override_store.cpp-style wire-format code" {
    # The whole file is allowlisted: format_lane_key builds BOTH forbidden
    # shapes on purpose - a 1-based "laneN" text key over a 0-based inner
    # field, per docs/specs/filament_slots.md.
    local d="${BATS_TEST_TMPDIR}/wire_format"
    mkdir -p "$d"
    cat > "$d/filament_slot_override_store.cpp" <<'EOF'
std::string format_lane_key(LaneKeyStyle style, int slot_index) {
    return style == LaneKeyStyle::Tool ? "T" + std::to_string(slot_index)
                                       : "lane" + std::to_string(slot_index + 1);
}
EOF
    run run_tool_label_gate "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the tool label gate stays quiet on a T<n> gcode emitter in ams_backend_*.cpp" {
    # ams_backend_*.cpp is allowlisted for shape 1 only: a T<n> built there is
    # gcode sent to firmware, not a label.
    local d="${BATS_TEST_TMPDIR}/gcode_emitter"
    mkdir -p "$d"
    cat > "$d/ams_backend_example.cpp" <<'EOF'
void select_tool(int tool_number) {
    execute_gcode("T" + std::to_string(tool_number));
}
EOF
    run run_tool_label_gate "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the tool label gate escape hatch suppresses a genuine one-off" {
    local d="${BATS_TEST_TMPDIR}/hatch"
    mkdir -p "$d"
    cat > "$d/hatch.cpp" <<'EOF'
void f(int i) { snprintf(b, 8, "T%d", i); } // DISPLAY_NUMBERING_OK: desktop-only debug scratch tool
EOF
    run run_tool_label_gate "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the control-socket-directory lint fires on an inlined lookup and stays quiet on the helper" {
    local d="$BATS_TEST_TMPDIR/socket"
    mkdir -p "$d"
    printf 'const char* d = getenv("XDG_RUNTIME_DIR");\n' > "$d/inlined.cpp"
    printf 'const char* d = getenv("RUNTIME_DIRECTORY");\n' > "$d/systemd.cpp"
    printf 'const std::string d = control_socket_dir();\n' > "$d/ok.cpp"
    run control_socket_dir_offenders "$d/inlined.cpp" "$d/systemd.cpp" "$d/ok.cpp"
    [ "${#lines[@]}" -eq 2 ]
    lacks "ok.cpp" "$output"
}

getenv_pointer_live_across_setenv_files() {
    python3 scripts/check_getenv_setenv.py "$@"
}

@test "the getenv/setenv gate fails on a missing scan root" {
    # A renamed root would otherwise scan nothing and pass vacuously.
    run getenv_pointer_live_across_setenv_files "${BATS_TEST_TMPDIR}/no-such-root"
    [ "$status" -ne 0 ]
    contains "does not exist" "$output"
}

@test "the getenv/setenv gate fires on a pointer restored after a setenv" {
    # Meta-test: a gate that cannot fail is not a gate.
    local d="${BATS_TEST_TMPDIR}/stale"
    mkdir -p "$d"
    cat > "$d/restore.cpp" <<'EOF'
TEST_CASE("restores PATH through a stale pointer") {
    const char* original = std::getenv("PATH");
    setenv("PATH", "", 1);
    run_thing();
    if (original) {
        setenv("PATH", original, 1);
    }
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -ne 0 ]
    contains "restore.cpp" "$output"
    contains "std::string" "$output"
}

@test "the getenv/setenv gate catches wrapped declarations and east-const" {
    # clang-format splits a long declaration across lines, and `char const*`
    # spells the same type; the capture regex has to see both.
    local d="${BATS_TEST_TMPDIR}/spelled"
    mkdir -p "$d"
    cat > "$d/wrapped.cpp" <<'EOF'
TEST_CASE("declaration wraps past the getenv") {
    const char* original_environment_path =
        std::getenv("PATH");
    setenv("PATH", "", 1);
    run_thing();
    setenv("PATH", original_environment_path, 1);
}
EOF
    cat > "$d/east_const.cpp" <<'EOF'
TEST_CASE("east const") {
    char const* original = std::getenv("PATH");
    setenv("PATH", "", 1);
    run_thing();
    setenv("PATH", original, 1);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -ne 0 ]
    contains "wrapped.cpp" "$output"
    contains "east_const.cpp" "$output"
}

@test "the getenv/setenv gate catches auto-spelled pointer captures" {
    # `auto*` and a bare `auto` initialized by the getenv() call itself
    # deduce the same raw pointer `char*` spells.
    local d="${BATS_TEST_TMPDIR}/autos"
    mkdir -p "$d"
    cat > "$d/auto_star.cpp" <<'EOF'
TEST_CASE("auto pointer spelling") {
    auto* original = std::getenv("PATH");
    setenv("PATH", "", 1);
    run_thing();
    setenv("PATH", original, 1);
}
EOF
    cat > "$d/auto_value.cpp" <<'EOF'
TEST_CASE("auto by value is still a raw pointer") {
    auto mode = std::getenv("HELIX_MODE");
    setenv("HELIX_MODE", "1", 1);
    use(mode);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -ne 0 ]
    contains "auto_star.cpp" "$output"
    contains "auto_value.cpp" "$output"
}

@test "the getenv/setenv gate fires on a use later on the same line as the setenv" {
    # Only a read inside the setenv()'s own parentheses is an argument,
    # evaluated before the call runs; a read after the call completes is
    # post-setenv however close it sits.
    local d="${BATS_TEST_TMPDIR}/sameline"
    mkdir -p "$d"
    cat > "$d/same_line.cpp" <<'EOF'
TEST_CASE("a use hidden past an in-string url") {
    const char* base = std::getenv("HELIX_URL");
    setenv("HELIX_URL", "https://example.invalid/a//b", 1); if (base) { use(base); }
    do_work();
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -ne 0 ]
    contains "same_line.cpp" "$output"
}

@test "the getenv/setenv gate fires on a long same-function save/restore span" {
    # The window is a backstop, not the coverage edge: a save at the top of a
    # long Catch2 case and its restore at the bottom are one function and must
    # be caught however many lines sit between them.
    local d="${BATS_TEST_TMPDIR}/longspan"
    mkdir -p "$d"
    {
        echo 'TEST_CASE("long span") {'
        echo '    const char* original = std::getenv("PATH");'
        echo '    setenv("PATH", "", 1);'
        for _ in $(seq 70); do echo ''; done
        echo '    setenv("PATH", original, 1);'
        echo '}'
    } > "$d/long.cpp"
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -ne 0 ]
    contains "long.cpp" "$output"
}

@test "the getenv/setenv gate stays quiet on capture-time copies and immediate reads" {
    local d="${BATS_TEST_TMPDIR}/owned"
    mkdir -p "$d"
    cat > "$d/copied.cpp" <<'EOF'
TEST_CASE("copies the value out before the setenv") {
    std::string saved;
    if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
        saved = prev;
    }
    setenv("HELIX_CONFIG_DIR", "/tmp/x", 1);
    setenv("HELIX_CONFIG_DIR", saved.c_str(), 1);
}
EOF
    cat > "$d/immediate.cpp" <<'EOF'
TEST_CASE("reads and decides in one breath") {
    const char* existing = getenv("HELIX_CACHE_DIR");
    if (!existing || existing[0] == '\0')
        setenv("HELIX_CACHE_DIR", "/tmp/x", 1);
}
EOF
    cat > "$d/shared_guard.cpp" <<'EOF'
TEST_CASE("uses the shared guard") {
    helix::ScopedEnv env("PATH");
    setenv("PATH", "", 1);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the getenv/setenv gate stays quiet on a same-named local in a later function" {
    # The scope of a capture ends at the column-0 brace that closes its
    # function or class, so an unrelated `v` in the next function is a
    # different variable however near it sits.
    local d="${BATS_TEST_TMPDIR}/shadow"
    mkdir -p "$d"
    cat > "$d/shadow.cpp" <<'EOF'
void guard(const char* name) {
    const char* v = getenv(name);
    std::string saved = v;
    setenv(name, "", 1);
}

void unrelated_below() {
    for (auto v = table.begin(); v != table.end(); ++v) {
        use(*v);
    }
}
EOF
    cat > "$d/qualified.cpp" <<'EOF'
TEST_CASE("a member access on another object") {
    const char* original = std::getenv("PATH");
    std::string saved = original;
    setenv("PATH", "", 1);
    REQUIRE(cfg.original == 1);
    setenv("PATH", saved.c_str(), 1);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the getenv/setenv gate stays quiet on commented-out code and in-string slashes" {
    # Dead code inside /* */ cannot be a live bug, and a URL in a string
    # literal must not truncate the line's remaining text.
    local d="${BATS_TEST_TMPDIR}/deadcode"
    mkdir -p "$d"
    cat > "$d/commented.cpp" <<'EOF'
TEST_CASE("commented out") {
    setenv("X", "1", 1);
    /*
    const char* old = getenv("X");
    setenv("X", "", 1);
    setenv("X", old, 1);
    */
    do_work();
}
EOF
    cat > "$d/url.cpp" <<'EOF'
TEST_CASE("slashes inside a string literal") {
    const char* base = getenv("HELIX_URL");
    std::string saved = base;
    setenv("HELIX_URL", "https://example.invalid/a//b", 1);
    use(saved);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the getenv/setenv gate stays quiet on string mentions and sibling methods" {
    # A name inside a log string is not a use, and a capture dies at its
    # closing brace, so one inline method's local never reaches the next
    # method's same-named local.
    local d="${BATS_TEST_TMPDIR}/quiet3"
    mkdir -p "$d"
    cat > "$d/log_string.cpp" <<'EOF'
TEST_CASE("mentions a name inside a log string") {
    const char* home = std::getenv("HOME");
    std::string saved = home;
    setenv("HOME", "/tmp/x", 1);
    log("could not expand $home in the config path");
    setenv("HOME", saved.c_str(), 1);
}
EOF
    cat > "$d/inline_methods.cpp" <<'EOF'
class Registry {
  public:
    const char* first_dir() {
        const char* dir = std::getenv("HELIX_FIRST_DIR");
        std::string saved = dir;
        setenv("HELIX_FIRST_DIR", "/tmp", 1);
        return saved.c_str();
    }
    void second() {
        for (auto dir = dirs.begin(); dir != dirs.end(); ++dir) {
            total += dir->size();
        }
    }
};
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the getenv/setenv gate stays quiet on a re-assert and an auto copy" {
    # A read inside the setenv()'s parentheses is an argument, evaluated
    # before the call runs; an auto capture copied into owned storage
    # before any setenv is safe to keep using afterwards.
    local d="${BATS_TEST_TMPDIR}/quiet4"
    mkdir -p "$d"
    cat > "$d/reassert.cpp" <<'EOF'
TEST_CASE("re-asserts the current value") {
    const char* cur = std::getenv("HELIX_MODE");
    setenv("HELIX_MODE", cur, 1);
}
EOF
    cat > "$d/auto_copy.cpp" <<'EOF'
TEST_CASE("auto captured straight into owned storage") {
    auto existing = std::getenv("HELIX_CACHE_DIR");
    std::string saved = existing;
    setenv("HELIX_CACHE_DIR", "/tmp/x", 1);
    use(saved);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the getenv/setenv gate stays quiet on a qualified ::name" {
    # A capture named `path` is not read by std::filesystem::path::iterator;
    # a variable can never appear as a scope qualifier on either side.
    local d="${BATS_TEST_TMPDIR}/quiet5"
    mkdir -p "$d"
    cat > "$d/qualified.cpp" <<'EOF'
TEST_CASE("iterates a filesystem path") {
    const char* path = std::getenv("HELIX_PATH");
    std::string saved = path;
    setenv("OTHER", "x", 1);
    std::filesystem::path::iterator it;
    for (auto seg = it; seg != end(); ++seg) {
        total += seg->string().size();
    }
    setenv("HELIX_PATH", saved.c_str(), 1);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the getenv/setenv gate stays quiet on a wrapped re-assert" {
    # The re-assert idiom split across lines by clang-format keeps the same
    # meaning: the read is still an argument of the call it belongs to.
    local d="${BATS_TEST_TMPDIR}/quiet6"
    mkdir -p "$d"
    cat > "$d/wrapped_reassert.cpp" <<'EOF'
TEST_CASE("re-asserts a long variable name") {
    const char* cur = std::getenv("HELIX_EXTENDED_CONFIGURATION_MODE");
    setenv("HELIX_EXTENDED_CONFIGURATION_MODE",
           cur,
           1);
}
EOF
    cat > "$d/wrapped_arg_use.cpp" <<'EOF'
TEST_CASE("restores with a wrapped call") {
    const char* original = std::getenv("PATH");
    std::string saved = original;
    setenv("PATH", "", 1);
    system("true");
    setenv("PATH",
           saved.c_str(),
           1);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the getenv/setenv gate stays quiet on a for-init capture's sibling" {
    # A capture declared in a for-init header is scoped to the statement:
    # its closing brace retires it, so a same-named local in a later loop
    # of the same function is never read against the dead capture.
    local d="${BATS_TEST_TMPDIR}/quiet7"
    mkdir -p "$d"
    cat > "$d/for_init.cpp" <<'EOF'
TEST_CASE("walks env then a table") {
    for (const char* p = std::getenv("HELIX_LIST"); *p; ++p) {
        count += (*p == ',');
    }
    setenv("OTHER", "x", 1);
    for (int i = 0; i < 3; ++i) {
        const char* p = table[i];
        use(p);
    }
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the getenv/setenv gate fires on a capture, setenv and use on one line" {
    local d="${BATS_TEST_TMPDIR}/fire5"
    mkdir -p "$d"
    cat > "$d/one_line_all.cpp" <<'EOF'
TEST_CASE("compresses the whole bug into one line") {
    const char* p = std::getenv("X"); setenv("X", "1", 1); use(p);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 1 ]
    contains "'p' from getenv() is read at line 2" "$output"
}

@test "the getenv/setenv gate fires on a read after an earlier same-line setenv" {
    # The first setenv completes before the second call's arguments are
    # evaluated, so passing the capture to the second is already a stale read.
    local d="${BATS_TEST_TMPDIR}/fire6"
    mkdir -p "$d"
    cat > "$d/two_setenv_one_line.cpp" <<'EOF'
TEST_CASE("sets two variables in one statement") {
    const char* p = std::getenv("B");
    setenv("A", "1", 1); setenv("B", p, 1);
}
EOF
    run getenv_pointer_live_across_setenv_files "$d"
    [ "$status" -eq 1 ]
    contains "'p' from getenv() is read at line 3" "$output"
}

@test "the override-store lifecycle gate fires on a hand-written construction" {
    # A gate that cannot fail is not a gate. Three spellings, all of which
    # persist nothing: the direct assignment, the split build-then-move, and a
    # differently-named member in a backend that never says override_store_.
    local d="${BATS_TEST_TMPDIR}/offender"
    mkdir -p "$d"
    cat > "$d/ams_backend_thing.cpp" <<'EOF'
void AmsBackendThing::on_started() {
    override_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        api_, "thing", helix::ams::lane_key_style_for(get_type()));
}
void AmsBackendThing::restart() {
    auto s = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        api_, "thing", helix::ams::lane_key_style_for(get_type()));
    override_store_ = std::move(s);
}
void AmsBackendThing::third() {
    slot_store_.reset(new helix::ams::FilamentSlotOverrideStore(api_, "thing", style));
}
EOF
    run override_store_offenders "$d"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 3 ]
    contains "FilamentSlotOverrideStore" "$output"
}

@test "the override-store gate stays quiet on the helper and on the publish store" {
    # The silent half: the sanctioned spelling moves a store the helper built,
    # and the publish store is a different member with no map behind it.
    local d="${BATS_TEST_TMPDIR}/quiet"
    mkdir -p "$d"
    cat > "$d/ams_backend_ok.cpp" <<'EOF'
void AmsBackendOk::on_started() {
    auto loaded = helix::ams::make_loaded_override_store(api_, "ok", get_type(),
                                                         backend_log_tag());
    std::lock_guard<std::mutex> lock(mutex_);
    override_store_ = std::move(loaded.store);
    overrides_ = std::move(loaded.overrides);
    lane_publish_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        api_, "ok", style, "lane_data");
}
EOF
    run override_store_offenders "$d"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the slot-validator gate fires on a backend-local copy" {
    local d="${BATS_TEST_TMPDIR}/offender"
    mkdir -p "$d"
    cat > "$d/ams_backend_thing.h" <<'EOF'
class AmsBackendThing : public AmsSubscriptionBackend {
    AmsError validate_slot_index(int slot_index) const;
};
EOF
    run slot_validator_offenders "$d"
    [ "$status" -eq 0 ]
    contains "validate_slot_index" "$output"
}

@test "the own-endpoint gate fires on a plain requests:: call" {
    local d="${BATS_TEST_TMPDIR}/offender/src/system"
    mkdir -p "$d"
    printf '    // requests::request is fine in a comment\n' > "$d/update_checker.cpp"
    printf '    auto r = requests::request(req);\n' > "$d/telemetry_manager.cpp"
    printf '    hv::HttpClient cli;\n' > "$d/crash_reporter.cpp"
    printf '    req->headers["X-API-Key"] = kApiKey;\n' > "$d/ingest_client.cpp"
    mkdir -p "$d/../ui"
    printf 'auto u = "https://api.github.com/x";\nauto r = requests::get(u);\n' > "$d/../ui/rogue.cpp"
    printf 'auto u = "https://helixscreen.org/x";\nauto r = helix::tls::trusted_request(q);\n' \
        > "$d/../ui/fine.cpp"
    printf 'auto u = "https://helixscreen.org/docs";\n' > "$d/../ui/link_only.cpp"
    printf '    req->headers["X-API-Key"] = key;\n' > "$d/../ui/keyed.cpp"
    printf 'int f() {\n    auto r = requests::request(req);\n}\nvoid up() {\n    const std::string url = worker_url();\n    auto r = requests::request(req);\n}\n' \
        > "$d/debug_bundle_collector.cpp"
    run own_endpoint_http_offenders "${BATS_TEST_TMPDIR}/offender"
    [ "$status" -eq 0 ]
    contains "telemetry_manager.cpp" "$output"
    contains "debug_bundle_collector.cpp:6" "$output"
    contains "crash_reporter.cpp" "$output"
    contains "src/ui/rogue.cpp" "$output"
    lacks "fine.cpp" "$output"
    lacks "link_only.cpp" "$output"
    lacks "update_checker.cpp" "$output"
    lacks "debug_bundle_collector.cpp:2" "$output"
    contains "keyed.cpp" "$output"
    lacks "ingest_client.cpp" "$output"
}

@test "the home-widget flex-style gate fires on a style without layout flex" {
    local d="${BATS_TEST_TMPDIR}/flex_style"
    mkdir -p "$d"
    printf '%s\n' '<style name="x_inline" flex_flow="row" flex_main_place="start"/>' > "$d/bad.xml"
    run home_widget_flex_styles_without_layout "$d/bad.xml"
    [ "$status" -eq 0 ]
    [[ "$output" == *"x_inline"* ]]
}

# --- Vendor branches in generic code ---
# A vendor name belongs to one module per capability (.claude/rules/vendor-abstraction.md).
# The ratchet lets counts fall and never rise. qc_vendor_branches in
# quality-checks.sh runs it over the tree; these cases pin the script.

@test "the vendor-branch gate catches each shape and honours VENDOR_OK" {
    local root="$BATS_TEST_TMPDIR/root"
    mkdir -p "$root/src/ui" "$root/scripts"
    : > "$root/scripts/vendor_branch_baseline.txt"
    cat > "$root/src/ui/x.cpp" <<'CPP'
#if defined(HELIX_PLATFORM_K2)
#endif
bool a(AmsType t) { return t == AmsType::CFS; }
bool b() { return flashforge_machine_id().has_value(); }
CPP
    run python3 scripts/check_vendor_branches.py --root "$root"
    [ "$status" -eq 1 ]
    contains "x.cpp:platform-macro" "$output"
    contains "x.cpp:amstype-branch" "$output"
    contains "x.cpp:board-probe" "$output"

    cat > "$root/src/ui/x.cpp" <<'CPP'
// VENDOR_OK: test
#if defined(HELIX_PLATFORM_K2)
#endif
// VENDOR_OK: test
bool a(AmsType t) { return t == AmsType::CFS; }
bool b() { return flashforge_machine_id().has_value(); } // VENDOR_OK: test
CPP
    run python3 scripts/check_vendor_branches.py --root "$root"
    [ "$status" -eq 0 ]
}

# The ratchet compares per-file counts to the baseline.
@test "the vendor-branch gate fails above a nonzero baseline and passes at or below it" {
    local root="$BATS_TEST_TMPDIR/ratchet"
    mkdir -p "$root/src/ui"
    printf 'bool a(AmsType t) { return t == AmsType::CFS; }\nbool b(AmsType t) { return t != AmsType::AFC; }\n' \
        > "$root/src/ui/x.cpp"
    printf '1 src/ui/x.cpp:amstype-branch\n' > "$root/base.txt"
    run python3 scripts/check_vendor_branches.py --root "$root" --baseline "$root/base.txt"
    [ "$status" -eq 1 ] || fail "two branches over a baseline of one passed"
    printf '2 src/ui/x.cpp:amstype-branch\n' > "$root/base.txt"
    run python3 scripts/check_vendor_branches.py --root "$root" --baseline "$root/base.txt"
    [ "$status" -eq 0 ] || fail "two branches at a baseline of two failed: $output"
    printf '3 src/ui/x.cpp:amstype-branch\n' > "$root/base.txt"
    run python3 scripts/check_vendor_branches.py --root "$root" --baseline "$root/base.txt"
    [ "$status" -eq 0 ] || fail "below baseline failed: $output"
    contains "lowered" "$output"
}

@test "the vendor-branch gate is quiet on AmsType::NONE and on build-shape macros" {
    local root="$BATS_TEST_TMPDIR/quiet"
    mkdir -p "$root/src/ui"
    cat > "$root/src/ui/x.cpp" <<'CPP'
bool a(AmsType t) { return t == AmsType::NONE; }
#if defined(HELIX_PLATFORM_ESP32)
#endif
CPP
    run python3 scripts/check_vendor_branches.py --root "$root"
    [ "$status" -eq 0 ] || fail "quiet cases reported: $output"
}

@test "the vendor-branch gate reads multi-line conditionals and honours VENDOR_OK above them" {
    local root="$BATS_TEST_TMPDIR/multi"
    mkdir -p "$root/src/ui"
    printf '#if defined(HELIX_PLATFORM_K2) || \\\n    defined(HELIX_PLATFORM_K1)\n#endif\n' > "$root/src/ui/x.cpp"
    run python3 scripts/check_vendor_branches.py --root "$root"
    [ "$status" -eq 1 ] || fail "multi-line conditional not counted"
    printf '// VENDOR_OK: test\n#if defined(HELIX_PLATFORM_K2) || \\\n    defined(HELIX_PLATFORM_K1)\n#endif\n' > "$root/src/ui/x.cpp"
    run python3 scripts/check_vendor_branches.py --root "$root"
    [ "$status" -eq 0 ] || fail "VENDOR_OK above a multi-line conditional ignored: $output"
}

@test "the vendor-branch gate catches a name() compare written either way round" {
    local root="$BATS_TEST_TMPDIR/names"
    mkdir -p "$root/src/ui"
    printf 'bool a(X* x) { return "AFC" == x->name(); }\nbool b(X x) { return x.name() == "AFC"; }\n' > "$root/src/ui/x.cpp"
    run python3 scripts/check_vendor_branches.py --root "$root"
    [ "$status" -eq 1 ] || fail "name() compares not counted"
    contains "2 branch" "$output"
}

@test "the short_clicked gate fires on an XML trigger and a C++ registration" {
    local d="${BATS_TEST_TMPDIR}/short_clicked"
    mkdir -p "$d/xml" "$d/src"
    printf '%s\n' '<lv_obj><event_cb trigger="short_clicked" callback="on_tap"/></lv_obj>' \
        > "$d/xml/offender.xml"
    printf '%s\n' 'void f(lv_obj_t* o) { lv_obj_add_event_cb(o, cb, LV_EVENT_SHORT_CLICKED, nullptr); }' \
        > "$d/src/offender.cpp"
    run short_clicked_taps "$d/xml" "$d/src"
    [ "$status" -eq 0 ]
    contains "offender.xml" "$output"
    contains "offender.cpp" "$output"
}

@test "the app-skip lint catches a hand-rolled skip and passes require_app" {
    local f="$BATS_TEST_TMPDIR/shapes.bats"
    cat >"$f" <<'SHAPES'
    [ -x "$BIN" ] || skip "helix-screen missing"
    [ -x ./build/bin/helix-screen ] || skip "no app"
    require_app "$BIN"
    [ -x "$PY" ] || skip "translations venv not set up"
    # the skip_count of helix-screen frames
    ./build/bin/helix-screen --test --skip-wizard
SHAPES
    run app_skip_offenders "$f"
    [ "${#lines[@]}" -eq 2 ]
    lacks "require_app" "$output"
    lacks "venv" "$output"
}

init_list_ternary_offenders() {
    python3 scripts/check_init_list_ternary.py "$@"
}

@test "the initializer_list ternary gate flags stored, braced and member picks and stays quiet on a call argument" {
    local f="${BATS_TEST_TMPDIR}/init_list_shapes.cpp"
    cat > "$f" <<'SHAPES'
void stored(bool c) {
    const auto keys = c ? std::initializer_list<const char*>{"a", "b"}
                        : std::initializer_list<const char*>{"a"};
}
std::initializer_list<int> returned(bool c) {
    return c ? std::initializer_list<int>{1} : std::initializer_list<int>{2};
}
void argument(bool c) {
    check_opts(L, 1,
               c ? std::initializer_list<const char*>{"fill", "radius"}
                 : std::initializer_list<const char*>{"fill"});
    const bool ok = known(c ? std::initializer_list<int>{1} : std::initializer_list<int>{2});
    c ? std::initializer_list<int>{1} : std::initializer_list<int>{2};
}
void braced(bool c) {
    std::initializer_list<int> k{c ? std::initializer_list<int>{1} : std::initializer_list<int>{2}};
}
struct Keys {
    Keys(bool c) : n_(0), keys_(c ? std::initializer_list<int>{1} : std::initializer_list<int>{2}) {}
    Keys(int c) : keys_{c ? std::initializer_list<int>{1} : std::initializer_list<int>{2}} {}
    std::initializer_list<int> keys_;
    int n_;
};
void braced_argument(bool c) {
    use({c ? std::initializer_list<int>{1} : std::initializer_list<int>{2}});
    if (known(c ? std::initializer_list<int>{1} : std::initializer_list<int>{2})) {}
}
void copied(bool c) {
    std::vector<int> v = c ? std::initializer_list<int>{1} : std::initializer_list<int>{2};
    std::vector<int> w{c ? std::initializer_list<int>{1} : std::initializer_list<int>{2}};
}
void hatch(bool c) {
    // INIT_LIST_TERNARY_OK: test fixture
    auto k = c ? std::initializer_list<int>{1} : std::initializer_list<int>{2};
}
// Prose: c ? std::initializer_list<int>{1} : std::initializer_list<int>{2} = bad
std::initializer_list<int> plain = {1, 2};
SHAPES
    run init_list_ternary_offenders "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 5 ]
    contains "init_list_shapes.cpp:2:" "$output"
    contains "init_list_shapes.cpp:6:" "$output"
    contains "init_list_shapes.cpp:16:" "$output"
    contains "init_list_shapes.cpp:19:" "$output"
    contains "init_list_shapes.cpp:20:" "$output"
    lacks "check_opts" "$output"
    lacks ":10:" "$output"
    lacks ":12:" "$output"
    lacks ":13:" "$output"
    lacks ":25:" "$output"
    lacks ":26:" "$output"
    lacks ":29:" "$output"
    lacks ":30:" "$output"
    lacks ":34:" "$output"
}

@test "the initializer_list ternary gate flags the AFC lane-name keys as they were before the fix" {
    local f="${BATS_TEST_TMPDIR}/afc_status_parse.cpp"
    cp "$BATS_TEST_DIRNAME/fixtures/afc_status_parse_pre_40318bf81d.cpp.txt" "$f"
    run init_list_ternary_offenders "$f"
    [ "$status" -eq 0 ]
    contains "afc_status_parse.cpp:" "$output"
    contains "std::initializer_list<const char*>{\"name\", \"filament_name\"}" "$output"
}

@test "the address-key gate catches constant keys, and stays quiet on writable keys and char values" {
    local f="${BATS_TEST_TMPDIR}/key_shapes.cpp"
    cat > "$f" <<'SHAPES'
const char kStateKey = 0;
static const char kStaticKey = 0;
constexpr char kConstexprKey{};
const unsigned char kByteKey = 0;
const RegistryKey kConstRegistry;
static const helix::plugin::RegistryKey kQualified;
RegistryKey const kEastConst;
RegistryKey g_state_key{"state"};
static RegistryKey g_static_key{"static"};
char g_plain_key;
constexpr char kSeparator = '/';
const char* kName = "x";
const char kHatch = 0; // ADDRESS_KEY_OK: test fixture
// const char kInComment = 0;
SHAPES
    run code_offenders "$(address_key_pattern)" ADDRESS_KEY_OK "$f"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 7 ]
    contains "key_shapes.cpp:1: " "$output"
    contains "kEastConst" "$output"
    lacks "g_state_key" "$output"
    lacks "g_static_key" "$output"
    lacks "g_plain_key" "$output"
    lacks "kSeparator" "$output"
    lacks "kName" "$output"
    lacks "ADDRESS_KEY_OK" "$output"
    lacks "kInComment" "$output"
}

@test "the _for_testing lints fire on production code and skip mocks" {
    fixture_root
    printf 'void reset_for_testing();\n' | put include/thing.h
    printf 'void reset_for_testing();\n' | put include/thing_mock.h
    run lint_for_testing_headers
    [ "$status" -eq 1 ]
    contains "include/thing.h" "$output"
    lacks "thing_mock.h" "$output"
    printf 'void Thing::reset_for_testing() {}\n' | put src/thing.cpp
    run lint_for_testing_sources
    [ "$status" -eq 1 ]
    rm include/thing.h src/thing.cpp
    run lint_for_testing_headers
    [ "$status" -eq 0 ]
    run lint_for_testing_sources
    [ "$status" -eq 0 ]
}

@test "the advance_for_test lint allows only BusyClock's declaration" {
    fixture_root
    printf '    static void advance_for_test(int ms);\n' | put include/idle_timeout_busy.h
    mkdir -p src
    run lint_busyclock_advance_for_test
    [ "$status" -eq 0 ]
    printf 'void f() { BusyClock::advance_for_test(5); }\n' | put src/idle.cpp
    run lint_busyclock_advance_for_test
    [ "$status" -eq 1 ]
    contains "src/idle.cpp" "$output"
}

@test "the raw set_temperature lint fires on direct sends and stays quiet on the controller" {
    local f="$BATS_TEST_TMPDIR/temps.cpp"
    cat > "$f" <<'EOF'
api_->set_temperature("extruder", 200, ok, err);
client->set_temperature(heater, 60);
controller->set_temperature(heater, 60);
EOF
    run raw_set_temperature_offenders "$f"
    contains "api_->set_temperature" "$output"
    contains "client->set_temperature" "$output"
    lacks "controller" "$output"
}

@test "the flush-stride lint fires on an inlined rule and on a missing helper call" {
    fixture_root
    printf 'int s = helix::flush_px_map_stride(buf, w, cf);\n' | put src/application/color_transform.cpp
    printf 'int s = helix::flush_px_map_stride(buf, w, cf);\n' | put src/application/display_manager.cpp
    printf 'return b->header.stride > 0 ? b->header.stride : fallback;\n' | put include/flush_stride.h
    run lint_flush_stride_helper
    [ "$status" -eq 0 ]
    printf 'int s = b->header.stride > 0 ? b->header.stride : f;\n' | put src/application/display_manager.cpp
    run lint_flush_stride_helper
    [ "$status" -eq 1 ]
    contains "inline stride rule" "$output"
    contains "display_manager.cpp does not call" "$output"
}

@test "the centidegree lint fires on tracked code and skips the changelog" {
    fixture_root
    git init -q .
    printf 'int centidegrees = 0;\n' | put src/temps.cpp
    printf 'Renamed centidegrees.\n' | put CHANGELOG.md
    git add src/temps.cpp CHANGELOG.md
    run lint_centidegree_misnomer
    [ "$status" -eq 1 ]
    contains "src/temps.cpp" "$output"
    lacks "CHANGELOG" "$output"
    printf 'int decidegrees = 0;\n' > src/temps.cpp
    git add src/temps.cpp
    run lint_centidegree_misnomer
    [ "$status" -eq 0 ]
}

@test "the inline decidegree lint catches x10 and x100 conversions and stays quiet on helpers" {
    local f="$BATS_TEST_TMPDIR/conv.cpp"
    cat > "$f" <<'EOF'
int deg = target / 10;
int deci = nozzle_temp * 10;
int shown = chamber_deci / 100;
int d = helix::units::deci_to_degrees(target);
int mm = value / 100;
int z = bed_mesh_z * 100;
// keypad value * 10 explains the scale
int pid = heater_count * 105;
EOF
    run inline_decidegree_offenders "$f"
    [ "${#lines[@]}" -eq 3 ]
    lacks "deci_to_degrees" "$output"
    lacks "value / 100" "$output"
    lacks "bed_mesh_z" "$output"
}

@test "the concrete Moonraker lint fires outside the network layer and skips prose and the allowlist" {
    fixture_root
    printf 'void f(MoonrakerAPI* api);\n' | put src/ui/ui_panel_thing.cpp
    printf '// split out of MoonrakerAPI::parse\nvoid g(helix::IMoonrakerAPI* api);\n' | put include/power_device_parse.h
    printf 'MoonrakerAPI::MoonrakerAPI() {}\n' | put src/api/moonraker_api_extra.cpp
    printf 'class MoonrakerAPIMock : public MoonrakerAPI {};\n' | put include/moonraker_api_mock.h
    run lint_moonraker_concrete_types
    [ "$status" -eq 1 ]
    contains "src/ui/ui_panel_thing.cpp:1" "$output"
    lacks "power_device_parse" "$output"
    lacks "moonraker_api_extra" "$output"
    lacks "moonraker_api_mock" "$output"
}

@test "the bare fmt lint catches a standalone fmt include and stays quiet on spdlog's" {
    local f="$BATS_TEST_TMPDIR/fmt.cpp"
    cat > "$f" <<'EOF'
#include <fmt/format.h>
#  include <fmt/ranges.h>
#include <spdlog/fmt/fmt.h>
// #include <fmt/core.h> in prose
#include <fmt/chrono.h> // FMT_INCLUDE_OK: test fixture
EOF
    run code_offenders "$(bare_fmt_include_pattern)" FMT_INCLUDE_OK "$f"
    [ "${#lines[@]}" -eq 2 ]
    lacks "spdlog" "$output"
    lacks "FMT_INCLUDE_OK" "$output"
}

@test "the Z-offset save-rule lint fails closed on a tree with no save surfaces" {
    fixture_root
    mkdir -p ui_xml
    printf '<lv_obj name="unrelated"/>\n' | put ui_xml/panel.xml
    run lint_zoffset_save_rule
    [ "$status" -eq 1 ]
    contains "fewer than 3" "$output"
}

@test "the OverlayBase cleanup lint fails closed when it finds too few overrides" {
    fixture_root
    printf 'void ThingOverlay::cleanup() { OverlayBase::cleanup(); }\n' | put src/ui/ui_overlay_thing.cpp
    mkdir -p include
    run lint_overlay_cleanup_calls_base
    [ "$status" -eq 1 ]
    contains "expected at least 10" "$output"
}

@test "the ui_nav_manager.h lint fails one includer over its limit" {
    local i
    fixture_root
    mkdir -p include
    for i in $(seq "$UI_NAV_MANAGER_INCLUDER_LIMIT"); do
        printf '#include "ui_nav_manager.h"\n' | put "src/ui/includer_$i.cpp"
    done
    printf '#include "ui_nav.h"\n' | put src/ui/narrow.cpp
    run lint_ui_nav_manager_includers
    [ "$status" -eq 0 ]
    printf '#include "ui_nav_manager.h"\n' | put src/ui/one_more.cpp
    run lint_ui_nav_manager_includers
    [ "$status" -eq 1 ]
    contains "limit $UI_NAV_MANAGER_INCLUDER_LIMIT" "$output"
}

@test "the ESP32 heap-walk lint fires on a walk in a per-event path and skips comments" {
    local f
    fixture_root
    for f in $ESP32_PER_EVENT_FILES; do printf 'int x = 0;\n' | put "$f"; done
    printf '// heap_caps_get_largest_free_block walks the heap\nsize_t n = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);\n' |
        put include/tinfl_inflate.h
    run lint_esp32_psram_walk
    [ "$status" -eq 0 ]
    printf 'size_t b = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);\n' | put src/ui/print/ui_panel_print_status.cpp
    run lint_esp32_psram_walk
    [ "$status" -eq 1 ]
    contains "ui_panel_print_status.cpp" "$output"
    printf 'int x = 0;\n' | put src/ui/print/ui_panel_print_status.cpp
    printf 'size_t n = 0;\n' | put include/tinfl_inflate.h
    run lint_esp32_psram_walk
    [ "$status" -eq 1 ]
    contains "no longer reads free PSRAM" "$output"
}

@test "the guided-flow lint fires on a family the builtin list or a boot path skips" {
    fixture_root
    printf 'void register_wifi_device_flows();\nvoid register_led_device_flows();\nvoid register_builtin_guided_device_flows();\n' |
        put include/ui_guided_device_flow.h
    printf 'void (*const kFlows[])() = {\n    register_wifi_device_flows,\n    register_led_device_flows,\n};\n' |
        put src/ui/ui_guided_device_flow.cpp
    printf '    register_builtin_guided_device_flows();\n' | put src/application/application.cpp
    printf '    register_builtin_guided_device_flows();\n' | put firmware/helixscreen-esp32/components/helixapp/app_boot.cpp
    run lint_guided_device_flows
    [ "$status" -eq 0 ] || fail "$output"
    printf 'void (*const kFlows[])() = {\n    register_wifi_device_flows,\n};\n' |
        put src/ui/ui_guided_device_flow.cpp
    printf 'void boot() {}\n' | put firmware/helixscreen-esp32/components/helixapp/app_boot.cpp
    run lint_guided_device_flows
    [ "$status" -eq 1 ]
    contains "register_led_device_flows" "$output"
    contains "app_boot.cpp does not call" "$output"
}

@test "the RegistryKey name lint names a name two keys share" {
    local d="$BATS_TEST_TMPDIR/keys"
    mkdir -p "$d"
    printf 'RegistryKey g_a_key{"state"};\nRegistryKey g_b_key{"other"};\n' > "$d/a.cpp"
    printf 'static RegistryKey g_c_key{"state"};\n' > "$d/b.cpp"
    run registry_key_duplicate_names "$d"
    [ "$output" = "state" ]
}

@test "the TestAccess lint names a class defined twice and fails closed on none" {
    local d="$BATS_TEST_TMPDIR/access"
    mkdir -p "$d"
    printf 'class ThingTestAccess {\n};\n' > "$d/a.cpp"
    printf 'class ThingTestAccess final {\n};\nstruct OtherTestAccess : Base {\n};\n' > "$d/b.cpp"
    run test_access_duplicates "$d"
    contains "ThingTestAccess:" "$output"
    lacks "OtherTestAccess" "$output"
    rm "$d/a.cpp" "$d/b.cpp"
    printf 'int x;\n' > "$d/c.cpp"
    run test_access_duplicates "$d"
    contains "no TestAccess class found" "$output"
}

@test "the chamber-target lint fires on the raw subject and stays quiet on the effective one" {
    fixture_root
    printf '<temp_display bind_target="chamber_effective_target" bind_mode="chamber_mode"/>\n' | put ui_xml/chamber.xml
    run lint_chamber_target_binding
    [ "$status" -eq 0 ]
    printf '<temp_display bind_target="chamber_target"/>\n' | put ui_xml/components/raw.xml
    run lint_chamber_target_binding
    [ "$status" -eq 1 ]
    contains "raw.xml" "$output"
}

@test "the plugin-separator lint catches file names, XML attributes and C++ registrations" {
    local d="$BATS_TEST_TMPDIR/sep"
    mkdir -p "$d/ui_xml" "$d/src/plugin"
    printf '<lv_obj/>\n' > "$d/ui_xml/my__panel.xml"
    printf '<lv_obj name="row__one"/>\n<bind_flag_if subject="a__b"/>\n<lv_obj name="row_one" text="a__b"/>\n' > "$d/ui_xml/ok.xml"
    printf 'lv_xml_register_subject(nullptr, "app__thing", &s);\nINIT_SUBJECT_INT(app__count, 0);\nUI_MANAGED_SUBJECT_INT(\n    s_, 0, "wrapped__name", m);\n' > "$d/src/registers.cpp"
    printf 'lv_xml_register_subject(nullptr, "plugin__owned", &s);\n' > "$d/src/plugin/host.cpp"
    run plugin_separator_offenders "$d/ui_xml" "$d/src"
    contains "my__panel.xml" "$output"
    contains 'name="row__one"' "$output"
    contains 'subject="a__b"' "$output"
    lacks 'text="a__b"' "$output"
    contains "app__thing" "$output"
    contains "app__count" "$output"
    contains "wrapped__name" "$output"
    lacks "plugin__owned" "$output"
}

@test "the filaments mirror lint fires on drift and passes when the mirror is absent" {
    fixture_root
    printf '{"a": 1}\n' | put assets/filaments.json
    run lint_filaments_android_mirror
    [ "$status" -eq 0 ]
    printf '{"a": 2}\n' | put android/app/src/main/assets/assets/filaments.json
    run lint_filaments_android_mirror
    [ "$status" -eq 1 ]
    contains "make regen-filaments" "$output"
}

# --- The driver and its wiring into quality-checks.sh ---

@test "every registered lint has a function and every lint function is registered" {
    local registered defined
    registered=$(code_lint_registry | awk '{print $2}' | sort)
    defined=$(declare -F | awk '{print $3}' | sed -n 's/^lint_//p' | sort)
    [ "$registered" = "$defined" ] || fail "registry and lint_ functions differ: $(diff <(echo "$registered") <(echo "$defined"))"
    [ "$(code_lint_registry | awk '{print $1}' | sort -u | paste -sd' ' -)" = "src tests ui" ]
}

@test "the driver names each failing lint and fails, and refuses an unknown lint or group" {
    lint_fake_pass() { :; }
    lint_fake_fail() { echo "offender.cpp:3: bad"; return 1; }
    run code_lint_run fake_pass fake_fail
    [ "$status" -eq 1 ]
    contains "FAIL fake_fail" "$output"
    contains "    offender.cpp:3: bad" "$output"
    lacks "FAIL fake_pass" "$output"
    contains "2 lint(s), 1 failed" "$output"
    run code_lint_run fake_pass
    [ "$status" -eq 0 ]
    run scripts/code_lint.sh no_such_lint
    [ "$status" -eq 2 ]
    run scripts/code_lint.sh --group no_such_group
    [ "$status" -eq 2 ]
}

@test "quality-checks.sh runs each code-lint group as its own gate, the source group at push" {
    local gate group
    eval "$(grep '^QC_ALL=' scripts/quality-checks.sh)"
    eval "$(grep '^QC_PUSH_ONLY=' scripts/quality-checks.sh)"
    for gate in code_lint:src test_lint:tests ui_lint:ui; do
        group=${gate#*:}
        gate=qc_${gate%%:*}
        [[ " $QC_ALL " == *" $gate "* ]] || fail "$gate is not in QC_ALL"
        grep -q "scripts/code_lint.sh --group $group " "scripts/qc/${gate#qc_}.sh" ||
            fail "$gate does not run the $group group"
    done
    [[ " $QC_PUSH_ONLY " == *" qc_code_lint "* ]] || fail "qc_code_lint is not push-only"
}

@test "a failing lint fails its quality-checks gate" {
    run bash -c '
        section_time() { :; }
        QC_TMP=$(mktemp -d)
        scripts_dir=$(mktemp -d)
        mkdir -p "$scripts_dir/scripts/qc"
        printf "#!/bin/bash\necho \"FAIL planted\"; exit 1\n" > "$scripts_dir/scripts/code_lint.sh"
        chmod +x "$scripts_dir/scripts/code_lint.sh"
        . scripts/qc/ui_lint.sh
        cd "$scripts_dir" && qc_ui_lint'
    [ "$status" -eq 1 ]
    contains "FAIL planted" "$output"
}

@test "a lint with no files, or a listed file that is gone, reports instead of passing" {
    run code_offenders "anything" ""
    contains "no files to scan" "$output"
    run code_offenders "anything" "" /dev/null
    contains "no files to scan" "$output"
    run code_offenders "anything" "" src/no_such_file_anywhere.cpp
    contains "no_such_file_anywhere.cpp is gone" "$output"
}
