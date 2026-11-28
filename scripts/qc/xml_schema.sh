# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re. Sourced before xml_linter.sh, which
# refreshes the same snapshot through qc_xml_schema_refresh.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# The XML linter's schema snapshot (tools/xml-linter/schema/schema.json)
# ====================================================================
# The linter resolves every #const reference against a committed snapshot. A
# commit whose staged diff adds or removes an input of that snapshot refreshes
# it here and stages it with them: a new <px> committed beside a stale snapshot
# is a red land gate and one more commit at the end of the batch. The lint that
# reads the snapshot needs all of ui_xml/, so it is qc_xml_linter's and runs in
# every full sweep; a reference to a name the snapshot lacks is the sweep's to
# report. A full sweep has nothing staged, and this gate says nothing there.
XML_SCHEMA_PATH="tools/xml-linter/schema/schema.json"

# True when the staged diff adds or removes a C++ input of the snapshot. Both
# patterns mirror extract_schema.py exactly:
#   - _REGISTER_WIDGET_RE / _auto_discover_cpp_widgets() rglob *.cpp under
#     --cpp-src src/ui for lv_xml_register_widget("name", …)
#   - _CPP_REGISTER_CONST_RE / extract_cpp_registered_constants() rglob *.cpp
#     under --cpp-const-dirs src for lv_xml_register_const(scope, "name")
# .cpp only, because that is all the extractor globs: a registration moved to a
# header is invisible to the schema either way. `-U0` leaves only changed lines,
# and no ---/+++ file header can contain either call, so `^[-+]` is enough.
qc_xml_schema_cpp_input_changed() {
  if git diff --cached -U0 -- 'src/ui/*.cpp' | \
     grep -qE '^[-+].*lv_xml_register_widget[[:space:]]*\('; then
    return 0
  fi
  if git diff --cached -U0 -- 'src/*.cpp' | \
     grep -qE '^[-+].*lv_xml_register_const[[:space:]]*\('; then
    return 0
  fi
  return 1
}

# True when a staged path is an input the extractor globs or its own code: the
# engine (lib/helix-xml is a gitlink, so only its pointer is ever staged), the
# theme defaults, the makefile that owns the extractor's arguments, and the
# extractor with the snapshot beside it.
qc_xml_schema_path_input_staged() {
  git diff --cached --name-only --diff-filter=ACMD | \
    grep -qE '^(lib/helix-xml$|mk/tools\.mk$|assets/config/themes/defaults/|tools/xml-linter/schema/)'
}

# A staged edit that removes a const definition. Against a stale snapshot a
# reference to the removed name still resolves, which is the one staleness a
# passing lint cannot rule out.
qc_xml_schema_const_removed() {
  git diff --cached -U0 -- 'ui_xml/*.xml' | \
    grep -qE '^-[[:space:]]*<(px|color|string|int|percentage|font|tiny_ttf|bin|const)[[:space:]][^>]*name='
}

# A staged edit that adds one: the snapshot lists every name, referenced or not,
# and nothing in the lint can tell that a name is missing.
qc_xml_schema_const_added() {
  git diff --cached -U0 -- 'ui_xml/*.xml' | \
    grep -qE '^\+[[:space:]]*<(px|color|string|int|percentage|font|tiny_ttf|bin|const)[[:space:]][^>]*name='
}

qc_xml_schema_input_in_diff() {
  qc_xml_schema_path_input_staged && return 0
  qc_xml_schema_const_removed && return 0
  qc_xml_schema_const_added && return 0
  qc_xml_schema_cpp_input_changed
}

# Regenerates the snapshot and stages it when it is this commit's to stage.
# Sets SCHEMA_WAS_STALE and SCHEMA_STAGED. Returns 1 when regeneration rewrote
# the snapshot and it is left for the committer to commit. A regeneration that
# fails is a warning and never a verdict: a broken extractor must not block
# every commit.
qc_xml_schema_refresh() {
  SCHEMA_WAS_STALE=false
  SCHEMA_STAGED=false
  local dirty_before=false before
  git diff --quiet -- "$XML_SCHEMA_PATH" || dirty_before=true
  # Stale means REGENERATION CHANGED THE BYTES, a different question from
  # "differs from the index". On a tree where the snapshot is regenerated and
  # not staged, the bytes on disk are correct yet differ from the index, and
  # asking the index there calls a correct file stale and blocks the commit on
  # a diagnosis nobody can act on. So the pre-regeneration bytes are kept and
  # the output is compared against those.
  before="$(mktemp "${TMPDIR:-/tmp}/helix_schema_before.XXXXXX")"
  cp "$XML_SCHEMA_PATH" "$before"
  # `make` here, not the raw extractor, keeps mk/tools.mk the single source of
  # truth for the extractor's argument list.
  if ! "$(qc_make)" regen-xml-schema >"$QC_TMP/xml_schema.regen.out" 2>&1; then
    rm -f "$before"
    sed -n '1,10p' "$QC_TMP/xml_schema.regen.out"
    echo "⚠️  Schema regeneration failed — the snapshot on disk is what gets read"
    return 0
  fi
  cmp -s "$before" "$XML_SCHEMA_PATH" || SCHEMA_WAS_STALE=true
  rm -f "$before"

  if [ "$SCHEMA_WAS_STALE" != true ]; then
    if [ "$dirty_before" = true ]; then
      # Nothing to fix, and nothing this hook may stage: those bytes belong to
      # whichever change regenerated them. CI lints the COMMITTED copy, so the
      # committer is told to commit it.
      echo "ℹ️  $XML_SCHEMA_PATH is already up to date but unstaged"
      echo "   CI's XML Lint job lints the committed copy — commit it:"
      echo "   git add $XML_SCHEMA_PATH"
    fi
    return 0
  fi
  # The snapshot is this commit's own collateral, so staging it with the change
  # that made it stale is right, unless it carried unstaged edits: those belong
  # to unrelated work in progress and are not ours to stage.
  if [ "$AUTO_FIX" = true ] && [ "$STAGED_ONLY" = true ] && [ "$dirty_before" = false ] &&
     git add "$XML_SCHEMA_PATH" 2>>"$QC_TMP/xml_schema.regen.out"; then
    SCHEMA_STAGED=true
    echo "   ✓ Regenerated and staged $XML_SCHEMA_PATH"
    return 0
  fi
  echo "⚠️  $XML_SCHEMA_PATH was stale — regenerated in place"
  echo "   Commit it or CI's XML Lint job will fail:"
  echo "   git add $XML_SCHEMA_PATH"
  return 1
}

qc_xml_schema() {
  [ "$STAGED_ONLY" = true ] || return 0
  qc_xml_schema_input_in_diff || return 0
  local SECTION_START rc
  SECTION_START=$(date +%s)
  echo -n "🧩 Refreshing the XML linter's schema snapshot..."
  qc_xml_schema_refresh
  rc=$?
  section_time "$SECTION_START"
  echo ""
  [ "$rc" -ne 0 ] || echo "✅ Schema snapshot is current"
  echo ""
  return $rc
}

# The XML half of the trigger plus every schema input the predicates above
# read: without them here the driver skips the gate before they can see them.
# A src/ .cpp reaches the gate only for its two cheap content greps.
QC_TRIGGER_qc_xml_schema="$QC_TRIGGER_XML"'|^src/.*\.cpp$|^assets/config/themes/defaults/|^lib/helix-xml$|^mk/tools\.mk$'
