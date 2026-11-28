# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh. Unlike the other files here this is not
# a gate function: it runs inline, ahead of every gate, and leaves FILES behind
# for qc_phase1 and qc_phase2, which check that same file list.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Copyright (C) 2025-2026 356C LLC
# ====================================================================
SECTION_START=$(date +%s)
echo -n "📝 Checking copyright headers..."

if [ "$STAGED_ONLY" = true ]; then
  # Pre-commit mode: check only staged files (git-ignored files can't be staged)
  # The firmware/ exclusions mirror the ones above it, which exist because a
  # generated or vendored file is not ours to license. Under firmware/ the same
  # three categories just sit at a different prefix: LVGL font-converter output
  # (as in assets/fonts/), vendored lv_conf.h, files vendored from
  # espressif/esp-bsp that carry their own Apache-2.0 SPDX line, and the
  # vendored esp_websocket_client component (Apache-2.0, upstream's formatting,
  # see its VENDORED.md). Stamping
  # GPL-3.0 on any of those would be a false licence claim on third-party code.
  # firmware/native-audit is the Phase 0 feasibility audit, self-described
  # throwaway scaffolding committed only for reproducibility.
  FILES=$(git diff --cached --name-only --diff-filter=ACM | \
    grep -E '\.(cpp|c|h|mm)$' | \
    grep -v '^lib/' | \
    grep -v '^assets/fonts/' | \
    grep -v '/fonts/' | \
    grep -v '^lv_conf\.h$' | \
    grep -v '/lv_conf\.h$' | \
    grep -v '/simd/esp_lvgl_port_' | \
    grep -v '^firmware/helixscreen-esp32/components/esp_websocket_client/' | \
    grep -v '^firmware/native-audit/' | \
    grep -v '^node_modules/' | \
    grep -v '^build/' | \
    grep -v '/\.' || true)
else
  # CI mode: check all files in src/ and include/ (lib/ and assets/fonts/ excluded as auto-generated)
  FILES=$(find src include -name "*.cpp" -o -name "*.c" -o -name "*.h" -o -name "*.mm" 2>/dev/null | \
    grep -v '/\.' | \
    grep -v '^lv_conf\.h$' || true)
fi

if [ -n "$FILES" ]; then
  MISSING_HEADERS=""
  for file in $FILES; do
    # At a commit the staged content; in a full sweep the working tree.
    qc_file="$(qc_read_path "$file")"
    if [ -f "$qc_file" ]; then
      if ! head -3 "$qc_file" | grep -q "SPDX-License-Identifier: GPL-3.0-or-later"; then
        echo "❌ Missing GPL v3 header: $file"
        MISSING_HEADERS="$MISSING_HEADERS $file"
        EXIT_CODE=1
      fi
    fi
  done

  if [ -n "$MISSING_HEADERS" ]; then
    section_time $SECTION_START
    echo ""
    echo "See docs/devel/COPYRIGHT_HEADERS.md for the required header format"
  else
    section_time $SECTION_START
    echo ""
    echo "✅ All source files have proper copyright headers"
  fi
else
  if [ "$STAGED_ONLY" = true ]; then
    section_time $SECTION_START
    echo ""
    echo "ℹ️  No source files staged for commit"
  else
    section_time $SECTION_START
    echo ""
    echo "ℹ️  No source files found"
  fi
fi

echo ""
