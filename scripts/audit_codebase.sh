#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen Codebase Audit Script
# Checks for violations of coding standards and best practices.
# Run periodically or in CI to catch regressions.
#
# Usage:
#   ./scripts/audit_codebase.sh [--strict] [--files FILE...]
#
# Modes:
#   Full audit (no files):  Scans entire codebase with threshold checks
#   File mode (--files):    Checks only specified files (for pre-commit hooks)
#
# Exit codes:
#   0 = All checks passed (or only warnings)
#   1 = Critical violations found (with --strict, any warnings also fail)
#
# ============================================================================
# ADDING NEW AUDIT CHECKS
# ============================================================================
#
# This script has two modes that need to stay in sync:
#   1. FILE MODE (lines ~124-259): Checks individual files (for pre-commit)
#   2. FULL MODE (lines ~263-end): Scans entire codebase with thresholds
#
# To add a new check:
#
# 1. DEFINE WHAT YOU'RE CHECKING
#    - What pattern indicates a problem? (regex for grep)
#    - Is it an ERROR (blocks commit) or WARNING (informational)?
#    - What files should be checked? (.cpp, .xml, ui_*.cpp, etc.)
#
# 2. ADD TO FULL MODE (bottom section)
#    - Add a new section with: section "P#: Your Check Name"
#    - Use grep to count violations: count=$(grep -rn 'pattern' path/ | wc -l)
#    - Set a threshold: YOUR_THRESHOLD=50
#    - Compare: if [ "$count" -gt "$YOUR_THRESHOLD" ]; then warning/error "..."; fi
#
# 3. ADD TO FILE MODE (top section, for pre-commit)
#    - Add matching check inside: if [ ${#relevant_files[@]} -gt 0 ]; then
#    - Loop over files: for f in "${relevant_files[@]}"; do ... done
#    - Use per-file grep, not recursive
#
# 4. UPDATE THRESHOLDS AS CODEBASE IMPROVES
#    - Start with current count + small buffer
#    - Ratchet down as violations are fixed
#
# Example check structure (Full Mode):
#
#   section "P99: Example Check"
#   set +e
#   bad_pattern=$(grep -rn 'dangerous_function' src/ --include='*.cpp' 2>/dev/null | wc -l)
#   set -e
#   echo "Dangerous function calls: $bad_pattern"
#   DANGEROUS_THRESHOLD=10
#   if [ "$bad_pattern" -gt "$DANGEROUS_THRESHOLD" ]; then
#       warning "Dangerous calls ($bad_pattern) exceed threshold ($DANGEROUS_THRESHOLD)"
#   fi
#
# Current check categories:
#   P1  - Memory Safety (timers, RAII)
#   P2  - RAII Compliance (new/delete, lv_malloc)
#   P2b - Memory Anti-Patterns (vector pointers, user_data leaks)
#   P2c - LVGL Shutdown Safety (manual cleanup calls)
#   P2d - spdlog in Destructors (crashes after shutdown)
#   P3  - Design Tokens (hardcoded spacing in XML)
#   P4  - Declarative UI (event handlers, text updates, visibility)
#   P5  - Code Organization (file size limits)
#   --  - C++ Hardcoded Colors
#
# ============================================================================

set -euo pipefail

