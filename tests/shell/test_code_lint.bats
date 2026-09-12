#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Code lint tests: enforce architectural rules on the codebase.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# --- No _for_testing methods in production code ---
# Test-only methods belong in test files via friend class TestAccess pattern.
# See commit removing these for the migration pattern.
#
# *_mock.h files are explicitly excluded: mocks ARE test infrastructure (the
# whole class exists only for tests), so a `_for_testing` setter on a mock
# carries no risk of shipping test code to users — the mock itself is gated
# by HELIX_ENABLE_MOCKS and never enters production builds.

@test "no _for_testing methods declared in headers" {
    run grep -rn '_for_testing' include/ --include='*.h' --exclude='*_mock.h'
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found
}

@test "no _for_testing methods defined in source files" {
    run grep -rn '_for_testing' src/ --include='*.cpp'
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found
}

# --- Migrated temperature VIEW files must route sends through the controller ---
# ui_overlay_temp_graph.cpp and ui_panel_controls.cpp were migrated to delegate
# temperature commands to helix::TemperatureController. They must NOT call the
# raw send API directly again — that would reintroduce the duplication the
# refactor removed. A direct send is either `api_->set_temperature(` or any
# `->set_temperature(` whose receiver is not a `controller`.

@test "migrated temp view files do not call the raw set_temperature send API" {
    local files="src/ui/ui_overlay_temp_graph.cpp src/ui/ui_panel_controls.cpp src/system/post_op_cooldown_manager.cpp src/ui/panel_widgets/preheat_widget.cpp src/ui/ui_panel_bed_mesh.cpp src/ui/ui_panel_filament.cpp src/ui/temperature_service.cpp src/ui/ui_ams_sidebar.cpp"

    # Direct API send on the cached MoonrakerAPI pointer.
    run grep -n 'api_->set_temperature' $files
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found

    # Any ->set_temperature( call whose receiver is not `controller`. The
    # controller's own ->set_temperature() is the sanctioned path, so exclude it.
    run bash -c "grep -nE '\->set_temperature\(' $files | grep -v 'controller'"
    [ "$status" -ne 0 ]  # non-zero == no disallowed direct send found
}

# --- Flush-path px_map stride must go through the shared helper ---
# The dbuf-stride-or-fallback rule (active_buf->header.stride when queryable
# and positive, else lv_draw_buf_width_to_stride(area_w, cf)) serves two
# flush-path consumers, ColorTransform::select_flush_region and the
# remote-screen mirror inside DisplayManager's flush hook. It lives once, in
# helix::flush_px_map_stride (include/flush_stride.h); an inline copy at
# either call site is a twin that can drift silently
# (prestonbrown/helixscreen#1610).

@test "flush px_map stride is derived via helix::flush_px_map_stride, not re-inlined" {
    local folded="src/application/color_transform.cpp src/application/display_manager.cpp"

    # The inline trust-check ternary must not come back at either call site.
    run grep -n 'header.stride > 0' $folded
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found

    # Both call sites must actually call the shared helper.
    for f in $folded; do
        run grep -n 'flush_px_map_stride(' "$f"
        [ "$status" -eq 0 ]
    done

    # The rule itself still exists, exactly once, in the helper.
    run bash -c "grep -c 'header.stride > 0' include/flush_stride.h"
    [ "$status" -eq 0 ]
    [ "$output" -eq 1 ]
}

# --- Chamber temp_display must use the maintain-aware effective target ---
# The raw `chamber_target` subject is the heater target only — it reads 0 during
# M141 "maintain" (cooling-ceiling) mode, so a display bound to it shows "—/Off"
# while the chamber is actually holding a setpoint. All chamber temp_display
# instances must bind to `chamber_effective_target` (+ `chamber_mode`), never the
# raw subject. (Drip-fixed missed-spot class; see chamber M141 routing work.)

@test "no temp_display binds chamber target to the raw chamber_target subject" {
    run grep -rn 'bind_target="chamber_target"' ui_xml/
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found
}

# --- Temperature "decidegrees" misnomer must not creep back ---
# Subject temperatures are stored as degrees × 10 = DECIdegrees (1 unit = 0.1°C).
# The codebase was historically (and wrongly) calling these "centidegrees" — a
# centidegree would be degrees × 100. The Phase-1 rename swept the misnomer out;
# this gate keeps it from reappearing in code or developer docs.
#
# Excluded paths are legitimately historical or out-of-scope:
#   CHANGELOG.md          - records the old name as part of release history
#   docs/superpowers/     - local working space, never committed
#   src/generated/        - generated code (regenerated from templates)
#   lib/                  - vendored submodules (LVGL, libhv, etc.)
#   */translations/       - translation catalogs (mirror upstream wording)

@test "no 'centidegree' misnomer in code or docs" {
    run bash -c "git grep -iIln 'centidegree' -- \
        ':!CHANGELOG.md' \
        ':(glob)!docs/superpowers/**' \
        ':(glob)!src/generated/**' \
        ':(glob)!lib/**' \
        ':(glob)!translations/**' \
        ':(glob)!ui_xml/translations/**' \
        ':(glob)!scripts/translations/**'"
    # git grep exits 0 when it finds matches, 1 when it finds none.
    [ "$status" -ne 0 ]  # non-zero == no misnomer found
}

# --- Temperature subject conversions must route through the unit helpers ---
# Subjects store decidegrees; converting to/from degrees inline (`x / 10`,
# `x * 10`) bypasses helix::units / helix::ui::temperature and silently risks a
# truncation/rounding mismatch (int trunc vs float). These files were migrated to
# the helpers (deci_to_degrees / deci_to_degrees_f / degrees_to_deci /
# to_decidegrees / from_decidegrees); they must not reintroduce a raw multiply or
# divide by 10 on a temperature-named value.
#
# The regex matches a temperature identifier (target/temp/deci/nozzle/bed/chamber/
# heater) or a bare keypad `value`, optionally closing a paren, then `* 10` or
# `/ 10` — the (\.0?f?)?([^0-9.]|$) tail rejects a trailing digit. `//` comments
# are stripped first so the "value * 10" explanatory comments don't trip the gate.
#
# A SECOND pattern catches the x100 form. Decidegrees are degrees x10, so a
# temperature never converts by 100 — `decidegrees / 100` is always the
# controls-panel class of bug (secondary sensors rendered at 1/10 scale: 45°C
# shown as "4°C"). That form slipped past the x10 gate precisely because the tail
# rejects a trailing digit, which is what keeps genuine centimillimetre `/ 100`
# conversions from being flagged.
#
# The x100 pattern therefore uses a NARROWER identifier set than the x10 one:
#   - `value` is dropped: the keypad legitimately converts centimm via `value / 100`.
#   - `bed` is dropped: bed-mesh Z values are distances, not temperatures.
# Everything left (target/temp/deci/nozzle/chamber/heater) is unambiguously a
# temperature in these files. Verified to produce zero hits on the clean tree.

