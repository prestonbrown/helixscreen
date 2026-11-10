# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Hidden test set (make test-hidden)
# ====================================================================
qc_hidden_tests() {
  local EXIT_CODE=0
# The 89 [.]-tagged tests are excluded from `make full-test-run`: they need ui_xml/
# on a relative path, own destructive global state, or are timing-sensitive
# stress harnesses. Nothing else runs them, which is how six of them rotted red
# without anyone noticing. See docs/devel/HIDDEN_TESTS_TRACKER.md.
#
# Kept off the hot path the same two ways the helix-xml block above is:
#   - Triggers only on staged code. A docs/installer/asset commit skips.
#   - NEVER builds. A cold test build is ten minutes, which must not land on a
#     commit hook, so this runs only when the test binary is already current
#     (`make -q`) — i.e. the author has built tests anyway and the marginal
#     cost is the ~65s run itself. Stale or absent binary skips with an
#     instruction. That also means CI mode skips: the Code Quality runner
#     never builds the binary. The hidden set belongs in nightly there.
# When it does run, a failure blocks like any other test gate.
SECTION_START=$(date +%s)
echo -n "🙈 Checking hidden test set..."

if [ "$STAGED_ONLY" = true ]; then
  HIDDEN_TRIGGERS=$(git diff --cached --name-only --diff-filter=ACM | \
    grep -E '^(src/|include/|ui_xml/|tests/)' || true)
else
  # CI mode has no staged set; the up-to-date check below is what keeps this
  # from triggering a ten-minute build on a runner that has no test binary.
  HIDDEN_TRIGGERS="all"
fi

section_time $SECTION_START
echo ""

if [ -z "$HIDDEN_TRIGGERS" ]; then
  echo "ℹ️  No src/include/ui_xml/tests changes staged — skipping hidden tests"
elif [ ! -x "build/bin/helix-tests" ]; then
  echo "⚠️  build/bin/helix-tests not built — skipping hidden tests"
  echo "   Run 'make test-hidden' by hand to enable this gate."
elif ! HIDDEN_STALE=$(scripts/check_test_binary_current.sh); then
  echo "⚠️  Test binary is stale — skipping hidden tests"
  echo "   Behind: $(sed -n '1,3p' <<<"$HIDDEN_STALE" | tr '\n' ' ')"
  echo "   A test build is too slow for a commit hook. Run: make test-hidden"
else
  SECTION_START=$(date +%s)
  # The binary runs directly here: `make test-hidden` carries a test-build
  # prerequisite that would relink, and this block never builds. It runs under
  # a timeout so a hung test cannot freeze the commit (SIGKILL backstop as in
  # mk/tests.mk#timeout_run).
  local HIDDEN_TIMEOUT=() TIMEOUT_BIN HIDDEN_RC
  TIMEOUT_BIN=$(command -v timeout || command -v gtimeout || true)
  [ -n "$TIMEOUT_BIN" ] && HIDDEN_TIMEOUT=("$TIMEOUT_BIN" -k "${TIMEOUT_KILL_AFTER:-30}" "${TEST_TIMEOUT:-900}")
  HIDDEN_RC=0
  "${HIDDEN_TIMEOUT[@]}" build/bin/helix-tests "[.]" >/tmp/test_hidden.out 2>&1 || HIDDEN_RC=$?
  if [ "$HIDDEN_RC" -eq 0 ]; then
    printf "✅ Hidden tests passed (%s)" "$(grep -E '^test cases:' /tmp/test_hidden.out | tail -1)"
    section_time $SECTION_START
    echo ""
  else
    grep -E '^(tests/|  |test cases:|assertions:)' /tmp/test_hidden.out | tail -40
    if [ "$HIDDEN_RC" -eq 124 ] || [ "$HIDDEN_RC" -eq 137 ]; then
      echo "❌ Hidden tests were killed after ${TEST_TIMEOUT:-900}s: a hung test, or a deadlocked crash handler"
    fi
    echo "❌ Hidden tests failed"
    echo "   Run: make test-hidden"
    EXIT_CODE=1
  fi
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_hidden_tests='^tests/|\.(cpp|h)$'
