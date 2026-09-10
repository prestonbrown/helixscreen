#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# version-compare.sh — Order two versions by Semantic Versioning 2.0.0 precedence.
#
# Prints -1, 0 or 1 for "A ranks below / equal to / above B" and exits 0. A
# version the grammar does not accept is a hard error (exit 1) with the reason
# on stderr, never a verdict: every caller reads 0 as "the same release, safe to
# republish", so a comparator that cannot rank a pair must not answer at all.
#
# `sort -V` cannot answer this question. GNU version sort ranks 1.1.0 BELOW
# 1.1.0-beta.1, the opposite of semver, so its verdict is inverted for any pair
# where one side carries a prerelease.
#
# The ordering cases live in tests/fixtures/version_precedence.txt, shared with
# the in-app comparator (src/util/version.cpp) and the Android versionCode
# packing (scripts/android-version-code.sh). The rule has to exist in three
# languages; one corpus is how the three are kept from drifting apart.
#
# Consumed by the pre-upload channel guard in .github/workflows/release.yml,
# which must know whether publishing a version to a channel moves that
# channel's manifest forward or strands everyone already on a newer build.

set -euo pipefail

# Prerelease identifiers compare in ASCII order, which is what the C locale
# gives `[[ < ]]`. Any other collation reorders letters and case.
export LC_ALL=C

usage() {
    cat <<EOF
Usage: version-compare.sh A B
       version-compare.sh --sort < versions

Order two versions by Semantic Versioning 2.0.0 precedence (section 11).

Prints one of:
  -1   A ranks below B
   0   A and B have equal precedence
   1   A ranks above B

Options:
  --sort       Read versions on stdin, one per line, and print them in
               ascending precedence order. A version the grammar rejects
               fails the whole sort rather than being ranked by guess.
  -h, --help   Show this help

Precedence rules:
  1. major, minor and patch compare numerically
  2. a version WITH a prerelease ranks below the same triple without one
  3. prerelease identifiers compare left to right, split on '.': two numeric
     identifiers numerically, two alphanumeric ones in ASCII order, and a
     numeric identifier always below an alphanumeric one
  4. when every shared identifier is equal, MORE identifiers wins
     (1.1.0-beta < 1.1.0-beta.1)
  5. build metadata (+sha) is ignored, so 1.1.0+abc and 1.1.0 rank equal

Accepted grammar: MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD], with no leading 'v'
and no leading zero on any numeric component. Anything else exits 1 with the
reason on stderr; a usage error exits 2.

Examples:
  version-compare.sh 1.1.0-beta.1 1.1.0     -> -1
  version-compare.sh 1.1.0 1.0.99           ->  1
  version-compare.sh 1.1.0+abc 1.1.0        ->  0
  printf '1.1.0\\n1.1.0-beta.1\\n' | version-compare.sh --sort
EOF
}

SORT_MODE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --sort)
            SORT_MODE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -?*)
            echo "error: unknown option '$1'" >&2
            usage >&2
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

# Validate one dot-separated identifier list. Every identifier must be non-empty
# and built only from ASCII alphanumerics and hyphens. $4=1 additionally rejects
# a leading zero on an all-digit identifier: semver forbids that in a prerelease
# (it would give one number two spellings that rank differently) and allows it
# in build metadata, which is opaque.
validate_identifiers() {
    local field="$1" role="$2" what="$3" strict="$4"
    local ident rest="$field"

    if [ -z "$field" ]; then
        printf 'error: %s version has an empty %s\n' "$role" "$what" >&2
        return 1
    fi

    while :; do
        ident="${rest%%.*}"
        case "$ident" in
            "")
                printf 'error: %s version has an empty %s identifier: %s\n' \
                    "$role" "$what" "$field" >&2
                return 1
                ;;
            *[!0-9A-Za-z-]*)
                printf 'error: %s version has an invalid character in %s identifier: %s\n' \
                    "$role" "$what" "$ident" >&2
                return 1
                ;;
        esac

        if [ "$strict" = 1 ]; then
            case "$ident" in
                *[!0-9]*)  : ;;   # alphanumeric, nothing more to check
                0|[1-9]*)  : ;;   # a lone zero, or no leading zero
                *)
                    printf 'error: %s version has a leading zero in %s identifier: %s\n' \
                        "$role" "$what" "$ident" >&2
                    return 1
                    ;;
            esac
        fi

        # An identifier with no '.' after it is the last one.
        [ "$rest" = "$ident" ] && break
        rest="${rest#*.}"
    done
}

