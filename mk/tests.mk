# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Test Module
# Handles all test compilation and execution targets

# ============================================================================
# Parallel Test Execution Infrastructure
# ============================================================================
# Uses Catch2 sharding (--shard-count N --shard-index M) to split tests across
# multiple processes. Each shard runs in its own process with its own LVGL
# instance, avoiding thread-safety issues entirely.
#
# Per Catch2 best practices, use more shards than cores to avoid long-tailed
# execution from uneven test distribution.

# Detect CPU count for parallel sharding (3x cores to avoid fat shards)
# Supports: Linux (nproc), macOS (sysctl), fallback to 4 cores
NPROCS := $(shell echo $$(( $$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) * 3 )))

# Detect timeout command: GNU timeout on Linux, gtimeout (from coreutils) on macOS
TIMEOUT_CMD := $(shell command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || echo "")

# Per-shard timeout in seconds — safety net for infinite hangs only.
# Must be generous: some shards with threading tests take 60-90s under load.
SHARD_TIMEOUT := 300

# Where per-shard logs land. Kept (not deleted) whenever a shard fails, crashes,
# or times out, so there is something to read afterwards — see diagnose_shards.
# Override to collect artifacts elsewhere, e.g. SHARD_ARTIFACT_ROOT=$(PWD)/build
# in CI so the logs are inside the workspace and get uploaded.
SHARD_ARTIFACT_ROOT ?= /tmp

# How many times diagnose_shards re-runs a suspect shard sequentially.
#
# Must be >1: a single re-run cannot tell a real fault from an intermittent one
# in either direction. A ~10% crash passes one retry ~90% of the time and used
# to be reported as a confirmed FLAKE; conversely one reproduction was reported
# as "a real fault, not a flake" on a single sample. Repeating turns the verdict
# into an observed rate. Raise it when chasing something rare.
SHARD_RETRIES ?= 3

# Run tests in parallel using Catch2 sharding
# Args: $(1) = test filter (e.g., "~[.] ~[slow]")
# Collects PIDs and waits for all, failing if any shard fails
# Each shard is wrapped in a timeout to prevent infinite hangs
# Each shard log starts with a host/nproc/git/ts header so flakes (e.g. #1121)
# are attributable to a shard/host instead of re-litigated each run.
# Output is prefixed with [shard N] for clarity
define run_tests_parallel
	echo "$(CYAN)Running $(NPROCS) test shards in parallel (timeout=$(SHARD_TIMEOUT)s)...$(RESET)"; \
	shard_dir=$$(mktemp -d "$(SHARD_ARTIFACT_ROOT)/helix-shards-XXXXXX"); \
	pids=""; \
	for i in $$(seq 0 $$(($(NPROCS)-1))); do \
		(echo "=== shard $$i/$(NPROCS) host=$$(hostname) nproc=$$(nproc 2>/dev/null || echo '?') git=$$(git rev-parse --short HEAD 2>/dev/null || echo unknown) ts=$$(date -Iseconds) order=decl seed=0"; $(if $(TIMEOUT_CMD),$(TIMEOUT_CMD) $(SHARD_TIMEOUT)) $(TEST_BIN) $(1) --shard-count $(NPROCS) --shard-index $$i 2>&1; echo $$? > "$$shard_dir/$$i.exit") | \
			tee "$$shard_dir/$$i.log" | sed "s/^/[shard $$i] /" & \
		pids="$$pids $$!"; \
	done; \
	for pid in $$pids; do \
		wait $$pid 2>/dev/null || true; \
	done; \
	failed=0; \
	suspect=""; \
	for i in $$(seq 0 $$(($(NPROCS)-1))); do \
		if [ ! -f "$$shard_dir/$$i.exit" ]; then \
			echo "$(RED)$(BOLD)✗ Shard $$i timed out after $(SHARD_TIMEOUT)s!$(RESET)"; \
			failed=1; suspect="$$suspect $$i"; \
		else \
			ec=$$(cat "$$shard_dir/$$i.exit" 2>/dev/null | tr -d '[:space:]'); \
			if [ "$$ec" != "0" ] 2>/dev/null; then \
				if grep -q "test cases.*|.*failed" "$$shard_dir/$$i.log" 2>/dev/null; then \
					echo "$(RED)$(BOLD)✗ Shard $$i had test failures (exit $$ec)$(RESET)"; \
					failed=1; suspect="$$suspect $$i"; \
				elif [ "$$ec" -gt 128 ] 2>/dev/null; then \
					sig=$$((ec - 128)); \
					echo "$(YELLOW)⚠ Shard $$i crashed after its assertions passed (signal $$sig)$(RESET)"; \
					suspect="$$suspect $$i"; \
				else \
					echo "$(RED)$(BOLD)✗ Shard $$i failed (exit $$ec)$(RESET)"; \
					failed=1; suspect="$$suspect $$i"; \
				fi; \
			fi; \
		fi; \
	done; \
	if [ -n "$$suspect" ]; then \
		$(call diagnose_shards,$$shard_dir,$$suspect,$(1)); \
	else \
		rm -rf "$$shard_dir"; \
	fi; \
	if [ $$failed -eq 1 ]; then \
		echo "$(RED)$(BOLD)✗ One or more test shards failed!$(RESET)"; \
		exit 1; \
	fi
endef

# Post-mortem for shards that failed, crashed, or timed out.
#
# Exists because the harness used to delete its scratch dir unconditionally: a
# shard could abort *after* every assertion passed and leave nothing to read,
# so "is this my diff or a flake?" cost a manual re-run cycle every time. Now
# the evidence survives and the question is answered inline.
#
# For each suspect shard: keep its log, name the tests it ran, and re-run it
# alone. A shard that is green in isolation but red under a full parallel run
# is a load/timing flake, not a fault in the diff under review — and adding or
# removing ANY test reshuffles Catch2's shard composition, so the shard number
# moving between runs is not evidence either.
#
# Args: $(1) = shard dir, $(2) = space-separated shard indices, $(3) = filter
define diagnose_shards
	echo ""; \
	echo "$(CYAN)$(BOLD)── shard diagnostics ──$(RESET)"; \
	echo "$(CYAN)logs preserved: $(1)$(RESET)"; \
	for s in $(2); do \
		echo ""; \
		echo "$(BOLD)shard $$s$(RESET)"; \
		$(TEST_BIN) $(3) --shard-count $(NPROCS) --shard-index $$s --list-tests 2>/dev/null \
			| grep -E '^  ' | sed 's/^  //' > "$(1)/$$s.tests" 2>/dev/null || true; \
		n=$$(wc -l < "$(1)/$$s.tests" 2>/dev/null | tr -d ' '); \
		echo "  ran $${n:-?} test case(s) → $(1)/$$s.tests"; \
		fails=$$(grep -oE '^[A-Za-z0-9_/.-]+\.cpp:[0-9]+: FAILED' "$(1)/$$s.log" 2>/dev/null \
			| sed 's/: FAILED$$//' | sort -u | tr '\n' ' '); \
		if [ -n "$$fails" ]; then \
			echo "  failing assertion(s): $$fails"; \
		else \
			echo "  no FAILED marker — died after its assertions passed (teardown/static dtor)"; \
		fi; \
		printf '  reproduce: %s %s --shard-count %s --shard-index %s\n' \
			"$(TEST_BIN)" '$(3)' "$(NPROCS)" "$$s"; \
		echo "  $(CYAN)re-running this shard sequentially x$(SHARD_RETRIES)…$(RESET)"; \
		hits=0; last_rc=0; \
		for attempt in $$(seq 1 $(SHARD_RETRIES)); do \
			if $(if $(TIMEOUT_CMD),$(TIMEOUT_CMD) $(SHARD_TIMEOUT)) $(TEST_BIN) $(3) \
					--shard-count $(NPROCS) --shard-index $$s > "$(1)/$$s.retry.log" 2>&1; then \
				: ; \
			else \
				last_rc=$$?; hits=$$((hits+1)); \
				cp -f "$(1)/$$s.retry.log" "$(1)/$$s.repro.log" 2>/dev/null || true; \
			fi; \
		done; \
		if [ $$hits -eq 0 ]; then \
			echo "  $(YELLOW)→ did not reproduce in $(SHARD_RETRIES) sequential re-runs$(RESET)"; \
			echo "     Consistent with a load/timing flake under parallel shards, but an"; \
			echo "     intermittent fault can also pass $(SHARD_RETRIES) times — this does not"; \
			echo "     prove the diff is clean. Raise SHARD_RETRIES to sample harder."; \
			echo "     (shard composition shifts whenever tests are added or removed)"; \
		else \
			echo "  $(RED)$(BOLD)→ reproduced $$hits/$(SHARD_RETRIES) sequential re-runs (last exit $$last_rc)$(RESET)"; \
			echo "     Sequential means no sibling shards ran concurrently. The shard still"; \
			echo "     ran ALL $${n:-?} of its test cases in one process, so this does NOT"; \
			echo "     isolate the named test — a leak from an earlier test in the same"; \
			echo "     shard lands on whichever test runs next. Confirm by running the"; \
			echo "     named test on its own before blaming it."; \
			echo "     repro log: $(1)/$$s.repro.log"; \
			tail -25 "$(1)/$$s.repro.log" | sed 's/^/     | /'; \
		fi; \
	done; \
	echo ""
endef

