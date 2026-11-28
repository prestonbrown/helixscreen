# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# spdlog only: no printf/cout/cerr/LV_LOG_ outside CLI subcommands
# ====================================================================
qc_spdlog_only() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "📢 Checking spdlog-only logging..."

# stdout IS the product in these files (CLI subcommands, splash, demo, ctl client),
# so printing there is correct. Everywhere else, logging goes through spdlog.
LOG_ALLOW='src/system/cli_args.cpp|src/application/detect_printer_cmd.cpp|src/application/probe_egl_cmd.cpp|src/application/bench_gcode_cmd.cpp|src/helix_splash.cpp|src/lvgl-demo/|src/remote/remote_client.cpp'
if [ "$STAGED_ONLY" = true ]; then
  # Each staged source as the commit will hold it, under its own name.
  LOG_HITS=""
  while IFS= read -r -d '' qc_f; do
    case "$qc_f" in
      src/*|include/*) ;;
      *) continue ;;
    esac
    qc_file="$(qc_read_path "$qc_f")"
    [ -f "$qc_file" ] || continue
    qc_hits="$(grep -nE '\bprintf\(|std::cout|std::cerr|\bLV_LOG_[A-Z]+\(' "$qc_file" 2>/dev/null \
                 | sed "s|^|$qc_f:|" || true)"
    LOG_HITS="$LOG_HITS$qc_hits
"
  done < <(git diff --cached --name-only -z --diff-filter=ACMRT)
  LOG_HITS="$(printf '%s' "$LOG_HITS" | grep -vE "$LOG_ALLOW" || true)"
else
  LOG_HITS=$(grep -rnE '\bprintf\(|std::cout|std::cerr|\bLV_LOG_[A-Z]+\(' src include 2>/dev/null \
               | grep -vE "$LOG_ALLOW" || true)
fi
if [ -z "$LOG_HITS" ]; then
  section_time $SECTION_START
  echo ""
  echo "✅ spdlog-only: no stray printf/cout/LV_LOG_"
else
  section_time $SECTION_START
  echo ""
  echo "$LOG_HITS"
  echo "❌ Use spdlog::info/debug/warn/error instead (docs/devel/LOGGING.md)."
  echo "   stdout printing belongs only in CLI subcommands."
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_spdlog_only="$QC_TRIGGER_NATIVE_SRC"