# Colors for output
RED='\033[0;31m'
YELLOW='\033[0;33m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Parse arguments
STRICT_MODE=false
FILE_MODE=false
FILES=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --strict)
            STRICT_MODE=true
            shift
            ;;
        --files)
            FILE_MODE=true
            shift
            # Collect all remaining arguments as files
            while [[ $# -gt 0 && ! "$1" =~ ^-- ]]; do
                FILES+=("$1")
                shift
            done
            ;;
        *)
            # Treat as file if not a flag
            if [[ -f "$1" ]]; then
                FILE_MODE=true
                FILES+=("$1")
            fi
            shift
            ;;
    esac
done

# Counters
ERRORS=0
WARNINGS=0

# Helper functions
error() {
    echo -e "${RED}ERROR:${NC} $1"
    ((ERRORS++)) || true
}

warning() {
    echo -e "${YELLOW}WARNING:${NC} $1"
    ((WARNINGS++)) || true
}

info() {
    echo -e "${CYAN}INFO:${NC} $1"
}

success() {
    echo -e "${GREEN}✓${NC} $1"
}

section() {
    echo ""
    echo -e "${CYAN}=== $1 ===${NC}"
}

# Change to repo root
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

# Filter files to only include relevant types
filter_cpp_files() {
    local result=()
    for f in "${FILES[@]}"; do
        if [[ "$f" == *.cpp && -f "$f" ]]; then
            result+=("$f")
        fi
    done
    echo "${result[@]:-}"
}

filter_xml_files() {
    local result=()
    for f in "${FILES[@]}"; do
        if [[ "$f" == *.xml && -f "$f" ]]; then
            result+=("$f")
        fi
    done
    echo "${result[@]:-}"
}

filter_ui_cpp_files() {
    local result=()
    for f in "${FILES[@]}"; do
        if [[ "$f" == *ui_*.cpp && -f "$f" ]]; then
            result+=("$f")
        fi
    done
    echo "${result[@]:-}"
}

# ============================================================================
# FILE MODE: Check only specified files (for pre-commit)
# ============================================================================
if [ "$FILE_MODE" = true ]; then
    echo "========================================"
    echo "HelixScreen Audit (File Mode)"
    echo "Checking ${#FILES[@]} file(s)"
    echo "========================================"

    # Get filtered file lists (filter_* echo one space-joined line; read -ra
    # reproduces the word splitting the old array assignment did)
    read -ra cpp_files <<< "$(filter_cpp_files)"
    read -ra ui_cpp_files <<< "$(filter_ui_cpp_files)"
    read -ra xml_files <<< "$(filter_xml_files)"

    if [ ${#cpp_files[@]} -eq 0 ] && [ ${#xml_files[@]} -eq 0 ]; then
        echo "No auditable files (.cpp, .xml) in changeset"
        exit 0
    fi

    #
    # === P1: Timer Safety (per-file) ===
    #
    if [ ${#cpp_files[@]} -gt 0 ]; then
        section "P1: Timer Safety"
        for f in "${cpp_files[@]}"; do
            set +e
            creates=$(grep -c "lv_timer_create" "$f" 2>/dev/null || echo "0")
            deletes=$(grep -c "lv_timer_del" "$f" 2>/dev/null || echo "0")
            set -e
            creates=$(echo "$creates" | tr -d '[:space:]')
            deletes=$(echo "$deletes" | tr -d '[:space:]')
            if [ "$creates" -gt 0 ] && [ "$creates" -gt "$deletes" ]; then
                warning "$(basename "$f"): creates $creates timers but only deletes $deletes"
            fi
        done
        success "Timer check complete"
    fi

    #
    # === P2b: Memory Safety Anti-Patterns (per-file, CRITICAL) ===
    #
    if [ ${#ui_cpp_files[@]} -gt 0 ]; then
        section "P2b: Memory Safety Anti-Patterns"

        for f in "${ui_cpp_files[@]}"; do
            fname=$(basename "$f")

            # Check for vector element pointer storage
            set +e
            vector_issues=$(grep -n '&[a-z_]*_\.\(back\|front\)()' "$f" 2>/dev/null)
            set -e
            if [ -n "$vector_issues" ]; then
                error "$fname: stores pointer to vector element (dangling pointer risk)"
                echo "$vector_issues" | head -3
            fi

            # Check for user_data allocation without DELETE handler
            set +e
            has_new_userdata=$(grep -l 'set_user_data.*new\|new.*set_user_data' "$f" 2>/dev/null)
            set -e
            if [ -n "$has_new_userdata" ]; then
                set +e
                has_delete_handler=$(grep -l 'LV_EVENT_DELETE' "$f" 2>/dev/null)
                set -e
                if [ -z "$has_delete_handler" ]; then
                    error "$fname: allocates user_data with 'new' but has no LV_EVENT_DELETE handler"
                fi
            fi
        done

        if [ "$ERRORS" -eq 0 ]; then
            success "No memory safety anti-patterns found"
        fi
    fi

    #
    # === P2c: LVGL Shutdown Safety (per-file, CRITICAL) ===
    #
    # These LVGL cleanup functions cause double-free crashes because lv_deinit()
    # already handles them internally. This check prevents this bug from regressing.
    #
    if [ ${#cpp_files[@]} -gt 0 ]; then
        section "P2c: LVGL Shutdown Safety"

        for f in "${cpp_files[@]}"; do
            fname=$(basename "$f")

            # Skip test files - they manage their own LVGL lifecycle without lv_deinit()
            if [[ "$fname" == test_* ]]; then
                continue
            fi

            # Check for manual lv_display_delete (lv_deinit iterates and deletes all displays)
            # Exclude comments: // style, Doxygen * style, and @tag documentation
            set +e
            display_delete=$(grep -n 'lv_display_delete' "$f" 2>/dev/null | grep -v '//' | grep -v '^\s*\*' | grep -v '@')
            set -e
            if [ -n "$display_delete" ]; then
                error "$fname: uses lv_display_delete() - lv_deinit() handles this, causes double-free"
                echo "$display_delete" | head -3
            fi

            # Check for manual lv_group_delete (lv_deinit calls lv_group_deinit)
            set +e
            group_delete=$(grep -n 'lv_group_delete' "$f" 2>/dev/null | grep -v '//' | grep -v '^\s*\*' | grep -v '@')
            set -e
            if [ -n "$group_delete" ]; then
                error "$fname: uses lv_group_delete() - lv_deinit() handles this, causes crash on dangling pointers"
                echo "$group_delete" | head -3
            fi

            # Check for manual lv_indev_delete (also managed by lv_deinit)
            set +e
            indev_delete=$(grep -n 'lv_indev_delete' "$f" 2>/dev/null | grep -v '//' | grep -v '^\s*\*' | grep -v '@')
            set -e
            if [ -n "$indev_delete" ]; then
                error "$fname: uses lv_indev_delete() - lv_deinit() handles this, causes double-free"
                echo "$indev_delete" | head -3
            fi
        done

        if [ "$ERRORS" -eq 0 ]; then
            success "No dangerous LVGL cleanup calls found"
        fi
    fi

    #
    # === P2d: spdlog Shutdown Guard (per-file, CRITICAL) ===
    #
    # Application::shutdown() must have a guard to prevent double-shutdown.
    # Objects destroyed after spdlog::shutdown() can't log safely.
    #
    if [ ${#cpp_files[@]} -gt 0 ]; then
        section "P2d: spdlog Shutdown Safety"

        for f in "${cpp_files[@]}"; do
            fname=$(basename "$f")

            # Only check application shutdown files - most destructors run before spdlog::shutdown
            if [[ "$fname" == "application.cpp" ]]; then
                set +e
                has_shutdown=$(grep -l 'spdlog::shutdown' "$f" 2>/dev/null)
                has_guard=$(grep -l 'm_shutdown_complete\|shutdown_complete' "$f" 2>/dev/null)
                set -e

                if [ -n "$has_shutdown" ] && [ -z "$has_guard" ]; then
                    error "$fname: calls spdlog::shutdown() but has no shutdown guard (causes double-shutdown crash)"
                fi
            fi
        done

        success "Shutdown safety check complete"
    fi

    #
    # === P3: Design Tokens (per-file) ===
    #
    if [ ${#xml_files[@]} -gt 0 ]; then
        section "P3: Design Token Compliance"

        for f in "${xml_files[@]}"; do
            fname=$(basename "$f")
            set +e
            hardcoded=$(grep -n 'style_pad[^=]*="[1-9]\|style_margin[^=]*="[1-9]\|style_gap[^=]*="[1-9]' "$f" 2>/dev/null)
            set -e
            if [ -n "$hardcoded" ]; then
                count=$(echo "$hardcoded" | wc -l | tr -d ' ')
                warning "$fname: $count hardcoded spacing value(s) (should use design tokens)"
            fi
        done

        if [ "$WARNINGS" -eq 0 ]; then
            success "All spacing uses design tokens"
        fi
    fi

    #
    # === C++ Hardcoded Colors (per-file) ===
    #
    if [ ${#ui_cpp_files[@]} -gt 0 ]; then
        section "C++ Hardcoded Colors"

        for f in "${ui_cpp_files[@]}"; do
            fname=$(basename "$f")
            set +e
            color_issues=$(grep -n 'lv_color_hex\|lv_color_make' "$f" 2>/dev/null | grep -v 'theme\|parse')
            set -e
            if [ -n "$color_issues" ]; then
                count=$(echo "$color_issues" | wc -l | tr -d ' ')
                warning "$fname: $count hardcoded color literal(s) (should use theme API)"
            fi
        done
    fi

    #
    # === P4: Declarative UI Compliance (per-file) ===
    #
    # [L007] Never use lv_obj_add_event_cb() in C++ - use XML event_cb instead
    # This catches imperative event wiring that should be declarative
    #
    if [ ${#ui_cpp_files[@]} -gt 0 ]; then
        section "P4: Declarative UI (Event Callbacks)"

        for f in "${ui_cpp_files[@]}"; do
            fname=$(basename "$f")

            # Skip files that legitimately need imperative callbacks:
            # - ui_toast.cpp: manages dynamic toast lifecycle
            # - ui_nav.cpp: navigation system internals
            # - ui_busy_overlay.cpp: overlay management
            # - ui_modal.cpp: base modal class with lifecycle management
            # - ui_wizard*.cpp: wizard framework with dynamic step navigation
            # - test_*.cpp: unit tests
            if [[ "$fname" == "ui_toast.cpp" ]] || \
               [[ "$fname" == "ui_nav.cpp" ]] || \
               [[ "$fname" == "ui_busy_overlay.cpp" ]] || \
               [[ "$fname" == "ui_modal.cpp" ]] || \
               [[ "$fname" == ui_wizard*.cpp ]] || \
               [[ "$fname" == test_* ]]; then
                continue
            fi

            set +e
            # Find lv_obj_add_event_cb calls, excluding comments
            event_cb_issues=$(grep -n 'lv_obj_add_event_cb' "$f" 2>/dev/null | grep -v '//' | grep -v '^\s*\*' | grep -v '@')
            set -e
            if [ -n "$event_cb_issues" ]; then
                count=$(echo "$event_cb_issues" | wc -l | tr -d ' ')
                warning "$fname: $count lv_obj_add_event_cb() call(s) - use XML event_cb instead [L007]"
                echo "$event_cb_issues" | head -3
            fi
        done

        if [ "$WARNINGS" -eq 0 ]; then
            success "All event callbacks use declarative XML pattern"
        fi
    fi

    #
    # === P5: XML Component Names (per-file) ===
    #
    # [L003] Always add name='component_name' on XML component tags
    # Missing names cause lv_obj_find_by_name() to fail silently
    #
    # Smart detection: Only warn on "actionable" widgets that likely need names:
    # - Interactive types: lv_button, lv_slider, lv_dropdown, lv_spinner, lv_roller, lv_textarea
    # - Widgets with bindings: bind_text, bind_value, bind_flag
    # - Widgets with events: event_cb
    #
    # Decorative widgets (layout containers, spacers, static labels) can safely omit names.
    #
    if [ ${#xml_files[@]} -gt 0 ]; then
        section "P5: XML Component Names"

        for f in "${xml_files[@]}"; do
            fname=$(basename "$f")

            # Skip globals.xml (defines styles, not components)
            if [[ "$fname" == "globals.xml" ]]; then
                continue
            fi

            set +e
            # Find interactive widget types without name= attribute
            # These should almost always have names for C++ lookup
            # Note: Exclude binding child elements like <lv_obj-bind_flag_if_eq>
            # Note: Exclude buttons with clickable="false" (decorative placeholders)
            # Note: lv_spinner excluded - they're loading indicators, not interactive controls
            # Use -A3 to catch clickable="false" on following lines (multi-line XML attributes)
            interactive_unnamed=$(grep -n '<lv_button\|<lv_slider\|<lv_dropdown\|<lv_roller\|<lv_textarea' "$f" 2>/dev/null | grep -v 'name=' | grep -v '<lv_obj-' | grep -v 'clickable="false"')
            # Filter out buttons where clickable="false" appears within 3 lines
            if [ -n "$interactive_unnamed" ]; then
                filtered=""
                while IFS= read -r line; do
                    lineno=$(echo "$line" | cut -d: -f1)
                    # Check if clickable="false" appears in this line or next 3 lines
                    if ! sed -n "${lineno},$((lineno+3))p" "$f" 2>/dev/null | grep -q 'clickable="false"'; then
                        filtered="${filtered}${line}"$'\n'
                    fi
                done <<< "$interactive_unnamed"
                interactive_unnamed="${filtered%$'\n'}"
            fi

            # Find widgets with inline bindings or events but no name
            # These are "actionable" and need names for proper subject/callback management
            # Note: Look for bind_* as ATTRIBUTES on widget tags, not child binding elements
            # Child elements like <lv_obj-bind_flag_if_eq> are not widgets and don't need names
            bound_unnamed=$(grep -n '<lv_obj\|<lv_label\|<lv_image' "$f" 2>/dev/null | grep -E 'bind_text=|bind_value=' | grep -v 'name=')
            set -e

            # Combine and dedupe
            actionable=""
            if [ -n "$interactive_unnamed" ]; then
                actionable="$interactive_unnamed"
            fi
            if [ -n "$bound_unnamed" ]; then
                if [ -n "$actionable" ]; then
                    actionable=$(echo -e "$actionable\n$bound_unnamed" | sort -u)
                else
                    actionable="$bound_unnamed"
                fi
            fi

            if [ -n "$actionable" ]; then
                count=$(echo "$actionable" | wc -l | tr -d ' ')
                # Stricter threshold for actionable widgets - these really should have names
                if [ "$count" -gt 0 ]; then
                    warning "$fname: $count interactive/bound widget(s) without name= attribute [L003]"
                    echo "$actionable" | head -3
                fi
            fi
        done

        if [ "$WARNINGS" -eq 0 ]; then
            success "XML component naming looks good (interactive widgets are named)"
        fi
    fi

    #
    # === Summary ===
    #
    section "Summary"
    echo ""
    echo "Files checked: ${#FILES[@]}"
    echo "Errors:   $ERRORS"
    echo "Warnings: $WARNINGS"
    echo ""

    if [ "$ERRORS" -gt 0 ]; then
        echo -e "${RED}AUDIT FAILED${NC} - $ERRORS critical error(s) found"
        exit 1
    elif [ "$WARNINGS" -gt 0 ] && [ "$STRICT_MODE" = true ]; then
        echo -e "${YELLOW}AUDIT FAILED (strict mode)${NC} - $WARNINGS warning(s) found"
        exit 1
    elif [ "$WARNINGS" -gt 0 ]; then
        echo -e "${YELLOW}AUDIT PASSED WITH WARNINGS${NC} - $WARNINGS warning(s) found"
        exit 0
    else
        echo -e "${GREEN}AUDIT PASSED${NC} - No issues found"
        exit 0
    fi
fi

# ============================================================================
# FULL MODE: Scan entire codebase (default)
# ============================================================================

echo "========================================"
echo "HelixScreen Codebase Audit"
echo "Date: $(date)"
echo "========================================"

#
# === P1: Memory Safety (Critical) ===
#
section "P1: Memory Safety (Critical)"

# Timer leak detection
set +e
timer_creates=$(grep -rn 'lv_timer_create' src/ --include='*.cpp' 2>/dev/null | wc -l | tr -d ' ')
timer_deletes=$(grep -rn 'lv_timer_del' src/ --include='*.cpp' 2>/dev/null | wc -l | tr -d ' ')
set -e
echo "Timer creates: $timer_creates"
echo "Timer deletes: $timer_deletes"

# Check for files with more creates than deletes (potential leaks)
echo ""
echo "Checking for unbalanced timer usage:"
unbalanced=0
for f in src/*.cpp; do
    [ -f "$f" ] || continue
    creates=$(grep -c "lv_timer_create" "$f" 2>/dev/null || true)
    creates=$(echo "$creates" | tr -d '[:space:]')
    creates=${creates:-0}
    deletes=$(grep -c "lv_timer_del" "$f" 2>/dev/null || true)
    deletes=$(echo "$deletes" | tr -d '[:space:]')
    deletes=${deletes:-0}
    if [ "$creates" -gt 0 ] 2>/dev/null && [ "$creates" -gt "$deletes" ] 2>/dev/null; then
        echo "  REVIEW: $f (creates: $creates, deletes: $deletes)"
        ((unbalanced++)) || true
    fi
done
if [ "$unbalanced" -eq 0 ]; then
    success "All timer usage appears balanced"
fi

#
# === P2: RAII Compliance ===
#
section "P2: RAII Compliance"

# Manual new/delete in UI files
# Note: Temporarily disable errexit for grep commands (grep returns 1 when no matches)
set +e
manual_new=$(grep -rn '\bnew \w\+(' src/ui_*.cpp 2>/dev/null | grep -v 'make_unique\|placement' | wc -l | tr -d ' ')
manual_delete=$(grep -rn '^\s*delete ' src/ --include='*.cpp' 2>/dev/null | grep -v lib/ | wc -l | tr -d ' ')
lv_malloc_count=$(grep -rn 'lv_malloc' src/ --include='*.cpp' 2>/dev/null | grep -v lib/ | wc -l | tr -d ' ')
set -e

echo "Manual 'new' in UI code: $manual_new"
echo "Manual 'delete': $manual_delete"
echo "lv_malloc in src/: $lv_malloc_count"

# Thresholds (adjust based on migration progress)
MANUAL_NEW_THRESHOLD=20
MANUAL_DELETE_THRESHOLD=35
LV_MALLOC_THRESHOLD=5

if [ "$manual_new" -gt "$MANUAL_NEW_THRESHOLD" ]; then
    warning "Manual 'new' count ($manual_new) exceeds threshold ($MANUAL_NEW_THRESHOLD)"
fi
if [ "$manual_delete" -gt "$MANUAL_DELETE_THRESHOLD" ]; then
    warning "Manual 'delete' count ($manual_delete) exceeds threshold ($MANUAL_DELETE_THRESHOLD)"
fi
if [ "$lv_malloc_count" -gt "$LV_MALLOC_THRESHOLD" ]; then
    warning "lv_malloc count ($lv_malloc_count) exceeds threshold ($LV_MALLOC_THRESHOLD)"
fi

#
# === P2b: Memory Safety Anti-Patterns (Critical) ===
#
section "P2b: Memory Safety Anti-Patterns"

# Check for dangerous vector element pointer storage
# Pattern: &vec.back(), &vec[i], &vec_.back() stored in user_data
echo "Checking for vector element pointer storage (dangling pointer risk):"
set +e
vector_ptr_issues=$(grep -rn '&[a-z_]*_\.\(back\|front\)()' src/ui_*.cpp 2>/dev/null | wc -l | tr -d ' ')
set -e

if [ "$vector_ptr_issues" -gt 0 ]; then
    error "Found $vector_ptr_issues instances of &vec.back()/.front() - dangling pointer risk!"
    set +e
    grep -rn '&[a-z_]*_\.\(back\|front\)()' src/ui_*.cpp 2>/dev/null | head -5
    set -e
else
    success "No vector element pointer storage found"
fi

# Check for user_data allocations without DELETE handlers
# Files that have 'new' + 'set_user_data' should also have 'LV_EVENT_DELETE'
echo ""
echo "Checking for user_data allocations without DELETE handlers:"
userdata_leak_risk=0
for f in src/ui_*.cpp; do
    [ -f "$f" ] || continue
    set +e
    has_new_userdata=$(grep -l 'set_user_data.*new\|new.*set_user_data' "$f" 2>/dev/null)
    if [ -n "$has_new_userdata" ]; then
        # Check if this file registers a DELETE handler
        has_delete_handler=$(grep -l 'LV_EVENT_DELETE' "$f" 2>/dev/null)
        if [ -z "$has_delete_handler" ]; then
            error "$(basename "$f"): allocates user_data but has no LV_EVENT_DELETE handler!"
            ((userdata_leak_risk++)) || true
        fi
    fi
    set -e
done
if [ "$userdata_leak_risk" -eq 0 ]; then
    success "All user_data allocations have DELETE handlers"
fi

#
# === P2c: LVGL Shutdown Safety (Critical) ===
#
# These LVGL cleanup functions cause double-free crashes because lv_deinit()
# already handles them internally. See display_manager.cpp comments.
#
section "P2c: LVGL Shutdown Safety"

echo "Checking for dangerous manual LVGL cleanup calls:"
# Exclude comments: // line comments, NOTE: documentation, Doxygen * style
set +e
lv_display_delete_count=$(grep -rn 'lv_display_delete' src/ --include='*.cpp' 2>/dev/null | grep -v 'NOTE:' | grep -v ' \* @' | grep -v ':\s*//' | wc -l | tr -d ' ')
lv_group_delete_count=$(grep -rn 'lv_group_delete' src/ --include='*.cpp' 2>/dev/null | grep -v 'NOTE:' | grep -v ' \* @' | grep -v ':\s*//' | wc -l | tr -d ' ')
lv_indev_delete_count=$(grep -rn 'lv_indev_delete' src/ --include='*.cpp' 2>/dev/null | grep -v 'NOTE:' | grep -v ' \* @' | grep -v ':\s*//' | wc -l | tr -d ' ')
set -e

echo "  lv_display_delete: $lv_display_delete_count (should be 0)"
echo "  lv_group_delete:   $lv_group_delete_count (should be 0)"
echo "  lv_indev_delete:   $lv_indev_delete_count (should be 0)"

total_dangerous=$((lv_display_delete_count + lv_group_delete_count + lv_indev_delete_count))
if [ "$total_dangerous" -gt 0 ]; then
    error "Found $total_dangerous dangerous LVGL cleanup calls - these cause double-free crashes!"
    echo ""
    echo "EXPLANATION: lv_deinit() already cleans up all displays, groups, and input devices."
    echo "Manually calling these functions causes double-free or dangling pointer crashes."
    echo ""
    echo "Violations:"
    set +e
    grep -rn 'lv_display_delete\|lv_group_delete\|lv_indev_delete' src/ --include='*.cpp' 2>/dev/null | head -10
    set -e
else
    success "No dangerous LVGL cleanup calls found"
fi

#
# === P2d: spdlog Shutdown Guard ===
#
# Application::shutdown() must have a guard to prevent double-shutdown.
# Most destructors run before spdlog::shutdown(), so we only check the
# Application shutdown path which is the high-risk area.
#
section "P2d: spdlog Shutdown Safety"

echo "Checking application shutdown guard..."
set +e
app_file=$(find src/ -name "application.cpp" 2>/dev/null | head -1)
set -e

if [ -n "$app_file" ] && [ -f "$app_file" ]; then
    set +e
    has_shutdown=$(grep -l 'spdlog::shutdown' "$app_file" 2>/dev/null)
    has_guard=$(grep -l 'm_shutdown_complete\|shutdown_complete' "$app_file" 2>/dev/null)
    set -e

    if [ -n "$has_shutdown" ] && [ -z "$has_guard" ]; then
        error "application.cpp: calls spdlog::shutdown() but has no shutdown guard!"
        echo ""
        echo "This causes crashes when shutdown() is called multiple times."
        echo "Add a guard like: if (m_shutdown_complete) return; m_shutdown_complete = true;"
    else
        success "Application shutdown has proper guard"
    fi
else
    echo "ℹ️  application.cpp not found - skipping"
fi

#
# === P3: Design Tokens ===
#
section "P3: XML Design Token Compliance"

set +e
hardcoded_padding=$(grep -rn 'style_pad[^=]*="[1-9]' ui_xml/ --include='*.xml' 2>/dev/null | wc -l | tr -d ' ')
hardcoded_margin=$(grep -rn 'style_margin[^=]*="[1-9]' ui_xml/ --include='*.xml' 2>/dev/null | wc -l | tr -d ' ')
hardcoded_gap=$(grep -rn 'style_gap[^=]*="[1-9]' ui_xml/ --include='*.xml' 2>/dev/null | wc -l | tr -d ' ')
set -e

echo "Hardcoded padding values: $hardcoded_padding"
echo "Hardcoded margin values: $hardcoded_margin"
echo "Hardcoded gap values: $hardcoded_gap"

HARDCODED_SPACING_THRESHOLD=30
total_hardcoded=$((hardcoded_padding + hardcoded_margin + hardcoded_gap))
if [ "$total_hardcoded" -gt "$HARDCODED_SPACING_THRESHOLD" ]; then
    warning "Hardcoded spacing values ($total_hardcoded) exceed threshold ($HARDCODED_SPACING_THRESHOLD)"
fi

#
# === P4: Declarative UI Compliance ===
#
section "P4: Declarative UI Compliance"

set +e
# Event handlers - categorize by type
event_delete=$(grep -r 'lv_obj_add_event_cb.*DELETE' src/ui_*.cpp 2>/dev/null | wc -l | tr -d ' ')
event_gesture=$(grep -rE 'lv_obj_add_event_cb.*(GESTURE|SCROLL|DRAW|PRESS)' src/ui_*.cpp 2>/dev/null | wc -l | tr -d ' ')
event_clicked=$(grep -rE 'lv_obj_add_event_cb.*(CLICKED|VALUE_CHANGED)' src/ui_*.cpp 2>/dev/null | wc -l | tr -d ' ')
event_total=$(grep -r 'lv_obj_add_event_cb' src/ui_*.cpp 2>/dev/null | wc -l | tr -d ' ')

# Text updates
text_updates=$(grep -rn 'lv_label_set_text' src/ui_*.cpp 2>/dev/null | wc -l | tr -d ' ')

# Visibility - categorize by pattern
visibility_pool=$(grep -rE 'lv_obj_(add|clear)_flag.*(pool|Pool).*HIDDEN' src/ui_*.cpp 2>/dev/null | wc -l | tr -d ' ')
visibility_total=$(grep -rn 'lv_obj_add_flag.*HIDDEN\|lv_obj_clear_flag.*HIDDEN' src/ui_*.cpp 2>/dev/null | wc -l | tr -d ' ')
visibility_actionable=$((visibility_total - visibility_pool))

# Inline styles
inline_styles=$(grep -rn 'lv_obj_set_style_' src/ui_*.cpp 2>/dev/null | wc -l | tr -d ' ')
set -e

echo ""
echo "Event Handlers:"
echo "  DELETE (legitimate):     $event_delete"
echo "  Gesture/Draw (legit):    $event_gesture"
echo "  CLICKED/VALUE_CHANGED:   $event_clicked  ← should use XML event_cb"
echo "  Total:                   $event_total"
echo ""
echo "Visibility Toggles:"
echo "  Widget pool (legit):     $visibility_pool"
echo "  Actionable:              $visibility_actionable  ← consider bind_flag"
echo "  Total:                   $visibility_total"
echo ""
echo "Text Updates:              $text_updates  ← consider bind_text subjects"
echo "Inline Style Setters:      $inline_styles  (many are dynamic/legitimate)"

# Thresholds for actionable items only
EVENT_CLICKED_THRESHOLD=100
VISIBILITY_THRESHOLD=120
TEXT_UPDATE_THRESHOLD=200
INLINE_STYLE_THRESHOLD=600

if [ "$event_clicked" -gt "$EVENT_CLICKED_THRESHOLD" ]; then
    warning "CLICKED/VALUE_CHANGED handlers ($event_clicked) exceed threshold ($EVENT_CLICKED_THRESHOLD)"
fi
if [ "$visibility_actionable" -gt "$VISIBILITY_THRESHOLD" ]; then
    warning "Actionable visibility toggles ($visibility_actionable) exceed threshold ($VISIBILITY_THRESHOLD)"
fi
if [ "$text_updates" -gt "$TEXT_UPDATE_THRESHOLD" ]; then
    warning "Direct text updates ($text_updates) exceed threshold ($TEXT_UPDATE_THRESHOLD)"
fi
if [ "$inline_styles" -gt "$INLINE_STYLE_THRESHOLD" ]; then
    warning "Inline style setters ($inline_styles) exceed threshold ($INLINE_STYLE_THRESHOLD)"
fi

#
# === P5: Code Size ===
#
section "P5: Code Organization (File Size)"

MAX_LINES=2500
echo "Files exceeding $MAX_LINES lines:"
oversized=0
for f in src/ui_panel_*.cpp; do
    [ -f "$f" ] || continue
    lines=$(wc -l < "$f")
    if [ "$lines" -gt "$MAX_LINES" ]; then
        warning "$(basename "$f") has $lines lines (max: $MAX_LINES)"
        ((oversized++)) || true
    fi
done
if [ "$oversized" -eq 0 ]; then
    success "No files exceed $MAX_LINES lines"
fi

#
# === Hardcoded Colors in C++ ===
#
section "C++ Hardcoded Colors"

set +e
color_literals=$(grep -rn 'lv_color_hex\|lv_color_make' src/ui_*.cpp 2>/dev/null | grep -v 'theme\|parse' | wc -l | tr -d ' ')
set -e
echo "Hardcoded color literals: $color_literals"

COLOR_LITERAL_THRESHOLD=50
if [ "$color_literals" -gt "$COLOR_LITERAL_THRESHOLD" ]; then
    warning "Hardcoded color literals ($color_literals) exceed threshold ($COLOR_LITERAL_THRESHOLD)"
fi

#
# === Summary ===
#
section "Summary"

echo ""
echo "Errors:   $ERRORS"
echo "Warnings: $WARNINGS"
echo ""

if [ "$ERRORS" -gt 0 ]; then
    echo -e "${RED}AUDIT FAILED${NC} - $ERRORS critical error(s) found"
    exit 1
elif [ "$WARNINGS" -gt 0 ] && [ "$STRICT_MODE" = true ]; then
    echo -e "${YELLOW}AUDIT FAILED (strict mode)${NC} - $WARNINGS warning(s) found"
    exit 1
elif [ "$WARNINGS" -gt 0 ]; then
    echo -e "${YELLOW}AUDIT PASSED WITH WARNINGS${NC} - $WARNINGS warning(s) found"
    exit 0
else
    echo -e "${GREEN}AUDIT PASSED${NC} - No issues found"
    exit 0
fi
