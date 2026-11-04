# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helpers every gate in scripts/qc/ may call. Sourced by scripts/quality-checks.sh
# before any gate, and by a bats test before it sources the one gate it drives.
# shellcheck disable=SC2034,SC2154

# Timing helper - prints elapsed time for a section (seconds)
section_time() {
  local start=$1
  local end
  end=$(date +%s)
  local elapsed=$((end - start))
  if [ $elapsed -gt 0 ]; then
    printf " (%ds)" "$elapsed"
  fi
}

# The -j for a make a gate runs. A commit hook runs on every session's commit,
# so an unbounded -j would be N unbounded builds at once: the number comes
# from `helix-claim jobs` (the jobpool when one is live, else the cores capped
# by memory). With the jobpool shim in front of make the -j is stripped and
# the pool decides. 6 is the answer for a tree without the script, and
# HELIX_QC_JOBS overrides both.
qc_build_jobs() {
  printf '%s\n' "${HELIX_QC_JOBS:-$(scripts/helix-claim jobs 2>/dev/null || echo 6)}"
}

# The Makefile requires GNU make >= 4.2 and errors below it. macOS ships 3.81
# as /usr/bin/make and Homebrew installs 4.x only as `gmake`. On Linux plain
# `make` stays: there it may be the jobpool shim, which `gmake` would bypass.
qc_make() {
  if [ "$(uname -s)" = Darwin ] && command -v gmake >/dev/null 2>&1; then
    echo gmake
  else
    echo make
  fi
}

# One src/ or tests/ source that includes HEADER (path as staged), or nothing.
# A header breaks the units that include it, and one of them compiling is the
# commit-time question; pre-push's clang gate checks the rest.
qc_one_includer() {
  local base
  base=$(basename "$1")
  grep -rlE --include='*.cpp' --include='*.cc' --include='*.mm' \
    "#include[[:space:]]+[\"<]([^\">]*/)?${base//./\\.}[\">]" src tests 2>/dev/null | head -1
}

# A verdict that carries a count is recorded in $QC_COUNTS as well as printed,
# so a cached pass can replay what the full run examined.
qc_note() { printf '%s\n' "$1" >> "$QC_COUNTS" 2>/dev/null || true; }
qc_count() { echo "$1"; qc_note "$1"; }

# Path triggers more than one gate shares (see QC_TRIGGER_<gate> in each file).
QC_TRIGGER_XML='\.xml$|^src/ui/|^tools/xml-linter/'
QC_TRIGGER_NATIVE_SRC='\.(cpp|c|h|mm)$'
QC_TRIGGER_ICONS='\.xml$|icon|font'

# The most units one commit compiles. A move that rewrites an #include line stages
# every includer, hundreds of sources, and each costs about a second of CPU; 50 keep
# such a commit to seconds, and land-gate's build compiles every one regardless.
QC_COMPILE_CAP=50
# Up to this many units compile one at a time with no pool token, so an ordinary
# commit never waits on a busy pool.
QC_COMPILE_SERIAL_MAX=4

# What a commit compiles from its staged paths (one per line): each staged source,
# and one source including each staged header. Sets CHECK_UNITS (sorted, one per
# line), QC_INCLUDER_UNITS (the includers among them) and UNREACHED_HDR (the staged
# headers no source includes directly, which pre-push's clang gate still reaches
# through their dependents).
qc_staged_compile_units() {
  local staged="$1" hdr unit
  CHECK_UNITS=$(printf '%s\n' "$staged" | grep -E '\.(cpp|cc|cxx|mm)$' || true)
  UNREACHED_HDR=""
  QC_INCLUDER_UNITS=""
  COMPILE_TOTAL=0
  COMPILE_SKIPPED=0
  for hdr in $(printf '%s\n' "$staged" | grep -E '\.(h|hh|hpp|hxx|inc)$' || true); do
    unit=$(qc_one_includer "$hdr")
    if [ -n "$unit" ]; then
      CHECK_UNITS="$CHECK_UNITS
$unit"
      QC_INCLUDER_UNITS="$QC_INCLUDER_UNITS
$unit"
    else
      UNREACHED_HDR="$UNREACHED_HDR $hdr"
    fi
  done
  CHECK_UNITS=$(printf '%s\n' "$CHECK_UNITS" | sed '/^$/d' | sort -u)
}

# Narrows CHECK_UNITS to CAP units when it holds more: every header includer, since
# a header breaks OTHER units, then an even stride through the rest of the sorted
# list. The stride spreads the sample across the paths and the same staging picks the
# same units; a directory with fewer units than one stride can get none.
# Includers alone may exceed CAP. Sets COMPILE_TOTAL and COMPILE_SKIPPED.
qc_sample_compile_units() {
  local cap="$1"
  COMPILE_TOTAL=$(printf '%s\n' "$CHECK_UNITS" | grep -c . || true)
  COMPILE_SKIPPED=0
  [ "$COMPILE_TOTAL" -gt "$cap" ] || return 0
  # The includers travel through the environment: BSD awk rejects a newline in -v.
  CHECK_UNITS=$(printf '%s\n' "$CHECK_UNITS" | QC_KEEP="$QC_INCLUDER_UNITS" awk -v cap="$cap" '
    BEGIN { split(ENVIRON["QC_KEEP"], k, "\n"); for (i in k) kept[k[i]] = 1 }
    ($0 in kept) { print; nk++; next }
    { rest[++n] = $0 }
    END { q = cap - nk; for (i = 0; i < q; i++) print rest[int(i * n / q) + 1] }
  ' | sort -u)
  COMPILE_SKIPPED=$((COMPILE_TOTAL - $(printf '%s\n' "$CHECK_UNITS" | grep -c .)))
}

# Sets QC_SAMPLE_NOTE to what a sampled commit left uncompiled, or empty, and leaves
# a copy for the driver to repeat beside the verdict, where a truncated log still
# shows it.
qc_compile_sample_note() {
  QC_SAMPLE_NOTE=""
  [ "$COMPILE_SKIPPED" -gt 0 ] || return 0
  QC_SAMPLE_NOTE="⚠️  Wide commit: compiled $((COMPILE_TOTAL - COMPILE_SKIPPED)) of $COMPILE_TOTAL units (cap $QC_COMPILE_CAP). $COMPILE_SKIPPED were NOT compiled here; land-gate's build and pre-push's clang gate cover them. HELIX_QC_ALL_AT_COMMIT=1 compiles them now."
  printf '%s\n' "$QC_SAMPLE_NOTE" > "$QC_TMP/compile_sample.note"
}

# Runs syntax_check.py over its arguments. A few units run exactly as a direct call
# would. More run in parallel under a pool hold: `helix-claim jobs` is only the upper
# bound (cores capped by memory, or the whole pool), and JOBPOOL_SLOTS is what this
# run was actually granted. --min 1 starts it on whatever is free: hold's default
# floor can stall a commit for its whole bounded wait.
qc_syntax_check() {
  local slots
  if [ "$#" -le "$QC_COMPILE_SERIAL_MAX" ]; then
    python3 scripts/syntax_check.py "$@"
    return
  fi
  slots=$(qc_build_jobs)
  [ "$slots" -le "$#" ] || slots=$#
  scripts/helix-claim hold -n "$slots" --min 1 -- \
    sh -c 'exec python3 scripts/syntax_check.py --jobs "$JOBPOOL_SLOTS" "$@"' _ "$@"
}