# Report the result of the immediately-preceding test command, timing it and
# FAILING the recipe if the command failed. Captures $$? as its first action, so
# it MUST directly follow the test invocation in the recipe:
#
#   @START_TIME=$$(date +%s); \
#   $(TEST_BIN) "[tag]"; \
#   $(call report_test_result,Human label)
#
# This replaces the historical `$(TEST_BIN) ... && END=...; echo passed` idiom,
# where the `&&` only guarded the END assignment: a failing test still fell
# through the `;`-separated echo and the recipe exited 0, masking the failure.
# Args: $(1) = human-readable label for the test group.
define report_test_result
	TEST_RC=$$?; \
	END_TIME=$$(date +%s); \
	DURATION=$$((END_TIME - START_TIME)); \
	if [ $$TEST_RC -ne 0 ]; then \
		echo "$(RED)$(BOLD)✗ $(1) FAILED (exit $$TEST_RC) after $${DURATION}s$(RESET)"; \
		exit $$TEST_RC; \
	fi; \
	echo "$(GREEN)$(BOLD)✓ $(1) passed in $${DURATION}s$(RESET)"
endef

# Report the Unity test-case total behind a ctest run.
#
# ctest counts EXECUTABLES, so a suite of twelve binaries reports "12 tests"
# no matter how many cases are inside them — which makes the headline number
# useless for spotting a case that stopped being registered. Unity prints its
# own "<n> Tests <n> Failures <n> Ignored" summary per binary, and ctest always
# archives full per-test output in Testing/Temporary/LastTest.log regardless of
# --output-on-failure, so the real total is recoverable without a second run.
#
# Best-effort: silent if the log is missing or has no Unity summaries (a
# non-Unity suite, or a ctest that never got that far).
# Args: $(1) = ctest build dir
define summarize_unity_cases
	_lt="$(1)/Testing/Temporary/LastTest.log"; \
	if [ -f "$$_lt" ]; then \
		awk '/^[0-9]+ Tests [0-9]+ Failures [0-9]+ Ignored/ { t += $$1; f += $$3; n += 1 } \
		     END { if (n > 0) printf "  %d Unity test case(s), %d failure(s), across %d executable(s)\n", t, f, n }' \
			"$$_lt"; \
	fi
endef

# ============================================================================
# Test Dependency System - AUTOMATIC DISCOVERY
# ============================================================================
# Instead of manually listing every .o file (error-prone, causes CI failures),
# we use automatic discovery: include ALL app objects except a small exclude list.
#
# Benefits:
# - New source files are automatically included in tests
# - No more "undefined symbol" CI failures from forgetting to add files
# - Exclude list is much smaller and easier to maintain
#
# What we keep:
# - TEST_CORE_DEPS: Test infrastructure (Catch2, test utilities)
# - TEST_LVGL_DEPS: LVGL library objects (from submodule)
# - TEST_PLATFORM_DEPS: Platform-specific (wpa_supplicant on Linux)
# - FONT_OBJS, OBJCPP_OBJS: Assets and platform code
#
# What we replace with automatic discovery:
# - All the manual TEST_*_DEPS groups → single TEST_APP_OBJS

# Core test infrastructure (always required)
TEST_CORE_DEPS := $(TEST_MAIN_OBJ) $(CATCH2_OBJ) $(UI_TEST_UTILS_OBJ) $(LVGL_TEST_FIXTURE_OBJ) $(HELIX_TEST_FIXTURE_OBJ) $(TEST_FIXTURES_OBJ) $(LVGL_UI_TEST_FIXTURE_OBJ) $(TEST_OBJS) $(TEST_APP_OBJS_EXTRA)

# LVGL + Graphics stack (required for all UI tests)
TEST_LVGL_DEPS := $(LVGL_OBJS) $(HELIX_XML_OBJS) $(THORVG_OBJS)

# Platform-specific dependencies (Linux wpa_supplicant, macOS frameworks via LDFLAGS)
TEST_PLATFORM_DEPS := $(WPA_DEPS)

# Tool objects needed by tests (src/tools/ is excluded from APP_OBJS)
TEST_TOOL_OBJS := $(OBJ_DIR)/tools/xml_attribute_validator.o

# libhv dns_resolv C source needed by test_dns_resolver (not in libhv.a)
DNS_RESOLV_OBJ := $(OBJ_DIR)/tests/dns_resolv.o

# ============================================================================
# AUTOMATIC APP OBJECT DISCOVERY
# ============================================================================
# Include ALL application objects except those that conflict with tests.
# This replaces ~150 lines of manual object listings that needed constant updates.
#
# Exclusions (add files here ONLY if they cause linker conflicts):
#
# Group 1: App entry point and globals
# - main.o: Has main() function, tests have their own entry point
# - app_globals.o: Contains global subjects/state, ui_test_utils.o provides stubs
#
# Group 2: Files where ui_test_utils.o provides stub implementations
# - ui_notification.o: Needs get_notification_subject() from app_globals.o
# - ui_toast.o, ui_toast_manager.o: ui_test_utils.o provides stub toast functions
# - ui_notification_manager.o: ui_test_utils.o stubs notification functions
# - ui_text_input.o: ui_test_utils.o stubs ui_text_input_get_keyboard_hint()
#
# ui_emergency_stop.o WAS here, stubbed by a hand-written copy in
# ui_test_utils.cpp. That copy silently drifted from production — it kept
# mutating recovery state inline after show_recovery_for() was changed to
# marshal to the main thread — so the "unified recovery dialog" tests were
# asserting against the duplicate rather than the shipped code. The real object
# is linked now and the copy is gone.
#
# Group 3: Test-specific conflicts
# - ui_switch.o: test_ui_switch.cpp includes the .cpp directly for unit testing
# - ui_button.o: test_ui_button_defer_reuse.cpp includes the .cpp directly to
#   reach anonymous-namespace internals (defer_button_contrast_update, #924)
#
# Group 4: remote_client.o — undocumented, and no conflict was found
#   `nm -g --defined-only` over remote_client.o intersected against every object
#   under $(OBJ_DIR)/tests/ yields the empty set, and nothing in the test link
#   references it, so it is inert either way. Left excluded only because nothing
#   needs it; it is NOT load-bearing. Drop it from this list the moment a test
#   wants helix::RemoteClient.
#
# NOT excluded any more — application.o and its four link-time dependencies.
#   These were listed here with no rationale, which left Application (app
#   lifecycle, shutdown, teardown, printer-state init — four of the five
#   "Critical Paths" in CLAUDE.md) structurally untestable: nothing in the test
#   binary could even name it. The stated rule for this list is "ONLY if they
#   cause linker conflicts", so the claim was checked symbol by symbol:
#
#     object                    strong symbols also defined by a test object
#     ------------------------  -------------------------------------------
#     application.o             helix_notify_app_backgrounded/_foregrounded
#     moonraker_manager.o       MoonrakerManager::connect, ::macro_analysis
#     subject_initializer.o     (none)
#     panel_factory.o           (none)
#     remote_control_server.o   (none)
#
#   Four real duplicates, all of them test-side stubs standing in for the
#   production code that is now linked, so all four stubs were deleted
#   (tests/test_fixtures.cpp, tests/ui_test_utils.cpp). The other three objects
#   had no conflict at all — they are here because application.o references
#   SubjectInitializer, PanelFactory and RemoteControlServer and the link needs
#   their definitions.
#
#   Un-excluding them leaves 8 symbols that application.o pulls in from objects
#   still on this list (app_globals.o, ui_notification.o); ui_test_utils.cpp now
#   stubs those alongside its existing app_globals stubs.
#
#   lvgl_initializer.o was also removed: no such source or object has ever
#   existed in the tree, so the entry filtered nothing.
#
# Everything else is automatically included - new files just work!

TEST_APP_OBJS := $(filter-out \
    $(OBJ_DIR)/main.o \
    $(OBJ_DIR)/app_globals.o \
    $(OBJ_DIR)/ui/ui_notification.o \
    $(OBJ_DIR)/ui/ui_toast.o \
    $(OBJ_DIR)/ui/ui_toast_manager.o \
    $(OBJ_DIR)/ui/ui_notification_manager.o \
    $(OBJ_DIR)/ui/ui_text_input.o \
    $(OBJ_DIR)/ui/ui_switch.o \
    $(OBJ_DIR)/ui/ui_button.o \
    $(OBJ_DIR)/remote/remote_client.o \
    ,$(APP_OBJS) $(APP_C_OBJS))

# The PWM buzzer backend is platform-gated out of APP_SRCS (only ad5m/ad5x ship
# it), but tests/unit/test_pwm_sound_backend.cpp exercises its pure helpers on
# the host. The gate decides what SHIPS, not what is testable, so add the object
# back for the test link. $(sort) keeps this idempotent on the platforms where
# APP_OBJS already contains it.
TEST_APP_OBJS := $(sort $(TEST_APP_OBJS) $(OBJ_DIR)/system/pwm_sound_backend.o)

# ============================================================================
# Test Targets
# ============================================================================

# Clean test artifacts (test objects, PCH, all test binary variants).
#
# Wipes $(OBJ_DIR)/tests/ recursively so stale .o/.ccj/.d files from
# prior sanitizer runs, reordered test sources, or removed test files
# don't linger and pollute the link.
#
# Also removes the PCH ($(PCH)).
#
# Sanitizer cross-contamination is no longer a concern here: test-asan
# and test-tsan build into $(ASAN_OBJ_DIR)/$(TSAN_OBJ_DIR) with their
# own PCH, so they cannot leave instrumented objects or a tainted PCH
# in this build's paths. Use `make clean-sanitizers` to drop those
# trees. (Historically they shared $(OBJ_DIR) and a plain `make test`
# would then fail to link with "undefined reference to __asan_init".)
clean-tests:
	$(ECHO) "$(YELLOW)Cleaning test artifacts...$(RESET)"
	$(Q)rm -rf $(OBJ_DIR)/tests
	$(Q)rm -f $(TEST_BIN) $(TEST_ASAN_BIN) $(TEST_TSAN_BIN)
	$(Q)rm -f $(PCH)
	$(ECHO) "$(GREEN)✓ Test artifacts cleaned$(RESET)"

