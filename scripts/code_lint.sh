#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tree-wide code lints: architectural rules each too small for a script of its
# own. Every lint is a lint_<name> function that reads the tree under the
# current directory and prints its offenders; the checks it is built from take
# their inputs as arguments, so tests/shell/test_code_lint.bats drives them on
# fixtures. quality-checks.sh runs them by group (scripts/qc/code_lint.sh,
# test_lint.sh, ui_lint.sh).
#
# Usage:
#   scripts/code_lint.sh                 # every lint, from the repo root
#   scripts/code_lint.sh --group src     # one group: src, tests or ui
#   scripts/code_lint.sh NAME...         # the named lints
#   scripts/code_lint.sh --list          # group and name of every lint
#
# Sourced, it defines the functions and runs nothing.

CODE_LINT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Group and name of every lint, one per line.
code_lint_registry() {
    cat <<'EOF'
src for_testing_headers
src for_testing_sources
src busyclock_advance_for_test
src raw_set_temperature
src flush_stride_helper
src centidegree_misnomer
src inline_decidegrees
src moonraker_concrete_types
src switch_printer_clears_caches
src rtti_shapes
src any_type_rtti
src std_regex
src bare_fmt_include
src raw_threads
src wifi_manager_owner
src subjects_value_initialized
src raw_defer_idioms
src queue_update_tagged
src old_observer_names
src history_lazy_fetch
src deinit_all_does_not_log
src observer_guard_move
src backend_friend_lists
src on_deactivate_overrides
src overlay_cleanup_calls_base
src zoffset_publisher_wiring
src weight_poll_is_weight_only
src lane_store_encapsulation
src mock_publishes_lane_readings_once
src tool_labels
src control_socket_dir
src getenv_setenv
src override_store_lifecycle
src slot_validator
src own_endpoint_http
src ui_nav_manager_includers
src esp32_psram_walk
src guided_device_flows
src init_list_ternary
src address_keys
src registry_key_names
src remove_event_sweep
tests test_access_defined_once
tests test_mode_restored
tests test_displays_deleted
tests ams_state_published
tests app_skip_require_app
ui chamber_target_binding
ui zoffset_save_rule
ui plugin_separator
ui flex_styles_layout
ui short_clicked_taps
ui filaments_android_mirror
EOF
}

# ====================================================================
# Shared helpers
# ====================================================================

# The one source file under src/ or include/ matching an ERE only a symbol's
# definition matches, so a check follows the code when it moves between files
# instead of reading a path that no longer holds it. Prints why and fails on none
# or several, so a check never runs against the wrong file or none. Only .cpp/.h
# count: a stray .orig or editor backup beside the real file is not a second one.
locate_by_symbol() {
    local pattern="$1" hits count
    hits=$(git grep --untracked -lE -e "$pattern" -- 'src/*.cpp' 'src/*.h' 'include/*.h' 2>/dev/null || true)
    count=$(printf '%s\n' "$hits" | grep -c . || true)
    if [ "$count" -ne 1 ]; then
        echo "locate_by_symbol: '$pattern' matches $count file(s) under src/ and include/, expected 1:" >&2
        # shellcheck disable=SC2086  # paths have no spaces; one per line
        [ -n "$hits" ] && printf '  %s\n' $hits >&2
        return 1
    fi
    printf '%s\n' "$hits"
}

# Fails, naming the lint, when a grep found anything. $1 = what was found.
none_found() {
    [ -z "$1" ] && return 0
    printf '%s\n' "$1"
    return 1
}

# C++ sources in the shared/portable scope: src/, include/, firmware/ and the
# helix-xml submodule. git ls-files rather than find: it keeps firmware/*/build/
# and the vendored managed_components/ out with no exclusion list to maintain
# (both are gitignored). --others --exclude-standard adds files that are new but
# not yet staged, so a lint fires on a fresh source file before it is committed.
# helix-xml needs its own ls-files: it is a submodule, and in a worktree it is a
# symlink, so neither the parent's index nor `find` reaches it the same way.
rtti_lint_files() {
    git ls-files --cached --others --exclude-standard src include firmware |
        grep -E '\.(cpp|h)$'
    git -C lib/helix-xml ls-files --cached --others --exclude-standard |
        grep -E '\.(cpp|h)$' | sed 's@^@lib/helix-xml/@'
}

# Print `file:line: text` for each line of $3.. matching the ERE in $1, ignoring
# comments and lines carrying the literal opt-out token in $2 (empty $2 = the
# lint has no opt-out). The pattern travels through the environment, not
# `awk -v`: -v runs escape processing over the value, which turns `\.` into a
# match-anything `.` and emits a warning for every escape.
# Optional CODE_LINT_JOIN (an ERE): a code line matching it is held and joined
# to the next line before matching, reported at the first line. That catches a
# declaration clang-format split after its type.
code_offenders() {
    local pat="$1" optout="$2"
    shift 2
    # A rule with nothing to read is a rule that stopped checking, so it reports.
    if [ "$#" -eq 0 ]; then
        echo "code_lint: no files to scan for: $pat"
        return 0
    fi
    local f
    for f in "$@"; do
        [ "$f" = /dev/null ] && { echo "code_lint: no files to scan for: $pat"; continue; }
        [ -f "$f" ] || echo "code_lint: $f is gone: update this lint's file list"
    done
    CODE_LINT_PAT="$pat" CODE_LINT_OPTOUT="$optout" awk '
        BEGIN { pat = ENVIRON["CODE_LINT_PAT"]; optout = ENVIRON["CODE_LINT_OPTOUT"]; join = ENVIRON["CODE_LINT_JOIN"] }
        FNR == 1 { in_block = 0; held = "" }
        optout != "" && index($0, optout) > 0 { held = ""; next }
        {
            # Walk the line left to right, honouring whichever delimiter opens
            # first. A `//` and a `/*` on one line is not hypothetical:
            # src/system/config.cpp documents a `scanner/*` config path inside a
            # `///` comment, and stripping block comments first would latch
            # in_block there and swallow the rest of the FILE.
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
            if (held != "") {
                code = held " " code
                text = held_text " " $0
                where = held_fnr
                held = ""
            } else {
                text = $0
                where = FNR
            }
            if (join != "" && code ~ join) {
                held = code; held_text = text; held_fnr = where
                next
            }
            if (code ~ pat) print FILENAME ":" where ": " text
        }
    ' "$@"
}

# ====================================================================
# No _for_testing methods in production code
# ====================================================================
# Test-only methods belong in test files via a friend TestAccess class.
# *_mock.h files are excluded: mocks ARE test infrastructure, gated by
# HELIX_ENABLE_MOCKS and never in a production build.
lint_for_testing_headers() {
    none_found "$(grep -rn '_for_testing' include --include='*.h' --exclude='*_mock.h')"
}

lint_for_testing_sources() {
    none_found "$(grep -rn '_for_testing' src --include='*.cpp')"
}

# Advancing BusyClock in a shipped build would skew the idle_timeout settle and
# the busy-episode ownership windows. Only its declaration may name it.
lint_busyclock_advance_for_test() {
    none_found "$(grep -rn 'advance_for_test' src include |
        grep -v '^include/idle_timeout_busy.h:.*static void advance_for_test(')"
}

# ====================================================================
# Temperature sends and conversions
# ====================================================================
# These view files delegate temperature commands to helix::TemperatureController.
# A direct send is either `api_->set_temperature(` or any `->set_temperature(`
# whose receiver is not a `controller`.
RAW_SET_TEMPERATURE_FILES="src/ui/ui_overlay_temp_graph.cpp src/ui/ui_panel_controls.cpp src/system/post_op_cooldown_manager.cpp src/ui/panel_widgets/preheat_widget.cpp src/ui/calibration/ui_panel_bed_mesh.cpp src/ui/filament/ui_panel_filament.cpp src/ui/temperature_service.cpp src/ui/filament/ui_ams_sidebar.cpp"

raw_set_temperature_offenders() {
    grep -n 'api_->set_temperature' "$@"
    grep -nE '\->set_temperature\(' "$@" | grep -v 'controller'
    return 0
}

lint_raw_set_temperature() {
    # shellcheck disable=SC2086  # paths have no spaces
    none_found "$(raw_set_temperature_offenders $RAW_SET_TEMPERATURE_FILES)"
}