@test "migrated temp files do not convert decidegrees inline (use unit helpers)" {
    local files="src/print/print_start_collector.cpp \
        src/api/moonraker_api_controls.cpp \
        src/api/moonraker_discovery_sequence.cpp \
        src/printer/ams_backend_ad5x_ifs.cpp \
        src/printer/ams_backend_cfs.cpp \
        src/system/telemetry_manager.cpp \
        src/ui/panel_widgets/nozzle_temps_widget.cpp \
        src/ui/ui_ams_sidebar.cpp \
        src/ui/temperature_service.cpp \
        src/ui/ui_overlay_temp_graph.cpp \
        src/ui/ui_panel_bed_mesh.cpp \
        src/ui/ui_panel_calibration_pid.cpp \
        src/ui/ui_panel_controls.cpp \
        src/ui/ui_panel_filament.cpp \
        src/ui/ui_print_preparation_manager.cpp \
        src/ui/ui_temp_display.cpp"
    local pat='(target|temp|deci|nozzle|bed|chamber|heater|value)[A-Za-z_]*(\s*\))?\s*[*/]\s*10(\.0?f?)?([^0-9.]|$)'
    local pat100='(target|temp|deci|nozzle|chamber|heater)[A-Za-z_]*(\s*\))?\s*[*/]\s*100(\.0?f?)?([^0-9.]|$)'
    run bash -c "sed -E 's@//.*@@' $files | grep -nE '$pat|$pat100'"
    [ "$status" -ne 0 ]  # non-zero == no inline decidegree conversion found
}

# --- Concrete Moonraker types must not leak outside the network layer (Plan 3) ---
# Every consumer of the Moonraker network layer now depends on the interfaces
# (helix::IMoonrakerClient, IMoonrakerAPI, and the ten IXxxAPI sub-API
# interfaces in include/i_moonraker_sub_apis.h), not the concrete classes. The
# concretes live behind MoonrakerManager, which owns them via
# std::unique_ptr<MoonrakerAPI> (the concrete facade; the mock inherits it) /
# std::unique_ptr<helix::IMoonrakerClient> and
# constructs them in create_client()/create_api(). Naming a concrete type
# outside the allowlist below reintroduces a hard dependency the interface
# split was meant to remove (mock-parity, and — for the ESP32 port — a
# non-libhv client swapped in behind the same interface).
#
# The allowlist covers the network-layer implementation files themselves
# (moonraker_client, moonraker_manager, moonraker_api + its split translation
# units, the ten sub-API pairs, moonraker_request_tracker,
# moonraker_discovery_sequence) and *_mock.{h,cpp} (mocks legitimately inherit
# the concretes). It intentionally has no bare "test_" entry: this lint only
# scans src/ and include/, so a substring that broad would silently exempt any
# non-test path containing "test_" (e.g. a hypothetical src/foo/test_helpers.cpp)
# — narrower and cheaper to just not have it.
#
# Compile-time-only exceptions NOT covered by this lint (by design, not by
# gap): a few consumers reference concrete-class static constexpr timeouts
# (MoonrakerAdvancedAPI::PROBING_TIMEOUT_MS, ::LEVELING_TIMEOUT_MS,
# MoonrakerJobAPI::CANCEL_TIMEOUT_MS) and the MoonrakerAdvancedAPI::MPCResult
# qualified-name alias. These aren't runtime polymorphism — MPCResult is
# actually defined on IAdvancedAPI with the concrete class providing a `using`
# alias purely so old qualified references keep resolving (see
# include/i_moonraker_sub_apis.h and include/moonraker_advanced_api.h). The
# ten sub-API concrete class names are deliberately left out of the grep
# pattern below rather than allowlisting each of those consumer files, which
# would blur the "outside the network layer" invariant this test communicates.

# The same two shapes as the file-narrowing grep below, spelled for awk: `\b` is
# a GNU grep extension awk does not implement (mawk reads it as a backspace), so
# the word boundaries around the bare class name are written out.
moonraker_concrete_pattern() {
    printf '%s' 'helix::MoonrakerClient|(^|[^A-Za-z0-9_])MoonrakerAPI([^A-Za-z0-9_]|$)'
}

@test "no concrete Moonraker types outside the network layer (Plan 3: interfaces are the consumer contract)" {
    local allowlist='moonraker_client|moonraker_manager|moonraker_api|moonraker_rest_api|moonraker_file_api|moonraker_file_transfer_api|moonraker_advanced_api|moonraker_history_api|moonraker_job_api|moonraker_motion_api|moonraker_queue_api|moonraker_spoolman_api|moonraker_timelapse_api|moonraker_request_tracker|moonraker_discovery_sequence|_mock'
    local candidates offenders

    # Narrow to candidate files first (cheap tree-wide grep), then re-match each
    # one with comments stripped. Prose naming a concrete class is not a
    # dependency on it: include/power_device_parse.h says which method its parser
    # was split out of, which the file-list grep alone read as a violation. The
    # matcher is passed no opt-out token — this gate's escape hatch is the
    # allowlist above, deliberately, so that widening it stays a visible edit.
    candidates=$(grep -rlE 'helix::MoonrakerClient|\bMoonrakerAPI\b' src/ include/ |
        grep -v -E "$allowlist" || true)
    [ -z "$candidates" ] && return 0

    # shellcheck disable=SC2086  # paths have no spaces; word splitting is intended
    offenders=$(code_offenders "$(moonraker_concrete_pattern)" "" $candidates)
    [ -z "$offenders" ] && return 0

    echo "Concrete Moonraker types referenced outside the network layer:"
    printf '%s\n' "$offenders"
    echo
    echo "Consumers depend on the interfaces only: IMoonrakerAPI (include/i_moonraker_api.h),"
    echo "helix::IMoonrakerClient (include/i_moonraker_client.h), and the ten sub-API"
    echo "interfaces in include/i_moonraker_sub_apis.h. The concretes live behind"
    echo "MoonrakerManager (include/moonraker_manager.h), which constructs them."
    return 1
}

