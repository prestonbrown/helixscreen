# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Declarative UI: no XML-owned widget driven imperatively from C++
# ====================================================================
# True when the caller has said this tree cannot answer the clang question. The
# gate derives its repo root from its own location and needs a compile database
# there; the pre-push isolated checkout has neither, and refusing to report green
# off an unbuilt tree is correct, so the caller re-asks where a database exists.
qc_clang_divergence_deferred() {
  [ -n "${HELIX_QC_SKIP_CLANG_DIVERGENCE:-}" ]
}

qc_decl_ui() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "🎨 Checking declarative UI (imperative XML-widget mutation)..."

# Ratcheting baseline. These are XML widgets fetched by name (lv_obj_find_by_name(),
# find_required(), find_optional()) and then mutated from C++ instead of bound
# to a subject. Some predate the gate
# as deliberate pragmatism (the XML engine couldn't express it at the time), some
# are plain mistakes — both are debt. The number may go DOWN (port a site, then
# lower this baseline) but must never go up.
if python3 scripts/check_imperative_ui.py --max-allowed 323 --summary >/tmp/imperative_ui.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/imperative_ui.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/imperative_ui.out
  echo "   Run: python3 scripts/check_imperative_ui.py --list"
  echo "   Bind subjects in XML; see .claude/rules/declarative-ui.md."
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🔌 Checking orphan subjects (registered, never read)..."

# A ratcheting baseline over the whole registered population. The XML linter
# already rejects a bind_* naming a subject nobody registers; this is the other
# direction — a subject registered and kept current but read by neither an XML
# binding nor a C++ consumer. It renders nothing and costs every update that
# writes it. Usually what a binding leaves behind when its widget is deleted or
# renamed. Genuinely-unreadable-by-static-analysis cases (a subject handed to a
# helper by pointer, or observed only from a test accessor) take
# `// SUBJECT_OK: <reason>` on the registration.
#
# scripts/orphan_subject_baseline.txt names the orphans that are accepted debt;
# the gate fails on any subject not on that list, and says so when one leaves
# it so the list can be shrunk.
if python3 scripts/check_orphan_subjects.py --baseline scripts/orphan_subject_baseline.txt        --summary >/tmp/orphan_subjects.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/orphan_subjects.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/orphan_subjects.out
  echo "   Run: python3 scripts/check_orphan_subjects.py --list"
  echo "   Bind it in XML, read it from C++, or delete it."
  echo "   scripts/orphan_subject_baseline.txt is the accepted-debt list, not a parking spot."
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🔌 Checking XML event callbacks against their registrations..."
# A ratchet keyed on names: an XML callback nothing registers, a registered name
# no XML uses, and a name registered from two files (the table is
# last-write-wins). scripts/orphan_callback_baseline.txt is accepted debt.
if python3 scripts/check_orphan_callbacks.py --baseline scripts/orphan_callback_baseline.txt \
    >/tmp/orphan_callbacks.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/orphan_callbacks.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/orphan_callbacks.out
  echo "   Run: python3 scripts/check_orphan_callbacks.py --list"
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🔎 Checking find_required() names exist in every layout variant..."
# find_required() aborts a --test run on a missing name, but only on paths the
# run reaches; this covers every literal statically, in every variant chain.
if python3 scripts/check_required_names.py --baseline scripts/required_names_baseline.txt \
    >/tmp/required_names.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/required_names.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/required_names.out
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🧱 Checking hand-rolled overlay boilerplate only shrinks..."
# OverlayBase::show() owns the create-once lifecycle and lazy_global<T> the
# instance; scripts/overlay_boilerplate_baseline.txt holds the shrink-only counts.
if python3 scripts/check_overlay_boilerplate.py --baseline scripts/overlay_boilerplate_baseline.txt \
    >/tmp/overlay_boilerplate.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/overlay_boilerplate.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/overlay_boilerplate.out
  EXIT_CODE=1
fi

echo ""

