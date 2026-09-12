#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: display_numbering.h's conversions must not be reimplemented by hand.
#
# include/display_numbering.h keeps the storage-index-to-display-number `+ 1`
# in exactly one function (helix::ui::lane_number()) and spells a gcode
# tool's label through exactly one function (helix::ui::tool_label()). Both
# are one line to rebuild by hand - a "T" glued to a number, or a raw `+ 1`
# on a slot/lane/unit index - and a hand-built copy drifts the moment one
# call site changes and its siblings do not.
#
# Two shapes are forbidden outside the allowlist below:
#
#   1. A hand-built tool label: "T%d", "T{}", or "T" + std::to_string(...).
#      Use helix::ui::tool_label(n) instead.
#   2. A bare `+ 1` on an identifier containing slot_index, lane or
#      unit_index, inside a formatting call (fmt::format / std::to_string /
#      snprintf / sprintf). Use helix::ui::lane_number(n) instead.
#
# Both shapes are matched on a single physical line: the arithmetic and the
# formatting call that consumes it must appear together, which is how every
# real call site in this tree is written.
#
# What must stay quiet:
#   - Anything inside an spdlog:: call - a developer diagnostic, not a label
#     shown to a user.
#   - src/printer/filament_slot_override_store.cpp, in full - it implements
#     the published wire format in docs/specs/filament_slots.md, where the
#     outer record key is deliberately 1-based text (laneN) over an inner
#     0-based field.
#   - src/rendering/gcode_tool_remapper.cpp and src/printer/ams_backend_*.cpp,
#     in full, for shape 1 only - a T<n> built there is gcode sent to
#     firmware, not a label. Shape 2 is not known to be wire-format-only in
#     these files, so it stays in scope for them.
#   - src/ui/display_numbering.cpp - the canonical implementation itself.
#
# Escape hatch for a genuine one-off: `// DISPLAY_NUMBERING_OK: <reason>` on
# the offending line.
#
# Usage:
#   scripts/check_tool_labels.sh                    # scans src/ and include/
#   SCAN_ROOT=<dir> scripts/check_tool_labels.sh    # scans every .cpp/.h under <dir>

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

OPTOUT="DISPLAY_NUMBERING_OK"

# File-scope exclusions, per shape - see header. A path is checked by its
# tail, so it matches whether it arrives as a repo-relative path (the default
# scan) or a bare filename under a fixture directory (SCAN_ROOT).
is_shape1_excluded() {
    case "$1" in
        */filament_slot_override_store.cpp | filament_slot_override_store.cpp) return 0 ;;
        */gcode_tool_remapper.cpp | gcode_tool_remapper.cpp) return 0 ;;
        */ams_backend_*.cpp | ams_backend_*.cpp) return 0 ;;
        */display_numbering.cpp | display_numbering.cpp) return 0 ;;
        *) return 1 ;;
    esac
}

is_shape2_excluded() {
    case "$1" in
        */filament_slot_override_store.cpp | filament_slot_override_store.cpp) return 0 ;;
        */display_numbering.cpp | display_numbering.cpp) return 0 ;;
        *) return 1 ;;
    esac
}

# Files in scope. SCAN_ROOT (set by the meta-tests below) walks every
# .cpp/.h under a fixture directory. The default scan is git ls-files over
# src/ and include/, which keeps generated and gitignored trees out with no
# exclusion list to maintain, and reaches a new, not-yet-staged file the
# same way a committed one is reached.
lint_files() {
    if [ -n "${SCAN_ROOT:-}" ]; then
        find "$SCAN_ROOT" -type f \( -name '*.cpp' -o -name '*.h' \)
        return
    fi
    git ls-files --cached --others --exclude-standard src include | grep -E '\.(cpp|h)$'
}

