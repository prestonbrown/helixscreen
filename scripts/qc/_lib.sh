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

# Exit status of a checker whose only findings are count ratchets
# (scripts/ratchet.py WARN_EXIT), such as hardcoded pixels and unreferenced
# callbacks. They warn over their baseline, pass under it, and block nothing;
# scripts/ratchet_report.py shows their trend.
QC_RATCHET_WARN_EXIT=3

# qc_ratchet_verdict RC OUT: the verdict on a checker that exited RC with its
# output in OUT. A pass prints OUT's last line, a ratchet warning prints all of
# OUT and passes, anything else prints OUT and fails.
qc_ratchet_verdict() {
  case "$1" in
    0) tail -1 "$2" ;;
    "$QC_RATCHET_WARN_EXIT")
      cat "$2"
      echo "   ⚠️  A ratchet warning blocks nothing; scripts/ratchet_report.py shows the weekly trend." ;;
    *) cat "$2"; return 1 ;;
  esac
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
# question HELIX_QC_ALL_AT_COMMIT=1 asks; pre-push's clang gate checks the rest.
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

# Under --staged-only the driver copies each staged file's content, as the
# commit will hold it, to QC_STAGED_DIR/<repository path>. A gate reads a staged
# file through qc_read_path, never through the working tree: the two differ
# whenever a file is partly staged or edited after it was staged, and the commit
# is what is staged. Empty in a full sweep, where qc_read_path names the file.
QC_STAGED_DIR=""

qc_export_staged() {
  # $1 = the directory to fill, every staged file under its repository path
  mkdir -p "$1" || return 1
  # A staged submodule pointer becomes an empty directory there; nothing reads it.
  git diff --cached --name-only -z --diff-filter=ACMRT |
    xargs -0 -r git checkout-index --prefix="$1/" -- >/dev/null 2>&1 || true
}

qc_read_path() {
  # $1 = a repository path. Prints the file a gate reads for it.
  if [ -n "$QC_STAGED_DIR" ]; then
    printf '%s/%s\n' "$QC_STAGED_DIR" "$1"
  else
    printf '%s\n' "$1"
  fi
}

# Every file the index holds under the pathspecs, written to $1 under its
# repository path: a gate that needs whole directories as the commit will hold
# them reads them there. Named in hundreds of files, not thousands: the whole
# index takes about a second. Whatever $1 held goes first: checkout-index never
# overwrites a file, so a file the index no longer has would linger.
qc_export_index_tree() {
  local dir="$1"
  shift
  rm -rf "$dir" && mkdir -p "$dir" || return 1
  git ls-files -z -- "$@" | xargs -0 -r git checkout-index --prefix="$dir/" -- >/dev/null 2>&1 || true
}

# The one way a gate writes the index. Git refuses its index lock instead of
# waiting for it, and gates fan out in parallel: more than one of them repairs
# what is staged, and a reader such as `git diff` takes the same lock for a
# moment to refresh stat data. A write that meets the lock is tried again until
# it goes through, for up to about thirty seconds; then git's own message is
# the verdict. The refusal is read by its wording, because the lock is not
# always index.lock: `git commit -- <paths>` hands its hook a temporary index,
# next-index-<pid>.lock, whose lock ends in .lock.lock.
qc_index_write() {
  local attempt=0 rc err
  err="$(mktemp "${TMPDIR:-/tmp}/helix_index_write.XXXXXX")" || return 1
  while :; do
    attempt=$((attempt + 1))
    "$@" 2>"$err"
    rc=$?
    if [ "$rc" -eq 0 ] || [ "$attempt" -ge 300 ]; then break; fi
    grep -qE "Unable to create '.*\.lock': File exists" "$err" || break
    sleep 0.1
  done
  cat "$err" >&2
  rm -f "$err"
  return $rc
}

# Under --staged-only, makes the content of $2 the staged content of repository
# path $1 and leaves the working tree alone. A repair of what is staged goes
# through here: `git add` would take the whole working file, hunks its author
# held back included. $1 has to be staged already.
qc_stage_content() {
  local mode blob
  mode="$(git ls-files -s -- "$1" | awk 'NR == 1 { print $1 }')"
  [ -n "$mode" ] || return 1
  blob="$(git hash-object -w --path="$1" -- "$2")" || return 1
  qc_index_write git update-index --cacheinfo "$mode,$blob,$1"
}

# xmllint --noout --recover on the content a gate reads for $1, its findings
# named under the repository path: read from standard input, xmllint calls
# the file "-".
qc_xmllint() {
  local read_file
  read_file="$(qc_read_path "$1")"
  if [ "$read_file" = "$1" ]; then
    xmllint --noout --recover "$1" 2>&1
  else
    xmllint --noout --recover - < "$read_file" 2>&1 | sed "s|^-:|$1:|"
  fi
}

# Path triggers more than one gate shares (see QC_TRIGGER_<gate> in each file).
QC_TRIGGER_XML='\.xml$|^src/ui/|^tools/xml-linter/'
QC_TRIGGER_NATIVE_SRC='\.(cpp|c|h|mm)$'
QC_TRIGGER_ICONS='\.xml$|icon|font'

# Up to this many units compile one at a time.
QC_COMPILE_SERIAL_MAX=4
# How many compile at once above that. The commit hook stays out of the jobpool: a
# running make keeps every token it holds until it has no work left, so a hook
# queued behind one can wait as long as the build does.
QC_COMPILE_JOBS=4

# What the compile check takes from the staged paths (one per line): each staged
# source, and one source including each staged header. Sets CHECK_UNITS (sorted, one
# per line) and UNREACHED_HDR (the staged headers no source includes directly, which
# pre-push's clang gate still reaches through their dependents).
qc_staged_compile_units() {
  local staged="$1" hdr unit
  CHECK_UNITS=$(printf '%s\n' "$staged" | grep -E '\.(cpp|cc|cxx|mm)$' || true)
  UNREACHED_HDR=""
  for hdr in $(printf '%s\n' "$staged" | grep -E '\.(h|hh|hpp|hxx|inc)$' || true); do
    unit=$(qc_one_includer "$hdr")
    if [ -n "$unit" ]; then
      CHECK_UNITS="$CHECK_UNITS
$unit"
    else
      UNREACHED_HDR="$UNREACHED_HDR $hdr"
    fi
  done
  CHECK_UNITS=$(printf '%s\n' "$CHECK_UNITS" | sed '/^$/d' | sort -u)
}

# Runs syntax_check.py over its arguments, in parallel past QC_COMPILE_SERIAL_MAX.
qc_syntax_check() {
  if [ "$#" -le "$QC_COMPILE_SERIAL_MAX" ]; then
    python3 scripts/syntax_check.py "$@"
    return
  fi
  python3 scripts/syntax_check.py --jobs "$QC_COMPILE_JOBS" "$@"
}