# The ratchet has reached zero (#1165) — every queue_update() in src/ now routes
# through an AsyncLifetimeGuard, so this is a hard gate, not a baseline.
# queue_update([this, ...]) runs at the next drain whether or not the owner is
# still alive; if the body touches a member lv_subject_t, lv_subject_notify walks
# a freed observer list (#1146, #1165). Keep it at 0: guard new sites with
# lifetime_.bg_cb() / tok.defer(), or annotate a genuine exception with
# // QUEUE_RAW_THIS_OK: <reason>.
#
# Pre-commit: scan the staged blob for each changed source, not the dirty
# working tree - a violation staged and then reverted on disk must still
# fail. A hard gate at 0 stays sound under a partial (staged-file) scan:
# any hit is real regardless of scope. CI and manual runs use the
# whole-working-tree scan (no flag).
if [ "$STAGED_ONLY" = true ]; then
  RAW_THIS_ARGS="--staged-only"
else
  RAW_THIS_ARGS=""
fi
# shellcheck disable=SC2086
if python3 scripts/check_raw_this_queue_update.py $RAW_THIS_ARGS --max-allowed 0 --summary >/tmp/raw_this_qu.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/raw_this_qu.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/raw_this_qu.out
  echo "   Run: python3 scripts/check_raw_this_queue_update.py --list"
  echo "   Guard with lifetime_.bg_cb() / tok.defer(); see docs/devel/THREADING.md §2."
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "⏱️  Checking gcode error ownership..."

# Hard gate at zero. execute_gcode's caller_surfaces_errors means "my on_error
# actually SHOWS a human something". Claiming it falsely makes the request
# tracker record the rejection for cross-channel dedup, and GcodeErrorRouter
# then suppresses its own report of Klipper's `!!` broadcast — so a failed
# macro is reported by NOBODY. It is invisible in review because the call site
# looks handled: there IS an error callback, it just writes to a log. Pass
# caller_surfaces_errors=false on a log-only callback, or annotate a genuine
# exception with // ERROR_OWNERSHIP_OK: <reason>. See include/rpc_error_policy.h.
if python3 scripts/check_gcode_error_ownership.py --max-allowed 0 --summary \
    >/tmp/gcode_err_own.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/gcode_err_own.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/gcode_err_own.out
  echo "   Run: python3 scripts/check_gcode_error_ownership.py --list"
  echo "   A log-only error callback must pass caller_surfaces_errors=false."
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "⏱️  Checking timer destructor cancels..."

# Ratcheting baseline. A raw lv_timer_t* cancelled only in cleanup()/stop_*()
# stays armed on any teardown that destroys the owner without that call, and
# StaticPanelRegistry::destroy_all() runs BEFORE lv_deinit() — so the callback
# fires into a freed `this` (#1173, twice: the wizard auto-probe timer and the
# PID ETA tick). The check is transitive, so a destructor that reaches the
# cancel through cleanup()/detach()/deinit_subjects() passes. Timers whose
# callback is LifetimeToken-guarded or routed through a singleton accessor are
# safe by another mechanism — annotate those `// TIMER_DTOR_OK: <reason>`.
if python3 scripts/check_timer_destructor_cancel.py --max-allowed 0 >/tmp/timer_dtor.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/timer_dtor.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/timer_dtor.out
  echo "   Run: python3 scripts/check_timer_destructor_cancel.py --list"
  echo "   Cancel from the destructor via lv_timer_cancel_safe(); see .claude/rules/threading.md (rule 5)."
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🪟 Checking X11 macro collisions..."