# --- switch_printer must invalidate every per-printer cache ---
# Per-printer state lives under /printers/<id>/ and is reached via Config::df().
# Any component that memoizes a df()-derived value serves the PREVIOUS printer's
# data after a switch — PanelWidgetConfig was the first case (#804), and the fix
# used to be a single hardcoded clear_all_panel_configs() call here. Components
# now self-register with PrinterCacheRegistry and switch_printer() fires them all,
# so this gate pins the registry walk rather than any one component.
#
# The registry's own behavior is pinned by tests/unit/test_printer_cache_registry.cpp,
# and clear_all_panel_configs() by the unit test "PanelWidgetManager:
# clear_all_panel_configs reloads after printer switch". What no unit test can
# reach is the success path: switch_printer() ends in a full teardown and
# display + Moonraker rebuild, which cannot run inside a shared Catch2 shard
# without handing every later test rebuilt global singletons.
# tests/unit/application/test_application_printer_switch.cpp covers the branches
# on the near side of teardown. This gate pins the wiring beyond them — that
# switch_printer() actually makes the call, and makes it BEFORE teardown, while
# Config::df() has already moved to the new printer.

# Print the body of Application::switch_printer() from the file given in $1.
switch_printer_body() {
    awk '
        /^void Application::switch_printer\(/ { inside = 1 }
        inside { print }
        inside && /^\}/ { exit }
    ' "$1"
}

# Emit a diagnostic and return non-zero if $1 does not wire up the invalidation.
check_switch_printer_clears_caches() {
    local body clear_line teardown_line
    body=$(switch_printer_body "$1")
    if [ -z "$body" ]; then
        echo "could not locate Application::switch_printer() in $1"
        return 1
    fi

    clear_line=$(printf '%s\n' "$body" | grep -n 'PrinterCacheRegistry::instance().invalidate_all()' | head -1 | cut -d: -f1)
    if [ -z "$clear_line" ]; then
        echo "switch_printer() does not call PrinterCacheRegistry::instance().invalidate_all() (#804)"
        return 1
    fi

    teardown_line=$(printf '%s\n' "$body" | grep -n 'tear_down_printer_state()' | head -1 | cut -d: -f1)
    if [ -z "$teardown_line" ]; then
        echo "switch_printer() does not call tear_down_printer_state()"
        return 1
    fi

    if [ "$clear_line" -ge "$teardown_line" ]; then
        echo "PrinterCacheRegistry::invalidate_all() must precede tear_down_printer_state()"
        return 1
    fi
    return 0
}

@test "switch_printer invalidates every registered per-printer cache before teardown" {
    run check_switch_printer_clears_caches src/application/application.cpp
    [ "$status" -eq 0 ]
}

@test "the switch_printer cache-invalidation gate fails when the call is removed" {
    # Meta-test: a gate that cannot fail is not a gate. Strip the call from a
    # copy and confirm the check reports the #804 regression.
    local mutated="${BATS_TEST_TMPDIR}/application_no_clear.cpp"
    grep -v 'PrinterCacheRegistry::instance().invalidate_all()' src/application/application.cpp > "$mutated"

    run check_switch_printer_clears_caches "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"#804"* ]]
}

@test "the switch_printer cache-invalidation gate fails when the call moves after teardown" {
    # The ordering half: invalidating after teardown re-reads the OLD printer's
    # values on the way down, so position matters as much as presence.
    local mutated="${BATS_TEST_TMPDIR}/application_late_clear.cpp"
    sed -e 's@^    helix::PrinterCacheRegistry::instance().invalidate_all();@@' \
        -e 's@^    tear_down_printer_state();@    tear_down_printer_state();\n    helix::PrinterCacheRegistry::instance().invalidate_all();@' \
        src/application/application.cpp > "$mutated"

    run check_switch_printer_clears_caches "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"must precede"* ]]
}

@test "the switch_printer cache-invalidation gate fails when the function cannot be located" {
    # Fail-closed: a rename or signature change must break the gate loudly rather
    # than silently pass on an empty body.
    local mutated="${BATS_TEST_TMPDIR}/application_no_fn.cpp"
    sed -e 's@^void Application::switch_printer(@void Application::switch_printer_renamed(@' \
        src/application/application.cpp > "$mutated"

    run check_switch_printer_clears_caches "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not locate"* ]]
}

@test "the switch_printer cache-invalidation gate fails when teardown is missing" {
    local mutated="${BATS_TEST_TMPDIR}/application_no_teardown.cpp"
    grep -v '^    tear_down_printer_state();' src/application/application.cpp > "$mutated"

    run check_switch_printer_clears_caches "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"tear_down_printer_state"* ]]
}

# --- No RTTI code shapes (the firmware builds -fno-rtti) ---
# The ESP32 firmware compiles with -fno-rtti (~296KB reclaimed), so dynamic_cast,
# typeid, std::type_index and std::function::target_type() do not compile there at
# all. This gate is for the DESKTOP side: firmware-compiled RTTI is already a hard
# compile error, but a desktop-only file can reintroduce a shape that silently
# blocks the next file getting pulled into the firmware slice.
#
# Scope is the shared/portable code — src/, include/, firmware/ and the helix-xml
# submodule. tests/ is deliberately NOT scanned: test code is desktop-only and
# never enters a firmware build.
#
# Two precision decisions, both forced by real code in the tree:
#
#   1. Comments are stripped before matching. The tree's only occurrences of these
#      tokens today are prose EXPLAINING the RTTI-free replacements
#      (include/ams_backend.h, include/ui_context_menu.h, include/helix_type_tag.h,
#      src/application/application.cpp) — three of the four inside /** */ doc
#      blocks, so a `//`-only strip is not enough. rtti_offenders() therefore runs
#      a small block-comment state machine.
#
#   2. The std::any `.type()` shape gets its OWN test, scoped to files that include
#      <any>. `.type()` is far too common a method name to match tree-wide —
#      LayoutManager::type() and lv_layout's lm.type() are ordinary enum getters
#      and would false-positive. Scoping by `#include <any>` is computed fresh on
#      every run, so a new file that starts using std::any is covered automatically.
#
# Escape hatch: `// RTTI_OK: <reason>` on the same line. The allowlist is empty.

# The forbidden shapes, as one ERE alternation. `\s` is avoided deliberately:
# it is a GNU extension that mawk (the default awk on Debian) does not support.
rtti_forbidden_pattern() {
    printf '%s' 'dynamic_cast[[:space:]]*<|typeid[[:space:]]*\(|std::type_index|\.target_type[[:space:]]*\('
}

# std::any's RTTI-dependent accessor: `a.type() == …`, `!=`, or `.type().name()`.
# Pointer-form std::any_cast is the sanctioned idiom — it returns null on a type
# mismatch and needs no RTTI.
rtti_any_pattern() {
    printf '%s' '\.type[[:space:]]*\([[:space:]]*\)[[:space:]]*(==|!=|\.name)'
}