# Build tests — delegates to $(TEST_BIN) which handles -j detection via Phase 1
test-build: prune-orphan-test-objs $(TEST_BIN)
	@true

# Delete object files whose test source no longer exists, and drop the binary so
# it relinks without them.
#
# Deleting a test .cpp does NOT make $(TEST_BIN) out of date — the removed file
# is simply absent from the prerequisite list, nothing the binary depends on got
# newer, so make skips the link and the stale .o keeps its tests in the binary.
# The deleted tests then keep running (and can keep failing) with no source to
# read, which is deeply confusing. Cheap to check, so do it every build.
.PHONY: prune-orphan-test-objs
prune-orphan-test-objs:
	@orphans=""; \
	for obj in $(OBJ_DIR)/tests/test_*.o $(OBJ_DIR)/tests/application/test_*.o; do \
		[ -f "$$obj" ] || continue; \
		base=$$(basename "$$obj"); \
		case "$$base" in \
			test_main.o|test_fixtures.o) continue;; \
		esac; \
		src=$$(echo "$$obj" | sed 's|$(OBJ_DIR)/tests/|$(TEST_UNIT_DIR)/|; s|\.o$$|.cpp|'); \
		[ -f "$$src" ] || orphans="$$orphans $$obj"; \
	done; \
	if [ -n "$$orphans" ]; then \
		echo "$(YELLOW)Pruning orphaned test objects (source deleted):$(RESET)"; \
		for o in $$orphans; do echo "  $$(basename $$o)"; rm -f "$$o"; done; \
		rm -f $(TEST_BIN); \
	fi

# ============================================================================
# Main Test Targets
# ============================================================================
# IMPORTANT: The ~[.] filter excludes tests with tags starting with '.'
# including: [.ui_integration], [.disabled], [.benchmark], [.slow], etc.
# This is Catch2's hidden test convention.

# Build tests only (does not run)
# Use 'make test-run' to actually execute the tests
# Delegates to test-build for parallel compilation, then shows usage hint
test: test-build
	$(ECHO) "$(CYAN)Run tests with: make test-run$(RESET)"

# Run unit tests in PARALLEL (excludes hidden and slow tests for fast iteration)
# Uses Catch2 sharding across multiple processes for ~4-8x speedup
# Use 'make test-serial' for sequential execution (debugging, clean output)
# Use 'make test-all' to run everything including slow tests
test-run: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running unit tests in parallel (excluding slow)...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(call run_tests_parallel,"~[.] ~[slow]"); \
	END_TIME=$$(date +%s); \
	DURATION=$$((END_TIME - START_TIME)); \
	echo "$(GREEN)$(BOLD)✓ Tests passed in $${DURATION}s$(RESET)"

# Run unit tests SEQUENTIALLY (for debugging or clean output)
# Slower but useful when you need to see exact test ordering or debug failures
test-serial: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running unit tests sequentially (excluding slow)...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "~[.] ~[slow]"; \
	$(call report_test_result,Unit tests)

# Run ALL tests including slow ones (for thorough validation)
# Fast tests run in parallel shards; [slow] tests run sequentially to avoid
# deadlocks from thread-based tests (hv::EventLoop, std::thread) under sharding.
test-all: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running fast tests in parallel...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(call run_tests_parallel,"~[.] ~[slow]"); \
	echo "$(CYAN)$(BOLD)Running [slow] tests sequentially...$(RESET)"; \
	$(TEST_BIN) "[slow]" --durations yes; \
	$(call report_test_result,All tests)

# Run the HIDDEN test set — every test whose first tag character is '.'.
#
# FILTER: "[.]" selects ALL of them, not just the ones literally tagged [.].
# Catch2 appends a bare "." tag to every hidden test when it registers it
# (TestCaseInfo ctor, tests/catch_amalgamated.cpp: `if (isHidden())
# internalAppendTag("."_sr)`), and the spec parser splits a `[.foo]` pattern
# into a required "." plus a required "foo". So one `[.]` covers
# [.ui_integration], [.xml_required], [.disabled], [.skip], [.slow],
# [.benchmark], [.memprobe] and [.integration] alike. Enumerating the sub-tags
# would only rot the moment someone invents a new one.
#
# `cd $(CURDIR)` is load-bearing, not decoration. The [.ui_integration] and
# [.xml_required] tests read ui_xml/ on a RELATIVE path; from any other working
# directory they fail by the hundred and it reads as a regression. That cwd
# coupling is the main reason this set was hidden in the first place. $(CURDIR)
# is the directory make was started in, which for this tree is the repo root
# (and stays correct under `make -C`).
#
# SEQUENTIAL on purpose. Several of these own destructive global state
# (StaticSubjectRegistry deinit/re-init cycles in test_config.cpp) and several
# are timing-sensitive stress harnesses; sharding buys little here and muddies
# attribution when something goes red.
#
# Still NOT part of test-run — these cannot share that run's sharded, parallel,
# arbitrary-cwd execution. scripts/quality-checks.sh does gate on this target
# now that the set is green, but only when the test binary is already current
# (it never builds one) — see the "Hidden test set" block there and
# docs/devel/HIDDEN_TESTS_TRACKER.md for the inventory.
#
# Override the filter to run one slice: make test-hidden HIDDEN_FILTER='[.ui_integration]'
HIDDEN_FILTER ?= [.]

test-hidden: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running HIDDEN tests ($(HIDDEN_FILTER)) sequentially from $(CURDIR)...$(RESET)"
	@START_TIME=$$(date +%s); \
	cd $(CURDIR) && $(TEST_BIN) "$(HIDDEN_FILTER)"; \
	$(call report_test_result,Hidden tests)

# List the hidden set without running it — the inventory behind the tracker doc.
test-hidden-list: test-build
	$(ECHO) "$(CYAN)$(BOLD)Hidden test cases ($(HIDDEN_FILTER)):$(RESET)"
	$(Q)cd $(CURDIR) && $(TEST_BIN) "$(HIDDEN_FILTER)" --list-tests

# Alias that rebuilds and runs tests (useful for development)
tests: test-run

# ============================================================================
# KIAUH Extension Tests
# ============================================================================
# Tests that our KIAUH extension is discoverable by KIAUH's discover_extensions().
# Catches the class of bug where KIAUH crashes with IndexError because extension
# code doesn't match KIAUH's discovery conventions.

test-kiauh:
	$(ECHO) "$(CYAN)$(BOLD)Running KIAUH extension tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	python3 -m unittest scripts.kiauh.tests.test_kiauh_extension -v; \
	$(call report_test_result,KIAUH extension tests)

# ============================================================================
# Shell/Bats Tests
# ============================================================================

# Run shell/bats tests for platform hooks and installer scripts
test-shell:
	$(ECHO) "$(CYAN)$(BOLD)Running shell tests (bats)...$(RESET)"
	@if command -v bats >/dev/null 2>&1; then \
		START_TIME=$$(date +%s); \
		if command -v parallel >/dev/null 2>&1; then \
			NPROC=$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4); \
			bats --jobs "$$NPROC" --no-parallelize-within-files tests/shell/; \
		else \
			bats tests/shell/; \
		fi; \
		STATUS=$$?; \
		END_TIME=$$(date +%s); \
		DURATION=$$((END_TIME - START_TIME)); \
		if [ $$STATUS -ne 0 ]; then \
			echo "$(RED)$(BOLD)✗ Shell tests FAILED in $${DURATION}s$(RESET)"; \
			exit $$STATUS; \
		fi; \
		echo "$(GREEN)$(BOLD)✓ Shell tests passed in $${DURATION}s$(RESET)"; \
	else \
		echo "$(YELLOW)⚠ bats not found - skipping shell tests$(RESET)"; \
		echo "  Install with: brew install bats-core (macOS) or apt install bats (Linux)"; \
	fi

# ============================================================================
# helix-xml Submodule Test Suite (standalone CMake + Unity + ctest)
# ============================================================================
# lib/helix-xml is our own submodule (github.com/prestonbrown/helix-xml) and
# carries its own test suite, which the HelixScreen test binary does NOT run:
# helix-tests exercises the engine only through the app. These tests cover the
# parser, registries, expressions and malformed-input handling directly.
#
# The suite is standalone by design — it builds against a PINNED UPSTREAM LVGL
# pulled by FetchContent, never against our lib/lvgl. Ours has patches/*.patch
# applied, which inject calls to app-side symbols (helix_crash_note_*) that a
# standalone link cannot resolve; tests/CMakeLists.txt probes for that marker
# and rejects -DLVGL_DIR pointing at it. So do NOT try to save the fetch by
# pointing this at lib/lvgl.
#
# Build tree lives under $(BUILD_DIR), NOT inside the submodule. Two reasons:
# the submodule is edited in place (it is ours), so keeping generated artifacts
# out of it keeps `git status` there readable; and `make clean` semantics stay
# with the rest of the build. A developer following the submodule's own README
# may separately have lib/helix-xml/build — that one is theirs, untouched here.
#
# First configure clones LVGL and is slow + needs network. Every run after that
# is a no-op configure and ~2s of ctest.
HELIX_XML_TEST_SRC_DIR := $(HELIX_XML_DIR)/tests
HELIX_XML_TEST_BUILD_DIR ?= $(BUILD_DIR)/helix-xml-tests

