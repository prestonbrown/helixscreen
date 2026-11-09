# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Shell Script Linting (shellcheck)
# ====================================================================
qc_shellcheck() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "🐚 Checking shell scripts (shellcheck)..."

# Two trees, held to two different bars.
#
#   config/  - platform hooks and the init script. Clean at shellcheck's
#              default severity; kept there.
#   scripts/ - installer modules, launcher, release tooling. These ship to
#              devices and are held to the same bar as config/: clean at
#              warning severity (minus the two excluded codes below). The 19
#              files that carried pre-existing findings when this gate landed
#              have since been fixed and SHELLCHECK_BASELINE is empty. A file
#              enters the baseline only by explicit decision after a fix is
#              judged riskier than the finding; the list may shrink, never
#              grow. Variables shared across `source` boundaries carry a
#              per-line disable directive naming their consumer at the
#              assignment site - SC1091 is excluded, so shellcheck cannot see
#              those reads itself.
#
# Two codes are excluded for scripts/:
#   SC3043 - "local is undefined in POSIX sh". Deliberate - the installer and
#            launcher target BusyBox ash, which does implement local.
#   SC1091 - "not following sourced file". The installer sources its modules
#            by a path that only exists once unpacked on the device.
SHELLCHECK_SCRIPTS_EXCLUDE="SC3043,SC1091"
SHELLCHECK_BASELINE=""

# Post-update maintenance scripts that live in config/ because the release zip
# ships them, but are written and held to the installer bar (warning severity,
# `local` allowed for BusyBox ash) rather than the hook/init bar.
SHELLCHECK_CONFIG_SCRIPTS="config/restore-user-config.sh config/refresh-service-units.sh config/fix-user-config-owner.sh"

SHELL_FILES=""
if [ "$STAGED_ONLY" = true ]; then
  SHELL_FILES=$(git diff --cached --name-only --diff-filter=ACM | \
    grep -E '(config/platform/.*\.sh|config/.*\.init|^scripts/.*\.sh|^config/(restore-user-config|refresh-service-units|fix-user-config-owner)\.sh)$' || true)
else
  SHELL_FILES=$(find config/platform -name "*.sh" 2>/dev/null || true)
  # Every init script at the top of config/ ships to devices (helixscreen.init,
  # creality-backend.init); lint all of them, not a hand-kept name list.
  SHELL_FILES="$SHELL_FILES $(find config -maxdepth 1 -name '*.init' 2>/dev/null || true)"
  SHELL_FILES="$SHELL_FILES $(git ls-files 'scripts/*.sh' 'scripts/**/*.sh' 2>/dev/null || true)"
  SHELL_FILES="$SHELL_FILES $SHELLCHECK_CONFIG_SCRIPTS"
fi

if [ -n "$SHELL_FILES" ]; then
  if command -v shellcheck >/dev/null 2>&1; then
    SHELL_ERRORS=0
    SHELL_BASELINED=0
    SHELL_FAILED_FILES=""
    # This was the longest section of a full run at ~8s. The cost is the
    # analysis itself, not process startup - one large script takes ~0.9s on
    # its own, and batching every file into a single invocation only saved 8%
    # because the analyser is single-threaded either way. Fanning the files out
    # across $QC_JOBS takes the section to ~1.6s. Findings are written per file
    # and replayed in list order, so the transcript stays deterministic.
    #
    # Keep comment lines in here from beginning with the word the linter
    # reserves for its own directives - one that does is parsed as a malformed
    # directive and fails the file.
    SC_DIR="$QC_TMP/shellcheck"
    mkdir -p "$SC_DIR"
    printf '%s\n' $SHELL_FILES > "$SC_DIR/files"
    SHELL_TOTAL=$(grep -c . "$SC_DIR/files")
    # scripts/ is linted at warning severity minus the two excluded codes;
    # config/ keeps the stricter default.
    #
    # The list arrives on stdin: reading it with the -a flag is a GNU xargs
    # extension that BSD xargs rejects outright, and the loop below counts findings, not files,
    # so a fan-out that never ran would report every script clean. Every
    # worker leaves a marker (.out, or .skip for a listed file that is not on
    # disk), and the verdict refuses a run that examined fewer files than it
    # was given.
    < "$SC_DIR/files" xargs -P "${QC_JOBS:-4}" -I{} sh -c '
      f="$1"
      out="$2/$(printf "%s" "$f" | tr "/" "_")"
      if [ ! -f "$f" ]; then : > "$out.skip"; exit 0; fi
      case "$f" in
        scripts/*|config/restore-user-config.sh|config/refresh-service-units.sh|config/fix-user-config-owner.sh) flags="-S warning -e $3" ;;
        *)         flags="" ;;
      esac
      shellcheck $flags "$f" > "$out.out" 2>/dev/null || : > "$out.bad"
    ' _ {} "$SC_DIR" "$SHELLCHECK_SCRIPTS_EXCLUDE"
    SHELL_EXAMINED=$(find "$SC_DIR" \( -name '*.out' -o -name '*.skip' \) | wc -l | tr -d ' ')
    for script in $SHELL_FILES; do
      sc_stem="$SC_DIR/$(printf '%s' "$script" | tr '/' '_')"
      [ -f "$sc_stem.bad" ] || continue
      cat "$sc_stem.out"
      if printf '%s\n' "$SHELLCHECK_BASELINE" | grep -Fxq "$script"; then
        SHELL_BASELINED=$((SHELL_BASELINED + 1))
      else
        SHELL_ERRORS=$((SHELL_ERRORS + 1))
        SHELL_FAILED_FILES="$SHELL_FAILED_FILES $script"
      fi
    done
    section_time $SECTION_START
    echo ""
    if [ "$SHELL_EXAMINED" -ne "$SHELL_TOTAL" ]; then
      echo "❌ shellcheck examined $SHELL_EXAMINED of $SHELL_TOTAL shell script(s): the fan-out did not run them"
      echo "   (an xargs that rejects GNU options does this - prestonbrown/helixscreen#1488)"
      EXIT_CODE=1
    elif [ $SHELL_ERRORS -eq 0 ]; then
      if [ $SHELL_BASELINED -gt 0 ]; then
        qc_count "✅ shellcheck clean ($SHELL_BASELINED baselined file(s) still dirty, $SHELL_TOTAL linted)"
      else
        qc_count "✅ All shell scripts pass shellcheck ($SHELL_TOTAL file(s) linted)"
      fi
    else
      echo "❌ shellcheck found issues in $SHELL_ERRORS file(s)"
      for script in $SHELL_FAILED_FILES; do
        echo "   Run: shellcheck $script"
      done
      EXIT_CODE=1
    fi
  else
    section_time $SECTION_START
    echo ""
    echo "⚠️  shellcheck not found - skipping shell script linting"
    echo "   Install with: brew install shellcheck (macOS) or apt install shellcheck (Linux)"
  fi
else
  section_time $SECTION_START
  echo ""
  if [ "$STAGED_ONLY" = true ]; then
    echo "ℹ️  No shell scripts staged for commit"
  else
    echo "ℹ️  No shell scripts found"
  fi
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_shellcheck='\.(sh|bats)$'