# C++ sources in the RTTI lint scope. git ls-files rather than find: it keeps
# firmware/*/build/ and the vendored managed_components/ out with no exclusion
# list to maintain (both are gitignored). --others --exclude-standard adds files
# that are new but not yet staged, so the gate fires on a fresh source file
# before it is committed rather than only after. helix-xml needs its own
# ls-files — it is a submodule, and in a worktree it is a symlink, so neither the
# parent's index nor `find` reaches it the same way.
rtti_lint_files() {
    git ls-files --cached --others --exclude-standard src include firmware |
        grep -E '\.(cpp|h)$'
    git -C lib/helix-xml ls-files --cached --others --exclude-standard |
        grep -E '\.(cpp|h)$' | sed 's@^@lib/helix-xml/@'
}

# Print `file:line: text` for each line of $3.. matching the ERE in $1, ignoring
# comments and lines carrying the literal opt-out token in $2 (empty $2 = the
# gate has no opt-out). The pattern travels through the environment, not
# `awk -v`: -v runs escape processing over the value, which turns `\.` into a
# match-anything `.` and emits a warning for every escape.
code_offenders() {
    local pat="$1" optout="$2"
    shift 2
    CODE_LINT_PAT="$pat" CODE_LINT_OPTOUT="$optout" awk '
        BEGIN { pat = ENVIRON["CODE_LINT_PAT"]; optout = ENVIRON["CODE_LINT_OPTOUT"] }
        FNR == 1 { in_block = 0 }
        optout != "" && index($0, optout) > 0 { next }
        {
            # Walk the line left to right, honouring whichever delimiter opens
            # first. A `//` and a `/*` on one line is not hypothetical:
            # src/system/config.cpp documents a `scanner/*` config path inside a
            # `///` comment, and stripping block comments first latched in_block
            # there and swallowed the rest of the FILE — every gate sharing this
            # matcher went quiet from that line on.
            line = $0
            code = ""
            if (in_block) {
                i = index(line, "*/")
                if (i == 0) next
                line = substr(line, i + 2)
                in_block = 0
            }
            while (1) {
                b = index(line, "/*")
                l = index(line, "//")
                if (l > 0 && (b == 0 || l < b)) {
                    code = code substr(line, 1, l - 1)
                    break
                }
                if (b == 0) { code = code line; break }
                code = code substr(line, 1, b - 1)
                rest = substr(line, b + 2)
                e = index(rest, "*/")
                if (e == 0) { in_block = 1; break }
                line = substr(rest, e + 2)
            }
            if (code ~ pat) print FILENAME ":" FNR ": " $0
        }
    ' "$@"
}

# RTTI flavor: the opt-out is `// RTTI_OK: <reason>`.
rtti_offenders() {
    local pat="$1"
    shift
    code_offenders "$pat" RTTI_OK "$@"
}

rtti_advice() {
    cat <<'EOF'

RTTI is disabled in firmware builds (-fno-rtti), where these do not compile at all.
Use instead:
  - type keys / map lookups   -> helix::type_tag<T>()          (include/helix_type_tag.h)
  - downcast to a subclass    -> a virtual kind query          (HELIX_CONTEXT_MENU_KIND,
                                                                include/ui_context_menu.h)
  - inspecting a std::any     -> pointer-form std::any_cast<T>(&a), null on mismatch
Genuinely unavoidable and desktop-only? Annotate the line: // RTTI_OK: <reason>
EOF
}

check_no_rtti_shapes() {
    local offenders
    # shellcheck disable=SC2046  # paths have no spaces; word splitting is intended
    offenders=$(rtti_offenders "$(rtti_forbidden_pattern)" $(rtti_lint_files))
    [ -z "$offenders" ] && return 0
    echo "Forbidden RTTI shapes found:"
    printf '%s\n' "$offenders"
    rtti_advice
    return 1
}

check_no_any_type_rtti() {
    local any_files offenders
    # shellcheck disable=SC2046  # paths have no spaces; word splitting is intended
    any_files=$(grep -l '#include <any>' $(rtti_lint_files) 2>/dev/null)
    [ -z "$any_files" ] && return 0
    # shellcheck disable=SC2086
    offenders=$(rtti_offenders "$(rtti_any_pattern)" $any_files)
    [ -z "$offenders" ] && return 0
    echo "std::any::type() is RTTI-dependent; use pointer-form std::any_cast:"
    printf '%s\n' "$offenders"
    rtti_advice
    return 1
}

@test "no RTTI code shapes (dynamic_cast / typeid / std::type_index / target_type)" {
    run check_no_rtti_shapes
    [ "$status" -eq 0 ]
}