# Extra args forwarded to ctest, e.g. HELIX_XML_CTEST_ARGS='-R test_expr'
HELIX_XML_CTEST_ARGS ?=

test-xml:
	$(ECHO) "$(CYAN)$(BOLD)Running helix-xml submodule tests (CMake + Unity)...$(RESET)"
	@if [ ! -f "$(HELIX_XML_TEST_SRC_DIR)/CMakeLists.txt" ]; then \
		echo "$(RED)$(BOLD)✗ $(HELIX_XML_TEST_SRC_DIR)/ not found — submodule not initialised$(RESET)"; \
		echo "  Run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	@if ! command -v cmake >/dev/null 2>&1; then \
		echo "$(RED)$(BOLD)✗ cmake not found — required to build the helix-xml test suite$(RESET)"; \
		echo "  Install with: brew install cmake (macOS) or apt install cmake (Linux)"; \
		exit 1; \
	fi
	@if [ ! -f "$(HELIX_XML_TEST_BUILD_DIR)/CMakeCache.txt" ]; then \
		echo "$(YELLOW)First configure: FetchContent will clone LVGL (needs network, minutes)...$(RESET)"; \
	fi
	@START_TIME=$$(date +%s); \
	cmake -S $(HELIX_XML_TEST_SRC_DIR) -B $(HELIX_XML_TEST_BUILD_DIR) > /tmp/helix_xml_cmake.log 2>&1 || { \
		cat /tmp/helix_xml_cmake.log; \
		echo "$(RED)$(BOLD)✗ helix-xml test configure failed$(RESET)"; \
		echo "  Log: /tmp/helix_xml_cmake.log"; \
		exit 1; \
	}; \
	cmake --build $(HELIX_XML_TEST_BUILD_DIR) -j $(NPROC) > /tmp/helix_xml_build.log 2>&1 || { \
		cat /tmp/helix_xml_build.log; \
		echo "$(RED)$(BOLD)✗ helix-xml test build failed$(RESET)"; \
		echo "  Log: /tmp/helix_xml_build.log"; \
		exit 1; \
	}; \
	ctest --test-dir $(HELIX_XML_TEST_BUILD_DIR) --output-on-failure --no-tests=error $(HELIX_XML_CTEST_ARGS); \
	$(call report_test_result,helix-xml submodule tests); \
	$(call summarize_unity_cases,$(HELIX_XML_TEST_BUILD_DIR))

# ============================================================================
# Out-of-Process UI Tests (pytest, tests/ui/)
# ============================================================================
# Named test-ui-pytest, not test-ui: that name is already taken by the
# in-process Catch2 convenience target above ([navigation],[theme],[wizard]).
# Renaming the existing one to make room was out of scope for wiring this
# suite in -- see docs/devel/UI_TESTING.md for what each covers.

# Run the out-of-process pytest suite (tests/ui/) against the real binary via
# `helix-screen ctl`. This is the FULL suite including the 8 golden-image
# tests -- locally, that's the point; CI runs a narrower slice (see
# .github/workflows/build.yml) because goldens are sensitive to renderer/font
# rasterization across machines.
test-ui-pytest:
	$(ECHO) "$(CYAN)$(BOLD)Running out-of-process UI tests (pytest, tests/ui/)...$(RESET)"
	@if [ ! -x "$(VENV_PYTHON)" ]; then \
		echo "$(RED)$(BOLD)✗ Python venv not found — run 'make venv-setup' first$(RESET)"; \
		exit 1; \
	fi
	@if [ ! -x "$(BIN_DIR)/helix-screen" ]; then \
		echo "$(RED)$(BOLD)✗ $(BIN_DIR)/helix-screen not built — run 'make -j' first$(RESET)"; \
		exit 1; \
	fi
	@START_TIME=$$(date +%s); \
	$(VENV_PYTHON) -m pytest tests/ui/ -v; \
	$(call report_test_result,Out-of-process UI tests)

# ============================================================================
# Moonraker Plugin Tests (pytest, moonraker-plugin/tests/)
# ============================================================================
# The plugin runs inside Moonraker on the printer, so nothing in the C++ suite
# reaches it -- these are its only coverage. They need no binary and no printer;
# the Moonraker component surface is mocked in the test file.

test-plugin:
	$(ECHO) "$(CYAN)$(BOLD)Running Moonraker plugin tests (pytest, moonraker-plugin/tests/)...$(RESET)"
	@if [ ! -x "$(VENV_PYTHON)" ]; then \
		echo "$(RED)$(BOLD)✗ Python venv not found — run 'make venv-setup' first$(RESET)"; \
		exit 1; \
	fi
	@START_TIME=$$(date +%s); \
	$(VENV_PYTHON) -m pytest moonraker-plugin/tests/ -v; \
	$(call report_test_result,Moonraker plugin tests)

# ============================================================================
# Convenience Test Targets - Run tests by component
# ============================================================================

# Run tests with per-test timing (shows slow tests)
test-verbose: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running tests with timing...$(RESET)"
	$(Q)$(TEST_BIN) --durations yes --use-colour yes

# Run G-code related tests
test-gcode: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running G-code tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[gcode]"; \
	$(call report_test_result,G-code tests)

# Run UI-related tests
test-ui: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running UI tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[navigation],[theme],[wizard]"; \
	$(call report_test_result,UI tests)

# Run Moonraker/mock-related tests
test-moonraker: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running Moonraker tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[mock],[sequencer],[capabilities]"; \
	$(call report_test_result,Moonraker tests)

# Run network-related tests (WiFi, Ethernet)
test-network: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running network tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[network],[scan],[connect]"; \
	$(call report_test_result,Network tests)

# Run security-related tests
test-security: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running security tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[security],[injection],[safety]"; \
	$(call report_test_result,Security tests)

# List all available test tags
test-list-tags: test-build
	$(ECHO) "$(CYAN)$(BOLD)Available test tags:$(RESET)"
	$(Q)$(TEST_BIN) --list-tags

# List all test cases
test-list: test-build
	$(Q)$(TEST_BIN) --list-tests

# Backwards compatibility alias
test-wizard: test

# ============================================================================
# Test Timing and Performance Targets
# ============================================================================
# Use these targets to identify slow tests and optimize test runtime.
# Tests tagged [slow] are excluded from test-fast for quick iteration.

# Show slowest tests (top 20) - useful for identifying optimization targets
test-timing: test-build
	$(ECHO) "$(CYAN)$(BOLD)Slowest tests (top 20):$(RESET)"
	@$(TEST_BIN) "~[.]" --durations yes 2>&1 | grep -E "^[0-9]+\.[0-9]+ s:" | sort -rn | head -20

# Run only fast tests in PARALLEL (skip hidden and slow tests) - for quick iteration
# Target: <15s total runtime for rapid development feedback with parallelism
test-fast: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running fast tests in parallel (skipping [slow] and hidden)...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(call run_tests_parallel,"~[.] ~[slow]"); \
	END_TIME=$$(date +%s); \
	DURATION=$$((END_TIME - START_TIME)); \
	echo "$(GREEN)$(BOLD)✓ Fast tests passed in $${DURATION}s$(RESET)"

# Run only slow tests - for thorough validation before commit
test-slow: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running slow tests only...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[slow]"; \
	$(call report_test_result,Slow tests)

# Run only eventloop tests - hv::EventLoop network tests (very slow, 5-10 min)
# These are the slowest tests due to WebSocket connection/disconnection cycles
test-eventloop: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running eventloop tests only (this will take 5-10 minutes)...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[eventloop]" "~[.]"; \
	$(call report_test_result,EventLoop tests)

# Smoke test - minimal critical tests for quick validation (<30s)
# Use during rapid iteration to catch obvious regressions
test-smoke: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running smoke tests (minimal critical subset)...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[config],[navigation],[ui_theme],[parser]" "~[slow]" "~[.]"; \
	$(call report_test_result,Smoke tests)

# Show test coverage summary by tag area
test-summary: test-build
	$(ECHO) "$(CYAN)$(BOLD)=== Test Coverage by Tag (top 25) ===$(RESET)"
	@$(TEST_BIN) --list-tags 2>&1 | grep -E "^\s+[0-9]+" | sort -rn | head -25
	$(ECHO) ""
	$(ECHO) "$(CYAN)$(BOLD)=== Test Count ===$(RESET)"
	@echo -n "  "; $(TEST_BIN) --list-tests 2>&1 | grep -c "^  " || echo "0"

# Generate timing report for major test categories
# Updates /tmp/test_timing.md with current timings
test-timing-report: test-build
	$(ECHO) "$(CYAN)$(BOLD)Generating test timing report...$(RESET)"
	@echo "| Tag | Tests | Time |" > /tmp/test_timing.md
	@echo "|-----|-------|------|" >> /tmp/test_timing.md
	@for tag in moonraker gcode printer_detector config parser navigation ui_theme security afc wizard mock; do \
		result=$$($(TEST_BIN) "[$$tag]" "~[.]" "~[slow]" --durations yes 2>&1); \
		count=$$(echo "$$result" | grep "test cases" | grep -o "[0-9]* passed" | head -1 || echo "0"); \
		time=$$(echo "$$result" | tail -1); \
		echo "| [\$$tag] | $$count | - |" >> /tmp/test_timing.md; \
	done
	@cat /tmp/test_timing.md
	$(ECHO) "$(GREEN)See /tmp/test_timing.md for full documentation$(RESET)"

