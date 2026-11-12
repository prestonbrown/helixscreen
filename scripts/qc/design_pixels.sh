# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Spacing and sizing go through design tokens, not raw pixel literals
# ====================================================================
qc_design_pixels() {
  local EXIT_CODE=0
# HelixScreen ships on 480x272 through 1440p. A literal style_pad_all="12" is
# 12px on all of them, so a layout tuned in a 1024x600 dev window is cramped on
# a Snapmaker U1 and lost in whitespace on a 1280x720 panel. The ladders in
# ui_xml/globals.xml resolve per breakpoint; a literal freezes one column of the
# ladder forever.
#
# Ratcheting baseline. The remaining sites are real debt — mostly 2px hairline
# gaps and two negative overlap margins with no token to name them. The number
# may go DOWN (convert a site, then lower this baseline) but must never go up.
# A reasoned exception is annotated SIZE_OK, the way ui_xml/color_picker.xml
# walks its swatch-grid content floor.
echo "📏 Checking design-token usage (hardcoded pixels)..."

# Pre-commit: scan the post-commit tree (index + HEAD), not the dirty working
# tree — so another session's unstaged WIP cannot trip the ratchet on a clean
# commit. CI and manual runs use the whole-working-tree scan (no flag).
if [ "$STAGED_ONLY" = true ]; then
  PIXELS_ARGS="--staged-only"
else
  PIXELS_ARGS=""
fi
# shellcheck disable=SC2086
if python3 scripts/check_hardcoded_pixels.py --max-allowed 121 --summary $PIXELS_ARGS \
    >/tmp/hardcoded_pixels.out 2>&1; then
  tail -1 /tmp/hardcoded_pixels.out
else
  cat /tmp/hardcoded_pixels.out
  echo "   Run: python3 scripts/check_hardcoded_pixels.py --list"
  echo "   Use a token; see .claude/rules/declarative-ui.md § Design Tokens."
  EXIT_CODE=1
fi

echo ""

echo "📏 Checking responsive token placement..."

# theme_manager_find_xml_files() skips subdirectories, so a responsive token
# declared below the top level of ui_xml/ is never registered and every #token
# reading it resolves to nothing, silently (prestonbrown/helixscreen#1211).
# Always whole-tree: the scan is a regex over ~330 small files, and the rule is
# about where a file SITS, so a staged-only view buys nothing.
if python3 scripts/check_responsive_token_scope.py >/tmp/responsive_token_scope.out 2>&1; then
  echo "✅ Responsive tokens are all top-level"
else
  cat /tmp/responsive_token_scope.out
  EXIT_CODE=1
fi

echo ""

echo "📐 Checking modal chrome budget..."

# #dialog_content_max is sized for ONE chrome shape: header + content + divider
# + button row. A modal that pins an extra block below the scroll area overruns
# the 85% card cap, and because the root is height="content" + scrollable=false
# the overflow falls off the BOTTOM — the button row, leaving a modal the user
# cannot dismiss (prestonbrown/helixscreen#1277). LVGL cannot rescue this in
# layout: lv_flex.c has grow but no shrink. Whole-tree: the rule is about a
# file's own element order, so a staged-only view would miss a modal whose
# budget was broken by an edit to a component it embeds.
if python3 scripts/check_modal_chrome_budget.py >/tmp/modal_chrome_budget.out 2>&1; then
  echo "✅ Modal chrome budget: every pinned block is accounted for"
else
  cat /tmp/modal_chrome_budget.out
  EXIT_CODE=1
fi

echo ""

echo "📜 Checking panel-widget scroll declarations..."

# <lv_obj> keeps LVGL's LV_OBJ_FLAG_SCROLLABLE default, which is ON. Our theme
# overrides lv_obj's size/border/background/padding but NOT scrollable, so an
# author who reads it as a pure layout container gets a scroll container. That
# draws chevrons over the print-status thumbnail on an 800x480 K-Touch, and
# inside a drag-scrolled home grid it also steals the drag.
#
# Ratcheting baseline. The rule is declared INTENT - scrollable="true" passes
# just as well as "false"; only saying nothing fails. The remaining 21 sites are
# not fixed in bulk on purpose: each needs its author's intent, and some really
# should scroll. The number may go DOWN, never up.
# Pre-commit: scan the post-commit tree (index + HEAD), not the dirty working
# tree - so another session's unstaged WIP cannot trip the ratchet on a clean
# commit. CI and manual runs use the whole-working-tree scan (no flag).
if [ "$STAGED_ONLY" = true ]; then
  PW_SCROLLABLE_ARGS="--staged-only"
else
  PW_SCROLLABLE_ARGS=""
fi
# shellcheck disable=SC2086
if python3 scripts/check_panel_widget_scrollable.py --max-allowed 5 --summary $PW_SCROLLABLE_ARGS \
    >/tmp/panel_widget_scrollable.out 2>&1; then
  tail -1 /tmp/panel_widget_scrollable.out
else
  cat /tmp/panel_widget_scrollable.out
  echo "   Run: python3 scripts/check_panel_widget_scrollable.py --list"
  EXIT_CODE=1
fi

echo ""

# android/app/src/main/assets/ is a Gradle build output (the copyAssets task
# wipes and re-copies it from ui_xml/, assets/ and config/). It is ignored
# wholesale, so a snapshot from an old build lingers on disk looking exactly like
# source: one went 4 months stale and cost four separate lint gates a
# hand-written exclusion apiece. This fails on a tracked file under that tree, or
# on a build rule writing into it behind Gradle's back (mk/filaments.mk did).
if python3 scripts/check_android_asset_staging.py >/tmp/android_staging.out 2>&1; then
  cat /tmp/android_staging.out
else
  cat /tmp/android_staging.out
  echo "   Run: python3 scripts/check_android_asset_staging.py --list"
  EXIT_CODE=1
fi

# A printer_database.json entry naming an image that does not exist is silent at
# runtime: the lookup falls through to generic-corexy and logs nothing above debug,
# so a bed-slinger just quietly shows a CoreXY frame. Twenty entries had drifted
# that way before anyone noticed.
if python3 scripts/check_printer_images.py >/tmp/printer_images.out 2>&1; then
  cat /tmp/printer_images.out
else
  cat /tmp/printer_images.out
  EXIT_CODE=1
fi

# Transparent margin on printer art is width the home widget's contain-fit spends
# on nothing, which can push its callout chips off their leader lines. A commit
# runs it only when it stages printer art or the script.
if [ "$STAGED_ONLY" = true ] &&
  ! printf '%s\n' "$QC_STAGED_ALL" | grep -qE '^assets/images/printers/|^scripts/trim_printer_images\.py$'; then
  :
elif python3 -c "import PIL" 2>/dev/null; then
  TRIM_OUT="$(mktemp)"
  if ! python3 scripts/trim_printer_images.py --check >"$TRIM_OUT" 2>&1; then
    cat "$TRIM_OUT"
    EXIT_CODE=1
  fi
  rm -f "$TRIM_OUT"
else
  echo "⚠️  Pillow not installed — skipping printer image trim check"
fi

# An async pytest case whose plugin is not in requirements.txt does not read as a
# missing dependency: plain pytest collects it and fails it with "async def
# functions are not natively supported", so CI shows N broken tests instead. That
# is how the moonraker-plugin suite went red for a day while passing locally on a
# .venv that had pytest-asyncio installed by hand. The gate also catches the
# mirror case — an unmarked async test, which strict mode SKIPS silently.
# Font tier coverage: the C++ font guards are `#if HELIX_MAX_FONT_TIER >= N`
# (a threshold) while mk/fonts.mk selects sources from the declared FONT_TIERS
# (a set), and cross.mk derives MAX from the highest declared tier. A platform
# that skips a middle tier makes those disagree and fails to link -- k2 declares
# "large xlarge", so `>= 3` compiles a reference to noto_sans_26 that its
# sources would not contain. Invisible on x86 (all guards true) and only the
# release matrix cross-builds k2, so it would surface long after the commit.
echo ""
echo "${BOLD}🔠 Checking font tier coverage...${RESET}"
if python3 scripts/check_font_tier_coverage.py >/tmp/font_tier_coverage.out 2>&1; then
  cat /tmp/font_tier_coverage.out
else
  cat /tmp/font_tier_coverage.out
  EXIT_CODE=1
fi

if python3 scripts/check_pytest_asyncio_deps.py >/tmp/pytest_asyncio_deps.out 2>&1; then
  cat /tmp/pytest_asyncio_deps.out
else
  cat /tmp/pytest_asyncio_deps.out
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_design_pixels='\.xml$'