@test "no std::any::type() RTTI inspection (use pointer-form any_cast)" {
    run check_no_any_type_rtti
    [ "$status" -eq 0 ]
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

@test "filaments.json android mirror matches when present" {
  # The Android mirror is gitignored (generated by `make regen-filaments`, not
  # tracked), so it may be absent on a fresh checkout / CI clone. Only assert
  # byte-identity when it exists — catches on-disk drift without breaking CI.
  if [ ! -f android/app/src/main/assets/assets/filaments.json ]; then
    skip "android mirror not generated (gitignored)"
  fi
  diff assets/filaments.json android/app/src/main/assets/assets/filaments.json
}

# --- Every extractable UI string is already in the translation catalogs ---
#
# `translation_sync.py sync` extracts from XML *and* C++, including the static
# tables that the UI translates through a variable (lv_tr(def.display_name) and
# friends -- see scripts/translations/cpp_tables.py). If a dry run would still
# add keys, someone shipped a user-facing string that no locale can translate.
# That is silent at runtime: lv_translation_get() falls back to the tag, so the
# string renders in English in all nine languages and only a debug-level log
# line says so. One bundle carried 1445 of those lines, 294 KB of ring buffer.
#
# Fix by running `make translation-sync && make translations`, then translating
# the new keys (consult translations/GLOSSARY.md and reuse the canonical term).
# A string that genuinely should not be translated gets `// i18n: do not
# translate` on its line or the line above.

@test "no user-facing strings are missing from the translation catalogs" {
  if [ ! -x .venv/bin/python ]; then
    skip "translations venv not set up (run 'make venv-setup')"
  fi
  run .venv/bin/python scripts/translation_sync.py sync --dry-run
  [ "$status" -eq 0 ]
  [[ "$output" == *"All XML strings already in YAML files"* ]]
}

# The same gate has to run from quality-checks.sh, not only from here. v0.99.116
# was tagged with five untranslated strings because this file is the only place
# that checked: quality-checks.sh was green on the same tree, so the pre-commit
# hook, the pre-push hook and the Code Quality workflow all passed it through,
# and `make test-shell` only runs late in the release. These pin the wiring.

@test "the translation coverage gate is wired into quality-checks.sh" {
  run grep -c 'qc_translation_coverage' scripts/quality-checks.sh
  [ "$status" -eq 0 ]
  # definition, QC_ALL registration, and the path-gating trigger row
  [ "$output" -ge 3 ]
}

@test "quality-checks.sh runs the coverage gate as a dry run" {
  # A bare `sync` REWRITES all nine catalogs. A gate that edits the tree it is
  # inspecting would stage catalog churn behind the committer's back, and would
  # then report green on the very drift it just introduced.
  run grep -n 'translation_sync.py sync' scripts/quality-checks.sh
  [ "$status" -eq 0 ]
  while IFS= read -r line; do
    contains "--dry-run" "$line"
  done <<< "$output"
}

@test "the coverage gate wakes on src and ui_xml, not just translations" {
  # A new lv_tr() in src/ or a label_tag in ui_xml/ is what ADDS an untranslated
  # string; gating the check on ^translations/ alone would sleep through exactly
  # the commit that introduces one.
  run bash -c "sed -n '/qc_translation_coverage)/,/;;/p' scripts/quality-checks.sh"
  [ "$status" -eq 0 ]
  contains "^ui_xml/" "$output"
  [[ "$output" == *"^src/"* ]]
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
    eval "$(sed -n "/^qc_translation_coverage() {/,/^}/p" scripts/quality-checks.sh)"
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

# --- Global RuntimeConfig::test_mode must be restored by whoever sets it ---
#
# test_mode is the master switch behind every should_mock_*() predicate, so a
# test file that writes it on the process-global RuntimeConfig and never puts it
# back changes behaviour for every test scheduled after it. That is what
# prestonbrown/helixscreen#1287 was: one unrestored write in a capabilities
# characterization test failed four unrelated tool_state/tool_switcher cases,
# but only under a filter that happened to order them after it. Each passed
# alone, which is what made it expensive to find.
#
# File-level on purpose. Every correct site today pairs its write with a restore
# somewhere in the same file, via tests/test_helpers/scoped_runtime_config.h.
# The gate asks only that the restore exists, so it stays quiet on all of them
# and fires on a file that only sets.
#
# Writes through a LOCAL `RuntimeConfig config;` (test_runtime_config.cpp,
# test_subject_initializer.cpp) are not global state and are not matched.

test_mode_global_writers() {
    # Files writing test_mode through a pointer to the global config.
    grep -rlE '(get_runtime_config\(\)|\brc|\bcfg|\br)->test_mode[[:space:]]*=' "$@" 2>/dev/null || true
}

test_mode_unrestored_files() {
    local f
    for f in $(test_mode_global_writers "$@"); do
        # A restore is any write of a saved value, or the shared RAII guard.
        if ! grep -qE 'ScopedRuntimeConfig|test_mode[[:space:]]*=[[:space:]]*(prev|saved|prev_test_mode|saved_test_mode)' "$f"; then
            echo "$f"
        fi
    done
}

@test "every test file that sets the global test_mode also restores it" {
    run test_mode_unrestored_files tests/ --include='*.cpp' --include='*.h'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
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

# --- AmsState is initialized with its XML names, always -------------------------
#
# AmsState is a process-wide singleton, so a single init_subjects(false) -
# written because the test under it binds through the C++ accessors - publishes
# no `ams_*` names, and every later case in the same shard that does not itself
# re-enter init_subjects(true) sees lv_xml_get_subject() answer null; an XML
# layout that binds one of those names comes up empty rather than erroring.
# Publishing costs a test nothing: XML subjects share one global scope in the
# test build whatever this argument says.

ams_state_unpublished_init_files() {
    # test_ams_lane_state_subject.cpp owns the one deliberate false init: its
    # [1374] case exercises re-entry publishing and republishes the names itself.
    grep -rlE 'AmsState::instance\(\)\.init_subjects\([[:space:]]*false' "$@" 2>/dev/null |
        grep -v 'test_ams_lane_state_subject\.cpp' || true
}

@test "no test initializes AmsState without its XML names" {
    run ams_state_unpublished_init_files tests/ --include='*.cpp' --include='*.h'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
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

# --- History lazy loads must use ensure_loaded(), never fetch() ---
# PrintHistoryManager::fetch() is the INVALIDATION entry point: when a request
# is already out it arms one re-issue, because a response issued before a delete
# cannot describe that delete. ensure_loaded() is the LAZY-LOAD entry point and
# returns instead, since the in-flight response already serves the caller.
#
# Every invalidation lives inside the manager itself, driven by Moonraker's
# notify_history_changed / notify_filelist_changed. So fetch() has no legitimate
# caller in src/ outside print_history_manager.cpp, and any that reappears is a
# lazy load that pulls the whole 500-job list twice.
#
# 19bfc451e split the two intents and converted the three panels, leaving the
# two home-panel widgets on fetch(). Bundles G3FE69L7 / P6HCTHQH (AD5X,
# v0.99.116) then caught four lazy loads arming the re-issue during startup,
# queued behind a first list that took 9.0s. tests/ is out of scope: a fixture
# priming a fresh manager with nothing in flight is an honest fetch().

history_lazy_fetch_offenders() {
    local dir="$1"
    grep -rnE '(history[A-Za-z_]*|hm)->fetch\(' "$dir" --include='*.cpp' --include='*.h' \
        | grep -v '/print_history_manager\.cpp:' || true
}

@test "history lazy loads use ensure_loaded(), not fetch()" {
    run history_lazy_fetch_offenders src
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

# --- SubjectManager::deinit_all() must not log ---
# A SubjectManager owned by a static (PrintStatusWidget::s_formatter_) reaches
# deinit_all() through the C++ atexit chain. spdlog's registry is a lazily
# constructed function-local static, so it registers for destruction after any
# object built during dynamic initialization and is therefore torn down before
# them: a log call here reads a freed logger. The empty-subjects early-out
# spares an app that ran Application::shutdown(); a binary that never does -
# every unit test - arrives with subjects still registered and takes the full
# path, where the trace fires.

check_deinit_all_does_not_log() {
    local file="$1"
    local body
    # Track brace depth rather than stopping at the first four-space "}": a
    # nested block closing in that column would end the extraction early and
    # leave the rest of the body unchecked.
    body=$(awk '
        /void deinit_all\(\) \{/ { f = 1 }
        f {
            print
            depth += gsub(/\{/, "{") - gsub(/\}/, "}")
            if (depth <= 0) exit
        }
    ' "$file")

    if [ -z "$body" ]; then
        echo "could not locate SubjectManager::deinit_all() in $file"
        return 1
    fi

    if echo "$body" | grep -q 'spdlog::'; then
        echo "spdlog call inside deinit_all(), which runs during static destruction:"
        echo "$body" | grep -n 'spdlog::'
        return 1
    fi
    return 0
}

@test "SubjectManager::deinit_all() makes no spdlog calls" {
    run check_deinit_all_does_not_log include/subject_managed_panel.h
    [ "$status" -eq 0 ]
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
    run bash -c 'make -s print-vacuous-max 2>/dev/null | tail -1'
    [ "$status" -eq 0 ]
    [[ "$output" =~ ^[0-9]+$ ]]
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

# --- ObserverGuard's move paths must clear the source's cleanup ---
# A moved-from std::function is valid but UNSPECIFIED. libc++ keeps the target
# when the callable fits its small-object buffer, and ObserverGuard's cleanup -
# one captured pointer - does. A source that kept it frees the observer's
# context from its own destructor while the destination keeps the observer
# attached, so the next notify reads freed memory (#1446). libstdc++ empties the
# source, so no test on this box can fail on it - which is exactly why the
# invariant is pinned here instead.

check_observer_guard_move_clears_cleanup() {
    local file="$1"
    local moves
    moves=$(grep -n 'cleanup_.*other\.cleanup_' "$file")

    if [ -z "$moves" ]; then
        echo "could not locate ObserverGuard's move paths in $file"
        return 1
    fi

    if [ "$(echo "$moves" | wc -l)" -ne 2 ]; then
        echo "expected exactly 2 cleanup_ move sites (move ctor + move assignment), found:"
        echo "$moves"
        return 1
    fi

    if echo "$moves" | grep -q 'std::move'; then
        echo "ObserverGuard move path uses std::move on cleanup_; a moved-from"
        echo "std::function keeps its target on libc++, so the source frees the"
        echo "observer context out from under the destination. Use std::exchange:"
        echo "$moves" | grep 'std::move'
        return 1
    fi
    return 0
}

@test "ObserverGuard's move paths clear the moved-from cleanup" {
    run check_observer_guard_move_clears_cleanup include/ui_observer_guard.h
    [ "$status" -eq 0 ]
}

@test "the ObserverGuard move gate fires when std::move comes back" {
    local mutated="${BATS_TEST_TMPDIR}/ui_observer_guard_moved.h"
    sed -e 's@std::exchange(other\.cleanup_, nullptr)@std::move(other.cleanup_)@' \
        include/ui_observer_guard.h > "$mutated"

    run check_observer_guard_move_clears_cleanup "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"libc++"* ]]
}

@test "the ObserverGuard move gate fails closed when a move path disappears" {
    local mutated="${BATS_TEST_TMPDIR}/ui_observer_guard_dropped.h"
    awk '/cleanup_.*other\.cleanup_/ && !seen { seen = 1; next } { print }' \
        include/ui_observer_guard.h > "$mutated"

    run check_observer_guard_move_clears_cleanup "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"expected exactly 2"* ]]
}

# ====================================================================
# A friend list is a fixed, reviewable set, not an open-ended one
# ====================================================================
# AmsBackendAfc and AmsBackendHappyHare each expose internals to tests through
# a single XTestAccess shim; LaneSourceStore (below) names its two writer
# funnels plus a test shim. In every case, widening what a header befriends is
# a review-worthy edit, so this compares the file's friend declarations
# against an exact expected set rather than merely counting them. The match
# is on normalised whitespace, not literal text, since clang-format may
# reflow a multi-argument signature, and it is order-insensitive: a header's
# friend declarations are a set of grants, not a sequence, so reordering two
# of them without adding or removing a grant is not a widening.
check_friend_list() {
    local file="$1"
    local expected
    expected=$(printf '%s\n' "$2" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//; s/[[:space:]]+/ /g')
    local expected_count
    expected_count=$(printf '%s\n' "$expected" | grep -c .)

    local friends
    friends=$(grep -E '^[[:space:]]*friend ' "$file" 2>/dev/null \
              | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//; s/[[:space:]]+/ /g' || true)

    if [ -z "$friends" ]; then
        echo "no friend declaration in $file; expected exactly $expected_count:"
        echo "$expected"
        return 1
    fi

    if [ "$(printf '%s\n' "$friends" | sort)" != "$(printf '%s\n' "$expected" | sort)" ]; then
        local found_count
        found_count=$(printf '%s\n' "$friends" | grep -c .)
        echo "expected exactly $expected_count test friend(s) in $file, found $found_count:"
        echo "found:"
        echo "$friends"
        echo "expected:"
        echo "$expected"
        return 1
    fi
    return 0
}

@test "AmsBackendAfc befriends exactly one test shim" {
    run check_friend_list include/ams_backend_afc.h "friend class AfcTestAccess;"
    [ "$status" -eq 0 ]
}

@test "AmsBackendHappyHare befriends exactly one test shim" {
    run check_friend_list include/ams_backend_happy_hare.h "friend class HappyHareTestAccess;"
    [ "$status" -eq 0 ]
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
# --- No on_deactivate() overrides on panels and overlays ---
# ViewLifecycleBase makes on_deactivate(DeactivateReason) final and dispatches to
# on_deactivating(reason), so a subclass re-declaring the no-argument spelling
# overrides nothing: it compiles, it is never called, and the work it meant to do
# silently stops happening. PanelWidget is a separate hierarchy with its own
# on_deactivate() hook and keeps the name.

check_no_on_deactivate_overrides() {
    local hits=""
    local file found
    while IFS= read -r file; do
        # The widget hierarchy, by directory and by the base it names — an
        # implementation file mentions PanelWidget only via its own header.
        case "$file" in */panel_widgets/*) continue ;; esac
        grep -q 'PanelWidget' "$file" && continue
        found=$(grep -nE '(\bvoid[[:space:]]+on_deactivate[[:space:]]*\(\)|::on_deactivate[[:space:]]*\(\))' "$file")
        [ -n "$found" ] && hits+="${file}:${found}"$'\n'
    done < <(grep -rl 'on_deactivate' "$@" --include='*.h' --include='*.cpp' 2>/dev/null)

    if [ -n "$hits" ]; then
        echo "on_deactivate() is final on ViewLifecycleBase — override"
        echo "on_deactivating(DeactivateReason) instead. Offending sites:"
        echo "$hits"
        return 1
    fi
    return 0
}

@test "no panel or overlay declares a no-argument on_deactivate() override" {
    run check_no_on_deactivate_overrides src include
    [ "$status" -eq 0 ]
    [ -z "$output" ]
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

# --- An OverlayBase cleanup() override must reach the base ---
# OverlayBase::cleanup() is the only thing that invalidates object_lifetime_, the
# guard whose whole point is surviving deactivation. A subclass that overrides
# cleanup() and never calls the base leaves that guard live for the rest of the
# process, so callbacks parked on it still fire into a torn-down overlay. The
# wizard steps and the list views own an unrelated cleanup() and are not in this
# hierarchy.

check_overlay_cleanup_calls_base() {
    local classes
    classes=$(grep -rhoE 'class[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*:[[:space:]]*public[[:space:]]+OverlayBase' \
        "$@" --include='*.h' --include='*.cpp' 2>/dev/null \
        | awk '{print $2}' | sort -u | paste -sd'|' -)
    [ -n "$classes" ] || classes="__no_overlay_subclass__"

    local hits="" file found inline
    while IFS= read -r file; do
        # An inline body carries no class name, so it is attributed by the file
        # it sits in; an out-of-line one names its class and needs no such help.
        inline=0
        grep -q 'public[[:space:]]*OverlayBase' "$file" && inline=1
        found=$(awk -v classes="$classes" -v allow_inline="$inline" '
            BEGIN {
                outofline = "^[ \t]*void[ \t]+(" classes ")::cleanup[ \t]*\\([ \t]*\\)"
                inline_def = "void[ \t]+cleanup[ \t]*\\([ \t]*\\)[ \t]*(override[ \t]*)?\\{"
            }
            !in_body {
                if ($0 ~ outofline || (allow_inline == 1 && $0 ~ inline_def)) {
                    in_body = 1; body = ""; start = NR; depth = 0; opened = 0
                }
            }
            in_body {
                body = body $0 "\n"
                o = gsub(/\{/, "{"); c = gsub(/\}/, "}")
                if (o > 0) opened = 1
                depth += o - c
                if (opened && depth <= 0) {
                    if (body !~ /OverlayBase::cleanup/) print FILENAME ":" start
                    in_body = 0
                }
            }
        ' "$file")
        [ -n "$found" ] && hits+="${found}"$'\n'
    done < <(grep -rl 'cleanup' "$@" --include='*.h' --include='*.cpp' 2>/dev/null)

    if [ -n "$hits" ]; then
        echo "an OverlayBase subclass that overrides cleanup() must call"
        echo "OverlayBase::cleanup() — it is what invalidates both lifetime guards."
        echo "Offending sites:"
        echo "$hits"
        return 1
    fi
    return 0
}

@test "every OverlayBase subclass overriding cleanup() calls the base" {
    run check_overlay_cleanup_calls_base src include
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "the cleanup gate examines the overrides that exist" {
    # A pass that inspected nothing reads exactly like a pass that inspected
    # everything, so the gate has to be shown finding the real overrides first.
    local found
    found=$(grep -rl 'OverlayBase::cleanup()' src --include='*.cpp' | wc -l | tr -d ' ')
    [ "$found" -ge 10 ]
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
# --- The Z-offset save-availability rule has ONE definition ---
# "Is there an offset worth saving" is helix::zoffset::save_available(), published
# to XML as the z_offset_save_available subject. An inline cond= that re-derives
# it from any_tool_z_dirty is a second copy, and copies agree by convention until
# one is widened and the others are not: a tool dirty on an axis the stale copy
# does not ask about gets no save affordance, and SET_TOOL_PARAMETER is
# runtime-only (prestonbrown/helixscreen#1517).

zoffset_save_rule_copies() {
    grep -rnE 'cond="[^"]*any_tool_z_dirty' "$@" 2>/dev/null || true
}

# Names every XML that offers the save without binding the published rule.
#
# The file list is DISCOVERED from the button names, not spelled out: a surface
# added later, or a breakpoint override that gets its own copy of the button,
# has to bind the rule too. A button binding something narrower is the failure
# this catches — a micro-breakpoint Save that asks only about the machine-wide
# offset hides itself over a dirty tool while the handler would save it.
zoffset_save_button_files() {
    grep -rlE 'name="(btn_save_z_offset|header_save_z_offset)"' "$@" 2>/dev/null || true
}

zoffset_save_buttons_unbound() {
    local f
    for f in "$@"; do
        grep -qF 'subject="z_offset_save_available"' "$f" || echo "$f"
    done
}

# The publisher observes subjects ToolState registers in init_ams_subjects(), so
# initialising it earlier would attach to nothing and leave every save surface
# bound to a subject that never moves. Silent in both directions: the XML binding
# resolves, the button just never appears.
zoffset_publisher_wiring() {
    local f="${1:-src/application/subject_initializer.cpp}"
    local ams init
    ams=$(grep -n 'init_ams_subjects();' "$f" | tail -1 | cut -d: -f1)
    init=$(grep -n 'zoffset::init_save_available_subject(' "$f" | head -1 | cut -d: -f1)
    if [ -z "$ams" ]; then
        echo "init_ams_subjects() call not found in $f"
        return
    fi
    if [ -z "$init" ]; then
        echo "init_save_available_subject() is never called in $f"
        return
    fi
    [ "$init" -gt "$ams" ] || echo "init_save_available_subject() runs before init_ams_subjects()"
}

@test "the Z-offset save publisher is initialised after ToolState" {
    run zoffset_publisher_wiring
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

@test "no XML re-derives the Z-offset save rule from any_tool_z_dirty" {
    run zoffset_save_rule_copies ui_xml/ --include='*.xml'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
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

@test "every Z-offset save surface binds the published rule" {
    local files
    files=$(zoffset_save_button_files ui_xml/ --include='*.xml')
    # Fail closed: an empty list would pass the emptiness check below having
    # examined nothing.
    [ "$(printf '%s\n' "$files" | grep -c .)" -ge 4 ]

    run zoffset_save_buttons_unbound $files
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

# --- The Spoolman weight poll must stay weight-only ---
# AmsBackend::update_slot_weight exists so an automated weight tracker never
# asserts filament identity. Handing a backend a whole SlotInfo lets it re-derive
# state from fields the poll never meant to touch: a backend that infers presence
# from identity resurrects a lane its sensors report empty, and the persist arm
# re-stages the user-lock flags from whatever the slot happened to hold. The
# consumption sink routes through update_slot_weight for this reason (#981); the
# Spoolman poll is the other automated weight writer, and it polls every linked
# lane on a timer.

check_weight_poll_is_weight_only() {
    local file="$1"
    if [ ! -f "$file" ]; then
        echo "could not locate $file"
        return 1
    fi
    if grep -qE '\->set_slot_info\(' "$file"; then
        echo "$file writes a whole SlotInfo; an automated weight poll must call update_slot_weight() instead"
        return 1
    fi
    return 0
}

@test "the Spoolman weight poll does not write whole SlotInfo structs" {
    run check_weight_poll_is_weight_only src/printer/spoolman_manager.cpp
    [ "$status" -eq 0 ]
}

@test "the weight-poll gate fires when the poll writes a whole SlotInfo" {
    # Meta-test: a gate that cannot fail is not a gate.
    local mutated="${BATS_TEST_TMPDIR}/spoolman_manager_whole_struct.cpp"
    sed -e 's@owner->update_slot_weight(@owner->set_slot_info(@' \
        src/printer/spoolman_manager.cpp > "$mutated"

    run check_weight_poll_is_weight_only "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"update_slot_weight"* ]]
}

@test "the weight-poll gate fails closed when the file is missing" {
    run check_weight_poll_is_weight_only "${BATS_TEST_TMPDIR}/does_not_exist.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not locate"* ]]
}

# --- The lane source store has exactly one mutable entry point ---
# LaneSourceStore::write() is private, so ingest() and commit_slot_edit() are
# the only code that can reach a lane's records; a third friend line, of any
# kind, would be a third writer. check_friend_list's grep matches "friend "
# rather than "friend class ", since both funnels here are free functions a
# class-only pattern would not see. Every entry in this list is a grant that
# some code actually uses: a friend that needs no friendship teaches the next
# reader that entries here need not be load-bearing.

LANE_STORE_FRIENDS_EXPECTED="friend void ingest(LaneId, const Observation&);
friend void commit_slot_edit(LaneId, const Observation&);"

@test "the lane source store names exactly its two funnels as friends" {
    run check_friend_list include/lane_source_store.h "$LANE_STORE_FRIENDS_EXPECTED"
    [ "$status" -eq 0 ]
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

# write() is the one method the friend list above actually gates. If it were
# renamed while the friend list stayed put, the list would describe a method
# that no longer exists, so this checks the guarded declaration separately.

check_lane_store_write_declared() {
    local file="$1"
    if ! grep -qE '^[[:space:]]*void write\(LaneId lane, const Observation& obs, bool amend\);' \
         "$file" 2>/dev/null; then
        echo "LaneSourceStore::write is not declared where the friend list guards it"
        return 1
    fi
    return 0
}

@test "the lane store's guarded write() is declared as the friend list expects" {
    run check_lane_store_write_declared include/lane_source_store.h
    [ "$status" -eq 0 ]
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

# The store's singleton is reachable only from the funnels' own translation
# unit. A caller holding the instance is a caller one edit away from a write.

check_no_lane_store_instance_callers() {
    local dir
    for dir in "$@"; do
        if [ ! -d "$dir" ]; then
            echo "search path $dir does not exist; cannot confirm the lane store has no outside callers"
            return 1
        fi
    done

    local hits
    hits=$(grep -rn 'LaneSourceStore::instance()' "$@" --include='*.cpp' --include='*.h' \
           2>/dev/null | grep -v 'src/printer/lane_source_store.cpp' || true)
    if [ -n "$hits" ]; then
        echo "LaneSourceStore::instance() is reachable only from the funnels."
        echo "Call helix::ams::ingest() or helix::ams::commit_slot_edit() instead:"
        echo "$hits"
        return 1
    fi
    return 0
}

@test "nothing outside the funnels holds the lane source store" {
    run check_no_lane_store_instance_callers src/ include/
    [ "$status" -eq 0 ]
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

# check_friend_list and check_lane_store_write_declared both read declaration
# text only, so a write() moved out of the private: block and into public:
# passes both of them with the friend list and the write() signature both
# untouched. The friend list only restricts anything while write() stays
# private, so that access specifier is a separate fact this checks for
# itself: which label is in force at the write() declaration inside class
# LaneSourceStore, defaulting to private before any label has appeared, since
# that is what an unlabelled C++ class body means.

check_lane_store_write_is_private() {
    local file="$1"
    local access
    access=$(awk '
        /^class LaneSourceStore[[:space:]]*\{/ { in_class = 1; access = "private"; next }
        in_class && /^\};/ { in_class = 0; next }
        in_class && /^[[:space:]]*public:[[:space:]]*$/ { access = "public"; next }
        in_class && /^[[:space:]]*protected:[[:space:]]*$/ { access = "protected"; next }
        in_class && /^[[:space:]]*private:[[:space:]]*$/ { access = "private"; next }
        in_class && /void write\(/ { print access; found = 1; exit }
        END { if (!found) print "NOTFOUND" }
    ' "$file" 2>/dev/null)

    if [ -z "$access" ] || [ "$access" = "NOTFOUND" ]; then
        echo "LaneSourceStore::write() was not found inside class LaneSourceStore in $file"
        return 1
    fi
    if [ "$access" != "private" ]; then
        echo "LaneSourceStore::write() is declared $access in $file; the friend list only matters while it stays private"
        return 1
    fi
    return 0
}

@test "the lane store's write() is declared private" {
    run check_lane_store_write_is_private include/lane_source_store.h
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

@test "no hand-built tool labels or lane/slot offsets in the real tree" {
    run bash "$BATS_TEST_DIRNAME/../../scripts/check_tool_labels.sh"
    [ "$status" -eq 0 ]
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


@test "the control socket directory is resolved in one place" {
    # Client and server each computed this inline. They agreed by convention
    # until they did not, and a client looking where the server never bound
    # reports "no instance found" (prestonbrown/helixscreen#1602). Both go
    # through control_socket_dir() now; reading the environment directly in
    # either one puts the second copy back.
    run bash -c "grep -nE 'getenv\(\"(XDG_RUNTIME_DIR|RUNTIME_DIRECTORY)\"' \
        src/remote/remote_client.cpp src/remote/remote_control_server.cpp"
    [ "$status" -ne 0 ]  # non-zero == no inline lookup found
}

@test "the control-socket-directory gate fails when a caller inlines the lookup" {
    local probe="$BATS_TEST_TMPDIR/inlined.cpp"
    printf 'const char* d = getenv("XDG_RUNTIME_DIR");\n' > "$probe"
    run bash -c "grep -nE 'getenv\(\"(XDG_RUNTIME_DIR|RUNTIME_DIRECTORY)\"' '$probe'"
    [ "$status" -eq 0 ]  # the gate above can go red
}