# ============================================================================
# Slow Test Candidates (>500ms) - Tag with [slow] incrementally
# ============================================================================
# Based on timing analysis, these tests are candidates for [slow] tagging:
#
# Connection retry tests (~5s each):
#   - test_moonraker_client.cpp: "Multiple rapid retries all work correctly"
#   - test_moonraker_client.cpp: "Moonraker connection retries work correctly"
#
# Mock print simulation tests (~2-4s each):
#   - test_mock_print_simulation.cpp: All phase behavior tests
#   - test_mock_print_simulation.cpp: Progress and layer tracking tests
#
# To tag a test as slow, add [slow] to its tags:
#   TEST_CASE("My slow test", "[feature][slow]") { ... }
#
# ============================================================================

# Run only config tests
test-config: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running config tests...$(RESET)"
	$(Q)$(TEST_BIN) "[config]" || { \
		echo "$(RED)$(BOLD)✗ Config tests failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)$(BOLD)✓ Config tests passed!$(RESET)"

# ============================================================================
# Feature-Based Test Targets (New Taxonomy)
# ============================================================================
# Tests are now tagged by FEATURE/IMPORTANCE rather than layer/speed.
# Use these targets to test specific functional areas.

# CORE tests - Critical functionality that MUST work (<15s, ~18 tests)
# If these fail, the app is fundamentally broken.
test-core: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running core tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[core]"; \
	$(call report_test_result,Core tests)

# CONNECTION tests - Moonraker connection lifecycle, retry, robustness
test-connection: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running connection tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[connection]" "~[slow]"; \
	$(call report_test_result,Connection tests)

# STATE tests - PrinterState, subjects, observers
test-state: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running state tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[state]"; \
	$(call report_test_result,State tests)

# PRINT tests - Print workflow, start/pause/cancel, exclude object
test-print: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running print tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[print]" "~[slow]"; \
	$(call report_test_result,Print tests)

# CALIBRATION tests - Bed mesh, input shaper
test-calibration: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running calibration tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[calibration]"; \
	$(call report_test_result,Calibration tests)

# PRINTER tests - Printer detection, capabilities, hardware
test-printer: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running printer tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[printer]"; \
	$(call report_test_result,Printer tests)

# AMS tests - All AMS/MMU backends (includes [afc], [ace])
test-ams: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running AMS tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[ams]"; \
	$(call report_test_result,AMS tests)

# FILAMENT tests - Spoolman, filament sensors
test-filament: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running filament tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[filament]"; \
	$(call report_test_result,Filament tests)

# ASSETS tests - Thumbnails, prerendered images
test-assets: test-build
	$(ECHO) "$(CYAN)$(BOLD)Running assets tests...$(RESET)"
	@START_TIME=$$(date +%s); \
	$(TEST_BIN) "[assets]"; \
	$(call report_test_result,Assets tests)

# Unified test binary - uses automatic app object discovery
# No more manual dependency lists! New source files are automatically included.
# If you get linker errors, check if the file needs to be excluded (TEST_APP_OBJS filter).
# Two-phase build for $(TEST_BIN) to handle unlimited -j detection
# Phase 1: No deps - check for unlimited -j and re-invoke if needed
# Phase 2: Normal deps and linking (when _PARALLEL_GUARD is set)
#
# Same three-way handling as the main build (mk/rules.mk `all:`), and for the
# same reason. Phase 1 used to ALWAYS force -j$(NPROC), which silently discarded
# an explicit bound: `make test -j6` still ran -j32 here. That is invisible until
# several sessions share one checkout, at which point it multiplies — four
# concurrent `make test` runs produced 103 cc1plus and drove a 123G box into
# swap.
#
# A bounded -jN puts a --jobserver-auth entry in MAKEFLAGS. `exec` replaces the
# process image but KEEPS open file descriptors, so the jobserver FDs survive and
# re-invoking without -j inherits the caller's limit instead of overriding it.
# An explicit -j$(NPROC) is only correct for the other two cases: unlimited `-j`
# (a 'j' in MAKEFLAGS with no jobserver), and no -j at all — the latter because
# exec'ing with neither would leave Phase 2 at -j1, building hundreds of files
# serially.
ifndef _PARALLEL_GUARD
$(TEST_BIN): FORCE
	@if echo "$(MAKEFLAGS)" | grep -q 'jobserver'; then \
		exec $(MAKE) _PARALLEL_GUARD=1 --no-print-directory $@; \
	else \
		if echo "$(MAKEFLAGS)" | grep -q 'j'; then \
			echo ""; \
			printf '\033[1;33m⚠️  make -j (unlimited) detected - auto-fixing to -j%s\033[0m\n' "$(NPROC)"; \
			echo ""; \
		fi; \
		exec $(MAKE) _PARALLEL_GUARD=1 --no-print-directory -j$(NPROC) $@; \
	fi
else
$(TEST_BIN): $(TEST_CORE_DEPS) \
             $(TEST_LVGL_DEPS) \
             $(TEST_APP_OBJS) \
             $(TEST_TOOL_OBJS) \
             $(DNS_RESOLV_OBJ) \
             $(MOCK_OBJS) \
             $(LV_MARKDOWN_OBJS) \
             $(QUIRC_OBJS) \
             $(FONT_OBJS) \
             $(TRANS_OBJS) \
             $(OBJCPP_OBJS) \
             $(TEST_PLATFORM_DEPS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) helix-tests"
	$(Q)$(CXX) $(CXXFLAGS) $(sort $^) -o $@ $(LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Test linking failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)✓ Unit test binary ready$(RESET)"
endif

# FORCE pattern - ensures Phase 1 always runs for dependency re-evaluation
# Without this, make would skip Phase 1 entirely when the binary exists,
# never reaching Phase 2 where real dependencies (.d files) are checked.
.PHONY: FORCE
FORCE:

# Integration test binary (uses mocks instead of real LVGL)
$(TEST_INTEGRATION_BIN): $(TEST_MAIN_OBJ) $(CATCH2_OBJ) $(TEST_INTEGRATION_OBJS) $(MOCK_OBJS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD]$(RESET) run_integration_tests"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ Integration test linking failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)✓ Integration test binary ready$(RESET)"

# Run integration tests
test-integration: $(TEST_INTEGRATION_BIN)
	$(ECHO) "$(CYAN)$(BOLD)Running integration tests (with mocks)...$(RESET)"
	$(Q)$(TEST_INTEGRATION_BIN) || { \
		echo "$(RED)$(BOLD)✗ Integration tests failed!$(RESET)"; \
		exit 1; \
	}
	$(ECHO) "$(GREEN)$(BOLD)✓ All integration tests passed!$(RESET)"

# Compile test main (Catch2 runner)
# Note: No DEPFLAGS for Catch2 infrastructure - rarely changes
$(TEST_MAIN_OBJ): $(TEST_DIR)/test_main.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST-MAIN]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -I$(TEST_DIR) -c $< -o $@

# Compile Catch2 amalgamated source
# Note: No DEPFLAGS - this is third-party code that never changes
$(CATCH2_OBJ): $(TEST_DIR)/catch_amalgamated.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[CATCH2]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

# Test compile rules depend on libhv-generated headers ($(LIBHV_JSON_HEADER))
# and the PCH so a fresh `make test-asan` / `make test-tsan` (without a prior
# `make`) triggers libhv-build before any .cpp that includes hv/json.hpp.
#
# Compile UI test utilities
# Uses DEPFLAGS to track header dependencies
$(UI_TEST_UTILS_OBJ): $(TEST_DIR)/ui_test_utils.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[UI-TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@

# Compile LVGL test fixture (shared base class for UI tests)
# Uses DEPFLAGS to track header dependencies
$(LVGL_TEST_FIXTURE_OBJ): $(TEST_DIR)/lvgl_test_fixture.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[LVGL-FIXTURE]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@

# Compile HelixScreen test fixture (base class that resets process singletons)
# Uses DEPFLAGS to track header dependencies
$(HELIX_TEST_FIXTURE_OBJ): $(TEST_DIR)/helix_test_fixture.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[HELIX-FIXTURE]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@

# Compile test fixtures (reusable fixtures with mock initialization helpers)
# Uses DEPFLAGS to track header dependencies
$(TEST_FIXTURES_OBJ): $(TEST_DIR)/test_fixtures.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[TEST-FIXTURE]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@

# Compile LVGL UI test fixture (full UI integration test fixture)
# Uses DEPFLAGS to track header dependencies
# Emits .ccj fragment for incremental compile_commands.json generation
$(LVGL_UI_TEST_FIXTURE_OBJ): $(TEST_DIR)/lvgl_ui_test_fixture.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[UI-FIXTURE]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@
	$(call emit-compile-command,$(CXX),$(CXXFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile test sources
# Uses DEPFLAGS to track header dependencies for incremental rebuilds
# Emits .ccj fragment for incremental compile_commands.json generation
$(OBJ_DIR)/tests/%.o: $(TEST_UNIT_DIR)/%.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@
	$(call emit-compile-command,$(CXX),$(CXXFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF),$<,$@)

# Compile application subdirectory test sources
# Emits .ccj fragment for incremental compile_commands.json generation
$(OBJ_DIR)/tests/application/%.o: $(TEST_UNIT_DIR)/application/%.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST-APP]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) -I$(TEST_UNIT_DIR)/application $(INCLUDES) $(LV_CONF) -c $< -o $@
	$(call emit-compile-command,$(CXX),$(CXXFLAGS) $(PCH_FLAGS) -I$(TEST_DIR) -I$(TEST_UNIT_DIR)/application $(INCLUDES) $(LV_CONF),$<,$@)

# Compile libhv dns_resolv.c for test_dns_resolver
# dns_resolv.c dependency on PATCHES_STAMP is declared in rules.mk
$(DNS_RESOLV_OBJ): $(LIBHV_DIR)/base/dns_resolv.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST-C]$(RESET) $<"
	$(Q)$(CC) $(CFLAGS) -I$(LIBHV_DIR)/base $(LIBHV_INC) -c $< -o $@