# Validate $1 (named $2 in error messages) and print "MAJOR MINOR PATCH PRERELEASE".
# The prerelease field is empty for a plain release.
parse_version() {
    local version="$1" role="$2"
    local rest core pre dots maj min pat component

    if [ -z "$version" ]; then
        printf 'error: %s version is empty\n' "$role" >&2
        return 1
    fi

    # Build metadata starts at the first '+' and never affects precedence, but a
    # malformed one still means the caller handed over something it did not
    # parse itself, so it is checked rather than discarded unread.
    rest="${version%%+*}"
    if [ "$rest" != "$version" ]; then
        validate_identifiers "${version#*+}" "$role" "build metadata" 0 || return 1
    fi

    # The core triple cannot contain '-', so the first hyphen opens the
    # prerelease. Hyphens inside the prerelease are legal and stay there.
    pre=""
    core="$rest"
    case "$rest" in
        *-*)
            pre="${rest#*-}"
            core="${rest%%-*}"
            validate_identifiers "$pre" "$role" "prerelease" 1 || return 1
            ;;
    esac

    case "$core" in
        *[!0-9.]*)
            printf 'error: %s version has a non-numeric core triple: %s\n' \
                "$role" "$core" >&2
            printf '       expected MAJOR.MINOR.PATCH with no leading v\n' >&2
            return 1
            ;;
    esac

    dots="${core//[!.]/}"
    if [ "${#dots}" -ne 2 ]; then
        printf 'error: %s version needs exactly three numeric components: %s\n' \
            "$role" "$core" >&2
        return 1
    fi

    IFS='.' read -r maj min pat <<< "$core"
    for component in "$maj" "$min" "$pat"; do
        case "$component" in
            "")
                printf 'error: %s version has an empty core component: %s\n' \
                    "$role" "$core" >&2
                return 1
                ;;
            0|[1-9]*) : ;;
            *)
                printf 'error: %s version has a leading zero in a core component: %s\n' \
                    "$role" "$core" >&2
                return 1
                ;;
        esac
    done

    printf '%s %s %s %s\n' "$maj" "$min" "$pat" "$pre"
}

# Order two all-digit identifiers, neither carrying a leading zero, so the
# longer string is the larger number and equal lengths compare bytewise. That
# holds for identifiers of any length, where shell arithmetic would overflow.
compare_numeric() {
    if [ "${#1}" -lt "${#2}" ]; then
        printf '%s\n' -1
    elif [ "${#1}" -gt "${#2}" ]; then
        printf '%s\n' 1
    elif [ "$1" = "$2" ]; then
        printf '%s\n' 0
    elif [[ "$1" < "$2" ]]; then
        printf '%s\n' -1
    else
        printf '%s\n' 1
    fi
}

compare_ascii() {
    if [ "$1" = "$2" ]; then
        printf '%s\n' 0
    elif [[ "$1" < "$2" ]]; then
        printf '%s\n' -1
    else
        printf '%s\n' 1
    fi
}

