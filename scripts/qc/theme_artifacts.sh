# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Generated theme artefacts: token table and [theme_pins] golden
# ====================================================================
# src/generated/theme_token_table.cpp and tests/fixtures/theme_const_golden.txt
# derive from the top-level ui_xml/*.xml tokens. Both are read from the index,
# as the commit will hold them. A commit renders the table from the staged
# tokens and the hook stages that render with them, so a token edit never
# commits a stale table. The golden only the test binary can produce is judged
# here: a change that provably invalidates it fails the commit with the command
# that regenerates it, since a stale one otherwise fails first in the unit
# sweep. ~0.1s. schema.json, the third artefact, is qc_xml_schema's.
qc_theme_artifacts() {
  local EXIT_CODE=0
  local mode="" out="$QC_TMP/theme_artifacts.out" table="$QC_TMP/theme_token_table.cpp"
  local table_path="src/generated/theme_token_table.cpp" table_clean=false
  [ "$STAGED_ONLY" = true ] && mode="--staged-only"
  # Whether the working table holds unstaged work is asked before anything is
  # written: afterwards it differs from the index either way.
  git diff --quiet -- "$table_path" 2>/dev/null && table_clean=true
  if [ "$STAGED_ONLY" = true ] && [ "$AUTO_FIX" = true ]; then
    mode="$mode --write-table $table"
  fi
  # shellcheck disable=SC2086
  if python3 scripts/check_theme_artifacts.py $mode >"$out" 2>&1; then
    qc_count "$(cat "$out")"
  else
    cat "$out"
    EXIT_CODE=1
  fi
  if [ -e "$table" ]; then
    # The index takes the render. The working file gets it too only where it
    # holds nothing unstaged, all that `git add` could have taken.
    if qc_stage_content "$table_path" "$table"; then
      echo "   ✓ Regenerated and staged $table_path"
      if [ "$table_clean" = true ]; then
        cp "$table" "$table_path"
      else
        echo "ℹ️  Regenerated in the index only, the working copy keeps its unstaged edits: $table_path"
      fi
    else
      echo "❌ Could not stage the regenerated $table_path"
      EXIT_CODE=1
    fi
  fi
  echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_theme_artifacts='^ui_xml/[^/]+\.xml$|^src/generated/theme_token_table\.cpp$|^tests/fixtures/theme_const_golden\.txt$|^scripts/(gen_theme_tokens|check_theme_artifacts)\.py$'
