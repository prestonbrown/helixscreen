#!/usr/bin/env bash
# Copyright 2025 356C LLC
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

# Colors for terminal output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m' # No Color

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
        local left_plain=$(echo -e "$left_line" | sed 's/\x1b\[[0-9;]*m//g')
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
trap "rm -rf $TMP_DIR" EXIT

echo -e "${CYAN}${BOLD}Analyzing git history for ${REPO_NAME}...${NC}"

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
read sessions avg_min total_hrs <<< $(python3 << 'PYEOF'
with open('/tmp/ts_temp.txt', 'w') as f:
    import sys
    for line in open(sys.argv[1] if len(sys.argv) > 1 else '/dev/stdin'):
        f.write(line)
PYEOF
python3 << PYEOF
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
)

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

# Most changed files
git log --name-only --format="" --no-merges 2>/dev/null | grep -E "\.(cpp|h|xml)$" | grep -v "^lib/" | sort | uniq -c | sort -rn | head -10 > "$TMP_DIR/top_files.txt"

# Late night and weekend
LATE_NIGHT=$(git log --format="%cd" --date=format:'%H' --no-merges | awk '$1 >= 0 && $1 < 6 {count++} END {print count+0}')
WEEKEND=$(git log --format="%cd" --date=format:'%u' --no-merges | awk '$1 == 6 || $1 == 7 {count++} END {print count+0}')

# Source code stats (adjust paths as needed)
CPP_LOC=0; H_LOC=0; XML_LOC=0
CPP_COUNT=0; H_COUNT=0; XML_COUNT=0

