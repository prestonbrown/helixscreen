#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# git-stats.sh - Comprehensive git repository statistics with effort estimation
# Usage: ./scripts/git-stats.sh

set -uo pipefail

# Terminal width detection for 2-column layout
# Try multiple methods: tput, COLUMNS env var, default to 80
if [[ -n "${COLUMNS:-}" ]] && [[ $COLUMNS -gt 0 ]]; then
    TERM_WIDTH=$COLUMNS
elif command -v tput &>/dev/null && tput cols &>/dev/null; then
    TERM_WIDTH=$(tput cols)
else
    TERM_WIDTH=80
fi
# Minimum width for 2-column layout (each column ~45 chars + gap)
TWO_COL_MIN_WIDTH=100
USE_TWO_COLS=false
if [[ $TERM_WIDTH -ge $TWO_COL_MIN_WIDTH ]]; then
    USE_TWO_COLS=true
    COL_WIDTH=$(( (TERM_WIDTH - 6) / 2 ))  # 6 chars for gap between columns
else
    COL_WIDTH=$((TERM_WIDTH - 4))
fi

# Colors for terminal output (using $'...' for portable escape interpretation)
GREEN=$'\033[0;32m'
YELLOW=$'\033[1;33m'
PURPLE=$'\033[0;35m'
CYAN=$'\033[0;36m'
BOLD=$'\033[1m'
DIM=$'\033[2m'
NC=$'\033[0m' # No Color

