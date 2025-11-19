#!/bin/bash
# SPDX-FileCopyrightText: 2024 Patrick Brown <opensource@pbdigital.org>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Quality checks script - single source of truth for pre-commit and CI
# Usage:
#   ./scripts/quality-checks.sh           # Check all files (for CI)
#   ./scripts/quality-checks.sh --staged-only  # Check only staged files (for pre-commit)

set -e

# Parse arguments
STAGED_ONLY=false
if [ "$1" = "--staged-only" ]; then
  STAGED_ONLY=true
fi

# Change to repo root
REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$REPO_ROOT"

EXIT_CODE=0

echo "🔍 Running quality checks..."
if [ "$STAGED_ONLY" = true ]; then
  echo "   Mode: Staged files only (pre-commit)"
else
  echo "   Mode: All files (CI)"
fi
echo ""

# ====================================================================
# Copyright Header Check
# ====================================================================
echo "📝 Checking copyright headers..."

if [ "$STAGED_ONLY" = true ]; then
  # Pre-commit mode: check only staged files
  FILES=$(git diff --cached --name-only --diff-filter=ACM | \
    grep -E '\.(cpp|c|h|mm)$' | \
    grep -v '^libhv/' | \
    grep -v '^lvgl/' | \
    grep -v '^lv_conf\.h$' | \
    grep -v '^spdlog/' | \
    grep -v '^wpa_supplicant/' | \
    grep -v '^node_modules/' | \
    grep -v '^build/' | \
    grep -v '/\.' || true)
else
  # CI mode: check all files in src/ and include/
  FILES=$(find src include -name "*.cpp" -o -name "*.c" -o -name "*.h" -o -name "*.mm" 2>/dev/null | \
    grep -v '/\.' | \
    grep -v '^lv_conf\.h$' || true)
fi

if [ -n "$FILES" ]; then
  MISSING_HEADERS=""
  for file in $FILES; do
    if [ -f "$file" ]; then
      if ! head -3 "$file" | grep -q "SPDX-License-Identifier: GPL-3.0-or-later"; then
        echo "❌ Missing GPL v3 header: $file"
        MISSING_HEADERS="$MISSING_HEADERS $file"
        EXIT_CODE=1
      fi
    fi
  done

  if [ -n "$MISSING_HEADERS" ]; then
    echo ""
    echo "See docs/COPYRIGHT_HEADERS.md for the required header format"
  else
    echo "✅ All source files have proper copyright headers"
  fi
else
  if [ "$STAGED_ONLY" = true ]; then
    echo "ℹ️  No source files staged for commit"
  else
    echo "ℹ️  No source files found"
  fi
fi

echo ""

# ====================================================================
# Phase 1: Critical Checks
# ====================================================================

# Merge Conflict Markers Check
echo "⚠️  Checking for merge conflict markers..."
if [ -n "$FILES" ]; then
  CONFLICT_FILES=$(echo "$FILES" | xargs grep -l "^<<<<<<< \|^=======$\|^>>>>>>> " 2>/dev/null || true)
  if [ -n "$CONFLICT_FILES" ]; then
    echo "❌ Merge conflict markers found in:"
    echo "$CONFLICT_FILES" | sed 's/^/   /'
    EXIT_CODE=1
  else
    echo "✅ No merge conflict markers"
  fi
else
  echo "ℹ️  No files to check"
fi

echo ""

# Trailing Whitespace Check
echo "🧹 Checking for trailing whitespace..."
if [ -n "$FILES" ]; then
  TRAILING_WS=$(echo "$FILES" | xargs grep -n "[[:space:]]$" 2>/dev/null || true)
  if [ -n "$TRAILING_WS" ]; then
    echo "⚠️  Found trailing whitespace:"
    echo "$TRAILING_WS" | head -10 | sed 's/^/   /'
    if [ $(echo "$TRAILING_WS" | wc -l) -gt 10 ]; then
      echo "   ... and $(($(echo "$TRAILING_WS" | wc -l) - 10)) more"
    fi
    echo "ℹ️  Fix with: sed -i 's/[[:space:]]*$//' <file>"
  else
    echo "✅ No trailing whitespace"
  fi
else
  echo "ℹ️  No files to check"
fi

echo ""

# XML Validation
echo "📄 Validating XML files..."
if [ "$STAGED_ONLY" = true ]; then
  XML_FILES=$(git diff --cached --name-only --diff-filter=ACM | grep "\.xml$" || true)
else
  XML_FILES=$(find ui_xml -name "*.xml" 2>/dev/null || true)
fi