# The dbuf-stride-or-fallback rule (active_buf->header.stride when queryable and
# positive, else lv_draw_buf_width_to_stride(area_w, cf)) serves two flush-path
# consumers, ColorTransform::select_flush_region and the remote-screen mirror in
# DisplayManager's flush hook. It lives once, in helix::flush_px_map_stride
# (include/flush_stride.h); an inline copy at either call site is a twin that
# can drift silently (prestonbrown/helixscreen#1610).
lint_flush_stride_helper() {
    local folded="src/application/color_transform.cpp src/application/display_manager.cpp" f rc=0
    # shellcheck disable=SC2086
    if grep -n 'header.stride > 0' $folded; then
        echo "inline stride rule: call helix::flush_px_map_stride()"
        rc=1
    fi
    for f in $folded; do
        grep -q 'flush_px_map_stride(' "$f" || { echo "$f does not call flush_px_map_stride()"; rc=1; }
    done
    [ "$(grep -c 'header.stride > 0' include/flush_stride.h)" = 1 ] ||
        { echo "include/flush_stride.h must hold the stride rule exactly once"; rc=1; }
    return $rc
}

# Subject temperatures are degrees x 10 = DECIdegrees (1 unit = 0.1C); a
# centidegree would be degrees x 100. Excluded paths record release history
# (CHANGELOG.md), are generated or vendored, mirror upstream wording
# (translation catalogs), or name this rule itself.
lint_centidegree_misnomer() {
    none_found "$(git grep -iIln 'centidegree' -- \
        ':(exclude)CHANGELOG.md' \
        ':(exclude)scripts/code_lint.sh' \
        ':(exclude)tests/shell/test_code_lint.bats' \
        ':(exclude)docs/devel/REVIEW_RUBRIC.md' \
        ':(exclude,glob)docs/superpowers/**' \
        ':(exclude,glob)src/generated/**' \
        ':(exclude,glob)lib/**' \
        ':(exclude,glob)translations/**' \
        ':(exclude,glob)ui_xml/translations/**' \
        ':(exclude,glob)scripts/translations/**')"
}

# Subjects store decidegrees; converting to/from degrees inline (`x / 10`,
# `x * 10`) bypasses helix::units / helix::ui::temperature and silently risks a
# truncation/rounding mismatch (int trunc vs float). These files use the helpers
# (deci_to_degrees / deci_to_degrees_f / degrees_to_deci / to_decidegrees /
# from_decidegrees).
#
# The x10 pattern matches a temperature identifier (target/temp/deci/nozzle/
# bed/chamber/heater) or a bare keypad `value`, optionally closing a paren, then
# `* 10` or `/ 10`; the (\.0?f?)?([^0-9.]|$) tail rejects a trailing digit. `//`
# comments are stripped first.
#
# The x100 pattern catches a decidegree read at 1/10 scale (45C shown as "4C"),
# which the x10 tail lets through. Its identifier set is narrower: the keypad
# converts centimm via `value / 100`, and bed-mesh Z values are distances.
INLINE_DECIDEGREE_FILES="src/print/print_start_collector.cpp src/api/moonraker_api_controls.cpp src/api/moonraker_discovery_sequence.cpp src/printer/ams_backend_ad5x_ifs.cpp src/printer/ams_backend_cfs.cpp src/system/telemetry_manager.cpp src/ui/panel_widgets/nozzle_temps_widget.cpp src/ui/filament/ui_ams_sidebar.cpp src/ui/temperature_service.cpp src/ui/ui_overlay_temp_graph.cpp src/ui/calibration/ui_panel_bed_mesh.cpp src/ui/calibration/ui_panel_calibration_pid.cpp src/ui/ui_panel_controls.cpp src/ui/filament/ui_panel_filament.cpp src/ui/print/ui_print_preparation_manager.cpp src/ui/ui_temp_display.cpp"

inline_decidegree_offenders() {
    local pat='(target|temp|deci|nozzle|bed|chamber|heater|value)[A-Za-z_]*(\s*\))?\s*[*/]\s*10(\.0?f?)?([^0-9.]|$)'
    local pat100='(target|temp|deci|nozzle|chamber|heater)[A-Za-z_]*(\s*\))?\s*[*/]\s*100(\.0?f?)?([^0-9.]|$)'
    sed -E 's@//.*@@' "$@" | grep -nE "$pat|$pat100"
    return 0
}

lint_inline_decidegrees() {
    # shellcheck disable=SC2086
    none_found "$(inline_decidegree_offenders $INLINE_DECIDEGREE_FILES)"
}

# ====================================================================
# Concrete Moonraker types stay inside the network layer
# ====================================================================
# Consumers depend on the interfaces (helix::IMoonrakerClient,
# helix::IMoonrakerAPI and the ten sub-API interfaces in
# include/i_moonraker_sub_apis.h). The concretes live behind MoonrakerManager,
# which constructs them. The allowlist is the network-layer implementation
# files and *_mock.{h,cpp} (mocks inherit the concretes). It has no bare
# "test_" entry: this lint scans only src/ and include/, so a substring that
# broad would exempt any non-test path containing it.
#
# Not covered, by design: a consumer naming a concrete-class static constexpr
# timeout (MoonrakerJobAPI::kCancelTimeoutMs) or MoonrakerAdvancedAPI::MPCResult.
# Those are compile-time names defined on the interfaces and reached by
# inheritance or a `using` alias, so the ten sub-API concrete class names are
# left out of the pattern rather than allowlisting each consumer.

# The two shapes, spelled for awk: `\b` is a GNU grep extension awk does not
# implement (mawk reads it as a backspace), so the word boundaries around the
# bare class name are written out.
moonraker_concrete_pattern() {
    printf '%s' 'helix::MoonrakerClient|(^|[^A-Za-z0-9_])MoonrakerAPI([^A-Za-z0-9_]|$)'
}

MOONRAKER_CONCRETE_ALLOWLIST='moonraker_client|moonraker_manager|moonraker_api|moonraker_rest_api|moonraker_file_api|moonraker_file_transfer_api|moonraker_advanced_api|moonraker_history_api|moonraker_job_api|moonraker_motion_api|moonraker_queue_api|moonraker_spoolman_api|moonraker_timelapse_api|moonraker_request_tracker|moonraker_discovery_sequence|_mock'

lint_moonraker_concrete_types() {
    local candidates offenders
    # Narrow to candidate files first, then re-match each with comments
    # stripped: prose naming a concrete class is not a dependency on it. The
    # escape hatch is the allowlist, so widening it stays a visible edit.
    candidates=$(grep -rlE 'helix::MoonrakerClient|\bMoonrakerAPI\b' src include |
        grep -v -E "$MOONRAKER_CONCRETE_ALLOWLIST" || true)
    [ -z "$candidates" ] && return 0
    # shellcheck disable=SC2086
    offenders=$(code_offenders "$(moonraker_concrete_pattern)" "" $candidates)
    [ -z "$offenders" ] && return 0
    echo "Concrete Moonraker types referenced outside the network layer:"
    printf '%s\n' "$offenders"
    echo "Consumers depend on the interfaces only: helix::IMoonrakerAPI (include/i_moonraker_api.h),"
    echo "helix::IMoonrakerClient (include/i_moonraker_client.h), and the ten sub-API"
    echo "interfaces in include/i_moonraker_sub_apis.h. The concretes live behind"
    echo "MoonrakerManager (include/moonraker_manager.h), which constructs them."
    return 1
}

# ====================================================================
# switch_printer invalidates every per-printer cache before teardown
# ====================================================================
# Per-printer state lives under /printers/<id>/ and is reached via
# Config::df(). Any component that memoizes a df()-derived value serves the
# PREVIOUS printer's data after a switch (#804). Components self-register with
# PrinterCacheRegistry and switch_printer() fires them all, BEFORE teardown,
# while Config::df() has already moved to the new printer. No unit test reaches
# that success path: it ends in a full teardown and rebuild.

# Print the body of PrinterSwitchFlow::switch_printer() from the file given in $1.
switch_printer_body() {
    awk '
        /^bool PrinterSwitchFlow::switch_printer\(/ { inside = 1 }
        inside { print }
        inside && /^\}/ { exit }
    ' "$1"
}