# X11's <X.h> defines None, Success, Above and friends as bare macros. SDL's
# Linux headers reach X.h through GL, so an identifier sharing one of those
# names preprocesses into a numeric constant in any TU that reaches SDL - and
# only there. Our own SDL is built without X11, so no local build reproduces
# it; it surfaces only on the x86_64 Debian and Raspberry Pi CI jobs, whose
# SDL does reach X11 (e.g. a symbol like InvalidationScope::None colliding
# with X11's None).
# Annotate a deliberate one `// X11_MACRO_OK: <reason>`.
if python3 scripts/check_x11_macro_collisions.py --max-allowed 0 >/tmp/x11_macros.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/x11_macros.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/x11_macros.out
  echo "   Rename the identifier; X11's macro always wins."
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🐉 Checking clang/GCC divergence..."

# Deliberately NOT in --staged-only: this is seconds per TU, and a changed header
# fans out to every TU that includes it (json_utils.h reaches 29), which is too
# slow to sit on every commit. pre-push runs this file in full mode, so the class
# is still caught before anything leaves the machine - just not on each commit.
# Its isolated checkout has no compile database of its own, so it defers this
# gate and re-asks it in the tree that has one.
#
# The class: CI's Ubuntu job compiles with clang and -Werror while every build
# here uses g++, so GCC accepting a comparison clang rejects (e.g.
# -Wtautological-type-limit-compare in json_utils.h) ships a red build that
# nothing local could see.
if qc_clang_divergence_deferred; then
  section_time $SECTION_START
  echo ""
  echo "⏭️  clang divergence: deferred to the tree that owns the compile database"
elif [ "$STAGED_ONLY" = false ]; then
  if python3 scripts/check_clang_diagnostics.py >/tmp/clang_diag.out 2>&1; then
    section_time $SECTION_START
    echo ""
    tail -1 /tmp/clang_diag.out
  else
    section_time $SECTION_START
    echo ""
    cat /tmp/clang_diag.out
    echo "   These are errors on CI's Ubuntu job even though g++ accepts them."
    EXIT_CODE=1
  fi
else
  section_time $SECTION_START
  echo ""
  echo "⏭️  clang divergence: skipped in pre-commit (runs on push and in CI)"
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🔢 Checking print-state enum casts..."

# lv_subject_get_int() returns int, so static_cast<PrintState>(...) compiles
# against whichever subject was named — and PrintJobState and PrintState do NOT
# share numbering past index 0 (COMPLETE=3 vs Paused=3). Pairing a cast with
# the wrong subject is silent: it compiles, runs, and answers a different
# question. Made twice while migrating guards onto the lifecycle. Use the typed
# accessors get_print_lifecycle() / get_print_job_state(), which own the
# pairing; annotate a genuine need `// PRINT_STATE_CAST_OK: <reason>`.
if python3 scripts/check_print_state_cast.py >/tmp/print_state_cast.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/print_state_cast.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/print_state_cast.out
  EXIT_CODE=1
fi

SECTION_START=$(date +%s)
echo -n "🔤 Checking JSON save paths for bare dumps..."

# nlohmann dumps with error_handler_t::strict, so a string holding bytes UTF-8
# cannot decode throws json::type_error.316. The text on a save path is the
# text nothing validates — SSIDs, printer and tool names, file names, macro
# text, gcode responses — so strict costs the whole document: the throw either
# unwinds through LVGL's C frames or, behind a catch, drops the user's change
# silently. Use helix::json_util::safe_dump(), which replaces the offending
# bytes; annotate a genuine need `// JSON_DUMP_OK: <reason>`.
if python3 scripts/check_json_dump_utf8.py >/tmp/json_dump_utf8.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/json_dump_utf8.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/json_dump_utf8.out
  EXIT_CODE=1
fi

SECTION_START=$(date +%s)
echo -n "🧵 Checking AMS backends reconcile lane bindings..."

# A backend whose firmware states a spool id must call reconcile_lane_binding()
# where it parses it, or a lane re-bound behind the app's back keeps painting
# the old spool forever (prestonbrown/helixscreen#1645).
if python3 scripts/check_lane_binding_reconcile.py >/tmp/lane_binding_reconcile.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/lane_binding_reconcile.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/lane_binding_reconcile.out
  EXIT_CODE=1
fi