if [[ -d "$REPO_ROOT/src" ]]; then
    CPP_COUNT=$(find "$REPO_ROOT/src" -name "*.cpp" 2>/dev/null | wc -l | tr -d ' ')
    CPP_LOC=$(find "$REPO_ROOT/src" -name "*.cpp" -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
fi
if [[ -d "$REPO_ROOT/include" ]]; then
    H_COUNT=$(find "$REPO_ROOT/include" -name "*.h" 2>/dev/null | wc -l | tr -d ' ')
    H_LOC=$(find "$REPO_ROOT/include" -name "*.h" -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
fi
if [[ -d "$REPO_ROOT/ui_xml" ]]; then
    XML_COUNT=$(find "$REPO_ROOT/ui_xml" -name "*.xml" 2>/dev/null | wc -l | tr -d ' ')
    XML_LOC=$(find "$REPO_ROOT/ui_xml" -name "*.xml" -exec cat {} + 2>/dev/null | wc -l | tr -d ' ')
fi

TOTAL_LOC=$((CPP_LOC + H_LOC + XML_LOC))
TOTAL_FILES=$((CPP_COUNT + H_COUNT + XML_COUNT))

# Commit message word frequency
cut -d'|' -f5 "$TMP_DIR/commits.txt" | tr '[:upper:]' '[:lower:]' | tr -cs '[:alpha:]' '\n' | grep -v "^$" | sort | uniq -c | sort -rn | head -10 > "$TMP_DIR/words.txt"

# ============================================================================
# OUTPUT
# ============================================================================

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

echo -e "${BOLD}╔${HORIZ_LINE}╗${NC}"
echo -e "${BOLD}║${PADDING_L_STR}${PURPLE}${TITLE}${NC}${BOLD}${PADDING_R_STR}║${NC}"
echo -e "${BOLD}╚${HORIZ_LINE}╝${NC}"
echo ""

# Project span (always single column - header section)
echo -e "${CYAN}${BOLD}📅 Project Timeline${NC}"
echo -e "   Start Date:     ${GREEN}$FIRST_COMMIT_DATE${NC}"
echo -e "   Latest Commit:  ${GREEN}$LAST_COMMIT_DATE${NC}"
echo -e "   Duration:       ${YELLOW}$PROJECT_DAYS days${NC} (${PROJECT_WEEKS} weeks)"
echo ""

# Core metrics table (always full width - too wide for columns)
echo -e "${CYAN}${BOLD}🔢 Core Metrics${NC}"
echo -e "   ┌────────────────────────┬───────────────┬────────────────────────────┐"
echo -e "   │ Metric                 │ Value         │ Notes                      │"
echo -e "   ├────────────────────────┼───────────────┼────────────────────────────┤"
printf "   │ %-22s │ %13s │ %-26s │\n" "Total Commits" "$TOTAL_COMMITS" "$COMMITS_PER_DAY commits/active day"
printf "   │ %-22s │ %13s │ %-26s │\n" "Active Coding Days" "$ACTIVE_DAYS" "${PCT_ACTIVE}% of calendar days"
printf "   │ %-22s │ %13s │ %-26s │\n" "Commits/Week" "$COMMITS_PER_WEEK" "Avg weekly velocity"
printf "   │ %-22s │ %13s │ %-26s │\n" "Longest Streak" "$LONGEST_STREAK days" "Consecutive days"
printf "   │ %-22s │ %13s │ %-26s │\n" "Work Sessions" "$sessions" ">2hr gap = new session"
printf "   │ %-22s │ %13s │ %-26s │\n" "Avg Session Length" "${avg_min} min" "~${AVG_HRS} hours"
echo -e "   └────────────────────────┴───────────────┴────────────────────────────┘"
echo ""

# Codebase composition (always full width - table format)
if [[ $TOTAL_LOC -gt 0 ]]; then
    echo -e "${CYAN}${BOLD}📁 Codebase Composition${NC}"
    echo -e "   ┌────────────────────────┬───────────────┬───────────────┐"
    echo -e "   │ File Type              │ Count         │ Lines         │"
    echo -e "   ├────────────────────────┼───────────────┼───────────────┤"
    [[ $CPP_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "C++ Source (.cpp)" "$CPP_COUNT" "$CPP_LOC"
    [[ $H_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "C++ Headers (.h)" "$H_COUNT" "$H_LOC"
    [[ $XML_LOC -gt 0 ]] && printf "   │ %-22s │ %13s │ %13s │\n" "XML Layouts (.xml)" "$XML_COUNT" "$XML_LOC"
    echo -e "   ├────────────────────────┼───────────────┼───────────────┤"
    printf "   │ ${BOLD}%-22s${NC} │ ${BOLD}%13s${NC} │ ${BOLD}%13s${NC} │\n" "TOTAL" "$TOTAL_FILES" "$TOTAL_LOC"
    echo -e "   └────────────────────────┴───────────────┴───────────────┘"
    echo ""
fi

# Time investment (always single column - important info)
echo -e "${CYAN}${BOLD}⏰ Time Investment Estimate${NC}"
echo -e "   Commit span analysis:      ${YELLOW}~${total_hrs} hours${NC} (lower bound)"
echo -e "   Adjusted coding time:      ${YELLOW}~${ADJUSTED_HRS} hours${NC} (1.5× - includes debugging)"
echo -e "   Total effort estimate:     ${YELLOW}~${TOTAL_EFFORT} hours${NC} (3× - includes research/design)"
echo -e "   ${BOLD}Weekly Average:             ~${WEEKLY_LOW}-${WEEKLY_HIGH} hours/week${NC}"
echo ""

# Two-column section: Day of Week + Peak Hours
if $USE_TWO_COLS; then
    # Build left column: Activity by day of week
    echo -e "${CYAN}${BOLD}📆 Activity by Day of Week${NC}" > "$TMP_DIR/left_col.txt"
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
    echo -e "${CYAN}${BOLD}🕐 Peak Hours (Top 5)${NC}" > "$TMP_DIR/right_col.txt"
    sort -k1rn "$TMP_DIR/hours.txt" | head -5 | while read count hour; do
        printf "   %s:00  %3d commits\n" "$hour" "$count" >> "$TMP_DIR/right_col.txt"
    done

    print_two_columns_files "$TMP_DIR/left_col.txt" "$TMP_DIR/right_col.txt"
    echo ""
else
    # Single column fallback
    echo -e "${CYAN}${BOLD}📆 Activity by Day of Week${NC}"
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

    echo -e "${CYAN}${BOLD}🕐 Peak Hours (Top 5)${NC}"
    sort -k1rn "$TMP_DIR/hours.txt" | head -5 | while read count hour; do
        printf "   %s:00  %3d commits\n" "$hour" "$count"
    done
    echo ""
fi

# Two-column section: Authorship + Hall of Fame
if $USE_TWO_COLS; then
    # Build left column: Authorship
    echo -e "${CYAN}${BOLD}👨‍💻 Authorship${NC}" > "$TMP_DIR/left_col.txt"
    while read count author; do
        pct=$((count * 100 / TOTAL_COMMITS))
        printf "   %-20s %4d (%d%%)\n" "$author" "$count" "$pct" >> "$TMP_DIR/left_col.txt"
    done < "$TMP_DIR/authors.txt"

    # Build right column: Hall of Fame
    echo -e "${CYAN}${BOLD}🏆 Hall of Fame${NC}" > "$TMP_DIR/right_col.txt"
    echo "   Most Productive:  ${BEST_DAY}" >> "$TMP_DIR/right_col.txt"
    echo "   Late Night:       ${LATE_NIGHT} commits (${LATE_PCT}%)" >> "$TMP_DIR/right_col.txt"
    echo "   Weekend:          ${WEEKEND} commits (${WEEKEND_PCT}%)" >> "$TMP_DIR/right_col.txt"
    [[ $TOTAL_LOC -gt 0 ]] && echo "   Lines/Day:        ~${LOC_PER_DAY}" >> "$TMP_DIR/right_col.txt"

    print_two_columns_files "$TMP_DIR/left_col.txt" "$TMP_DIR/right_col.txt"
    echo ""
else
    # Single column fallback
    echo -e "${CYAN}${BOLD}👨‍💻 Authorship${NC}"
    while read count author; do
        pct=$((count * 100 / TOTAL_COMMITS))
        printf "   %-25s %5d commits (%d%%)\n" "$author" "$count" "$pct"
    done < "$TMP_DIR/authors.txt"
    echo ""

    echo -e "${CYAN}${BOLD}🏆 Hall of Fame${NC}"
    echo -e "   Most Productive Day:   ${BEST_DAY}"
    echo -e "   Late Night (00-06):    ${LATE_NIGHT} commits (${LATE_PCT}%)"
    echo -e "   Weekend Warrior:       ${WEEKEND} commits (${WEEKEND_PCT}%)"
    [[ $TOTAL_LOC -gt 0 ]] && echo -e "   Lines/Active Day:      ~${LOC_PER_DAY}"
    echo ""
fi

# Two-column section: Top Files + Commit Keywords
if $USE_TWO_COLS; then
    # Build left column: Most changed files
    echo -e "${CYAN}${BOLD}🔥 Most Modified Files${NC}" > "$TMP_DIR/left_col.txt"
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
    echo -e "${CYAN}${BOLD}💬 Top Commit Keywords${NC}" > "$TMP_DIR/right_col.txt"
    head -6 "$TMP_DIR/words.txt" | while read count word; do
        printf "   %-12s %4d\n" "$word" "$count" >> "$TMP_DIR/right_col.txt"
    done

    print_two_columns_files "$TMP_DIR/left_col.txt" "$TMP_DIR/right_col.txt"
    echo ""
else
    # Single column fallback
    if [[ -s "$TMP_DIR/top_files.txt" ]]; then
        echo -e "${CYAN}${BOLD}🔥 Most Frequently Modified Files${NC}"
        head -5 "$TMP_DIR/top_files.txt" | while read count file; do
            printf "   %4d  %s\n" "$count" "$file"
        done
        echo ""
    fi

    echo -e "${CYAN}${BOLD}💬 Top Commit Keywords${NC}"
    head -8 "$TMP_DIR/words.txt" | while read count word; do
        printf "   %-15s %4d\n" "$word" "$count"
    done
    echo ""
fi

# Velocity summary (always single column - summary)
echo -e "${CYAN}${BOLD}📈 Velocity Summary${NC}"
echo -e "   Commits/Week:          $COMMITS_PER_WEEK"
echo -e "   Commits/Session:       $COMMITS_PER_SESSION"
[[ $TOTAL_LOC -gt 0 ]] && echo -e "   Source LOC:            $TOTAL_LOC"
echo ""

echo -e "${DIM}Generated by git-stats.sh on $(date '+%Y-%m-%d %H:%M:%S')${NC}"