# Compile mock sources
# Uses DEPFLAGS to track header dependencies
# Emits .ccj fragment for incremental compile_commands.json generation
$(OBJ_DIR)/tests/mocks/%.o: $(TEST_MOCK_DIR)/%.cpp $(LIBHV_LIB) $(LIBHV_JSON_HEADER) $(PCH)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(YELLOW)[MOCK]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) -I$(TEST_MOCK_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@
	$(call emit-compile-command,$(CXX),$(CXXFLAGS) $(PCH_FLAGS) -I$(TEST_MOCK_DIR) $(INCLUDES) $(LV_CONF),$<,$@)

# Dynamic card instantiation test
TEST_CARDS_BIN := $(BIN_DIR)/test_dynamic_cards
TEST_CARDS_OBJ := $(OBJ_DIR)/test_dynamic_cards.o

test-cards: $(TEST_CARDS_BIN)
	$(ECHO) "$(CYAN)Running dynamic card test...$(RESET)"
	$(Q)$(TEST_CARDS_BIN)

$(TEST_CARDS_BIN): $(TEST_CARDS_OBJ) $(LVGL_OBJS) $(FONT_OBJS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_dynamic_cards"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ Test binary ready$(RESET)"

$(TEST_CARDS_OBJ): $(SRC_DIR)/test_dynamic_cards.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# LV_SIZE_CONTENT behavior test
# Tests nested flex containers with SIZE_CONTENT - LVGL handles this natively
TEST_SIZE_CONTENT_BIN := $(BIN_DIR)/test_size_content
TEST_SIZE_CONTENT_OBJ := $(OBJ_DIR)/tests/test_size_content.o

test-size-content: $(TEST_SIZE_CONTENT_BIN)
	$(ECHO) "$(CYAN)Running LV_SIZE_CONTENT behavior test...$(RESET)"
	$(Q)$(TEST_SIZE_CONTENT_BIN)

$(TEST_SIZE_CONTENT_BIN): $(TEST_SIZE_CONTENT_OBJ) $(CATCH2_OBJ) $(TEST_MAIN_OBJ) $(LVGL_OBJS) $(THORVG_OBJS)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_size_content"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ Test binary ready: $@$(RESET)"

$(TEST_SIZE_CONTENT_OBJ): $(TEST_UNIT_DIR)/test_size_content.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) -I$(TEST_DIR) $(INCLUDES) $(LV_CONF) -c $< -o $@

# Responsive theme and breakpoint test
TEST_RESPONSIVE_THEME_BIN := $(BIN_DIR)/test_responsive_theme
TEST_RESPONSIVE_THEME_OBJ := $(OBJ_DIR)/test_responsive_theme.o

test-responsive-theme: $(TEST_RESPONSIVE_THEME_BIN)
	$(ECHO) "$(CYAN)Running responsive theme and breakpoint tests...$(RESET)"
	$(Q)$(TEST_RESPONSIVE_THEME_BIN)

$(TEST_RESPONSIVE_THEME_BIN): $(TEST_RESPONSIVE_THEME_OBJ) $(LVGL_OBJS) $(THORVG_OBJS) $(OBJ_DIR)/ui_theme.o
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_responsive_theme"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ Responsive theme test binary ready$(RESET)"

$(TEST_RESPONSIVE_THEME_OBJ): $(SRC_DIR)/test_responsive_theme.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# ============================================================================
# G-Code Geometry & SDF Tests
# ============================================================================
# G-Code Geometry Builder test
TEST_GCODE_GEOMETRY_BIN := $(BIN_DIR)/test_gcode_geometry
TEST_GCODE_GEOMETRY_OBJ := $(OBJ_DIR)/test_gcode_geometry.o

test-gcode-geometry: $(TEST_GCODE_GEOMETRY_BIN)
	$(ECHO) "$(CYAN)Running G-code geometry test...$(RESET)"
	$(Q)$(TEST_GCODE_GEOMETRY_BIN)
	$(ECHO) ""

$(TEST_GCODE_GEOMETRY_BIN): $(TEST_GCODE_GEOMETRY_OBJ) $(OBJ_DIR)/gcode_parser.o $(OBJ_DIR)/gcode_geometry_builder.o
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_gcode_geometry"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ -lm $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ G-code geometry test binary ready$(RESET)"

$(TEST_GCODE_GEOMETRY_OBJ): $(SRC_DIR)/test_gcode_geometry.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# G-Code SDF Reconstruction test
TEST_SDF_RECONSTRUCTION_BIN := $(BIN_DIR)/test_sdf_reconstruction
TEST_SDF_RECONSTRUCTION_OBJ := $(OBJ_DIR)/test_sdf_reconstruction.o

test-sdf-reconstruction: $(TEST_SDF_RECONSTRUCTION_BIN)
	$(ECHO) "$(CYAN)Running SDF reconstruction test...$(RESET)"
	$(Q)$(TEST_SDF_RECONSTRUCTION_BIN)
	$(ECHO) ""

$(TEST_SDF_RECONSTRUCTION_BIN): $(TEST_SDF_RECONSTRUCTION_OBJ) $(OBJ_DIR)/gcode_parser.o $(OBJ_DIR)/gcode_sdf_builder.o
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_sdf_reconstruction"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ -lm $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ SDF reconstruction test binary ready$(RESET)"

$(TEST_SDF_RECONSTRUCTION_OBJ): $(SRC_DIR)/test_sdf_reconstruction.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Sparse Grid test (validates NanoVDB integration)
TEST_SPARSE_GRID_BIN := $(BIN_DIR)/test_sparse_grid
TEST_SPARSE_GRID_OBJ := $(OBJ_DIR)/test_sparse_grid.o

test-sparse-grid: $(TEST_SPARSE_GRID_BIN)
	$(ECHO) "$(CYAN)Running sparse grid test...$(RESET)"
	$(Q)$(TEST_SPARSE_GRID_BIN)
	$(ECHO) ""

$(TEST_SPARSE_GRID_BIN): $(TEST_SPARSE_GRID_OBJ) $(OBJ_DIR)/gcode_parser.o $(OBJ_DIR)/gcode_sdf_builder.o $(OBJ_DIR)/gcode_sparse_grid.o
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_sparse_grid"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ -lm $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ Sparse grid test binary ready$(RESET)"

$(TEST_SPARSE_GRID_OBJ): $(SRC_DIR)/test_sparse_grid.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Partial mesh extraction test
TEST_PARTIAL_EXTRACTION_BIN := $(BIN_DIR)/test_partial_extraction
TEST_PARTIAL_EXTRACTION_OBJ := $(OBJ_DIR)/test_partial_extraction.o

test-partial-extraction: $(TEST_PARTIAL_EXTRACTION_BIN)
	$(ECHO) "$(CYAN)Running partial extraction test...$(RESET)"
	$(Q)$(TEST_PARTIAL_EXTRACTION_BIN)
	$(ECHO) ""

$(TEST_PARTIAL_EXTRACTION_BIN): $(TEST_PARTIAL_EXTRACTION_OBJ) $(OBJ_DIR)/gcode_parser.o $(OBJ_DIR)/gcode_sdf_builder.o
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)[LD]$(RESET) test_partial_extraction"
	$(Q)$(CXX) $(CXXFLAGS) $^ -o $@ -lm $(LDFLAGS)
	$(ECHO) "$(GREEN)✓ Partial extraction test binary ready$(RESET)"

$(TEST_PARTIAL_EXTRACTION_OBJ): $(SRC_DIR)/test_partial_extraction.cpp
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(BLUE)[TEST]$(RESET) $<"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@





# ============================================================================
# Sanitizer Targets (Memory Safety Testing)
# ============================================================================
# These targets rebuild the test binary with sanitizers enabled for detecting:
# - ASAN: Memory leaks, use-after-free, buffer overflows
# - TSAN: Data races, deadlocks, thread safety issues
#
# Note: Sanitizer builds are slower and use more memory.
# Results are printed to stderr with detailed stack traces.

# Sanitizer flags
ASAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer -g
TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g

# Sanitizer builds get their own object tree and PCH.
#
# They used to compile into $(OBJ_DIR) alongside the normal build, which made
# `make test-asan` a trap: instrumented .o files landed on top of the ordinary
# ones, so the next plain `make test` died at link with "undefined reference to
# __asan_init" — from files the user never touched. The documented recovery was
# a full `make clean`, i.e. a from-scratch rebuild, and the failure gave no hint
# that a sanitizer run was the cause. Recovering by hand cost an evening once.
#
# Separate trees make the collision impossible instead of recoverable, and the
# two builds no longer invalidate each other, so switching between them is
# incremental rather than a full rebuild each way.
ASAN_OBJ_DIR := $(BUILD_DIR)/obj-asan
TSAN_OBJ_DIR := $(BUILD_DIR)/obj-tsan
ASAN_PCH := $(BUILD_DIR)/asan-lvgl_pch.h.gch
TSAN_PCH := $(BUILD_DIR)/tsan-lvgl_pch.h.gch