SECTION_START=$(date +%s)
echo -n "🧭 Checking raw print-state reads..."

# helix::PrintJobState is the WIRE — what print_stats.state said. It cannot
# express a job the app has committed to but the printer has not reported yet,
# so a semantic question asked of it is blind for the whole of a pre-print
# window. That blindness shipped: 21 motion controls live while the toolhead
# homed, the home print card reading idle, a queue tap deleting the job it then
# failed to start. Plenty of sites DO want the wire — the parse, terminal
# formatting, telemetry's phase tracker, the PRINT_START collector — so this
# does not forbid it. It forbids reading it SILENTLY, because a deliberate wire
# read and a stale one look identical. Annotate: `// RAW_PRINT_STATE_OK: <why>`.
if python3 scripts/check_raw_print_job_state.py >/tmp/raw_print_state.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/raw_print_state.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/raw_print_state.out
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🧷 Checking raw cached widget pointers..."

# Ratcheting baseline: the count of raw lv_obj_t* data members may fall, never
# rise. A cached widget outlives its widget whenever something other than its
# owner deletes the tree, and owner-keyed guards still read valid then. Hold
# new ones as helix::ui::WidgetRef, or annotate `// WIDGET_PTR_OK: <why>`.
if python3 scripts/check_cached_widget_pointers.py --max-allowed=443 \
    >/tmp/cached_widget_ptrs.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/cached_widget_ptrs.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/cached_widget_ptrs.out
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🖥️  Checking DRM dumb-buffer mmap offset width..."

# DRM allocates dumb-buffer mmap offsets from 4 GiB upward, so a 32-bit off_t
# truncates them and the mapping fails. HelixScreen then falls back to fbdev and
# the KMS path is silently dead on every 32-bit device (pi32).
if python3 scripts/check_drm_mmap_lfs.py >/tmp/drm_mmap_lfs.out 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ DRM mmap uses a 64-bit file offset"
else
  section_time $SECTION_START
  echo ""
  cat /tmp/drm_mmap_lfs.out
  echo "   Run: python3 scripts/check_drm_mmap_lfs.py"
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "📄 Checking gcode reader large-file support..."

# The static_assert in gcode_data_source.cpp only fires on a 32-bit build, and
# pi32/ad5m/cc1/k1 are in release.yml's matrix rather than build.yml's - so a
# dropped mk/rules.mk override stays green here and detonates at release.
if python3 scripts/check_gcode_lfs.py >/tmp/gcode_lfs.out 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ gcode reader builds with a 64-bit off_t"
else
  section_time $SECTION_START
  echo ""
  cat /tmp/gcode_lfs.out
  echo "   Run: python3 scripts/check_gcode_lfs.py"
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🔧 Checking target-specific flag rules use override..."

# test-asan/test-tsan re-invoke make with CXXFLAGS on the command line, and a
# command-line variable discards makefile assignments to it unless they say
# override. A rule missing the keyword builds its object without the flag, with
# no diagnostic and with the rule still visibly present in the makefile.
if python3 scripts/check_target_specific_override.py >/tmp/tgt_override.out 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ every target-specific flag rule uses override"
else
  section_time $SECTION_START
  echo ""
  cat /tmp/tgt_override.out
  echo "   Run: python3 scripts/check_target_specific_override.py"
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🔄 Checking touch-range rotation source..."

# The gate lives in create_input_pointer(), which needs a real fbdev/DRM device
# and cannot run headless - mutation testing confirmed no test kills a revert to
# the config key. A backend reading /display/rotate instead of the applied
# rotation leaves #1394 live on any unit rotated via CLI/env.
if python3 scripts/check_touch_rotation_source.py >/tmp/touch_rotation.out 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ display backends gate the stored touch range on the applied rotation"
else
  section_time $SECTION_START
  echo ""
  cat /tmp/touch_rotation.out
  echo "   Run: python3 scripts/check_touch_rotation_source.py"
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🗺️  Checking the platform manifest against its consumers..."