# Function to print two temp files of lines side by side
# Usage: print_two_columns_files left_file right_file
# Works with older Bash (no nameref required)
print_two_columns_files() {
    local left_file=$1
    local right_file=$2

    # Read files into arrays (compatible with older bash)
    local left_lines=()
    local right_lines=()
    while IFS= read -r line || [[ -n "$line" ]]; do
        left_lines+=("$line")
    done < "$left_file"
    while IFS= read -r line || [[ -n "$line" ]]; do
        right_lines+=("$line")
    done < "$right_file"

    local max_lines=${#left_lines[@]}
    [[ ${#right_lines[@]} -gt $max_lines ]] && max_lines=${#right_lines[@]}

    for ((i=0; i<max_lines; i++)); do
        local left_line="${left_lines[$i]:-}"
        local right_line="${right_lines[$i]:-}"
        # Strip ANSI codes for length calculation
        local left_plain
        left_plain=$(echo "$left_line" | sed 's/\x1b\[[0-9;]*m//g')
        local left_len=${#left_plain}
        local padding=$((COL_WIDTH - left_len))
        [[ $padding -lt 0 ]] && padding=0
        printf "%b%*s   %b\n" "$left_line" "$padding" "" "$right_line"
    done
}

# Check if we're in a git repo
if ! git rev-parse --is-inside-work-tree &>/dev/null; then
    echo "Error: Not in a git repository"
    exit 1
fi

# Get repo root
REPO_ROOT=$(git rev-parse --show-toplevel)
REPO_NAME=$(basename "$REPO_ROOT")

# Temp files
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

echo "${CYAN}${BOLD}Analyzing git history for ${REPO_NAME}...${NC}"

# ============================================================================
# DATA COLLECTION
# ============================================================================

# Collect commit data to temp file
git log --format="%H|%at|%ad|%an|%s" --date=short --no-merges > "$TMP_DIR/commits.txt"
TOTAL_COMMITS=$(wc -l < "$TMP_DIR/commits.txt" | tr -d ' ')

if [[ $TOTAL_COMMITS -eq 0 ]]; then
    echo "No commits found!"
    exit 1
fi

# First and last dates
FIRST_COMMIT_DATE=$(tail -1 "$TMP_DIR/commits.txt" | cut -d'|' -f3)
LAST_COMMIT_DATE=$(head -1 "$TMP_DIR/commits.txt" | cut -d'|' -f3)

# Calculate project span
PROJECT_DAYS=$(python3 -c "
from datetime import datetime
d1 = datetime.strptime('$FIRST_COMMIT_DATE', '%Y-%m-%d')
d2 = datetime.strptime('$LAST_COMMIT_DATE', '%Y-%m-%d')
print((d2 - d1).days + 1)
")
PROJECT_WEEKS=$(echo "scale=1; $PROJECT_DAYS / 7" | bc)

# Active coding days
cut -d'|' -f3 "$TMP_DIR/commits.txt" | sort -u > "$TMP_DIR/active_days.txt"
ACTIVE_DAYS=$(wc -l < "$TMP_DIR/active_days.txt" | tr -d ' ')

# Session analysis
cut -d'|' -f2 "$TMP_DIR/commits.txt" | sort -n > "$TMP_DIR/timestamps.txt"
read sessions avg_min total_hrs <<< "$(python3 << PYEOF
import sys
with open("$TMP_DIR/timestamps.txt") as f:
    times = [int(line.strip()) for line in f if line.strip()]

if not times:
    print("0 0 0")
    sys.exit()

sessions = 1
session_start = times[0]
prev = times[0]
total_coded = 0

for t in times[1:]:
    gap = t - prev
    if gap > 7200:  # 2 hour gap
        session_time = prev - session_start
        total_coded += session_time
        sessions += 1
        session_start = t
    prev = t

total_coded += (prev - session_start)
avg_session_min = int((total_coded / sessions) / 60) if sessions > 0 else 0
total_hours = int(total_coded / 3600)
print(f"{sessions} {avg_session_min} {total_hours}")
PYEOF
)"
# Ensure session variables have defaults if read failed (sessions min 1 for division)
sessions=${sessions:-1}
avg_min=${avg_min:-0}
total_hrs=${total_hrs:-0}

# Longest streak
LONGEST_STREAK=$(cat "$TMP_DIR/active_days.txt" | python3 -c "
import sys
from datetime import datetime
dates = sorted(set(line.strip() for line in sys.stdin if line.strip()))
if not dates:
    print(0)
else:
    parsed = [datetime.strptime(d, '%Y-%m-%d') for d in dates]
    max_streak = cur_streak = 1
    for i in range(1, len(parsed)):
        if (parsed[i] - parsed[i-1]).days == 1:
            cur_streak += 1
        else:
            max_streak = max(max_streak, cur_streak)
            cur_streak = 1
    print(max(max_streak, cur_streak))
")

# Author stats
cut -d'|' -f4 "$TMP_DIR/commits.txt" | sort | uniq -c | sort -rn > "$TMP_DIR/authors.txt"

# Day of week stats
git log --format="%cd" --date=format:'%u' --no-merges | sort | uniq -c | sort -k2n > "$TMP_DIR/dow.txt"

# Hour stats
git log --format="%cd" --date=format:'%H' --no-merges | sort | uniq -c | sort -k2n > "$TMP_DIR/hours.txt"

# Most productive day
cut -d'|' -f3 "$TMP_DIR/commits.txt" | sort | uniq -c | sort -rn | head -1 > "$TMP_DIR/best_day.txt"

# Most changed files - excludes lib/, build/, .venv/, and other non-project directories
git log --name-only --format="" --no-merges 2>/dev/null | \
    grep -E "\.(cpp|h|xml|py|sh|mk)$|^Makefile$" | \
    grep -Ev "^(lib|build|\.venv|node_modules|vendor|external|third_party|deps)/" | \
    sort | uniq -c | sort -rn | head -10 > "$TMP_DIR/top_files.txt"

# Late night and weekend
LATE_NIGHT=$(git log --format="%cd" --date=format:'%H' --no-merges | awk '$1 >= 0 && $1 < 6 {count++} END {print count+0}')
WEEKEND=$(git log --format="%cd" --date=format:'%u' --no-merges | awk '$1 == 6 || $1 == 7 {count++} END {print count+0}')

# Source code stats - EXCLUDES lib/ and other external/vendor directories
# Only counts project-specific code, not libraries/submodules
CPP_LOC=0; H_LOC=0; XML_LOC=0; PY_LOC=0; SH_LOC=0; MK_LOC=0; TEST_LOC=0
CPP_COUNT=0; H_COUNT=0; XML_COUNT=0; PY_COUNT=0; SH_COUNT=0; MK_COUNT=0; TEST_COUNT=0

# Directories to EXCLUDE from all stats (libraries, submodules, build artifacts)
EXCLUDE_DIRS="lib|build|node_modules|vendor|external|third_party|deps|.git"

# C++ source files (src/ only, not lib/)
if [[ -d "$REPO_ROOT/src" ]]; then
    CPP_COUNT=$(find "$REPO_ROOT/src" -name "*.cpp" 2>/dev/null | wc -l | tr -d ' ')
    CPP_LOC=$(find "$REPO_ROOT/src" -name "*.cpp" -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
fi
# C++ headers (include/ only, not lib/)
if [[ -d "$REPO_ROOT/include" ]]; then
    H_COUNT=$(find "$REPO_ROOT/include" -name "*.h" 2>/dev/null | wc -l | tr -d ' ')
    H_LOC=$(find "$REPO_ROOT/include" -name "*.h" -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
fi
# XML layouts
if [[ -d "$REPO_ROOT/ui_xml" ]]; then
    XML_COUNT=$(find "$REPO_ROOT/ui_xml" -name "*.xml" 2>/dev/null | wc -l | tr -d ' ')
    XML_LOC=$(find "$REPO_ROOT/ui_xml" -name "*.xml" -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
fi
# Python scripts (moonraker-plugin/, scripts/, but NOT .venv or lib/)
PY_FILES=$(find "$REPO_ROOT" -name "*.py" -type f 2>/dev/null | grep -Ev "/(${EXCLUDE_DIRS}|\.venv|__pycache__)/")
if [[ -n "$PY_FILES" ]]; then
    PY_COUNT=$(echo "$PY_FILES" | grep -c .)
    PY_LOC=$(echo "$PY_FILES" | xargs cat 2>/dev/null | wc -l | tr -d ' ')
fi
# Shell scripts (scripts/, but NOT lib/)
if [[ -d "$REPO_ROOT/scripts" ]]; then
    SH_COUNT=$(find "$REPO_ROOT/scripts" -name "*.sh" 2>/dev/null | wc -l | tr -d ' ')
    SH_LOC=$(find "$REPO_ROOT/scripts" -name "*.sh" -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
fi
# Makefiles and .mk files (project build config)
MK_FILES=$(find "$REPO_ROOT" -maxdepth 2 \( -name "Makefile" -o -name "*.mk" \) -type f 2>/dev/null | grep -Ev "/(${EXCLUDE_DIRS})/")
if [[ -n "$MK_FILES" ]]; then
    MK_COUNT=$(echo "$MK_FILES" | grep -c .)
    MK_LOC=$(echo "$MK_FILES" | xargs cat 2>/dev/null | wc -l | tr -d ' ')
fi
# Test files (tests/ directory)
if [[ -d "$REPO_ROOT/tests" ]]; then
    TEST_COUNT=$(find "$REPO_ROOT/tests" \( -name "*.cpp" -o -name "*.h" -o -name "*.py" \) 2>/dev/null | wc -l | tr -d ' ')
    TEST_LOC=$(find "$REPO_ROOT/tests" \( -name "*.cpp" -o -name "*.h" -o -name "*.py" \) -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
fi

# Ensure all counts default to 0 for arithmetic (handles empty wc -l output)
CPP_LOC=${CPP_LOC:-0}; H_LOC=${H_LOC:-0}; XML_LOC=${XML_LOC:-0}
PY_LOC=${PY_LOC:-0}; SH_LOC=${SH_LOC:-0}; MK_LOC=${MK_LOC:-0}; TEST_LOC=${TEST_LOC:-0}
CPP_COUNT=${CPP_COUNT:-0}; H_COUNT=${H_COUNT:-0}; XML_COUNT=${XML_COUNT:-0}
PY_COUNT=${PY_COUNT:-0}; SH_COUNT=${SH_COUNT:-0}; MK_COUNT=${MK_COUNT:-0}; TEST_COUNT=${TEST_COUNT:-0}

TOTAL_LOC=$((CPP_LOC + H_LOC + XML_LOC + PY_LOC + SH_LOC + MK_LOC + TEST_LOC))
TOTAL_FILES=$((CPP_COUNT + H_COUNT + XML_COUNT + PY_COUNT + SH_COUNT + MK_COUNT + TEST_COUNT))

# Commit message word frequency
cut -d'|' -f5 "$TMP_DIR/commits.txt" | tr '[:upper:]' '[:lower:]' | tr -cs '[:alpha:]' '\n' | grep -v "^$" | sort | uniq -c | sort -rn | head -10 > "$TMP_DIR/words.txt"

# ============================================================================
# OUTPUT
# ============================================================================

# Ensure key metrics have defaults for arithmetic (avoid division by zero)
ACTIVE_DAYS=${ACTIVE_DAYS:-1}
PROJECT_DAYS=${PROJECT_DAYS:-1}
PROJECT_WEEKS=${PROJECT_WEEKS:-1}
LONGEST_STREAK=${LONGEST_STREAK:-0}

# Calculate derived metrics
COMMITS_PER_DAY=$(echo "scale=1; $TOTAL_COMMITS / $ACTIVE_DAYS" | bc)
PCT_ACTIVE=$((ACTIVE_DAYS * 100 / PROJECT_DAYS))
COMMITS_PER_WEEK=$(echo "scale=1; $TOTAL_COMMITS / $PROJECT_WEEKS" | bc)
AVG_HRS=$(echo "scale=1; $avg_min / 60" | bc)
ADJUSTED_HRS=$((total_hrs * 3 / 2))
TOTAL_EFFORT=$((total_hrs * 3))
WEEKLY_LOW=$(echo "scale=0; $ADJUSTED_HRS / $PROJECT_WEEKS" | bc)
WEEKLY_HIGH=$(echo "scale=0; $TOTAL_EFFORT / $PROJECT_WEEKS" | bc)
BEST_DAY=$(cat "$TMP_DIR/best_day.txt" | awk '{print $1 " commits on " $2}')
LATE_PCT=$(echo "scale=1; $LATE_NIGHT * 100 / $TOTAL_COMMITS" | bc)
WEEKEND_PCT=$(echo "scale=1; $WEEKEND * 100 / $TOTAL_COMMITS" | bc)
LOC_PER_DAY=0
[[ $TOTAL_LOC -gt 0 ]] && LOC_PER_DAY=$((TOTAL_LOC / ACTIVE_DAYS))
COMMITS_PER_SESSION=$(echo "scale=1; $TOTAL_COMMITS / $sessions" | bc)

echo ""
# Dynamic header box sizing
TITLE="📊 $REPO_NAME Development Statistics"
TITLE_PLAIN=$(echo "$TITLE" | sed 's/📊/XX/')  # Emoji counts as ~2 chars
TITLE_LEN=${#TITLE_PLAIN}
BOX_WIDTH=$((TITLE_LEN + 8))  # 4 chars padding each side
[[ $BOX_WIDTH -lt 50 ]] && BOX_WIDTH=50
[[ $BOX_WIDTH -gt $((TERM_WIDTH - 4)) ]] && BOX_WIDTH=$((TERM_WIDTH - 4))
INNER_WIDTH=$((BOX_WIDTH - 2))
PADDING=$(( (INNER_WIDTH - TITLE_LEN) / 2 ))
PADDING_R=$((INNER_WIDTH - TITLE_LEN - PADDING))

HORIZ_LINE=$(printf '═%.0s' $(seq 1 $INNER_WIDTH))
PADDING_L_STR=$(printf ' %.0s' $(seq 1 $PADDING))
PADDING_R_STR=$(printf ' %.0s' $(seq 1 $PADDING_R))

echo "${BOLD}╔${HORIZ_LINE}╗${NC}"
echo "${BOLD}║${PADDING_L_STR}${PURPLE}${TITLE}${NC}${BOLD}${PADDING_R_STR}║${NC}"
echo "${BOLD}╚${HORIZ_LINE}╝${NC}"
echo ""

# Project span (always single column - header section)
echo "${CYAN}${BOLD}📅 Project Timeline${NC}"
echo "   Start Date:     ${GREEN}$FIRST_COMMIT_DATE${NC}"
echo "   Latest Commit:  ${GREEN}$LAST_COMMIT_DATE${NC}"
echo "   Duration:       ${YELLOW}$PROJECT_DAYS days${NC} (${PROJECT_WEEKS} weeks)"
echo ""

# Core Metrics and Codebase Composition - side by side if room
if $USE_TWO_COLS && [[ $TOTAL_LOC -gt 0 ]]; then
    # Build left column: Core Metrics (compact format)
    {
        echo "${CYAN}${BOLD}🔢 Core Metrics${NC}"
        printf "   %-20s %s\n" "Total Commits:" "${YELLOW}$TOTAL_COMMITS${NC} ($COMMITS_PER_DAY/day)"
        printf "   %-20s %s\n" "Active Days:" "${GREEN}$ACTIVE_DAYS${NC} (${PCT_ACTIVE}% of calendar)"
        printf "   %-20s %s\n" "Commits/Week:" "$COMMITS_PER_WEEK"
        printf "   %-20s %s\n" "Longest Streak:" "${GREEN}$LONGEST_STREAK days${NC}"
        printf "   %-20s %s\n" "Work Sessions:" "$sessions (>2hr gap)"
        printf "   %-20s %s\n" "Avg Session:" "${avg_min} min (~${AVG_HRS} hrs)"
    } > "$TMP_DIR/left_col.txt"

    # Build right column: Codebase Composition (compact format)
    # NOTE: Excludes lib/, build/, .venv/, and other non-project directories
    {
        echo "${CYAN}${BOLD}📁 Codebase Composition${NC}"
        [[ $CPP_LOC -gt 0 ]] && printf "   %-16s %4d files  %6d lines\n" "C++ Source:" "$CPP_COUNT" "$CPP_LOC"
        [[ $H_LOC -gt 0 ]] && printf "   %-16s %4d files  %6d lines\n" "C++ Headers:" "$H_COUNT" "$H_LOC"
        [[ $XML_LOC -gt 0 ]] && printf "   %-16s %4d files  %6d lines\n" "XML Layouts:" "$XML_COUNT" "$XML_LOC"
        [[ $TEST_LOC -gt 0 ]] && printf "   %-16s %4d files  %6d lines\n" "Tests:" "$TEST_COUNT" "$TEST_LOC"
        [[ $PY_LOC -gt 0 ]] && printf "   %-16s %4d files  %6d lines\n" "Python:" "$PY_COUNT" "$PY_LOC"
        [[ $SH_LOC -gt 0 ]] && printf "   %-16s %4d files  %6d lines\n" "Shell:" "$SH_COUNT" "$SH_LOC"
        [[ $MK_LOC -gt 0 ]] && printf "   %-16s %4d files  %6d lines\n" "Makefiles:" "$MK_COUNT" "$MK_LOC"
        echo "   ────────────────────────────────────"
        printf "   ${BOLD}%-16s %4d files  %6d lines${NC}\n" "TOTAL:" "$TOTAL_FILES" "$TOTAL_LOC"
        echo "   ${DIM}(excludes lib/, build/, .venv/)${NC}"
    } > "$TMP_DIR/right_col.txt"

    print_two_columns_files "$TMP_DIR/left_col.txt" "$TMP_DIR/right_col.txt"
    echo ""
else
    # Single column: Core Metrics table
    echo "${CYAN}${BOLD}🔢 Core Metrics${NC}"
    echo "   ┌────────────────────────┬───────────────┬────────────────────────────┐"
    echo "   │ Metric                 │ Value         │ Notes                      │"
    echo "   ├────────────────────────┼───────────────┼────────────────────────────┤"
    printf "   │ %-22s │ %13s │ %-26s │\n" "Total Commits" "$TOTAL_COMMITS" "$COMMITS_PER_DAY commits/active day"
    printf "   │ %-22s │ %13s │ %-26s │\n" "Active Coding Days" "$ACTIVE_DAYS" "${PCT_ACTIVE}% of calendar days"
    printf "   │ %-22s │ %13s │ %-26s │\n" "Commits/Week" "$COMMITS_PER_WEEK" "Avg weekly velocity"
    printf "   │ %-22s │ %13s │ %-26s │\n" "Longest Streak" "$LONGEST_STREAK days" "Consecutive days"
    printf "   │ %-22s │ %13s │ %-26s │\n" "Work Sessions" "$sessions" ">2hr gap = new session"
    printf "   │ %-22s │ %13s │ %-26s │\n" "Avg Session Length" "${avg_min} min" "~${AVG_HRS} hours"
    echo "   └────────────────────────┴───────────────┴────────────────────────────┘"
    echo ""

    # Single column: Codebase composition table
    # NOTE: Excludes lib/, build/, .venv/, and other non-project directories
    if [[ $TOTAL_LOC -gt 0 ]]; then
        echo "${CYAN}${BOLD}📁 Codebase Composition${NC}"
        echo "   ┌────────────────────────┬───────────────┬───────────────┐"
        echo "   │ File Type              │ Count         │ Lines         │"
        echo "   ├────────────────────────┼───────────────┼───────────────┤"
        [[ $CPP_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "C++ Source (.cpp)" "$CPP_COUNT" "$CPP_LOC"
        [[ $H_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "C++ Headers (.h)" "$H_COUNT" "$H_LOC"
        [[ $XML_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "XML Layouts (.xml)" "$XML_COUNT" "$XML_LOC"
        [[ $TEST_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "Tests" "$TEST_COUNT" "$TEST_LOC"
        [[ $PY_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "Python (.py)" "$PY_COUNT" "$PY_LOC"
        [[ $SH_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "Shell (.sh)" "$SH_COUNT" "$SH_LOC"
        [[ $MK_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "Makefiles" "$MK_COUNT" "$MK_LOC"
        echo "   ├────────────────────────┼───────────────┼───────────────┤"
        printf "   │ ${BOLD}%-22s${NC} │ ${BOLD}%13s${NC} │ ${BOLD}%13s${NC} │\n" "TOTAL" "$TOTAL_FILES" "$TOTAL_LOC"
        echo "   └────────────────────────┴───────────────┴───────────────┘"
        echo "   ${DIM}(excludes lib/, build/, .venv/)${NC}"
        echo ""
    fi
fi

# Time investment (always single column - important info)
echo "${CYAN}${BOLD}⏰ Time Investment Estimate${NC}"
echo "   Commit span analysis:      ${YELLOW}~${total_hrs} hours${NC} (lower bound)"
echo "   Adjusted coding time:      ${YELLOW}~${ADJUSTED_HRS} hours${NC} (1.5× - includes debugging)"
echo "   Total effort estimate:     ${YELLOW}~${TOTAL_EFFORT} hours${NC} (3× - includes research/design)"
echo "   ${BOLD}Weekly Average:             ~${WEEKLY_LOW}-${WEEKLY_HIGH} hours/week${NC}"
echo ""

# Two-column section: Day of Week + Peak Hours
if $USE_TWO_COLS; then
    # Build left column: Activity by day of week
    echo "${CYAN}${BOLD}📆 Activity by Day of Week${NC}" > "$TMP_DIR/left_col.txt"
    MAX_DOW=$(awk '{print $1}' "$TMP_DIR/dow.txt" | sort -rn | head -1)
    while read count dow; do
        bar_len=$((count * 20 / MAX_DOW))  # Shorter bars for column layout
        bar=$(printf '%*s' "$bar_len" '' | tr ' ' '█')
        case $dow in
            1) dow_name="Mon" ;; 2) dow_name="Tue" ;; 3) dow_name="Wed" ;;
            4) dow_name="Thu" ;; 5) dow_name="Fri" ;; 6) dow_name="Sat" ;; 7) dow_name="Sun" ;;
        esac
        printf "   %s %-20s %3d\n" "$dow_name" "$bar" "$count" >> "$TMP_DIR/left_col.txt"
    done < "$TMP_DIR/dow.txt"

    # Build right column: Peak hours
    echo "${CYAN}${BOLD}🕐 Peak Hours (Top 5)${NC}" > "$TMP_DIR/right_col.txt"
    sort -k1rn "$TMP_DIR/hours.txt" | head -5 | while read count hour; do
        printf "   %s:00  %3d commits\n" "$hour" "$count" >> "$TMP_DIR/right_col.txt"
    done

    print_two_columns_files "$TMP_DIR/left_col.txt" "$TMP_DIR/right_col.txt"
    echo ""
else
    # Single column fallback
    echo "${CYAN}${BOLD}📆 Activity by Day of Week${NC}"
    MAX_DOW=$(awk '{print $1}' "$TMP_DIR/dow.txt" | sort -rn | head -1)
    while read count dow; do
        bar_len=$((count * 30 / MAX_DOW))
        bar=$(printf '%*s' "$bar_len" '' | tr ' ' '█')
        case $dow in
            1) dow_name="Mon" ;; 2) dow_name="Tue" ;; 3) dow_name="Wed" ;;
            4) dow_name="Thu" ;; 5) dow_name="Fri" ;; 6) dow_name="Sat" ;; 7) dow_name="Sun" ;;
        esac
        printf "   %s %-30s %3d\n" "$dow_name" "$bar" "$count"
    done < "$TMP_DIR/dow.txt"
    echo ""

    echo "${CYAN}${BOLD}🕐 Peak Hours (Top 5)${NC}"
    sort -k1rn "$TMP_DIR/hours.txt" | head -5 | while read count hour; do
        printf "   %s:00  %3d commits\n" "$hour" "$count"
    done
    echo ""
fi

# Two-column section: Authorship + Hall of Fame
if $USE_TWO_COLS; then
    # Build left column: Authorship
    echo "${CYAN}${BOLD}👨‍💻 Authorship${NC}" > "$TMP_DIR/left_col.txt"
    while read count author; do
        pct=$((count * 100 / TOTAL_COMMITS))
        printf "   %-20s %4d (%d%%)\n" "$author" "$count" "$pct" >> "$TMP_DIR/left_col.txt"
    done < "$TMP_DIR/authors.txt"

    # Build right column: Hall of Fame
    echo "${CYAN}${BOLD}🏆 Hall of Fame${NC}" > "$TMP_DIR/right_col.txt"
    echo "   Most Productive:  ${BEST_DAY}" >> "$TMP_DIR/right_col.txt"
    echo "   Late Night:       ${LATE_NIGHT} commits (${LATE_PCT}%)" >> "$TMP_DIR/right_col.txt"
    echo "   Weekend:          ${WEEKEND} commits (${WEEKEND_PCT}%)" >> "$TMP_DIR/right_col.txt"
    [[ $TOTAL_LOC -gt 0 ]] && echo "   Lines/Day:        ~${LOC_PER_DAY}" >> "$TMP_DIR/right_col.txt"

    print_two_columns_files "$TMP_DIR/left_col.txt" "$TMP_DIR/right_col.txt"
    echo ""
else
    # Single column fallback
    echo "${CYAN}${BOLD}👨‍💻 Authorship${NC}"
    while read count author; do
        pct=$((count * 100 / TOTAL_COMMITS))
        printf "   %-25s %5d commits (%d%%)\n" "$author" "$count" "$pct"
    done < "$TMP_DIR/authors.txt"
    echo ""

    echo "${CYAN}${BOLD}🏆 Hall of Fame${NC}"
    echo "   Most Productive Day:   ${BEST_DAY}"
    echo "   Late Night (00-06):    ${LATE_NIGHT} commits (${LATE_PCT}%)"
    echo "   Weekend Warrior:       ${WEEKEND} commits (${WEEKEND_PCT}%)"
    [[ $TOTAL_LOC -gt 0 ]] && echo "   Lines/Active Day:      ~${LOC_PER_DAY}"
    echo ""
fi

# Two-column section: Top Files + Commit Keywords
if $USE_TWO_COLS; then
    # Build left column: Most changed files
    echo "${CYAN}${BOLD}🔥 Most Modified Files${NC}" > "$TMP_DIR/left_col.txt"
    if [[ -s "$TMP_DIR/top_files.txt" ]]; then
        head -5 "$TMP_DIR/top_files.txt" | while read count file; do
            # Truncate long filenames for column width
            if [[ ${#file} -gt 35 ]]; then
                file="...${file: -32}"
            fi
            printf "   %3d  %s\n" "$count" "$file" >> "$TMP_DIR/left_col.txt"
        done
    else
        echo "   (no data)" >> "$TMP_DIR/left_col.txt"
    fi

    # Build right column: Commit keywords
    echo "${CYAN}${BOLD}💬 Top Commit Keywords${NC}" > "$TMP_DIR/right_col.txt"
    head -6 "$TMP_DIR/words.txt" | while read count word; do
        printf "   %-12s %4d\n" "$word" "$count" >> "$TMP_DIR/right_col.txt"
    done

    print_two_columns_files "$TMP_DIR/left_col.txt" "$TMP_DIR/right_col.txt"
    echo ""
else
    # Single column fallback
    if [[ -s "$TMP_DIR/top_files.txt" ]]; then
        echo "${CYAN}${BOLD}🔥 Most Frequently Modified Files${NC}"
        head -5 "$TMP_DIR/top_files.txt" | while read count file; do
            printf "   %4d  %s\n" "$count" "$file"
        done
        echo ""
    fi

    echo "${CYAN}${BOLD}💬 Top Commit Keywords${NC}"
    head -8 "$TMP_DIR/words.txt" | while read count word; do
        printf "   %-15s %4d\n" "$word" "$count"
    done
    echo ""
fi

# Velocity summary (always single column - summary)
echo "${CYAN}${BOLD}📈 Velocity Summary${NC}"
echo "   Commits/Week:          $COMMITS_PER_WEEK"
echo "   Commits/Session:       $COMMITS_PER_SESSION"
[[ $TOTAL_LOC -gt 0 ]] && echo "   Source LOC:            $TOTAL_LOC"
echo ""

# ============================================================================
# ACTIVITY TIMELINE CHARTS
# ============================================================================

# Generate VERTICAL activity chart for a time period
# $1 = period name (unused; callers pass it positionally)
# $2 = git --since argument
# $3 = max height (default 8)
# $4 = grouping: "month" | "week" (default "week")
# $5 = expected_items (for centering) - optional
generate_vertical_chart() {
    local since_date=$2
    local max_height=${3:-8}
    local grouping=${4:-"week"}
    local expected_items=${5:-0}  # 0 = no centering
    local bar_char="█"

    local data=""
    local labels=()
    local counts=()
    local max_count=0

    if [[ "$grouping" == "month" ]]; then
        # Group by month, label as "Oct", "Nov", etc.
        data=$(git log --format="%cd" --date=format:'%Y-%m' --since="$since_date" --no-merges 2>/dev/null | \
            sort | uniq -c | sort -k2)

        if [[ -z "$data" ]]; then
            echo "   (no commits in period)"
            return
        fi

        while read count ym; do
            local month_num="${ym#*-}"
            local month_name=""
            case "$month_num" in
                01) month_name="Jan" ;; 02) month_name="Feb" ;; 03) month_name="Mar" ;;
                04) month_name="Apr" ;; 05) month_name="May" ;; 06) month_name="Jun" ;;
                07) month_name="Jul" ;; 08) month_name="Aug" ;; 09) month_name="Sep" ;;
                10) month_name="Oct" ;; 11) month_name="Nov" ;; 12) month_name="Dec" ;;
            esac
            labels+=("$month_name")
            counts+=("$count")
            [[ $count -gt $max_count ]] && max_count=$count
        done <<< "$data"
    else
        # Group by week, label as "Mon D" (week start date)
        # Get commits with their week-start Monday date
        data=$(git log --format="%cd" --date=format:'%Y-%m-%d' --since="$since_date" --no-merges 2>/dev/null | \
            python3 -c "
import sys
from datetime import datetime, timedelta
from collections import Counter

dates = []
for line in sys.stdin:
    line = line.strip()
    if line:
        dates.append(line)

# Group by week (Monday start)
week_counts = Counter()
week_mondays = {}
for d in dates:
    dt = datetime.strptime(d, '%Y-%m-%d')
    monday = dt - timedelta(days=dt.weekday())
    week_key = monday.strftime('%Y-%m-%d')
    week_counts[week_key] += 1
    week_mondays[week_key] = monday

for week_key in sorted(week_counts.keys()):
    monday = week_mondays[week_key]
    label = monday.strftime('%b %-d')  # 'Dec 2', 'Dec 9', etc.
    print(f'{week_counts[week_key]} {label}')
" 2>/dev/null)

        if [[ -z "$data" ]]; then
            echo "   (no commits in period)"
            return
        fi

        while read count label_rest; do
            labels+=("$label_rest")
            counts+=("$count")
            [[ $count -gt $max_count ]] && max_count=$count
        done <<< "$data"
    fi

    [[ $max_count -eq 0 ]] && max_count=1
    local num_items=${#labels[@]}

    # Calculate left padding for centering if expected_items specified
    local left_pad=""
    if [[ $expected_items -gt 0 ]] && [[ $num_items -lt $expected_items ]]; then
        local missing=$((expected_items - num_items))
        local pad_cols=$((missing / 2))
        # Each column is 3 chars (bar + 2 spaces)
        left_pad=$(printf '%*s' "$((pad_cols * 3))" '')
    fi

    # Build the chart row by row (top to bottom)
    for ((row=max_height; row>=1; row--)); do
        printf "   %s" "$left_pad"
        for ((col=0; col<num_items; col++)); do
            local val=${counts[$col]}
            local bar_height=$(( (val * max_height + max_count - 1) / max_count ))  # Ceiling
            [[ $val -eq 0 ]] && bar_height=0
            if [[ $bar_height -ge $row ]]; then
                printf "%s  " "$bar_char"
            else
                printf "   "
            fi
        done
        # Show scale on right edge for top and middle rows
        if [[ $row -eq $max_height ]]; then
            printf " ${DIM}%d${NC}" "$max_count"
        elif [[ $row -eq $((max_height / 2)) ]]; then
            printf " ${DIM}%d${NC}" "$((max_count / 2))"
        fi
        echo ""
    done

    # Vertical labels at bottom - each row is one character of the label
    # Build label strings first
    local formatted_labels=()
    if [[ "$grouping" == "month" ]]; then
        # Month labels: "Oct", "Nov", "Dec"
        for ((col=0; col<num_items; col++)); do
            formatted_labels+=("${labels[$col]}")
        done
    else
        # Week labels: show month initial only on first col and when month changes
        local prev_month=""
        for ((col=0; col<num_items; col++)); do
            local lbl="${labels[$col]}"
            local month_part="${lbl%% *}"   # "Dec" from "Dec 2"
            local day_part="${lbl#* }"      # "2" from "Dec 2"
            local month_initial="${month_part:0:1}"

            # Show month initial only when month changes
            if [[ "$month_part" != "$prev_month" ]]; then
                formatted_labels+=("${month_initial}${day_part}")
            else
                formatted_labels+=("${day_part}")
            fi
            prev_month="$month_part"
        done
    fi

    # Find max label length
    local max_label_len=0
    for lbl in "${formatted_labels[@]}"; do
        [[ ${#lbl} -gt $max_label_len ]] && max_label_len=${#lbl}
    done

    # Print vertical labels (one row per character position)
    for ((char_pos=0; char_pos<max_label_len; char_pos++)); do
        printf "   %s" "$left_pad"
        for ((col=0; col<num_items; col++)); do
            local lbl="${formatted_labels[$col]}"
            if [[ $char_pos -lt ${#lbl} ]]; then
                printf "%s  " "${lbl:$char_pos:1}"
            else
                printf "   "
            fi
        done
        echo ""
    done
}

# For three charts, we need more width
THREE_COL_MIN_WIDTH=140
USE_THREE_COLS=false
if [[ $TERM_WIDTH -ge $THREE_COL_MIN_WIDTH ]]; then
    USE_THREE_COLS=true
fi

echo "${CYAN}${BOLD}📊 Commit Activity Timeline${NC}"

# Chart height based on terminal - taller for wider terminals
CHART_HEIGHT=8
[[ $TERM_WIDTH -ge 120 ]] && CHART_HEIGHT=10
[[ $TERM_WIDTH -lt 80 ]] && CHART_HEIGHT=6

if $USE_THREE_COLS; then
    # Three columns side by side
    CHART_COL_WIDTH=$(( (TERM_WIDTH - 12) / 3 ))

    # Build year chart (grouped by month) - expect 12 months
    {
        echo "${PURPLE}  📅 1 Year (by month)${NC}"
        generate_vertical_chart "1 year" "12 months ago" $CHART_HEIGHT "month" 12
    } > "$TMP_DIR/chart_year.txt"

    # Build quarter chart (grouped by week) - expect ~13 weeks
    {
        echo "${PURPLE}  📅 3 Months (by week)${NC}"
        generate_vertical_chart "3 months" "3 months ago" $CHART_HEIGHT "week" 13
    } > "$TMP_DIR/chart_quarter.txt"

    # Build month chart (grouped by week) - expect ~5 weeks
    {
        echo "${PURPLE}  📅 1 Month (by week)${NC}"
        generate_vertical_chart "1 month" "1 month ago" $CHART_HEIGHT "week" 5
    } > "$TMP_DIR/chart_month.txt"

    # Print three columns
    year_lines=()
    quarter_lines=()
    month_lines=()
    while IFS= read -r line || [[ -n "$line" ]]; do
        year_lines+=("$line")
    done < "$TMP_DIR/chart_year.txt"
    while IFS= read -r line || [[ -n "$line" ]]; do
        quarter_lines+=("$line")
    done < "$TMP_DIR/chart_quarter.txt"
    while IFS= read -r line || [[ -n "$line" ]]; do
        month_lines+=("$line")
    done < "$TMP_DIR/chart_month.txt"

    max_lines=${#year_lines[@]}
    [[ ${#quarter_lines[@]} -gt $max_lines ]] && max_lines=${#quarter_lines[@]}
    [[ ${#month_lines[@]} -gt $max_lines ]] && max_lines=${#month_lines[@]}

    for ((i=0; i<max_lines; i++)); do
        left="${year_lines[$i]:-}"
        mid="${quarter_lines[$i]:-}"
        right="${month_lines[$i]:-}"

        left_plain=$(echo "$left" | sed 's/\x1b\[[0-9;]*m//g')
        mid_plain=$(echo "$mid" | sed 's/\x1b\[[0-9;]*m//g')

        pad_left=$((CHART_COL_WIDTH - ${#left_plain}))
        pad_mid=$((CHART_COL_WIDTH - ${#mid_plain}))
        [[ $pad_left -lt 0 ]] && pad_left=0
        [[ $pad_mid -lt 0 ]] && pad_mid=0

        printf "%b%*s  %b%*s  %b\n" "$left" "$pad_left" "" "$mid" "$pad_mid" "" "$right"
    done
    echo ""

elif $USE_TWO_COLS; then
    # Two columns: year on left, quarter+month stacked on right
    {
        echo "${PURPLE}  📅 1 Year (by month)${NC}"
        generate_vertical_chart "1 year" "12 months ago" $CHART_HEIGHT "month" 12
    } > "$TMP_DIR/chart_year.txt"

    {
        echo "${PURPLE}  📅 3 Months (by week)${NC}"
        generate_vertical_chart "3 months" "3 months ago" $((CHART_HEIGHT - 2)) "week" 13
        echo ""
        echo "${PURPLE}  📅 1 Month (by week)${NC}"
        generate_vertical_chart "1 month" "1 month ago" $((CHART_HEIGHT - 2)) "week" 5
    } > "$TMP_DIR/chart_recent.txt"

    print_two_columns_files "$TMP_DIR/chart_year.txt" "$TMP_DIR/chart_recent.txt"
    echo ""
else
    # Single column: stack all three vertically
    echo "${PURPLE}  📅 1 Year (by month)${NC}"
    generate_vertical_chart "1 year" "12 months ago" $CHART_HEIGHT "month" 12
    echo ""

    echo "${PURPLE}  📅 3 Months (by week)${NC}"
    generate_vertical_chart "3 months" "3 months ago" $CHART_HEIGHT "week" 13
    echo ""

    echo "${PURPLE}  📅 1 Month (by week)${NC}"
    generate_vertical_chart "1 month" "1 month ago" $CHART_HEIGHT "week" 5
    echo ""
fi

echo "${DIM}Generated by git-stats.sh on $(date '+%Y-%m-%d %H:%M:%S')${NC}"