# Print `file:line: text` for each line of $2.. matching the ERE in $1,
# ignoring comments and any line carrying the opt-out token. Comments are
# stripped with a small block-comment state machine, the same shape the RTTI
# gate uses, so a doc comment describing the real helper is never mistaken
# for a call to a forbidden one. The pattern travels through the
# environment, not `awk -v`: -v runs escape processing over the value, which
# turns `\.` into a match-anything `.` and warns on every escape. `[[:space:]]`
# rather than `\s` for the same reason RTTI's gate uses it: `\s` is a GNU
# extension mawk (Debian's default awk) does not support.
code_offenders() {
    local pat="$1"
    shift
    TOOL_LABEL_PAT="$pat" TOOL_LABEL_OPTOUT="$OPTOUT" awk '
        BEGIN { pat = ENVIRON["TOOL_LABEL_PAT"]; optout = ENVIRON["TOOL_LABEL_OPTOUT"] }
        FNR == 1 { in_block = 0 }
        index($0, optout) > 0 { next }
        {
            line = $0
            code = ""
            if (in_block) {
                i = index(line, "*/")
                if (i == 0) next
                line = substr(line, i + 2)
                in_block = 0
            }
            while (1) {
                b = index(line, "/*")
                l = index(line, "//")
                if (l > 0 && (b == 0 || l < b)) {
                    code = code substr(line, 1, l - 1)
                    break
                }
                if (b == 0) { code = code line; break }
                code = code substr(line, 1, b - 1)
                rest = substr(line, b + 2)
                e = index(rest, "*/")
                if (e == 0) { in_block = 1; break }
                line = substr(rest, e + 2)
            }
            if (code ~ pat) print FILENAME ":" FNR ": " $0
        }
    ' "$@"
}

# Shape 1: a literal "T%d"/"T{}" format string, or "T" string-concatenated
# with std::to_string(...).
tool_label_pattern() {
    printf '%s' '"T%d"|"T\{\}"|"T"[[:space:]]*\+[[:space:]]*std::to_string[[:space:]]*\('
}

# Shape 2: identifier + 1 (any spacing), the identifier containing
# slot_index, lane or unit_index, inside a formatting call's argument list.
# `[^)]*` stops at the call's own close paren so an unrelated `+ 1` later on
# the line, past a nested call, is not swept in; `([^0-9A-Za-z_]|$)` closes
# the "1" against a following digit so `+ 10`/`+ 15` do not match — mawk has
# no `\b` word-boundary extension to lean on instead.
lane_number_pattern() {
    printf '%s' '(fmt::format|std::to_string|snprintf|sprintf)\([^)]*(slot_index|lane|unit_index)[A-Za-z0-9_]*[[:space:]]*\+[[:space:]]*1([^0-9A-Za-z_]|$)'
}

advice() {
    cat <<'EOF'

include/display_numbering.h owns both conversions:
  hand-built tool label ("T%d", "T{}", "T" + std::to_string(n))
      -> helix::ui::tool_label(n)
  raw index + 1 on a slot/lane/unit index inside a formatting call
      -> helix::ui::lane_number(n)
Genuinely not a display label? Annotate the line: // DISPLAY_NUMBERING_OK: <reason>
EOF
}

main() {
    local f
    local -a shape1_files=() shape2_files=()
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        is_shape1_excluded "$f" || shape1_files+=("$f")
        is_shape2_excluded "$f" || shape2_files+=("$f")
    done < <(lint_files)

    local shape1_hits="" shape2_hits=""
    if [ "${#shape1_files[@]}" -gt 0 ]; then
        shape1_hits=$(code_offenders "$(tool_label_pattern)" "${shape1_files[@]}")
    fi
    if [ "${#shape2_files[@]}" -gt 0 ]; then
        shape2_hits=$(code_offenders "$(lane_number_pattern)" "${shape2_files[@]}")
    fi

    if [ -z "$shape1_hits" ] && [ -z "$shape2_hits" ]; then
        return 0
    fi

    echo "Hand-built tool label or lane/slot offset found:"
    [ -n "$shape1_hits" ] && printf '%s\n' "$shape1_hits"
    [ -n "$shape2_hits" ] && printf '%s\n' "$shape2_hits"
    advice
    return 1
}

main