check_switch_printer_clears_caches() {
    local body clear_line teardown_line
    body=$(switch_printer_body "$1")
    if [ -z "$body" ]; then
        echo "could not locate PrinterSwitchFlow::switch_printer() in $1"
        return 1
    fi

    clear_line=$(printf '%s\n' "$body" | grep -n 'PrinterCacheRegistry::instance().invalidate_all()' | head -1 | cut -d: -f1)
    if [ -z "$clear_line" ]; then
        echo "switch_printer() does not call PrinterCacheRegistry::instance().invalidate_all() (#804)"
        return 1
    fi

    # tear_down() is the flow's guarded wrapper around the m_restart.teardown hook.
    teardown_line=$(printf '%s\n' "$body" | grep -nE '^[[:space:]]*tear_down\(\);' | head -1 | cut -d: -f1)
    if [ -z "$teardown_line" ]; then
        echo "switch_printer() does not call tear_down()"
        return 1
    fi

    if [ "$clear_line" -ge "$teardown_line" ]; then
        echo "PrinterCacheRegistry::invalidate_all() must precede tear_down()"
        return 1
    fi
    return 0
}

lint_switch_printer_clears_caches() {
    local file
    file=$(locate_by_symbol '^bool PrinterSwitchFlow::switch_printer\(') || return 1
    check_switch_printer_clears_caches "$file"
}

# ====================================================================
# No RTTI code shapes (the firmware builds -fno-rtti)
# ====================================================================
# dynamic_cast, typeid, std::type_index and std::function::target_type() do not
# compile in the ESP32 firmware at all. This lint is for the DESKTOP side: a
# desktop-only file can reintroduce a shape that silently blocks the next file
# getting pulled into the firmware slice. tests/ is not scanned: test code never
# enters a firmware build.
#
# Comments are stripped before matching (the tree's prose explaining the
# RTTI-free replacements sits in /** */ blocks too). The std::any `.type()`
# shape is scoped to files that include <any>: `.type()` is an ordinary getter
# name elsewhere (LayoutManager::type()).
#
# Escape hatch: `// RTTI_OK: <reason>` on the same line.

# The forbidden shapes, as one ERE alternation. `\s` is avoided: it is a GNU
# extension that mawk (the default awk on Debian) does not support.
rtti_forbidden_pattern() {
    printf '%s' 'dynamic_cast[[:space:]]*<|typeid[[:space:]]*\(|std::type_index|\.target_type[[:space:]]*\('
}

# std::any's RTTI-dependent accessor: `a.type() == ...`, `!=`, or `.type().name()`.
# Pointer-form std::any_cast is the sanctioned idiom: it returns null on a type
# mismatch and needs no RTTI.
rtti_any_pattern() {
    printf '%s' '\.type[[:space:]]*\([[:space:]]*\)[[:space:]]*(==|!=|\.name)'
}

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

lint_rtti_shapes() {
    local offenders
    # shellcheck disable=SC2046  # paths have no spaces; word splitting is intended
    offenders=$(rtti_offenders "$(rtti_forbidden_pattern)" $(rtti_lint_files))
    [ -z "$offenders" ] && return 0
    echo "Forbidden RTTI shapes found:"
    printf '%s\n' "$offenders"
    rtti_advice
    return 1
}

lint_any_type_rtti() {
    local any_files offenders
    # shellcheck disable=SC2046
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

# ====================================================================
# std::regex, bare <fmt/...>, raw threads
# ====================================================================
# libstdc++'s std::regex backtracks by recursion, one frame per input character,
# so a long enough input overflows a thread stack. musl builds give every thread
# a small default stack and the Moonraker callbacks run on one of them.
# helix::Regex (include/helix_regex.h) keeps its backtracking on the heap.
std_regex_pattern() {
    printf '%s' '#[[:space:]]*include[[:space:]]*<regex>|std::(regex|smatch|cmatch|sregex|wregex)'
}

# src/tools/ holds host-only build tools.
std_regex_lint_files() {
    rtti_lint_files | grep -Ev '^src/tools/'
}

lint_std_regex() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(code_offenders "$(std_regex_pattern)" STD_REGEX_OK $(std_regex_lint_files))
    [ -z "$offenders" ] && return 0
    echo "std::regex in app code (its recursive backtracking overflows a thread stack):"
    printf '%s\n' "$offenders"
    echo "Use helix::Regex (include/helix_regex.h). Unavoidable? // STD_REGEX_OK: <reason>"
    return 1
}

# Only <spdlog/fmt/fmt.h> is on every build's include path; the standalone
# <fmt/...> headers are absent on hosts that build against spdlog's bundled fmt.
bare_fmt_include_pattern() {
    printf '%s' '#[[:space:]]*include[[:space:]]*<fmt/'
}

lint_bare_fmt_include() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(code_offenders "$(bare_fmt_include_pattern)" FMT_INCLUDE_OK \
        $(git ls-files --cached --others --exclude-standard src include tests firmware | grep -E '\.(cpp|h)$'))
    [ -z "$offenders" ] && return 0
    echo "bare <fmt/...> include (not on every host's include path):"
    printf '%s\n' "$offenders"
    echo "Use <spdlog/fmt/fmt.h>."
    return 1
}

# sigaltstack is per thread, so a thread that never installs its own signal
# stack dies on a stack overflow without writing a crash file. helix::make_thread
# (include/helix_thread.h) installs one before running the thread's body.
raw_thread_pattern() {
    printf '%s' 'std::j?thread[[:space:]]*[({]|std::j?thread[[:space:]]+[A-Za-z_][A-Za-z_0-9]*[[:space:]]*[({]|std::async[[:space:]]*\(|pthread_create[[:space:]]*\('
}

lint_raw_threads() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(code_offenders "$(raw_thread_pattern)" THREAD_OK \
        $(git ls-files --cached --others --exclude-standard src include |
            grep -E '\.(cpp|h)$' | grep -v '^include/helix_thread\.h$'))
    [ -z "$offenders" ] && return 0
    echo "Raw thread spawn (a stack overflow on it leaves no crash file):"
    printf '%s\n' "$offenders"
    echo "Use helix::make_thread (include/helix_thread.h). Unavoidable? Call"
    echo "helix::install_thread_altstack() first in the body and mark it // THREAD_OK: <reason>"
    return 1
}

# ====================================================================
# WiFiManager is built only by get_wifi_manager()
# ====================================================================
# ~WiFiManager waits, unbounded, for the WiFi control call in progress: the lane
# worker dereferences the manager for the whole call, so a bounded wait would
# free it under the worker. That wait is safe only because no production code
# destroys a WiFiManager: get_wifi_manager()'s instance is never freed. Any
# other owner would put a wedged wpa_supplicant on its destructor's path.
# Unavoidable? // WIFI_MANAGER_OWNER_OK: <reason>
wifi_manager_owner_pattern() {
    printf '%s' '(make_shared|make_unique)<(helix::)?WiFiManager>|new[[:space:]]+(helix::)?WiFiManager[[:space:]]*[({]|(^|[^_[:alnum:]:])(helix::)?WiFiManager[[:space:]]+[_[:alpha:]][_[:alnum:]]*[[:space:]]*[({;]'
}

lint_wifi_manager_owner() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(code_offenders "$(wifi_manager_owner_pattern)" WIFI_MANAGER_OWNER_OK \
        $(git ls-files --cached --others --exclude-standard src include firmware | grep -E '\.(cpp|h)$'))
    [ -z "$offenders" ] && return 0
    echo "WiFiManager built outside get_wifi_manager() (its destructor can wait forever on a wedged daemon):"
    printf '%s\n' "$offenders"
    echo "Use helix::get_wifi_manager(). Unavoidable? // WIFI_MANAGER_OWNER_OK: <reason>"
    return 1
}