# Overrides handed to the sanitizer sub-makes. Passed as command-line variables
# so they beat the `:=` assignments in the top-level Makefile. BIN_DIR is left
# shared deliberately — the binaries already have distinct names, and keeping it
# stable means $(TEST_ASAN_BIN) resolves to the same path in both makes.
ASAN_MAKE_OVERRIDES := OBJ_DIR=$(ASAN_OBJ_DIR) PCH=$(ASAN_PCH) \
	CXXFLAGS='$(CXXFLAGS) $(ASAN_FLAGS)' LDFLAGS='$(LDFLAGS) $(ASAN_FLAGS)'
TSAN_MAKE_OVERRIDES := OBJ_DIR=$(TSAN_OBJ_DIR) PCH=$(TSAN_PCH) \
	CXXFLAGS='$(CXXFLAGS) $(TSAN_FLAGS)' LDFLAGS='$(LDFLAGS) $(TSAN_FLAGS)'

# Patterns that mean "the sanitizer reported something". Kept as variables so
# the four sanitizer recipes share one definition. No commas — these are passed
# as $(call) arguments.
# LeakSanitizer is deliberately NOT in this pattern. Leaks are gated separately by
# scripts/check_asan_leaks.py against a shrink-only baseline, because the suite
# leaves widgets and subjects alive on purpose — freeing them means close-time
# teardown (prestonbrown/helixscreen#1246), not a one-line fix. An ASan error is a
# real memory bug and stays fatal here; a leak is measured against the baseline.
# Working that baseline down is tracked in prestonbrown/helixscreen#1279.
ASAN_REPORT_RE := ERROR: AddressSanitizer
TSAN_REPORT_RE := (WARNING|ERROR): ThreadSanitizer

# Report the result of the immediately-preceding sanitizer run, FAILING the
# recipe if the sanitizer said anything. Captures $$? as its first action, so it
# MUST directly follow the run, and the run MUST be under `set -o pipefail`.
#
# Two independent checks, because neither alone is sufficient:
#
#   - Exit status alone misses reports from forked children. The [subprocess]
#     crash-handler tests fork, and a child killed by ASan leaves the parent
#     free to exit 0 — that is how eight stack-buffer-underflow reports rode
#     along in a "passing" run.
#   - Grepping the log alone misses an ordinary Catch2 assertion failure, which
#     produces no sanitizer report at all.
#
# `set -o pipefail` at the call site is what makes the status meaningful in the
# first place. Make runs recipes under bash WITHOUT pipefail, so a pipeline into
# tee otherwise yields tee's status, which is always 0. Combined with an
# unconditional "✓ complete" banner on the next line, that made these gates
# incapable of reporting red: the 2026-08-14 nightly logged ten
# "ERROR: AddressSanitizer" reports and was still reported green.
#
# Note on halt_on_error=0: it only affects RECOVERABLE errors, which need
# -fsanitize-recover=address at compile time. We do not build with that, so
# every finding is fatal and the run stops at the first one in the parent
# process. The option is left in place as documentation of intent, but do not
# read it as "this run collected every finding".
#
# Args: $(1) = human label, $(2) = output file, $(3) = report regex.
define report_sanitizer_result
	SAN_RC=$$?; \
	if grep -qE '$(3)' "$(2)" 2>/dev/null; then \
		echo "$(RED)$(BOLD)✗ $(1) FAILED — $$(grep -cE '$(3)' "$(2)") sanitizer report(s) in $(2)$(RESET)"; \
		grep -nE '$(3)' "$(2)" | head -20 | sed 's/^/     /'; \
		exit 1; \
	fi; \
	if [ $$SAN_RC -ne 0 ]; then \
		echo "$(RED)$(BOLD)✗ $(1) FAILED (exit $$SAN_RC) — see $(2)$(RESET)"; \
		exit $$SAN_RC; \
	fi; \
	echo "$(GREEN)$(BOLD)✓ $(1) clean — no sanitizer reports$(RESET)"
endef

# Runtime options for the sanitizer binaries. Defined once so the -one variants
# cannot drift from the full-suite ones — that drift is how test-tsan-one ended
# up running without the suppressions file.
#
# allocator_may_return_null=1 makes an oversized request return NULL, which is
# what a real allocator does under exhaustion; the sanitizers' default is to
# report allocation-size-too-big and ABORT. The [ui_utils][l069] tests
# deliberately ask lv_malloc for SIZE_MAX-1 to prove set_owned_user_string()
# reports the failure instead of memcpy'ing into a null result, so the default
# aborted the whole run ~1000 lines in and hid everything after it. The cost is
# that a genuine overflow-driven huge allocation now returns NULL rather than
# raising here; every other error class is unaffected, and our own callers log
# the null.
#
# BOTH sanitizers need it — the option is shared runtime, not an ASan feature.
# TSan was left without it and died on that same test in the 2026-08-16 nightly,
# taking every test ordered after it down with it.
ASAN_RUN_OPTIONS := detect_leaks=1:halt_on_error=0:allocator_may_return_null=1
TSAN_RUN_OPTIONS := halt_on_error=0 allocator_may_return_null=1 \
	suppressions=$(CURDIR)/tests/tsan_suppressions.txt

# Leaks still get DETECTED and printed (detect_leaks=1 above); exitcode=0 only stops
# them from setting the process exit status. That keeps the two verdicts separate:
# a non-zero exit now means a genuine failure — an ASan error, a crash, or a failed
# assertion — and never "the suite passed but leaked". Without this the gate would
# need to guess why the binary exited non-zero, which is how it got masked before.
LSAN_RUN_OPTIONS := exitcode=0

# AddressSanitizer test binary
TEST_ASAN_BIN := $(BIN_DIR)/helix-tests-asan

# ThreadSanitizer test binary
TEST_TSAN_BIN := $(BIN_DIR)/helix-tests-tsan

# Build and run tests with AddressSanitizer
test-asan:
	$(ECHO) "$(CYAN)$(BOLD)Building tests with AddressSanitizer...$(RESET)"
	@$(MAKE) $(ASAN_MAKE_OVERRIDES) TEST_BIN=$(TEST_ASAN_BIN) $(TEST_ASAN_BIN)
	$(ECHO) "$(CYAN)$(BOLD)Running tests with AddressSanitizer...$(RESET)"
	@set -o pipefail; \
	ASAN_OPTIONS=$(ASAN_RUN_OPTIONS) LSAN_OPTIONS=$(LSAN_RUN_OPTIONS) \
	  $(TEST_ASAN_BIN) "~[.]" 2>&1 | tee /tmp/asan_output.txt; \
	$(call report_sanitizer_result,ASAN,/tmp/asan_output.txt,$(ASAN_REPORT_RE))
	@python3 scripts/check_asan_leaks.py \
	  --baseline scripts/asan_leak_baseline.txt /tmp/asan_output.txt

# Build and run tests with ThreadSanitizer
# Override TSAN_FILTER to change which tests run (default: all non-hidden)
TSAN_FILTER ?= ~[.]
# ThreadSanitizer runs sharded, and not only for speed. Unsharded, the run
# aborts inside TSan's own reporter roughly three times in four (#1293): a libhv
# event loop from an earlier test tears down during a later, server-less test,
# by which point the closing fd's creating thread has exited, so TSan's fd table
# yields kInvalidTid and ScopedReportBase::AddLocation indexes its thread
# registry out of bounds. That abort is upstream and unreachable by a
# suppression, because the CHECK fires while the report is still being built.
#
# Sharding removes the cross-test pairing that triggers it: measured 0 aborts
# across 32 shards, against 3 aborts in 4 unsharded runs of the same binary. It
# is also ~4x faster, and a shard that does trip it is cheap to re-run.
TSAN_SHARDS ?= 32
# Each TSAN process carries its own shadow memory, so cap concurrency on cores
# rather than launching every shard at once the way the non-sanitizer suite does.
TSAN_SHARD_JOBS ?= $(shell n=$$(nproc 2>/dev/null || echo 4); if [ $$n -gt 16 ]; then echo 16; else echo $$n; fi)
# A shard that dies on the upstream reporter abort is re-run, but ONLY when its
# log carries the CHECK and no sanitizer report at all. A shard that found a
# genuine race is never retried, so this cannot turn a real finding green.
TSAN_SHARD_RETRIES ?= 3