is_numeric() {
    case "$1" in
        ""|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

# Order two non-empty prerelease strings by rules 3 and 4.
compare_prerelease() {
    local a="$1" b="$2"
    local ai bi order

    while [ -n "$a" ] && [ -n "$b" ]; do
        ai="${a%%.*}"
        bi="${b%%.*}"

        if is_numeric "$ai"; then
            if is_numeric "$bi"; then
                order=$(compare_numeric "$ai" "$bi")
            else
                order=-1   # a numeric identifier always ranks below an alphanumeric one
            fi
        elif is_numeric "$bi"; then
            order=1
        else
            order=$(compare_ascii "$ai" "$bi")
        fi

        if [ "$order" != 0 ]; then
            printf '%s\n' "$order"
            return 0
        fi

        # Peel the identifier just compared. One with no '.' after it is the
        # last, and emptying the string ends the loop.
        if [ "$a" = "$ai" ]; then a=""; else a="${a#*.}"; fi
        if [ "$b" = "$bi" ]; then b=""; else b="${b#*.}"; fi
    done

    # Every shared identifier is equal, so whichever list still has identifiers
    # left ranks above the other.
    if [ -z "$a" ] && [ -n "$b" ]; then
        printf '%s\n' -1
    elif [ -n "$a" ] && [ -z "$b" ]; then
        printf '%s\n' 1
    else
        printf '%s\n' 0
    fi
}

compare_versions() {
    local a_fields b_fields
    local a_maj a_min a_pat a_pre b_maj b_min b_pat b_pre order

    a_fields=$(parse_version "$1" "first") || return 1
    b_fields=$(parse_version "$2" "second") || return 1

    IFS=' ' read -r a_maj a_min a_pat a_pre <<< "$a_fields"
    IFS=' ' read -r b_maj b_min b_pat b_pre <<< "$b_fields"

    order=$(compare_numeric "$a_maj" "$b_maj")
    if [ "$order" = 0 ]; then
        order=$(compare_numeric "$a_min" "$b_min")
    fi
    if [ "$order" = 0 ]; then
        order=$(compare_numeric "$a_pat" "$b_pat")
    fi

    if [ "$order" = 0 ]; then
        if [ -z "$a_pre" ] && [ -n "$b_pre" ]; then
            order=1     # a release outranks every prerelease of the same triple
        elif [ -n "$a_pre" ] && [ -z "$b_pre" ]; then
            order=-1
        elif [ -n "$a_pre" ]; then
            order=$(compare_prerelease "$a_pre" "$b_pre")
        fi
    fi

    printf '%s\n' "$order"
}

# Order stdin ascending. Callers prune with `head -n -N`, so a version this
# cannot rank must abort the sort: a partial order silently deletes the wrong
# release. Every entry is validated before any comparison for that reason.
sort_versions() {
    local -a items=()
    local line key order i j

    while IFS= read -r line; do
        [ -n "$line" ] || continue
        items+=("$line")
    done

    for line in "${items[@]:+${items[@]}}"; do
        parse_version "$line" "input" >/dev/null || return 1
    done

    # Insertion sort. The lists are release inventories, tens of entries at
    # most, and an in-process comparison keeps it to no subprocesses per pair.
    for ((i = 1; i < ${#items[@]}; i++)); do
        key="${items[i]}"
        j=$((i - 1))
        while [ "$j" -ge 0 ]; do
            order=$(compare_versions "${items[j]}" "$key") || return 1
            [ "$order" = 1 ] || break
            items[j + 1]="${items[j]}"
            j=$((j - 1))
        done
        items[j + 1]="$key"
    done

    [ "${#items[@]}" -eq 0 ] || printf '%s\n' "${items[@]}"
}

if [ "$SORT_MODE" = 1 ]; then
    if [ $# -ne 0 ]; then
        echo "error: --sort reads versions on stdin and takes no arguments" >&2
        usage >&2
        exit 2
    fi
    sort_versions
    exit $?
fi

if [ $# -ne 2 ]; then
    echo "error: need exactly two versions to compare, got $#" >&2
    usage >&2
    exit 2
fi

compare_versions "$1" "$2"