# ====================================================================
# lv_subject_t declarations are value-initialized (#1423)
# ====================================================================
# A subject that has not been through lv_subject_init_*() must read as
# LV_SUBJECT_TYPE_INVALID with an empty subscriber list, so a publish that
# arrives first is LVGL's warned no-op. Without `{}` a member or local holds
# whatever the allocation held, and garbage that reads as an INT subject sends
# lv_subject_set_int() down a wild subscriber list. `static` and `extern`
# declarations are not matched: static storage is zeroed already. A bare
# namespace-scope global is zeroed too but is still flagged, since the line
# alone cannot tell it from a member; brace it.
# Unavoidable? // SUBJECT_INIT_OK: <reason>
subject_no_init_pattern() {
    local lead='^[[:space:]]*(mutable[[:space:]]+)?'
    local name='[_[:alnum:]]+[[:space:]]*(\[[^]]*\][[:space:]]*)*'
    printf '%s' "${lead}(::)?lv_subject_t[[:space:]]+([^;]*,[[:space:]]*)?${name}[,;]|${lead}std::array<[[:space:]]*(::)?lv_subject_t[[:space:]]*,.*>[[:space:]]+${name};"
}

# A type alone on its line: clang-format splits long declarations there.
subject_split_pattern() {
    printf '%s' '^[[:space:]]*(mutable[[:space:]]+)?(::)?lv_subject_t[[:space:]]*$'
}

subject_offenders() {
    CODE_LINT_JOIN="$(subject_split_pattern)" code_offenders "$(subject_no_init_pattern)" SUBJECT_INIT_OK "$@"
}

lint_subjects_value_initialized() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(subject_offenders $(rtti_lint_files))
    [ -z "$offenders" ] && return 0
    echo "lv_subject_t declared without an initializer (reads as garbage until init):"
    printf '%s\n' "$offenders"
    echo "Write \`lv_subject_t name_{};\`. Unavoidable? // SUBJECT_INIT_OK: <reason>"
    return 1
}

# ====================================================================
# One way to defer to the main thread per job
# ====================================================================
# UpdateQueue work bound to an object goes through AsyncLifetimeGuard::defer or
# bg_cb, unowned work through queue_update(tag, fn), and work that needs LVGL's
# own async list through run_next_tick (include/ui_next_tick.h). A raw
# lv_async_call or a void* async_call carries a bare `this` with no lifetime
# check, so it is banned outside run_next_tick itself.
# Unavoidable? // LV_ASYNC_OK: <reason>
defer_idiom_pattern() {
    printf '%s' '(^|[^_[:alnum:]])(lv_)?async_call[[:space:]]*\(|helix::async::'
}

lint_raw_defer_idioms() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(code_offenders "$(defer_idiom_pattern)" LV_ASYNC_OK \
        $(rtti_lint_files | grep -Ev '^include/ui_next_tick\.h$'))
    [ -z "$offenders" ] && return 0
    echo "Raw main-thread deferral (no lifetime check on what it captures):"
    printf '%s\n' "$offenders"
    echo "Use lifetime_.defer / bg_cb, queue_update(tag, fn), or helix::ui::run_next_tick."
    return 1
}

# The test-isolation listener reports a leaked UpdateQueue callback by its tag;
# an untagged one reads `<untagged>`, which names no owner (#1685). App code
# passes a string literal first: queue_update("Owner::method", fn). The
# forwarding wrappers in include/ thread file/line instead and are not linted.
# A tag forwarded through a variable? // QUEUE_TAG_OK: <reason>
queue_untagged_pattern() {
    printf '%s' '(^|[^_[:alnum:]])queue_update[[:space:]]*\([[:space:]]*[^"[:space:]]'
}

# clang-format wraps a long call after the open paren; join that line to the next.
queue_split_pattern() {
    printf '%s' '(^|[^_[:alnum:]])queue_update[[:space:]]*\([[:space:]]*$'
}

lint_queue_update_tagged() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(CODE_LINT_JOIN="$(queue_split_pattern)" code_offenders \
        "$(queue_untagged_pattern)" QUEUE_TAG_OK \
        $(git ls-files --cached --others --exclude-standard src firmware | grep -E '\.(cpp|h)$'))
    [ -z "$offenders" ] && return 0
    echo "Untagged queue_update() (a leak would report <untagged>):"
    printf '%s\n' "$offenders"
    echo "Pass the owner as a literal first: queue_update(\"Owner::method\", fn)."
    return 1
}

# observe<int> / observe<const char*> with an optional Dispatch::Immediate is
# the one observer factory; the per-type names do not compile, and
# scripts/rename_observers.py rewrites them mechanically.
old_observer_pattern() {
    printf '%s' '(^|[^_[:alnum:]])observe_(int_sync|int_immediate|int_async|string|string_immediate|print_state_immediate)[[:space:]]*[<(]'
}

lint_old_observer_names() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(code_offenders "$(old_observer_pattern)" "" $(rtti_lint_files) \
        $(git ls-files --cached --others --exclude-standard tests/unit | grep -E '\.(cpp|h)$'))
    [ -z "$offenders" ] && return 0
    echo "Retired observer factory name:"
    printf '%s\n' "$offenders"
    echo "Run scripts/rename_observers.py (observe<int>/observe<const char*>, Dispatch::Immediate)."
    return 1
}

# ====================================================================
# History lazy loads use ensure_loaded(), never fetch()
# ====================================================================
# PrintHistoryManager::fetch() is the INVALIDATION entry point: with a request
# already out it arms one re-issue, because a response issued before a delete
# cannot describe that delete. ensure_loaded() is the LAZY-LOAD entry point and
# returns instead. Every invalidation lives inside the manager itself, so
# fetch() has no legitimate caller in src/ outside print_history_manager.cpp,
# and any other is a lazy load that pulls the whole 500-job list twice. tests/
# is out of scope: a fixture priming a fresh manager is an honest fetch().
history_lazy_fetch_offenders() {
    grep -rnE '(history[A-Za-z_]*|hm)->fetch\(' "$1" --include='*.cpp' --include='*.h' |
        grep -v '/print_history_manager\.cpp:' || true
}

lint_history_lazy_fetch() {
    none_found "$(history_lazy_fetch_offenders src)"
}