if [ -n "$XML_FILES" ]; then
  if command -v xmllint >/dev/null 2>&1; then
    XML_ERRORS=0
    for xml in $XML_FILES; do
      if [ -f "$xml" ]; then
        if ! xmllint --noout "$xml" 2>&1; then
          echo "❌ Invalid XML: $xml"
          XML_ERRORS=$((XML_ERRORS + 1))
          EXIT_CODE=1
        fi
      fi
    done
    if [ $XML_ERRORS -eq 0 ]; then
      echo "✅ All XML files are valid"
    fi
  else
    echo "⚠️  xmllint not found - skipping XML validation"
    echo "   Install with: brew install libxml2 (macOS) or apt install libxml2-utils (Linux)"
  fi
else
  echo "ℹ️  No XML files to validate"
fi

echo ""

# ====================================================================
# Phase 2: Code Quality Checks
# ====================================================================

# Code Formatting Check (clang-format)
echo "🎨 Checking code formatting..."
if [ -n "$FILES" ]; then
  if command -v clang-format >/dev/null 2>&1; then
    if [ -f ".clang-format" ]; then
      FORMAT_ISSUES=""
      for file in $FILES; do
        if [ -f "$file" ]; then
          # Check if file needs formatting
          FORMATTED=$(clang-format "$file")
          ORIGINAL=$(cat "$file")
          if [ "$FORMATTED" != "$ORIGINAL" ]; then
            FORMAT_ISSUES="$FORMAT_ISSUES $file"
          fi
        fi
      done

      if [ -n "$FORMAT_ISSUES" ]; then
        echo "⚠️  Files need formatting:"
        echo "$FORMAT_ISSUES" | tr ' ' '\n' | grep -v '^$' | sed 's/^/   /'
        echo "ℹ️  Fix with: clang-format -i <file>"
        echo "ℹ️  Or run: make format (if available)"
      else
        echo "✅ All files properly formatted"
      fi
    else
      echo "ℹ️  No .clang-format file found - skipping format check"
    fi
  else
    echo "ℹ️  clang-format not found - skipping format check"
    echo "   Install with: brew install clang-format (macOS) or apt install clang-format (Linux)"
  fi
else
  echo "ℹ️  No files to check"
fi

echo ""

# XML Formatting Check
echo "📐 Checking XML formatting..."
if [ "$STAGED_ONLY" = true ]; then
  XML_FILES=$(git diff --cached --name-only --diff-filter=ACM | grep "\.xml$" || true)
else
  XML_FILES=$(find ui_xml -name "*.xml" 2>/dev/null || true)
fi

if [ -n "$XML_FILES" ]; then
  if command -v xmllint >/dev/null 2>&1; then
    FORMAT_ISSUES=""
    for file in $XML_FILES; do
      if [ -f "$file" ]; then
        # Check if file needs formatting (xmllint --format for consistent indentation)
        FORMATTED=$(xmllint --format "$file" 2>/dev/null || echo "PARSE_ERROR")
        if [ "$FORMATTED" = "PARSE_ERROR" ]; then
          echo "⚠️  Cannot format $file (may have XML errors)"
        else
          ORIGINAL=$(cat "$file")
          if [ "$FORMATTED" != "$ORIGINAL" ]; then
            FORMAT_ISSUES="$FORMAT_ISSUES $file"
          fi
        fi
      fi
    done

    if [ -n "$FORMAT_ISSUES" ]; then
      echo "⚠️  XML files need formatting:"
      echo "$FORMAT_ISSUES" | tr ' ' '\n' | grep -v '^$' | sed 's/^/   /'
      echo "ℹ️  Fix with: xmllint --format -o <file> <file>"
      echo "ℹ️  Or run: make format"
    else
      echo "✅ All XML files properly formatted"
    fi
  else
    echo "ℹ️  xmllint not found - skipping XML format check"
  fi
else
  echo "ℹ️  No XML files to check"
fi

echo ""

# Build Verification
if [ "$STAGED_ONLY" = true ]; then
  echo "🔨 Verifying incremental build..."
  if make -j >/dev/null 2>&1; then
    echo "✅ Build successful"
  else
    echo "❌ Build failed - fix compilation errors before committing"
    echo "   Run 'make' to see full error output"
    EXIT_CODE=1
  fi
  echo ""
fi

# ====================================================================
# Code Style Check
# ====================================================================
echo "🔍 Checking for TODO/FIXME markers..."

# Check for TODO/FIXME/XXX comments (informational only)
if [ -n "$FILES" ]; then
  if echo "$FILES" | xargs grep -n "TODO\|FIXME\|XXX" 2>/dev/null | head -20; then
    echo "ℹ️  Found TODO/FIXME markers (informational only)"
  else
    echo "✅ No TODO/FIXME markers found"
  fi
else
  echo "ℹ️  No source files to check"
fi

echo ""

# ====================================================================
# Final Result
# ====================================================================
if [ $EXIT_CODE -eq 0 ]; then
  echo "✅ Quality checks passed!"
  exit 0
else
  echo "❌ Quality checks failed!"
  exit 1
fi