# Advisory while the consumers are migrated onto assets/config/platforms.json.
# It reports drift between the manifest and the build files, install-root lists
# and renders that derive from it; --strict makes the same findings fail once
# every consumer reads the manifest.
# Without --strict it exits 0 on findings, so a non-zero exit is the script
# itself failing to run.
python3 scripts/check_platform_manifest.py --quiet >/tmp/platform_manifest.out 2>&1
PLATFORM_MANIFEST_RC=$?
if [ "$PLATFORM_MANIFEST_RC" -ne 0 ]; then
  section_time $SECTION_START
  echo ""
  cat /tmp/platform_manifest.out
  EXIT_CODE=1
elif [ -s /tmp/platform_manifest.out ]; then
  section_time $SECTION_START
  echo ""
  echo "ℹ️  platform manifest findings (advisory):"
  cat /tmp/platform_manifest.out
else
  section_time $SECTION_START
  echo ""
  echo "✅ platform manifest agrees with its consumers"
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🕰️  Checking comments for commit-SHA citations..."

# Ratchet. Comments explain the code as it is; how it got here belongs in the
# commit message, where git blame will surface it on demand.
if python3 scripts/check_comment_archaeology.py >/tmp/comment_arch.out 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ no new commit-SHA citations in comments"
else
  section_time $SECTION_START
  echo ""
  cat /tmp/comment_arch.out
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "🖼️  Checking guarded ThumbnailCache access..."

# Hard gate, never a baseline: src/ has no legacy call sites left. The two
# unguarded overloads — fetch(api, path, ...) and get_if_cached(path, mtime) —
# stay public only because tests exercise them deliberately, so the compiler
# cannot enforce this. They take no ThumbnailLoadContext, which is what lets
# fetch() drop a superseded on_success; without it an in-flight download that
# has already been outdated still lands and overwrites a NEWER thumbnail.
# Build a ThumbnailRequest + ThumbnailLoadContext, or annotate a genuine
# exception with // THUMB_LEGACY_OK: <reason>.
#
# Pre-commit: scan the staged blob for each changed source, not the dirty
# working tree - a violation staged and then reverted on disk must still
# fail. A hard gate stays sound under a partial (staged-file) scan: any hit
# is real regardless of scope. CI and manual runs use the whole-working-tree
# scan (no flag).
if [ "$STAGED_ONLY" = true ]; then
  THUMB_GUARD_ARGS="--staged-only"
else
  THUMB_GUARD_ARGS=""
fi
# shellcheck disable=SC2086
if python3 scripts/check_thumbnail_cache_guard.py $THUMB_GUARD_ARGS >/tmp/thumb_guard.out 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ ThumbnailCache: every src/ consumer passes a ThumbnailLoadContext"
else
  section_time $SECTION_START
  echo ""
  cat /tmp/thumb_guard.out
  echo "   Run: python3 scripts/check_thumbnail_cache_guard.py"
  echo "   Use fetch(req, ctx, ...) / get_if_cached(req); see include/thumbnail_cache.h."
  EXIT_CODE=1
fi

echo ""

SECTION_START=$(date +%s)
echo -n "⏱️  Checking grid cell-metrics single source..."

# Every drag/resize/preview/lattice path needs the same cols/rows/cell size,
# and each independent computation is free to drift from the others on
# gutter handling or int-vs-float rounding. GridEditMode::current_metrics()
# is the one place allowed to ask GridLayout for the grid's dimensions; this
# caps GridLayout::get_cols/get_rows/get_dimensions call sites at 2 (the pair
# inside current_metrics() itself) so a new call site cannot grow a second copy.
if python3 scripts/check_grid_metrics_single_source.py >/tmp/grid_metrics.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/grid_metrics.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/grid_metrics.out
  echo "   Take a helix::CellMetrics from GridEditMode::current_metrics() instead."
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_decl_ui="$QC_TRIGGER_NATIVE_SRC"