# ====================================================================
# SubjectManager::deinit_all() must not log
# ====================================================================
# A SubjectManager owned by a static reaches deinit_all() through the C++
# atexit chain. spdlog's registry is a lazily constructed function-local
# static, so it is torn down before objects built during dynamic
# initialization: a log call here reads a freed logger.
check_deinit_all_does_not_log() {
    local file="$1" body
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

lint_deinit_all_does_not_log() {
    local file
    file=$(locate_by_symbol 'void deinit_all\(\) \{') || return 1
    check_deinit_all_does_not_log "$file"
}

# ====================================================================
# ObserverGuard's move paths clear the moved-from cleanup (#1446)
# ====================================================================
# A moved-from std::function is valid but UNSPECIFIED. libc++ keeps the target
# when the callable fits its small-object buffer, and ObserverGuard's cleanup
# does, so a source that kept it frees the observer's context from its own
# destructor while the destination keeps the observer attached. libstdc++
# empties the source, so no unit test on Linux can fail on it.
check_observer_guard_move_clears_cleanup() {
    local file="$1" moves
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

lint_observer_guard_move() {
    local file
    file=$(locate_by_symbol '^ObserverGuard::ObserverGuard\(ObserverGuard&& other\)') || return 1
    check_observer_guard_move_clears_cleanup "$file"
}

# ====================================================================
# A friend list is a fixed, reviewable set
# ====================================================================
# AmsBackendAfc and AmsBackendHappyHare each expose internals to tests through
# a single XTestAccess shim; LaneSourceStore names its two writer funnels.
# Widening what a header befriends is a review-worthy edit, so this compares
# the file's friend declarations against an exact expected set. The match is
# on normalised whitespace (clang-format may reflow a signature) and
# order-insensitive: friend declarations are a set of grants, not a sequence.
check_friend_list() {
    local file="$1" expected expected_count friends found_count
    expected=$(printf '%s\n' "$2" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//; s/[[:space:]]+/ /g')
    expected_count=$(printf '%s\n' "$expected" | grep -c .)

    friends=$(grep -E '^[[:space:]]*friend ' "$file" 2>/dev/null |
        sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//; s/[[:space:]]+/ /g' || true)

    if [ -z "$friends" ]; then
        echo "no friend declaration in $file; expected exactly $expected_count:"
        echo "$expected"
        return 1
    fi

    if [ "$(printf '%s\n' "$friends" | sort)" != "$(printf '%s\n' "$expected" | sort)" ]; then
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

lint_backend_friend_lists() {
    local file rc=0
    file=$(locate_by_symbol '^class AmsBackendAfc[[:space:]]*:') &&
        check_friend_list "$file" "friend class AfcTestAccess;" || rc=1
    file=$(locate_by_symbol '^class AmsBackendHappyHare[[:space:]]*:') &&
        check_friend_list "$file" "friend class HappyHareTestAccess;" || rc=1
    return $rc
}

# ====================================================================
# No on_deactivate() overrides on panels and overlays
# ====================================================================
# ViewLifecycleBase makes on_deactivate(DeactivateReason) final and dispatches
# to on_deactivating(reason), so a subclass re-declaring the no-argument
# spelling overrides nothing: it compiles, it is never called, and the work it
# meant to do silently stops happening. PanelWidget is a separate hierarchy
# with its own on_deactivate() hook and keeps the name.
check_no_on_deactivate_overrides() {
    local hits="" file found
    while IFS= read -r file; do
        # The widget hierarchy, by directory and by the base it names: an
        # implementation file mentions PanelWidget only via its own header.
        case "$file" in */panel_widgets/*) continue ;; esac
        grep -q 'PanelWidget' "$file" && continue
        found=$(grep -nE '(\bvoid[[:space:]]+on_deactivate[[:space:]]*\(\)|::on_deactivate[[:space:]]*\(\))' "$file")
        [ -n "$found" ] && hits+="${file}:${found}"$'\n'
    done < <(grep -rl 'on_deactivate' "$@" --include='*.h' --include='*.cpp' 2>/dev/null)

    if [ -n "$hits" ]; then
        echo "on_deactivate() is final on ViewLifecycleBase - override"
        echo "on_deactivating(DeactivateReason) instead. Offending sites:"
        echo "$hits"
        return 1
    fi
    return 0
}

lint_on_deactivate_overrides() {
    check_no_on_deactivate_overrides src include
}

# ====================================================================
# An OverlayBase cleanup() override must reach the base
# ====================================================================
# OverlayBase::cleanup() is the only thing that invalidates object_lifetime_,
# the guard whose whole point is surviving deactivation. A subclass that
# overrides cleanup() and never calls the base leaves that guard live for the
# rest of the process, so callbacks parked on it still fire into a torn-down
# overlay. The wizard steps and the list views own an unrelated cleanup().
check_overlay_cleanup_calls_base() {
    local classes
    classes=$(grep -rhoE 'class[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*:[[:space:]]*public[[:space:]]+OverlayBase' \
        "$@" --include='*.h' --include='*.cpp' 2>/dev/null |
        awk '{print $2}' | sort -u | paste -sd'|' -)
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
        echo "OverlayBase::cleanup() - it is what invalidates both lifetime guards."
        echo "Offending sites:"
        echo "$hits"
        return 1
    fi
    return 0
}

lint_overlay_cleanup_calls_base() {
    # A pass that inspected nothing reads exactly like a pass that inspected
    # everything, so the overrides that exist must be found first.
    local found
    found=$(grep -rl 'OverlayBase::cleanup()' src --include='*.cpp' | wc -l | tr -d ' ')
    if [ "$found" -lt 10 ]; then
        echo "found $found OverlayBase::cleanup() callers under src/, expected at least 10"
        return 1
    fi
    check_overlay_cleanup_calls_base src include
}

# ====================================================================
# The Z-offset save-availability rule has ONE definition
# ====================================================================
# "Is there an offset worth saving" is helix::zoffset::save_available(),
# published to XML as the z_offset_save_available subject. An inline cond= that
# re-derives it from a tool-dirty subject is a second copy, and copies agree by
# convention until one is widened and the others are not
# (prestonbrown/helixscreen#1517).
zoffset_save_rule_copies() {
    grep -rnE 'cond="[^"]*any_tool_(offset|[xyz]_)dirty' "$@" 2>/dev/null || true
}

# The file list is DISCOVERED from the button names, not spelled out: a surface
# added later, or a breakpoint override with its own copy of the button, has to
# bind the rule too.
zoffset_save_button_files() {
    grep -rlE 'name="(btn_save_z_offset|header_save_z_offset)"' "$@" 2>/dev/null || true
}

zoffset_save_buttons_unbound() {
    local f
    for f in "$@"; do
        grep -qF 'subject="z_offset_save_available"' "$f" || echo "$f"
    done
}

lint_zoffset_save_rule() {
    local files rc=0
    none_found "$(zoffset_save_rule_copies ui_xml --include='*.xml')" || rc=1
    files=$(zoffset_save_button_files ui_xml --include='*.xml')
    # Fail closed: an empty list would pass having examined nothing.
    if [ "$(printf '%s\n' "$files" | grep -c .)" -lt 3 ]; then
        echo "found fewer than 3 Z-offset save surfaces under ui_xml/"
        rc=1
    fi
    # shellcheck disable=SC2086
    none_found "$(zoffset_save_buttons_unbound $files)" || rc=1
    return $rc
}

# The publisher observes subjects ToolState registers in init_ams_subjects(),
# so initialising it earlier would attach to nothing and leave every save
# surface bound to a subject that never moves.
# shellcheck disable=SC2120  # the fixture tests pass a path; the lint reads the tree
zoffset_publisher_wiring() {
    local f="${1:-src/application/subject_initializer.cpp}" ams init
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

lint_zoffset_publisher_wiring() {
    none_found "$(zoffset_publisher_wiring)"
}

# ====================================================================
# The Spoolman weight poll files a lane observation, never a whole SlotInfo
# ====================================================================
# The poll states what the server said by filing the lane's Spoolman source
# (helix::ams::ingest), and the resolver decides what the slot shows. Handing a
# backend a whole SlotInfo lets it re-derive state from fields the poll never
# meant to touch (#981, #1632), and the poll repeats on a timer.
check_weight_poll_is_weight_only() {
    local file="$1"
    if [ ! -f "$file" ]; then
        echo "could not locate $file"
        return 1
    fi
    if grep -qE '(\.|->)(apply_user_edit|sync_external_identity)\(' "$file"; then
        echo "$file writes a whole SlotInfo; an automated weight poll must file the lane's Spoolman source through helix::ams::ingest() instead"
        return 1
    fi
    return 0
}

lint_weight_poll_is_weight_only() {
    local file
    file=$(locate_by_symbol '^void SpoolmanManager::refresh_spoolman_weights\(\)') || return 1
    check_weight_poll_is_weight_only "$file"
}

# ====================================================================
# The lane source store has exactly one mutable entry point
# ====================================================================
# LaneSourceStore::write() is private, so ingest() and commit_slot_edit() are
# the only code that can reach a lane's records; a third friend line, of any
# kind, would be a third writer. Every entry in this list is a grant some code
# uses.
LANE_STORE_FRIENDS_EXPECTED="friend void ingest(LaneId, const Observation&);
friend void commit_slot_edit(LaneId, const Observation&);"

# write() is the one method the friend list gates. Renamed while the list
# stayed put, the list would describe a method that no longer exists.
check_lane_store_write_declared() {
    if ! grep -qE '^[[:space:]]*void write\(LaneId lane, const Observation& obs, bool amend\);' \
        "$1" 2>/dev/null; then
        echo "LaneSourceStore::write is not declared where the friend list guards it"
        return 1
    fi
    return 0
}

# The store's singleton is reachable only from the funnels' own translation
# unit. A caller holding the instance is one edit away from a write.
check_no_lane_store_instance_callers() {
    local dir hits
    for dir in "$@"; do
        if [ ! -d "$dir" ]; then
            echo "search path $dir does not exist; cannot confirm the lane store has no outside callers"
            return 1
        fi
    done
    hits=$(grep -rn 'LaneSourceStore::instance()' "$@" --include='*.cpp' --include='*.h' 2>/dev/null |
        grep -v 'printer/lane_source_store\.cpp:' || true)
    if [ -n "$hits" ]; then
        echo "LaneSourceStore::instance() is reachable only from the funnels."
        echo "Call helix::ams::ingest() or helix::ams::commit_slot_edit() instead:"
        echo "$hits"
        return 1
    fi
    return 0
}

# The friend list only restricts anything while write() stays private: which
# access label is in force at the write() declaration inside class
# LaneSourceStore, private before any label (an unlabelled class body).
check_lane_store_write_is_private() {
    local file="$1" access
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

lint_lane_store_encapsulation() {
    local file rc=0
    file=$(locate_by_symbol '^class LaneSourceStore[[:space:]]*\{') || return 1
    check_friend_list "$file" "$LANE_STORE_FRIENDS_EXPECTED" || rc=1
    check_lane_store_write_declared "$file" || rc=1
    check_lane_store_write_is_private "$file" || rc=1
    check_no_lane_store_instance_callers src include || rc=1
    return $rc
}

# ====================================================================
# The mock files its lane readings from exactly one call site
# ====================================================================
# AmsBackendMock::publish_lane_observations() reads the persistent SlotInfo
# that apply_user_edit() writes a user's colour and material into, so it states
# simulated firmware truth only where no edit can have landed yet: the tail of
# start(). A second call site would file the user's own values back as
# something the simulated machine reported.
check_mock_publishes_lane_readings_once() {
    local file="$1" calls count
    if [ ! -f "$file" ]; then
        echo "could not locate $file"
        return 1
    fi
    if ! grep -qE '^void AmsBackendMock::publish_lane_observations\(\)' "$file"; then
        echo "AmsBackendMock::publish_lane_observations is not defined in $file"
        return 1
    fi
    calls=$(grep -nE 'publish_lane_observations\(\)' "$file" |
        grep -v '::publish_lane_observations' || true)
    count=$(printf '%s\n' "$calls" | grep -c . || true)
    if [ "$count" -ne 1 ]; then
        echo "expected exactly 1 call to publish_lane_observations() in $file, found $count:"
        echo "$calls"
        return 1
    fi
    return 0
}

lint_mock_publishes_lane_readings_once() {
    local file
    file=$(locate_by_symbol '^void AmsBackendMock::publish_lane_observations\(\)') || return 1
    check_mock_publishes_lane_readings_once "$file"
}

# ====================================================================
# Small single-rule lints
# ====================================================================

# Tool labels and lane/slot display numbers go through display_numbering.h.
lint_tool_labels() {
    bash "$CODE_LINT_DIR/check_tool_labels.sh"
}

# Client and server each find the control socket directory through
# control_socket_dir(); reading the environment directly in either one is a
# second copy, and a client looking where the server never bound reports "no
# instance found" (prestonbrown/helixscreen#1602).
control_socket_dir_offenders() {
    grep -nE 'getenv\("(XDG_RUNTIME_DIR|RUNTIME_DIRECTORY)"' "$@"
    return 0
}

lint_control_socket_dir() {
    none_found "$(control_socket_dir_offenders src/remote/remote_client.cpp src/remote/remote_control_server.cpp)"
}

# Every first-party C and C++ root; lib/ is vendored submodules and scripts/ is
# not C++.
lint_getenv_setenv() {
    python3 "$CODE_LINT_DIR/check_getenv_setenv.py" \
        tests/ src/ include/ tools/ firmware/ android/ server/ moonraker-plugin/ ui_xml/
}

# A FilamentSlotOverrideStore is only useful once loaded into the map the
# backend reads; helix::ams::make_loaded_override_store() returns both, so
# "constructed but never loaded" has no spelling. A hand-built store persists
# nothing and fails silently until the next launch. Keyed on the TYPE, not the
# member name, so a split build-then-move or a differently named member is
# caught too. AFC's and Happy Hare's lane_publish_store_ holds no map to load.
override_store_offenders() {
    grep -rnE 'make_unique<[^>]*FilamentSlotOverrideStore|new[[:space:]]+[A-Za-z:]*FilamentSlotOverrideStore' \
        --include='*.cpp' --include='*.h' "$1" 2>/dev/null |
        grep -v 'printer/filament_slot_override_store\.cpp' |
        grep -v 'lane_publish_store_' || true
}

lint_override_store_lifecycle() {
    none_found "$(override_store_offenders src; override_store_offenders include)"
}

# AmsSubscriptionBackend::validate_slot_index() is the one answer to "is this
# slot index valid" (#1624). A backend declaring its own hides the base's and
# picks its own bound; a backend whose range is not total_slots overrides the
# bound instead.
slot_validator_offenders() {
    grep -nE 'AmsError[[:space:]]+validate_slot_index(_locked)?[[:space:]]*\(' \
        "$1"/ams_backend_*.h 2>/dev/null || true
}

lint_slot_validator() {
    none_found "$(slot_validator_offenders include)"
}

# trusted_request()/trusted_download() verify the server certificate; a plain
# requests:: call to the update, telemetry, crash or debug-bundle servers does
# not. Uploads to the ingest workers go through helix::ingest::post, the one
# sender that carries the API key. Requests to the printer's LAN services keep
# requests:: on purpose. $1 = the root holding src/.
own_endpoint_http_offenders() {
    local root="$1" f
    for f in update_checker telemetry_manager crash_reporter ingest_client; do
        grep -nE '^[^/]*(requests::|HttpClient)' "$root/src/system/$f.cpp" | sed "s|^|$f.cpp:|"
    done
    # Any other file naming one of our hosts must reach it through trusted_*.
    grep -rlE 'helixscreen\.org|api\.github\.com|github(usercontent)?\.com/prestonbrown' \
        "$root/src" | while read -r f; do
        if grep -qE '^[^/]*(requests::|HttpClient)' "$f" && ! grep -q 'tls::trusted_' "$f"; then
            echo "${f#"$root"/}: names one of our hosts but sends without tls::trusted_*"
        fi
    done
    awk '/= worker_url\(\);|= kWorkerUrl;/ { on = 1; seen = 1 }
         on && /^[^\/]*(requests::|HttpClient)/ { print "debug_bundle_collector.cpp:" FNR ": " $0 }
         on && /^}/ { on = 0 }
         END { if (!seen) print "debug_bundle_collector.cpp: upload to kWorkerUrl not found" }' \
        "$root/src/system/debug_bundle_collector.cpp"
    grep -rnF '["X-API-Key"]' "$root/src" | grep -v '^[^:]*/src/system/ingest_client\.cpp:' |
        sed 's|$| (ingest uploads go through helix::ingest::post)|'
}

lint_own_endpoint_http() {
    none_found "$(own_endpoint_http_offenders .)"
}

# ui_nav.h holds the navigation calls most callers need (push, go_back, close,
# overlay registration, close callbacks, set_active). Editing ui_nav_manager.h
# rebuilds every file that includes it, so a caller that only needs those
# takes helix::nav::* and ui_nav.h. The count may fall, never rise: lower the
# limit when an includer narrows.
UI_NAV_MANAGER_INCLUDER_LIMIT=15

lint_ui_nav_manager_includers() {
    local includers count
    includers=$(grep -rlE '^#include "ui_nav_manager\.h"' src include | sort)
    count=$(printf '%s\n' "$includers" | grep -c .)
    [ "$count" -le "$UI_NAV_MANAGER_INCLUDER_LIMIT" ] && return 0
    echo "ui_nav_manager.h has $count includers in src/ and include/, limit $UI_NAV_MANAGER_INCLUDER_LIMIT."
    echo "Use helix::nav::* and ui_nav.h for push_overlay/go_back/close_overlay/"
    echo "register_overlay/unregister_overlay/on_close/clear_on_close/set_active/is_on_top/is_in_stack."
    printf '%s\n' "$includers"
    return 1
}

# heap_caps_get_largest_free_block() walks every block with interrupts masked
# for 20-30ms on the K-Touch, and the RGB panel's bounce-buffer refill misses
# for the whole walk: the screen glitches once per call. These files run per
# thumbnail decode, per file-detail open and per print-status build.
ESP32_PER_EVENT_FILES="include/esp_psram_thumbnail.h include/tinfl_inflate.h src/system/memory_utils.cpp src/ui/print/ui_panel_print_select.cpp src/print/active_print_media_manager.cpp src/ui/print/ui_panel_print_status.cpp"

lint_esp32_psram_walk() {
    local rc=0
    # shellcheck disable=SC2086
    none_found "$(grep -n 'heap_caps_get_largest_free_block\|heap_caps_get_info' $ESP32_PER_EVENT_FILES |
        grep -v ':[0-9]*: *//')" || rc=1
    # The thumbnail decode's allocator: its free_bytes() gates every decode.
    grep -q 'heap_caps_get_free_size(MALLOC_CAP_SPIRAM)' include/tinfl_inflate.h ||
        { echo "include/tinfl_inflate.h no longer reads free PSRAM with heap_caps_get_free_size"; rc=1; }
    return $rc
}

# A device action naming a guided flow is refused when the flow is
# unregistered, so every boot path that registers XML components must also
# register every family of flows the guided-flow header declares.
lint_guided_device_flows() {
    local f fn rc=0
    local -a families
    while IFS= read -r fn; do families+=("$fn"); done < <(
        grep -oE 'register_[a-z0-9_]+_device_flows\(\);' include/ui_guided_device_flow.h |
            grep -v '^register_builtin_')
    if [ "${#families[@]}" -lt 2 ]; then
        echo "found ${#families[@]} flow families in include/ui_guided_device_flow.h"
        return 1
    fi
    for fn in "${families[@]}"; do
        [ "$(grep -cE "^[[:space:]]*${fn%();},\$" src/ui/filament/ui_guided_device_flow.cpp)" = 1 ] ||
            { echo "register_builtin_guided_device_flows() does not run ${fn%;}"; rc=1; }
    done
    for f in src/application/application.cpp firmware/helixscreen-esp32/components/helixapp/app_boot.cpp; do
        [ "$(grep -cF "register_builtin_guided_device_flows();" "$f")" = 1 ] ||
            { echo "$f does not call register_builtin_guided_device_flows()"; rc=1; }
    done
    return $rc
}

lint_init_list_ternary() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(python3 "$CODE_LINT_DIR/check_init_list_ternary.py" \
        $(git ls-files --cached --others --exclude-standard src include firmware | grep -E '\.(cpp|h|hpp)$'))
    [ -z "$offenders" ] && return 0
    echo "initializer_list stored from a ternary (it dangles):"
    printf '%s\n' "$offenders"
    return 1
}

# The MIPS and K2 builds pass -fmerge-all-constants with LTO, which folds
# objects with identical contents onto one address. Lua registry keys
# (lua_rawsetp / lua_rawgetp) are addresses, so constant keys share one
# registry slot and read each other's state (#1995). Keys are writable
# RegistryKey objects with a unique name each (include/lua_runtime.h).
# Unavoidable? // ADDRESS_KEY_OK: <reason>
address_key_pattern() {
    local q='(^|[^_[:alnum:]])(const|constexpr)[[:space:]]+'
    printf '%s' "${q}((helix::)?(plugin::)?RegistryKey)[[:space:]]|RegistryKey[[:space:]]+const[[:space:]]|${q}((un)?signed[[:space:]]+)?char[[:space:]]+[_[:alnum:]]+[[:space:]]*(=[[:space:]]*0|\{[[:space:]]*0?[[:space:]]*\})[[:space:]]*;"
}

lint_address_keys() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(code_offenders "$(address_key_pattern)" ADDRESS_KEY_OK $(rtti_lint_files))
    [ -z "$offenders" ] && return 0
    echo "A constant whose address is a key (merged with every other such constant on MIPS/K2):"
    printf '%s\n' "$offenders"
    echo "Declare a writable \`RegistryKey g_name_key{\"name\"};\` with a unique name. Unavoidable? // ADDRESS_KEY_OK: <reason>"
    return 1
}

registry_key_duplicate_names() {
    grep -rhoE 'RegistryKey[[:space:]]+[_[:alnum:]]+[[:space:]]*\{"[^"]*"\}' "$@" |
        sed -E 's/.*\{"([^"]*)"\}/\1/' | sort | uniq -d
}

lint_registry_key_names() {
    local dupes
    dupes=$(registry_key_duplicate_names src include)
    [ -z "$dupes" ] && return 0
    echo "RegistryKey names used twice: $dupes"
    return 1
}

# Removing an event by index is how a handler sweep is written, and a widget's
# event list also holds the unsubscribe-on-delete hooks of its bound styles and
# observers. A sweep takes those too, so the observers outlive the widget and
# fire into freed memory at the next subject change. lv_obj_remove_event_cb names
# what it removes, and an XML component takes its handler from a callback prop.
remove_event_sweep_pattern() {
    printf '%s' '(^|[^_[:alnum:]])lv_obj_remove_event[[:space:]]*\('
}

lint_remove_event_sweep() {
    local offenders
    # shellcheck disable=SC2046
    offenders=$(code_offenders "$(remove_event_sweep_pattern)" REMOVE_EVENT_OK \
        $(git ls-files --cached --others --exclude-standard src include | grep -E '\.(cpp|h)$'))
    [ -z "$offenders" ] && return 0
    echo "lv_obj_remove_event by index (it also removes the widget's observer cleanup hooks):"
    printf '%s\n' "$offenders"
    echo "Pass the handler through the component's callback prop, or lv_obj_remove_event_cb one by name. Unavoidable? // REMOVE_EVENT_OK: <reason>"
    return 1
}

# ====================================================================
# Test-tree lints
# ====================================================================

# A friend accessor defined in two test TUs is an ODR violation: the linker
# keeps one copy of each inline member, so a test can silently run another
# file's body. Define it once in tests/test_helpers/<name>_test_access.h.
test_access_duplicates() {
    local defs
    defs=$(grep -rEo --include='*.cpp' --include='*.h' \
        '^[[:space:]]*(class|struct)[[:space:]]+[A-Za-z0-9_]*TestAccess[[:space:]]*(final[[:space:]]*)?(:[^;]*)?\{' "$1" |
        sed -E 's/^([^:]+):[[:space:]]*(class|struct)[[:space:]]+([A-Za-z0-9_]+).*/\3 \1/' | sort -u)
    if [ -z "$defs" ]; then
        # An empty scan means the pattern stopped matching, not a clean tree.
        echo "no TestAccess class found under $1"
        return
    fi
    echo "$defs" | awk '{n[$1]++; f[$1]=f[$1]" "$2} END {for (c in n) if (n[c] > 1) print c ":" f[c]}'
}

lint_test_access_defined_once() {
    local dups
    dups=$(test_access_duplicates tests)
    [ -z "$dups" ] && return 0
    echo "TestAccess classes defined in more than one file:"
    echo "$dups"
    return 1
}

# test_mode is the master switch behind every should_mock_*() predicate, so a
# test file that writes it on the process-global RuntimeConfig and never puts
# it back changes behaviour for every test scheduled after it
# (prestonbrown/helixscreen#1287). File-level on purpose: every correct site
# pairs its write with a restore in the same file, via
# tests/test_helpers/scoped_runtime_config.h. Writes through a LOCAL
# `RuntimeConfig config;` are not global state and are not matched.
test_mode_global_writers() {
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

lint_test_mode_restored() {
    none_found "$(test_mode_unrestored_files tests --include='*.cpp' --include='*.h')"
}

# LVGLTestFixture owns the test binary's display. A test that creates another
# and keeps it makes that one the default, so every responsive decision after
# it reads a geometry production never sees. A display made in a static
# initializer exists in every process the file links into
# (prestonbrown/helixscreen#1943). File-level on purpose: a test that needs a
# second display for its own body deletes it in the same file.
# tests/lvgl_test_fixture.cpp makes the one display that is meant to live.

# Calls on a line of their own count; comment lines that name one do not.
display_calls() {
    grep -E "$1[[:space:]]*\\(" "$2" | grep -cvE '^[[:space:]]*(//|\*)'
}

unreleased_display_files() {
    local f creates deletes
    for f in $(grep -rlE 'lv_display_create[[:space:]]*\(' "$@" 2>/dev/null); do
        [[ "$f" == */lvgl_test_fixture.cpp ]] && continue
        creates=$(display_calls lv_display_create "$f")
        deletes=$(display_calls lv_display_delete "$f")
        [ "$deletes" -lt "$creates" ] && echo "$f"
    done
    return 0
}

lint_test_displays_deleted() {
    local files
    files=$(unreleased_display_files tests --include='*.cpp' --include='*.h')
    [ -z "$files" ] && return 0
    echo "derive from LVGLTestFixture instead of creating a display:"
    echo "$files"
    return 1
}

# AmsState is a process-wide singleton, so a single init_subjects(false)
# publishes no `ams_*` names, and every later case in the same shard that does
# not itself re-enter init_subjects(true) sees lv_xml_get_subject() answer
# null. Publishing costs a test nothing. test_ams_lane_state_subject.cpp owns
# the one deliberate false init: its [1374] case republishes the names itself.
ams_state_unpublished_init_files() {
    grep -rlE 'AmsState::instance\(\)\.init_subjects\([[:space:]]*false' "$@" 2>/dev/null |
        grep -v 'test_ams_lane_state_subject\.cpp' || true
}

lint_ams_state_published() {
    none_found "$(ams_state_unpublished_init_files tests --include='*.cpp' --include='*.h')"
}

# build.yml's compile job finds the shell tests to run against its fresh binary
# by their require_app calls (tests/shell/helpers.bash). A file that skips on a
# missing app with its own `skip` is skipped by the Shell Tests job and never
# picked up by the compile job, so it runs nowhere in CI.
app_skip_offenders() { # <bats files...>
    grep -nE '(^|[|&;[:space:]])skip([[:space:]]|$)' "$@" | grep -E 'helix-screen|build/bin/' || true
}

lint_app_skip_require_app() {
    local files offenders
    # test_code_lint.bats spells the forbidden shapes as its fixture.
    files=$(printf '%s\n' tests/shell/*.bats | grep -v '/test_code_lint\.bats$')
    # shellcheck disable=SC2086  # paths have no spaces
    offenders=$(app_skip_offenders $files)
    if [ -n "$offenders" ]; then
        echo "These skip on a missing app without require_app, so CI runs them nowhere:"
        echo "$offenders"
        return 1
    fi
    # shellcheck disable=SC2086
    [ -n "$(grep -lE '^[[:space:]]*require_app[[:space:]]' $files)" ] ||
        { echo "no shell test calls require_app"; return 1; }
}

# ====================================================================
# ui_xml lints
# ====================================================================

# The raw `chamber_target` subject is the heater target only: it reads 0 during
# M141 "maintain" (cooling-ceiling) mode, so a display bound to it shows
# "-/Off" while the chamber holds a setpoint. Chamber temp_display instances
# bind `chamber_effective_target` (+ `chamber_mode`).
lint_chamber_target_binding() {
    none_found "$(grep -rn 'bind_target="chamber_target"' ui_xml)"
}

# `__` separates a plugin id from the rest of a name that plugin owns
# (plugin_manifest.h kPluginNameSeparator). An app file name, app-bound
# attribute value or C++-registered subject containing it would sit inside the
# plugin namespace, where the ownership checks read it as plugin-owned and a
# plugin with that id could bind it. Plugin code is excluded: building `__`
# names is its job. $1 = ui_xml root, $2.. = C++ roots.
plugin_separator_offenders() {
    local xml="$1"
    shift
    local -a cxx=(--include='*.cpp' --include='*.h' --exclude-dir=plugin --exclude='plugin_*' --exclude='lua_*')
    find "$xml" -name '*__*'
    grep -rEn '((^|[^_a-z0-9])subject|[_a-z0-9]+_subject|bind_[_a-z0-9]+|[_a-z0-9]*cond|name)="[^"]*__[^"]*"' "$xml"
    grep -rn -A 1 "${cxx[@]}" 'lv_xml_register_subject(' "$@" | grep -E '"[^"]*__'
    grep -rnE "${cxx[@]}" 'INIT_SUBJECT_[A-Z_]+\([[:space:]]*[_A-Za-z0-9]*__' "$@"
    # register_subject_in_current_scope(name, ...) publishes the same scope entry
    # as lv_xml_register_subject; UI_MANAGED_SUBJECT_* and
    # UI_SUBJECT_INIT_AND_REGISTER_* take the XML name as a string literal. The
    # name can sit on the call's next line, so -A 1; these also carry non-name
    # literals (initial values), where a __ fails closed.
    grep -rn -A 1 "${cxx[@]}" -E 'register_subject_in_current_scope\(|\.publish\(|UI_MANAGED_SUBJECT_[A-Z_]+\(|UI_SUBJECT_INIT_AND_REGISTER_[A-Z_]+\(' "$@" |
        grep -E '"[^"]*__'
    return 0
}

lint_plugin_separator() {
    none_found "$(plugin_separator_offenders ui_xml src include)"
}

# flex_flow / flex_main_place on a style only configure a flex layout the object
# already has. A bind_style that sets them without layout="flex" leaves the
# children on the default layout, stacked at the top-left on top of each other.
home_widget_flex_styles_without_layout() {
    grep -nE '<style [^>]*(flex_flow|flex_main_place)=' "$@" | grep -v 'layout="flex"'
}

lint_flex_styles_layout() {
    none_found "$(home_widget_flex_styles_without_layout ui_xml/components/panel_widget_*.xml \
        ui_xml/components/speed_flow_part.xml ui_xml/components/chamber_dryer_row.xml \
        ui_xml/components/host_health_mcu_row.xml)"
}

# A tap binds clicked; a hold handler that acts ends its press with
# helix::ui::end_press() (docs/devel/LVGL9_XML_GUIDE.md "Tap and hold").
# short_clicked is never a tap's trigger: LVGL skips it after any press that ran
# past the long-press time, whether or not a hold handler acted, so a slow tap
# on a short_clicked widget does nothing. $1 = XML root, $2 = C++ root.
short_clicked_taps() {
    grep -rnE 'trigger="short_clicked"' "$1" --include='*.xml' 2>/dev/null
    grep -rnE 'lv_obj_add_event_cb\([^;]*LV_EVENT_SHORT_CLICKED' "$2" --include='*.cpp' \
        --include='*.h' 2>/dev/null
    return 0
}

lint_short_clicked_taps() {
    none_found "$(short_clicked_taps ui_xml src)"
}

# The Android mirror is generated by `make regen-filaments` and gitignored, so
# it may be absent; when present it must match the source byte for byte.
lint_filaments_android_mirror() {
    local mirror=android/app/src/main/assets/assets/filaments.json
    [ -f "$mirror" ] || return 0
    diff assets/filaments.json "$mirror" >/dev/null && return 0
    echo "$mirror differs from assets/filaments.json; run make regen-filaments"
    return 1
}

# ====================================================================
# Driver
# ====================================================================

# Runs the named lints, printing each failure under its name. Exit 1 if any failed.
# Every lint is a scan of its own, so they run at once and report in order:
# one after another they take half a minute.
code_lint_run() {
    local name tmp failed=0 total=0
    tmp=$(mktemp -d) || return 2
    for name in "$@"; do
        ( "lint_$name" > "$tmp/$name.out" 2>&1; echo $? > "$tmp/$name.rc" ) &
    done
    wait
    for name in "$@"; do
        total=$((total + 1))
        if [ "$(cat "$tmp/$name.rc" 2>/dev/null)" != 0 ]; then
            failed=$((failed + 1))
            echo "FAIL $name"
            sed 's/^/    /' "$tmp/$name.out"
        fi
    done
    rm -rf "$tmp"
    echo "code_lint: $total lint(s), $failed failed"
    [ "$failed" -eq 0 ]
}

code_lint_main() {
    local -a names=()
    # Run from a git hook, GIT_DIR and GIT_INDEX_FILE point at the superproject
    # and a commit's temporary index: `git -C lib/helix-xml` would then list
    # the superproject, and the work tree would be wherever the hook stands.
    # The lints read the files on disk, so plain repository discovery is right.
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_PREFIX GIT_COMMON_DIR
    cd "$CODE_LINT_DIR/.." || return 2
    case "${1:-}" in
        --list) code_lint_registry; return 0 ;;
        --group)
            [ -n "${2:-}" ] || { echo "--group needs src, tests or ui" >&2; return 2; }
            while read -r group name; do
                [ "$group" = "$2" ] && names+=("$name")
            done < <(code_lint_registry)
            [ "${#names[@]}" -gt 0 ] || { echo "no lint in group '$2'" >&2; return 2; }
            ;;
        "")
            while read -r _ name; do names+=("$name"); done < <(code_lint_registry)
            ;;
        *)
            for name in "$@"; do
                declare -F "lint_$name" >/dev/null || { echo "no lint named '$name'" >&2; return 2; }
                names+=("$name")
            done
            ;;
    esac
    code_lint_run "${names[@]}"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    code_lint_main "$@"
fi