test-tsan:
	$(ECHO) "$(CYAN)$(BOLD)Building tests with ThreadSanitizer...$(RESET)"
	@$(MAKE) $(TSAN_MAKE_OVERRIDES) TEST_BIN=$(TEST_TSAN_BIN) $(TEST_TSAN_BIN)
	$(ECHO) "$(CYAN)$(BOLD)Running tests with ThreadSanitizer ($(TSAN_SHARDS) shards, $(TSAN_SHARD_JOBS) at a time, filter: $(TSAN_FILTER))...$(RESET)"
	@shard_dir=$$(mktemp -d "$(SHARD_ARTIFACT_ROOT)/tsan-shards-XXXXXX"); \
	inconclusive=""; \
	for i in $$(seq 0 $$(($(TSAN_SHARDS)-1))); do \
		while [ "$$(jobs -rp | wc -l)" -ge $(TSAN_SHARD_JOBS) ]; do sleep 1; done; \
		( TSAN_OPTIONS="$(TSAN_RUN_OPTIONS)" $(TEST_TSAN_BIN) "$(TSAN_FILTER)" \
			--shard-count $(TSAN_SHARDS) --shard-index $$i > "$$shard_dir/$$i.log" 2>&1; \
		  echo $$? > "$$shard_dir/$$i.exit" ) & \
	done; \
	wait; \
	rc=0; \
	for i in $$(seq 0 $$(($(TSAN_SHARDS)-1))); do \
		ec=$$(cat "$$shard_dir/$$i.exit" 2>/dev/null | tr -d '[:space:]'); \
		if [ -z "$$ec" ]; then \
			echo "$(RED)$(BOLD)✗ TSAN shard $$i produced no exit status$(RESET)"; rc=1; continue; \
		fi; \
		if [ "$$ec" = "0" ]; then continue; fi; \
		if grep -q 'CHECK failed' "$$shard_dir/$$i.log" 2>/dev/null && \
		   ! grep -qE '$(TSAN_REPORT_RE)' "$$shard_dir/$$i.log" 2>/dev/null; then \
			recovered=0; \
			for attempt in $$(seq 1 $(TSAN_SHARD_RETRIES)); do \
				echo "$(YELLOW)⚠ TSAN shard $$i hit the upstream reporter abort (no findings); retry $$attempt/$(TSAN_SHARD_RETRIES)$(RESET)"; \
				if TSAN_OPTIONS="$(TSAN_RUN_OPTIONS)" $(TEST_TSAN_BIN) "$(TSAN_FILTER)" \
					--shard-count $(TSAN_SHARDS) --shard-index $$i > "$$shard_dir/$$i.log" 2>&1; then \
					recovered=1; break; \
				fi; \
				if grep -qE '$(TSAN_REPORT_RE)' "$$shard_dir/$$i.log" 2>/dev/null; then break; fi; \
			done; \
			if [ $$recovered -eq 1 ]; then continue; fi; \
			inconclusive="$$inconclusive $$i"; \
			echo "$(YELLOW)$(BOLD)⚠ TSAN shard $$i still aborting after $(TSAN_SHARD_RETRIES) retries; its tests went UNCHECKED$(RESET)"; \
			continue; \
		fi; \
		echo "$(RED)$(BOLD)✗ TSAN shard $$i exited $$ec$(RESET)"; rc=$$ec; \
	done; \
	cat "$$shard_dir"/*.log > /tmp/tsan_output.txt 2>/dev/null; \
	rm -rf "$$shard_dir"; \
	if [ -n "$$inconclusive" ]; then \
		echo "$(YELLOW)$(BOLD)⚠ TSAN: shard(s)$$inconclusive were INCONCLUSIVE (upstream reporter abort, see #1293).$(RESET)"; \
		echo "$(YELLOW)  Those shards reported no race before dying, so nothing is being hidden -$(RESET)"; \
		echo "$(YELLOW)  but their tests were not checked. Set TSAN_STRICT=1 to fail on this.$(RESET)"; \
		if [ -n "$(TSAN_STRICT)" ]; then rc=1; fi; \
	fi; \
	( exit $$rc ); \
	$(call report_sanitizer_result,TSAN,/tmp/tsan_output.txt,$(TSAN_REPORT_RE))

# Run specific test with ASAN (usage: make test-asan-one TEST="[streaming]")
test-asan-one:
	$(ECHO) "$(CYAN)$(BOLD)Building tests with AddressSanitizer...$(RESET)"
	@$(MAKE) $(ASAN_MAKE_OVERRIDES) TEST_BIN=$(TEST_ASAN_BIN) $(TEST_ASAN_BIN)
	$(ECHO) "$(CYAN)$(BOLD)Running test '$(TEST)' with AddressSanitizer...$(RESET)"
	@set -o pipefail; \
	ASAN_OPTIONS=$(ASAN_RUN_OPTIONS) LSAN_OPTIONS=$(LSAN_RUN_OPTIONS) \
	  $(TEST_ASAN_BIN) "$(TEST)" 2>&1 | tee /tmp/asan_output.txt; \
	$(call report_sanitizer_result,ASAN,/tmp/asan_output.txt,$(ASAN_REPORT_RE))
# NOTE: no leak ratchet here. The baseline is pinned to the full-suite invocation
# above; a filtered run leaks a different population, so checking it would fail on
# a subset and pass on nothing useful.

# Run specific test with TSAN (usage: make test-tsan-one TEST="[streaming]")
test-tsan-one:
	$(ECHO) "$(CYAN)$(BOLD)Building tests with ThreadSanitizer...$(RESET)"
	@$(MAKE) $(TSAN_MAKE_OVERRIDES) TEST_BIN=$(TEST_TSAN_BIN) $(TEST_TSAN_BIN)
	$(ECHO) "$(CYAN)$(BOLD)Running test '$(TEST)' with ThreadSanitizer...$(RESET)"
	@set -o pipefail; \
	TSAN_OPTIONS="$(TSAN_RUN_OPTIONS)" $(TEST_TSAN_BIN) "$(TEST)" 2>&1 | tee /tmp/tsan_output.txt; \
	$(call report_sanitizer_result,TSAN,/tmp/tsan_output.txt,$(TSAN_REPORT_RE))

# Clean sanitizer binaries
clean-sanitizers:
	$(ECHO) "$(YELLOW)Cleaning sanitizer binaries and object trees...$(RESET)"
	$(Q)rm -f $(TEST_ASAN_BIN) $(TEST_TSAN_BIN)
	$(Q)rm -f $(ASAN_PCH) $(TSAN_PCH)
	$(Q)rm -rf $(ASAN_OBJ_DIR) $(TSAN_OBJ_DIR)
	$(ECHO) "$(GREEN)✓ Sanitizer artifacts cleaned (normal build untouched)$(RESET)"

# ============================================================================
# Test Help
# ============================================================================

.PHONY: help-test test-kiauh test-shell test-xml test-ui-pytest test-plugin test-serial test-hidden test-hidden-list test-asan test-tsan test-asan-one test-tsan-one clean-sanitizers
help-test:
	@if [ -t 1 ] && [ -n "$(TERM)" ] && [ "$(TERM)" != "dumb" ]; then \
		B='$(BOLD)'; G='$(GREEN)'; Y='$(YELLOW)'; C='$(CYAN)'; X='$(RESET)'; \
	else \
		B=''; G=''; Y=''; C=''; X=''; \
	fi; \
	echo "$${B}Test Targets$${X}"; \
	echo ""; \
	echo "$${C}Main Test Targets:$${X}"; \
	echo "  $${G}test$${X}                 - Build tests (does not run)"; \
	echo "  $${G}test-run$${X}             - Run tests in PARALLEL (default, ~4-8x faster)"; \
	echo "  $${G}test-serial$${X}          - Run tests sequentially (for debugging)"; \
	echo "  $${G}test-smoke$${X}           - Quick smoke test (~30s) for rapid iteration"; \
	echo "  $${G}test-all$${X}             - Run ALL tests in parallel (including slow)"; \
	echo "  $${G}test-slow$${X}            - Run only [slow] tagged tests"; \
	echo "  $${G}test-hidden$${X}          - Run the hidden set ([.]) serially from the repo root"; \
	echo "  $${G}test-hidden-list$${X}     - List the hidden set without running it"; \
	echo "  $${G}test-eventloop$${X}       - Run only [eventloop] tests (5-10 min)"; \
	echo "  $${G}test-verbose$${X}         - Run with per-test timing (sequential)"; \
	echo ""; \
	echo "$${C}Component Tests:$${X}"; \
	echo "  $${G}test-gcode$${X}           - G-code parsing and geometry tests"; \
	echo "  $${G}test-ui$${X}              - UI navigation, theme, wizard tests"; \
	echo "  $${G}test-moonraker$${X}       - Moonraker client and mock tests"; \
	echo "  $${G}test-network$${X}         - WiFi and Ethernet tests"; \
	echo "  $${G}test-security$${X}        - Security and injection tests"; \
	echo "  $${G}test-config$${X}          - Configuration tests"; \
	echo "  $${G}test-integration$${X}     - Integration tests (with mocks)"; \
	echo "  $${G}test-xml$${X}             - helix-xml submodule suite (CMake+Unity)"; \
	echo "  $${G}test-shell$${X}           - Shell/installer tests (bats)"; \
	echo "  $${G}test-plugin$${X}          - Moonraker plugin tests (pytest)"; \
	echo ""; \
	echo "$${C}Geometry Tests:$${X}"; \
	echo "  $${G}test-gcode-geometry$${X}  - G-code to 3D geometry test"; \
	echo ""; \
	echo "$${C}Discovery:$${X}"; \
	echo "  $${G}test-list$${X}            - List all test cases"; \
	echo "  $${G}test-list-tags$${X}       - List available test tags"; \
	echo "  $${G}test-timing$${X}          - Show slowest tests (top 20)"; \
	echo "  $${G}test-summary$${X}         - Test coverage by tag"; \
	echo ""; \
	echo "$${C}Sanitizers (Memory/Thread Safety):$${X}"; \
	echo "  $${G}test-asan$${X}            - Run all tests with AddressSanitizer"; \
	echo "  $${G}test-tsan$${X}            - Run all tests with ThreadSanitizer"; \
	echo "  $${G}test-asan-one TEST=X$${X} - Run specific test with ASAN"; \
	echo "  $${G}test-tsan-one TEST=X$${X} - Run specific test with TSAN"; \
	echo ""; \
	echo "$${C}Cleanup:$${X}"; \
	echo "  $${G}clean-tests$${X}          - Remove test build artifacts"; \
	echo "  $${G}clean-sanitizers$${X}     - Remove sanitizer test binaries"
