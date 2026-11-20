# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Translation catalog coverage (user-facing strings with no key, or an empty
# value in a locale)
# ====================================================================
qc_translation_coverage() {
  local EXIT_CODE=0
# A user-facing string that never reached translations/*.yml fails silently at
# runtime: lv_translation_get() falls back to the tag, so the string renders in
# English in all nine languages and only a debug-level line says so. One bundle
# carried 1445 of those lines. v0.99.116 was tagged with five such strings on
# main because this gate lived only in tests/shell/test_code_lint.bats, which a
# release runs after quality-checks and which nothing runs pre-commit.
#
# The dry run proves a KEY exists, nothing more: `make translation-sync` writes
# a brand-new key as an EMPTY placeholder, and an empty value renders as empty
# text in that locale (lv_translation_get() only falls back on a MISSING key).
# The empty values are tests/python/test_cpp_translation_coverage.py's and
# test_explicit_tag_coverage.py's: CI's Code Quality job and the land gate's
# python step run both, and gate_select.py runs them for any C++, XML or
# catalog change, so this gate does not run them a second time.
#
# --dry-run is load-bearing: a bare `sync` REWRITES all nine catalogs, and a
# check that edits the tree it is inspecting would stage catalog churn behind
# the committer's back.
SECTION_START=$(date +%s)
echo -n "🌐 Checking translation catalog coverage..."

if [ -x "$VENV_PYTHON" ]; then
  if "$VENV_PYTHON" scripts/translation_sync.py sync --dry-run >/tmp/trans_cov.out 2>&1 \
     && grep -q "All XML strings already in YAML files" /tmp/trans_cov.out; then
    section_time $SECTION_START
    echo ""
    echo "✅ Every user-facing string has a catalog key (empty values: the python step)"
  else
    section_time $SECTION_START
    echo ""
    grep -vE "^[[:space:]]*$" /tmp/trans_cov.out | tail -12
    echo "   Fix: make translation-sync && make translations"
    echo "   Then translate the new keys - consult translations/GLOSSARY.md and"
    echo "   reuse the canonical term rather than coining a new one - and stage"
    echo "   translations/*.yml alongside ui_xml/translations/*.xml."
    echo "   A string that genuinely should not be translated gets"
    echo "   '// i18n: do not translate' on its line or the line above."
    EXIT_CODE=1
  fi
elif [ "$STAGED_ONLY" = true ]; then
  section_time $SECTION_START
  echo ""
  echo "⚠️  .venv not set up — skipping (run 'make venv-setup')"
else
  # The full sweep is the last gate before main (pre-push, CI mode). A skip
  # here reads as green in the hook output while an untranslated lv_tr() key
  # sails through to break Code Quality and the BATS job on the same head
  # (prestonbrown/helixscreen#1507). Only the staged-mode pre-commit pass may
  # treat a missing venv as "not my problem".
  section_time $SECTION_START
  echo ""
  echo "❌ .venv not set up — the translation-coverage gate cannot run"
  echo "   Fix: make venv-setup   (once per clone; the full sweep refuses to guess)"
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

# Any src/ or ui_xml/ file can introduce a user-facing string, so this
# wakes on both trees rather than only on the catalogs they land in.
QC_TRIGGER_qc_translation_coverage='^ui_xml/|^src/|^translations/|^scripts/translation_sync\.py$|^scripts/translations/'
