# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# XML event callbacks against their C++ registrations
# ====================================================================
qc_orphan_callbacks() {
  local out="$QC_TMP/orphan_callbacks.out" root=. rc=0
  SECTION_START=$(date +%s)
  echo -n "🔌 Checking XML event callbacks against their registrations..."
  # A ratchet keyed on names: an XML callback nothing registers, a registered
  # name no XML uses, and a name registered from two files (the table is
  # last-write-wins). scripts/orphan_callback_baseline.txt is accepted debt.
  # A new unregistered or duplicate name fails; a new unreferenced one is dead
  # code and only warns. A commit reads the index, not the working tree, so a
  # peer's unstaged edits in a shared tree cannot fail it.
  if [ "$STAGED_ONLY" = true ]; then
    root="$QC_TMP/orphan_callbacks.index"
    mkdir -p "$root"
    git ls-files -z -- src include ui_xml scripts/orphan_callback_baseline.txt |
      git checkout-index -z --stdin --prefix="$root/"
  fi
  python3 scripts/check_orphan_callbacks.py --repo-root "$root" \
    --baseline "$root/scripts/orphan_callback_baseline.txt" >"$out" 2>&1 || rc=$?
  section_time "$SECTION_START"
  echo ""
  if ! qc_ratchet_verdict "$rc" "$out"; then
    echo "   Run: python3 scripts/check_orphan_callbacks.py --list"
    echo ""
    return 1
  fi
  echo ""
  return 0
}

QC_TRIGGER_qc_orphan_callbacks='^ui_xml/.*\.xml$|^(src|include)/.*\.(cpp|h)$|^scripts/(check_orphan_callbacks\.py|orphan_callback_baseline\.txt)$'
