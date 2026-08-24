#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Resolve raw backtrace addresses to function names using symbol maps.
#
# Usage:
#   resolve-backtrace.sh <version> <platform> <addr1> [addr2] ...
#   resolve-backtrace.sh --base <load_base> <version> <platform> <addr1> ...
#   resolve-backtrace.sh --crash-file <crash.txt> [platform]
#
# Downloads the symbol map from R2 (cached locally) and resolves each
# hex address to the nearest function name + offset.
#
# Examples:
#   ./scripts/resolve-backtrace.sh 0.9.9 pi 0x00412abc 0x00401234
#   ./scripts/resolve-backtrace.sh --base 0xaaaab0449000 0.9.19 pi 0xaaaab04a1234
#   ./scripts/resolve-backtrace.sh --crash-file config/crash.txt
#   ./scripts/resolve-backtrace.sh --crash-file config/crash.txt pi

set -euo pipefail

readonly CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/helixscreen/symbols"
readonly R2_BASE_URL="${HELIX_R2_URL:-https://releases.helixscreen.org}/symbols"
readonly CACHE_RETAIN_COUNT=10

# Prune old cached symbol versions, keeping the most recent N by modification time
prune_cache() {
    [[ -d "$CACHE_DIR" ]] || return 0
    local count
    count=$(find "$CACHE_DIR" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l | tr -d ' ')
    if [[ "$count" -gt "$CACHE_RETAIN_COUNT" ]]; then
        local to_delete
        to_delete=$(find "$CACHE_DIR" -mindepth 1 -maxdepth 1 -type d -name 'v*' -printf '%T@\t%p\n' 2>/dev/null | sort -rn | tail -n +"$((CACHE_RETAIN_COUNT + 1))" | cut -f2-)
        if [[ -n "$to_delete" ]]; then
            echo "Pruning local cache (keeping $CACHE_RETAIN_COUNT most recent)..." >&2
            echo "$to_delete" | while read -r dir; do
                echo "  Removing $(basename "$dir")" >&2
                rm -rf "$dir"
            done
        fi
    fi
}

# Print the code addresses under one "## <heading>" section of a crash-report
# issue body (read from stdin).
#
# The worker emits the backtrace in two shapes: a `| # | \`0xADDR\` | symbol |`
# table when it resolved symbols server-side, and a fenced code block of bare
# addresses when it didn't (#1240). Both are handled here.
#
# Scoping to one section is the point: the Registers and All Registers tables
# hold SP and r0-r12, which are data, not return addresses — resolving them as
# frames invents call sites that were never on the stack. `<sub>` footnotes are
# skipped too, since they carry load_base.
extract_section_addrs() {
    local heading="$1"
    awk -v heading="$heading" '
        $0 ~ heading { in_section = 1; next }
        /^## / || /^<details/ || /^---$/ { in_section = 0 }
        !in_section { next }
        /^<sub>/ { next }
        # Table row: the Address column is the first backticked address.
        /^\|/ {
            if (match($0, /`0[xX][0-9a-fA-F]+`/)) {
                print substr($0, RSTART + 1, RLENGTH - 2)
            }
            next
        }
        # Bare address line inside a code fence.
        /^[[:space:]]*0[xX][0-9a-fA-F]+[[:space:]]*$/ {
            gsub(/[[:space:]]/, "")
            print
        }
    '
}

LOAD_BASE=0
AUTO_DETECT_BASE=false
CRASH_FILE=""
BUNDLE_FILE=""
ISSUE_NUMBER=""
ISSUE_REPO=""
# Index into ADDRS where stack-scan candidates begin (-1 = no scan marker).
# Frames before this are reliable (PC/RA/fp-walk); frames at/after are noisy
# stack-scanned return-address candidates emitted after a "bt_source:stack_scan"
# line by the crash handler.
PRIMARY_COUNT=-1

# Linker/runtime boundary symbols that are NOT real functions.
# Resolving to these means the address wasn't in any real function.
# Uses a pipe-delimited string for O(1)-ish matching (compatible with bash 3.2+).
readonly GARBAGE_SYMBOLS="|data_start|_edata|_end|__bss_start|__bss_start__|__bss_end__|__data_start|__dso_handle|__libc_csu_init|__libc_csu_fini|_fini|_init|_fp_hw|_IO_stdin_used|__init_array_start|__init_array_end|__fini_array_start|__fini_array_end|__FRAME_END__|__GNU_EH_FRAME_HDR|__TMC_END__|__ehdr_start|__exidx_start|__exidx_end|_GLOBAL_OFFSET_TABLE_|_DYNAMIC|_PROCEDURE_LINKAGE_TABLE_|completed.0|"

# Check if a symbol name is a garbage linker boundary symbol
is_garbage_symbol() {
    [[ "$GARBAGE_SYMBOLS" == *"|$1|"* ]]
}

# Repo-relative path to LVGL's event enum, used to decode numeric event_code →
# symbolic name (e.g. 53 → LV_EVENT_GET_SELF_SIZE) in the crash-context analysis.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly LV_EVENT_HEADER="${SCRIPT_DIR}/../lib/lvgl/src/misc/lv_event.h"

# Decode an lv_event_code_t ordinal to its LV_EVENT_* name by parsing the enum
# in lv_event.h. Manual counting is error-prone (the enum has comment-only lines
# and section dividers), so parse it. Prints "" if the header is absent or the
# code doesn't resolve.
lvgl_event_name() {
    local code="$1"
    [[ "$code" =~ ^[0-9]+$ ]] || return 0
    [[ -f "$LV_EVENT_HEADER" ]] || return 0
    python3 - "$LV_EVENT_HEADER" "$code" <<'PY' 2>/dev/null || true
import re, sys
hdr, target = sys.argv[1], int(sys.argv[2])
m = re.search(r'enum\s*\{(.*?)\}\s*lv_event_code_t', open(hdr).read(), re.S)
if not m:
    sys.exit()
val = 0
for raw in m.group(1).splitlines():
    code = re.sub(r'/\*.*?\*/', '', raw).strip().rstrip(',')
    mm = re.match(r'(LV_EVENT_[A-Z0-9_]+)\s*(=\s*(0x[0-9a-fA-F]+|\d+))?$', code)
    if not mm:
        continue
    if mm.group(3):
        val = int(mm.group(3), 0)
    if val == target:
        print(mm.group(1))
        break
    val += 1
PY
}

# Memory map entries parsed from crash file (array of "start_dec end_dec path" strings)
MEMORY_MAPS=()

usage() {
    echo "Usage: $(basename "$0") [options] <version> <platform> <addr1> [addr2] ..."
    echo "       $(basename "$0") --bundle <debug-bundle.json> [platform]"
    echo "       $(basename "$0") --crash-file <crash.txt> [platform]"
    echo "       $(basename "$0") --issue <number> [--repo owner/repo]"
    echo ""
    echo "Resolves raw backtrace addresses to function names using symbol maps."
    echo ""
    echo "Options:"
    echo "  --base <hex>         ELF load base (ASLR offset) to subtract from addresses"
    echo "  --bundle <path>      Parse a debug-bundle JSON: extracts the RAW recent crash"
    echo "                       (crash_txt), warns if crash_report is a different/older crash,"
    echo "                       and splits reliable frames from stack-scan candidates"
    echo "  --crash-file <path>  Parse crash.txt directly (extracts version, backtrace, load_base)"
    echo "  --issue <number>     Parse a GitHub crash report issue (extracts everything automatically)"
    echo "  --repo <owner/repo>  GitHub repo for --issue (default: auto-detect from git remote)"
    echo ""
    echo "Arguments:"
    echo "  version   Release version (e.g., 0.9.9)"
    echo "  platform  Build platform (pi, pi32, ad5m, k1, k2)"
    echo "  addr*     Hex addresses to resolve (with or without 0x prefix)"
    echo ""
    echo "Environment:"
    echo "  HELIX_R2_URL    Override R2 base URL (default: https://releases.helixscreen.org)"
    echo "  HELIX_SYM_FILE  Use a local .sym file instead of downloading"
    echo ""
    echo "Examples:"
    echo "  $(basename "$0") 0.9.19 pi 0x00412abc 0x00401234"
    echo "  $(basename "$0") --base 0xaaaab0449000 0.9.19 pi 0xaaaab04a1234 0xaaaab04b5678"
    echo "  $(basename "$0") --crash-file ~/helixscreen/config/crash.txt"
    echo "  $(basename "$0") --issue 154"
    echo "  $(basename "$0") --issue 154 --repo prestonbrown/helixscreen"
    exit 1
}

# Parse options
while [[ $# -gt 0 ]]; do
    case "${1:-}" in
        --base)
            if [[ $# -lt 2 ]]; then
                echo "Error: --base requires a hex address argument" >&2
                exit 1
            fi
            base_hex="${2#0x}"
            base_hex="${base_hex#0X}"
            LOAD_BASE=$((16#$base_hex))
            shift 2
            ;;
        --crash-file)
            if [[ $# -lt 2 ]]; then
                echo "Error: --crash-file requires a file path argument" >&2
                exit 1
            fi
            CRASH_FILE="$2"
            shift 2
            ;;
        --bundle)
            if [[ $# -lt 2 ]]; then
                echo "Error: --bundle requires a debug-bundle JSON path argument" >&2
                exit 1
            fi
            BUNDLE_FILE="$2"
            shift 2
            ;;
        --issue)
            if [[ $# -lt 2 ]]; then
                echo "Error: --issue requires a GitHub issue number" >&2
                exit 1
            fi
            ISSUE_NUMBER="$2"
            shift 2
            ;;
        --repo)
            if [[ $# -lt 2 ]]; then
                echo "Error: --repo requires owner/repo" >&2
                exit 1
            fi
            ISSUE_REPO="$2"
            shift 2
            ;;
        --help|-h)
            usage
            ;;
        *)
            break
            ;;
    esac
done

# Bundle mode: pull the raw recent crash (crash_txt) out of a debug-bundle JSON.
#
# A debug bundle can carry TWO different crashes:
#   - crash_txt    : the RAW most-recent crash file (full register dump + the
#                    fp-walk / stack-scan backtrace). This is what actually
#                    detonated most recently and is what we resolve.
#   - crash_report : a pretty-printed report that may describe an OLDER, already
#                    triaged crash with a different signature. Resolving the
#                    bundle's crash_report when crash_txt differs sends you
#                    chasing a stale, possibly-fixed bug (this cost real time on
#                    bundle YZQ47HQ6 — crash_report was a fixed shutdown SIGSEGV
#                    while crash_txt was a live gcode-streaming SIGBUS).
# This mode extracts crash_txt and loudly flags when crash_report disagrees.
if [[ -n "$BUNDLE_FILE" ]]; then
    if [[ ! -f "$BUNDLE_FILE" ]]; then
        echo "Error: Bundle file not found: $BUNDLE_FILE" >&2
        exit 1
    fi
    if ! command -v python3 &>/dev/null; then
        echo "Error: python3 is required for --bundle mode" >&2
        exit 1
    fi
    _bundle_tmp=$(mktemp "${TMPDIR:-/tmp}/helix-crashtxt.XXXXXX")
    # Extract crash_txt to the temp file; emit a human summary + dual-crash
    # warning to stderr. Exit non-zero if the bundle has no crash_txt.
    if ! python3 - "$BUNDLE_FILE" "$_bundle_tmp" <<'PYEOF' >&2
import json, sys, re
bundle_path, out_path = sys.argv[1], sys.argv[2]
with open(bundle_path) as f:
    d = json.load(f)

def field(name):
    v = d.get(name)
    return v if isinstance(v, str) else ("" if v is None else str(v))

crash_txt = field("crash_txt")
if not crash_txt.strip():
    sys.stderr.write("Bundle has no crash_txt (raw crash file) — "
                     "try --crash-file on the crash_report instead.\n")
    sys.exit(2)

def kv(text, key):
    m = re.search(r'(?im)^\s*%s\s*[:=]\s*(.+?)\s*$' % re.escape(key), text)
    return m.group(1).strip() if m else ""

# crash_txt carries version: but not platform: — the platform lives in the
# bundle's system section. Synthesize the line the crash-file parser expects so
# --bundle works without the caller repeating a platform already in the JSON.
if not kv(crash_txt, "platform"):
    sysinfo = d.get("system")
    bundle_platform = ""
    if isinstance(sysinfo, dict):
        p = sysinfo.get("platform")
        if isinstance(p, str):
            bundle_platform = p.strip()
    if bundle_platform:
        crash_txt = crash_txt.rstrip("\n") + "\nplatform:%s\n" % bundle_platform

with open(out_path, "w") as f:
    f.write(crash_txt)

bv  = field("version")
sig = kv(crash_txt, "name") or kv(crash_txt, "signal")
ts  = kv(crash_txt, "timestamp")
up  = kv(crash_txt, "uptime")
sys.stderr.write("=== crash_txt (raw recent crash — resolving THIS) ===\n")
sys.stderr.write("  signal=%s version=%s platform=%s uptime=%ss timestamp=%s\n"
                 % (sig or "?", kv(crash_txt, "version") or bv or "?",
                    kv(crash_txt, "platform") or "?", up or "?", ts or "?"))
for k in ("reg_pc", "reg_ra", "fault_addr", "queue_prev"):
    v = kv(crash_txt, k)
    if v:
        sys.stderr.write("  %s=%s\n" % (k, v))

# Compare against the pretty-printed crash_report to catch the dual-crash trap.
report = field("crash_report")
if report.strip():
    r_sig = kv(report, "Signal") or kv(report, "name")
    r_ts  = kv(report, "Timestamp") or kv(report, "timestamp")
    # Normalize "11 (SIGSEGV)" -> "SIGSEGV" for comparison.
    def norm(s):
        m = re.search(r'SIG[A-Z]+', s or "")
        return m.group(0) if m else (s or "")
    if (norm(r_sig) and norm(sig) and norm(r_sig) != norm(sig)) or (r_ts and ts and r_ts != ts):
        sys.stderr.write("\n  ⚠️  crash_report describes a DIFFERENT crash than crash_txt:\n")
        sys.stderr.write("        crash_report: signal=%s timestamp=%s\n" % (r_sig or "?", r_ts or "?"))
        sys.stderr.write("        crash_txt:    signal=%s timestamp=%s\n" % (sig or "?", ts or "?"))
        sys.stderr.write("      crash_report may be an older / already-fixed crash. Resolving crash_txt.\n")

hist = d.get("crash_history")
if isinstance(hist, list):
    for h in hist:
        if isinstance(h, dict) and h.get("github_issue"):
            sys.stderr.write("  note: crash_history references issue #%s (%s)\n"
                             % (h.get("github_issue"), h.get("fingerprint", "")))
sys.stderr.write("\n")
PYEOF
    then
        rm -f "$_bundle_tmp"
        exit 1
    fi
    CRASH_FILE="$_bundle_tmp"
    # shellcheck disable=SC2064
    trap "rm -f '$_bundle_tmp'" EXIT
fi

# GitHub issue mode: fetch and parse a crash report issue
if [[ -n "$ISSUE_NUMBER" ]]; then
    if ! command -v gh &>/dev/null; then
        echo "Error: gh CLI is required for --issue mode (install: https://cli.github.com)" >&2
        exit 1
    fi

    # Auto-detect repo from git remote if not specified
    if [[ -z "$ISSUE_REPO" ]]; then
        ISSUE_REPO=$(git remote get-url origin 2>/dev/null | sed 's/.*github\.com[:/]\(.*\)\.git$/\1/' | sed 's/.*github\.com[:/]\(.*\)$/\1/' || true)
        if [[ -z "$ISSUE_REPO" ]]; then
            echo "Error: Cannot detect repo from git remote. Use --repo owner/repo" >&2
            exit 1
        fi
    fi

    echo "Fetching issue #${ISSUE_NUMBER} from ${ISSUE_REPO}..." >&2
    if ! ISSUE_BODY=$(gh issue view "$ISSUE_NUMBER" --repo "$ISSUE_REPO" --json body -q '.body' 2>&1) || [[ -z "$ISSUE_BODY" ]]; then
        echo "Error: Failed to fetch issue #${ISSUE_NUMBER}: ${ISSUE_BODY}" >&2
        exit 1
    fi

    # Extract version from "| **Version** | X.Y.Z |"
    VERSION=$(echo "$ISSUE_BODY" | grep -o '\*\*Version\*\* *| *[0-9][0-9.]*' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)
    if [[ -z "$VERSION" ]]; then
        echo "Error: Could not extract version from issue #${ISSUE_NUMBER} — is this a crash report?" >&2
        exit 1
    fi

    # Extract platform from "| **Platform** | piXX |"
    PLATFORM=$(echo "$ISSUE_BODY" | grep -o '\*\*Platform\*\* *| *[a-z0-9]*' | sed 's/.*| *//' | head -1 || true)
    if [[ -z "$PLATFORM" ]]; then
        echo "Error: Could not extract platform from issue #${ISSUE_NUMBER}" >&2
        exit 1
    fi

    # Extract load_base from "<sub>load_base: 0xNNNN"
    if (( LOAD_BASE == 0 )); then
        issue_base=$(echo "$ISSUE_BODY" | grep -oE 'load_base: *0x[0-9a-fA-F]+' | grep -oE '0x[0-9a-fA-F]+' | head -1 || true)
        if [[ -n "$issue_base" ]]; then
            base_hex="${issue_base#0x}"
            base_hex="${base_hex#0X}"
            LOAD_BASE=$((16#$base_hex))
            echo "Using load_base from issue: $issue_base" >&2
        fi
    fi

    # Extract the backtrace frames (table or bare code block — see
    # extract_section_addrs).
    ADDRS=()
    while IFS= read -r addr; do
        [[ -n "$addr" ]] && ADDRS+=("$addr")
    done < <(echo "$ISSUE_BODY" | extract_section_addrs '^## Backtrace' || true)

    # Stack-scan candidates live in their own section and are noisy by
    # construction (real return addresses interleaved with stale ones). Record
    # where they start so the output separates them from the reliable frames,
    # the same way --bundle and --crash-file mode do.
    SCAN_ADDRS=()
    while IFS= read -r addr; do
        [[ -n "$addr" ]] && SCAN_ADDRS+=("$addr")
    done < <(echo "$ISSUE_BODY" | extract_section_addrs '^## Stack Scan' || true)

    if [[ ${#SCAN_ADDRS[@]} -gt 0 ]]; then
        PRIMARY_COUNT=${#ADDRS[@]}
        ADDRS+=("${SCAN_ADDRS[@]}")
    fi

    # No backtrace section at all — fall back to PC and LR, the only registers
    # that hold a code address. SP and the general-purpose registers are data.
    if [[ ${#ADDRS[@]} -eq 0 ]]; then
        for reg in PC LR; do
            reg_addr=$(echo "$ISSUE_BODY" | grep -E "\*\*${reg}\*\*" | grep -oE '`0x[0-9a-fA-F]+`' | head -1 | tr -d '`' || true)
            [[ -n "$reg_addr" ]] && ADDRS+=("$reg_addr")
        done
        if [[ ${#ADDRS[@]} -gt 0 ]]; then
            echo "No backtrace section in issue #${ISSUE_NUMBER} — falling back to PC/LR" >&2
        fi
    fi

    if [[ ${#ADDRS[@]} -eq 0 ]]; then
        echo "Error: No backtrace addresses found in issue #${ISSUE_NUMBER}" >&2
        exit 1
    fi

    echo "Parsed issue #${ISSUE_NUMBER}: v${VERSION}/${PLATFORM}, ${#ADDRS[@]} addresses" >&2
    set -- "${ADDRS[@]}"

# Crash file mode: extract version, platform, backtrace, load_base from file
elif [[ -n "$CRASH_FILE" ]]; then
    if [[ ! -f "$CRASH_FILE" ]]; then
        echo "Error: Crash file not found: $CRASH_FILE" >&2
        exit 1
    fi

    # Extract version — supports both raw format (version:X.Y.Z) and
    # pretty-printed crash_report.txt format (Version:   X.Y.Z)
    VERSION=$(grep "^version:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
    if [[ -z "$VERSION" ]]; then
        # Try pretty-printed format: "Version:   0.97.2"
        VERSION=$(grep -i "^Version:" "$CRASH_FILE" | sed 's/^[Vv]ersion:[[:space:]]*//' | tr -d '[:space:]' || true)
    fi
    if [[ -z "$VERSION" ]]; then
        echo "Error: No version found in crash file" >&2
        exit 1
    fi

    # Extract platform from file, or use command-line override
    # Supports raw (platform:pi) and pretty-printed (Platform:  pi)
    FILE_PLATFORM=$(grep "^platform:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
    if [[ -z "$FILE_PLATFORM" ]]; then
        FILE_PLATFORM=$(grep -i "^Platform:" "$CRASH_FILE" | sed 's/^[Pp]latform:[[:space:]]*//' | tr -d '[:space:]' || true)
    fi
    if [[ $# -ge 1 ]]; then
        PLATFORM="$1"
        shift
    elif [[ -n "$FILE_PLATFORM" ]]; then
        PLATFORM="$FILE_PLATFORM"
    else
        echo "Error: No platform in crash file — specify as argument" >&2
        exit 1
    fi

    # Extract load_base if present and not overridden by --base
    # Supports raw (load_base:0x...) and pretty-printed (Load Base: 0x...)
    if (( LOAD_BASE == 0 )); then
        file_base=$(grep "^load_base:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
        if [[ -z "$file_base" ]]; then
            file_base=$(grep -i "^Load Base:" "$CRASH_FILE" | sed 's/^[Ll]oad [Bb]ase:[[:space:]]*//' | tr -d '[:space:]' || true)
        fi
        if [[ -n "$file_base" ]]; then
            base_hex="${file_base#0x}"
            base_hex="${base_hex#0X}"
            LOAD_BASE=$((16#$base_hex))
            echo "Using load_base from crash file: $file_base" >&2
        fi
    fi

    # Extract backtrace addresses
    # Supports raw format (bt:0x...) and pretty-printed (bare hex addresses
    # under a "--- Backtrace ---" section).
    #
    # The crash handler emits reliable frames first (ucontext PC/RA, then the
    # fp-walk chain), then a "bt_source:stack_scan" marker, then noisy
    # stack-scanned candidates (any stack word that lands in .text — a mix of
    # real return addresses and stale ones). We record PRIMARY_COUNT at the
    # marker so the output can separate trustworthy frames from candidates.
    ADDRS=()
    while IFS= read -r line; do
        if [[ "$line" == bt_source:stack_scan* ]]; then
            PRIMARY_COUNT=${#ADDRS[@]}
            continue
        fi
        if [[ "$line" == bt:* ]]; then
            addr="${line#bt:}"
            addr="${addr//[[:space:]]/}"
            [[ -n "$addr" ]] && ADDRS+=("$addr")
        fi
    done < "$CRASH_FILE"

    if [[ ${#ADDRS[@]} -eq 0 ]]; then
        # Try pretty-printed format: bare hex addresses after "--- Backtrace ---"
        in_backtrace=false
        while IFS= read -r line; do
            line_trimmed=$(echo "$line" | tr -d '[:space:]')
            if [[ "$line" == *"--- Backtrace ---"* ]]; then
                in_backtrace=true
                continue
            fi
            if [[ "$line" == *"---"* ]] && $in_backtrace; then
                break
            fi
            if $in_backtrace && [[ "$line_trimmed" =~ ^0x[0-9a-fA-F]+$ ]]; then
                ADDRS+=("$line_trimmed")
            fi
        done < "$CRASH_FILE"
    fi

    if [[ ${#ADDRS[@]} -eq 0 ]]; then
        echo "Error: No backtrace addresses found in crash file" >&2

        # Fall back to registers
        reg_pc=$(grep "^reg_pc:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
        reg_lr=$(grep "^reg_lr:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
        # Also try pretty-printed register format: "  PC: 0x..."
        if [[ -z "$reg_pc" ]]; then
            reg_pc=$(grep -i "^[[:space:]]*PC:" "$CRASH_FILE" | sed 's/^[[:space:]]*[Pp][Cc]:[[:space:]]*//' | tr -d '[:space:]' || true)
        fi
        if [[ -z "$reg_lr" ]]; then
            reg_lr=$(grep -i "^[[:space:]]*LR:" "$CRASH_FILE" | sed 's/^[[:space:]]*[Ll][Rr]:[[:space:]]*//' | tr -d '[:space:]' || true)
        fi
        if [[ -n "$reg_pc" ]]; then
            echo "Using PC/LR registers as fallback" >&2
            ADDRS+=("$reg_pc")
            [[ -n "$reg_lr" ]] && ADDRS+=("$reg_lr")
        else
            exit 1
        fi
    fi

    # Extract memory map entries for shared library resolution
    # Supports raw format (map:...) and pretty-printed (bare maps lines
    # under "--- Memory Map" section)
    _parse_map_line() {
        local map_line="$1"
        if [[ "$map_line" =~ ^([0-9a-fA-F]+)-([0-9a-fA-F]+)[[:space:]]+(r|-)(w|-)(x)(p|s) ]]; then
            local map_start_hex="${BASH_REMATCH[1]}"
            local map_end_hex="${BASH_REMATCH[2]}"
            local map_start_dec=$((16#$map_start_hex))
            local map_end_dec=$((16#$map_end_hex))
            local map_path
            map_path=$(echo "$map_line" | awk '{print $NF}')
            if [[ "$map_path" == /* ]]; then
                MEMORY_MAPS+=("${map_start_dec} ${map_end_dec} ${map_start_hex} ${map_path}")
            fi
        fi
    }

    while IFS= read -r line; do
        _parse_map_line "${line#map:}"
    done < <(grep "^map:" "$CRASH_FILE" || true)

    # Fall back to pretty-printed format if no raw map: entries found
    if [[ ${#MEMORY_MAPS[@]} -eq 0 ]]; then
        in_maps=false
        while IFS= read -r line; do
            if [[ "$line" == *"--- Memory Map"* ]]; then
                in_maps=true
                continue
            fi
            if [[ "$line" == "---"* ]] && $in_maps; then
                break
            fi
            if $in_maps; then
                _parse_map_line "$line"
            fi
        done < "$CRASH_FILE"
    fi

    if [[ ${#MEMORY_MAPS[@]} -gt 0 ]]; then
        echo "Parsed ${#MEMORY_MAPS[@]} executable memory mappings from crash file" >&2
    fi

    echo "Parsed crash file: v${VERSION}/${PLATFORM}, ${#ADDRS[@]} addresses" >&2
    set -- "${ADDRS[@]}"
else
    # Normal mode: version platform addr...
    if [[ $# -lt 3 ]]; then
        usage
    fi
    VERSION="$1"
    PLATFORM="$2"
    shift 2
fi

# Determine symbol file path
if [[ -n "${HELIX_SYM_FILE:-}" ]]; then
    SYM_FILE="$HELIX_SYM_FILE"
    if [[ ! -f "$SYM_FILE" ]]; then
        echo "Error: Symbol file not found: $SYM_FILE" >&2
        exit 1
    fi
else
    SYM_FILE="${CACHE_DIR}/v${VERSION}/${PLATFORM}.sym"

    if [[ ! -f "$SYM_FILE" ]]; then
        echo "Downloading symbol map for v${VERSION}/${PLATFORM}..." >&2
        mkdir -p "$(dirname "$SYM_FILE")"
        # Symbol maps are stored compressed (.sym.zst) since v0.99.73 — nm output
        # compresses ~25:1. Try .sym.zst first (R2, then GitHub), then fall back
        # to the legacy uncompressed .sym for older releases. R2 only keeps the 5
        # most recent versions, so GitHub release assets are the long-tail fallback.
        REPO="${HELIX_GITHUB_REPO:-prestonbrown/helixscreen}"
        SYM_ZST_URL="${R2_BASE_URL}/v${VERSION}/${PLATFORM}.sym.zst"
        SYM_URL="${R2_BASE_URL}/v${VERSION}/${PLATFORM}.sym"
        # GitHub-release symbol assets gained a `symbols-` prefix so they stop
        # sorting ahead of the helixscreen-* artifacts (helixscreen#993 — Moonraker
        # falls back to assets[0]). Try the prefixed name first, then the legacy
        # unprefixed one for releases published before the rename.
        GH_SYM_ZST_URL="https://github.com/${REPO}/releases/download/v${VERSION}/symbols-${PLATFORM}.sym.zst"
        GH_SYM_URL="https://github.com/${REPO}/releases/download/v${VERSION}/symbols-${PLATFORM}.sym"
        GH_SYM_ZST_URL_LEGACY="https://github.com/${REPO}/releases/download/v${VERSION}/${PLATFORM}.sym.zst"
        GH_SYM_URL_LEGACY="https://github.com/${REPO}/releases/download/v${VERSION}/${PLATFORM}.sym"
        have_zstd=0
        command -v zstd >/dev/null 2>&1 && have_zstd=1

        if [[ "$have_zstd" == 1 ]] && curl -fsSL -o "${SYM_FILE}.zst" "$SYM_ZST_URL" 2>/dev/null; then
            zstd -d --rm -q "${SYM_FILE}.zst"
        elif curl -fsSL -o "$SYM_FILE" "$SYM_URL" 2>/dev/null; then
            : # legacy uncompressed map on R2 (pre-v0.99.73)
        elif [[ "$have_zstd" == 1 ]] && curl -fsSL -L -o "${SYM_FILE}.zst" "$GH_SYM_ZST_URL" 2>/dev/null; then
            echo "R2 symbol not found, downloaded compressed map from GitHub release..." >&2
            zstd -d --rm -q "${SYM_FILE}.zst"
        elif curl -fsSL -L -o "$SYM_FILE" "$GH_SYM_URL" 2>/dev/null; then
            echo "Downloaded from GitHub release (R2 version was pruned)" >&2
        elif [[ "$have_zstd" == 1 ]] && curl -fsSL -L -o "${SYM_FILE}.zst" "$GH_SYM_ZST_URL_LEGACY" 2>/dev/null; then
            echo "Downloaded legacy unprefixed compressed map from GitHub release..." >&2
            zstd -d --rm -q "${SYM_FILE}.zst"
        elif curl -fsSL -L -o "$SYM_FILE" "$GH_SYM_URL_LEGACY" 2>/dev/null; then
            echo "Downloaded legacy unprefixed map from GitHub release (R2 version was pruned)" >&2
        else
            rm -f "$SYM_FILE" "${SYM_FILE}.zst"
            echo "Error: Failed to download symbol map from R2 or GitHub:" >&2
            echo "  R2:     $SYM_ZST_URL (and .sym)" >&2
            echo "  GitHub: $GH_SYM_ZST_URL (and .sym, and legacy unprefixed)" >&2
            echo "  Check version/platform or set HELIX_SYM_FILE for a local file." >&2
            [[ "$have_zstd" == 0 ]] && \
                echo "  Note: zstd not installed — only uncompressed .sym was attempted. Install: brew install zstd / apt install zstd" >&2
            exit 1
        fi
        echo "Cached: $SYM_FILE" >&2
        prune_cache
    fi
fi

# Validate symbol file has content
if [[ ! -s "$SYM_FILE" ]]; then
    echo "Error: Symbol file is empty: $SYM_FILE" >&2
    exit 1
fi

# =============================================================================
# Auto-detect ASLR load base by matching _start and main to backtrace frames
# =============================================================================
auto_detect_load_base() {
    local -a addrs=("$@")

    # Get _start and main addresses from symbol file
    local start_line main_line
    start_line=$(grep ' T _start$' "$SYM_FILE" | head -1)
    main_line=$(grep ' T main$' "$SYM_FILE" | head -1)

    if [[ -z "$start_line" ]] || [[ -z "$main_line" ]]; then
        return 1
    fi

    local start_file_hex start_file_dec main_file_hex main_file_dec
    start_file_hex=$(echo "$start_line" | awk '{print $1}')
    main_file_hex=$(echo "$main_line" | awk '{print $1}')
    start_file_dec=$((16#$start_file_hex))
    main_file_dec=$((16#$main_file_hex))

    # The distance between _start and main in the file should match in the backtrace
    local expected_gap=$(( start_file_dec - main_file_dec ))

    # Try each pair of backtrace addresses to see if any pair has the same gap
    for (( i=0; i<${#addrs[@]}; i++ )); do
        local addr_i_hex="${addrs[$i]#0x}"
        addr_i_hex="${addr_i_hex#0X}"
        local addr_i_dec=$((16#$addr_i_hex))

        for (( j=i+1; j<${#addrs[@]}; j++ )); do
            local addr_j_hex="${addrs[$j]#0x}"
            addr_j_hex="${addr_j_hex#0X}"
            local addr_j_dec=$((16#$addr_j_hex))

            local gap=$(( addr_j_dec - addr_i_dec ))

            # Check if this pair matches main→_start gap
            if (( gap == expected_gap )); then
                # addr_i = main, addr_j = _start
                local candidate_base=$(( addr_i_dec - main_file_dec ))
                if (( candidate_base > 0 )); then
                    printf '%d' "$candidate_base"
                    return 0
                fi
            fi

            # Check reverse: addr_i = _start, addr_j = main
            local neg_gap=$(( addr_i_dec - addr_j_dec ))
            if (( neg_gap == expected_gap )); then
                local candidate_base=$(( addr_j_dec - main_file_dec ))
                if (( candidate_base > 0 )); then
                    printf '%d' "$candidate_base"
                    return 0
                fi
            fi
        done
    done

    # Fallback: try matching individual addresses to _start or main
    # (less reliable, but works if only one is in the backtrace)
    for addr_raw in "${addrs[@]}"; do
        local addr_hex="${addr_raw#0x}"
        addr_hex="${addr_hex#0X}"
        local addr_dec=$((16#$addr_hex))

        # Try as _start
        local base_candidate=$(( addr_dec - start_file_dec ))
        if (( base_candidate > 0 )); then
            # Verify: does main also land on a symbol?
            local main_runtime=$(( base_candidate + main_file_dec ))
            for verify_addr in "${addrs[@]}"; do
                local v_hex="${verify_addr#0x}"
                v_hex="${v_hex#0X}"
                local v_dec=$((16#$v_hex))
                if (( v_dec == main_runtime )); then
                    printf '%d' "$base_candidate"
                    return 0
                fi
            done
        fi

        # Try as main
        base_candidate=$(( addr_dec - main_file_dec ))
        if (( base_candidate > 0 )); then
            local start_runtime=$(( base_candidate + start_file_dec ))
            for verify_addr in "${addrs[@]}"; do
                local v_hex="${verify_addr#0x}"
                v_hex="${v_hex#0X}"
                local v_dec=$((16#$v_hex))
                if (( v_dec == start_runtime )); then
                    printf '%d' "$base_candidate"
                    return 0
                fi
            done
        fi
    done

    return 1
}

# Auto-detect load base if not provided
if (( LOAD_BASE == 0 )); then
    detected_base=$(auto_detect_load_base "$@" || true)
    if [[ -n "$detected_base" ]] && (( detected_base > 0 )); then
        LOAD_BASE=$detected_base
        AUTO_DETECT_BASE=true
        printf "Auto-detected ASLR load base: 0x%x (matched _start + main in backtrace)\n" "$LOAD_BASE" >&2
    fi
fi

# resolve_address <hex_addr>
# Scans the sorted symbol table (nm -nC output) to find the
# containing function. nm output format: "00000000004xxxxx T function_name"
resolve_address() {
    local addr_input="$1"
    # Normalize: strip 0x prefix, lowercase
    local addr_hex="${addr_input#0x}"
    addr_hex="${addr_hex#0X}"
    addr_hex=$(echo "$addr_hex" | tr '[:upper:]' '[:lower:]')

    # Convert to decimal for comparison
    local addr_dec
    addr_dec=$((16#$addr_hex))

    # Subtract ASLR load base if provided
    local orig_addr_hex="$addr_hex"
    if (( LOAD_BASE > 0 )); then
        addr_dec=$(( addr_dec - LOAD_BASE ))
        addr_hex=$(printf '%x' "$addr_dec")
    fi

    local best_name=""
    local best_addr=0

    # Read symbol file: each line is "ADDR TYPE NAME"
    # We only care about T/t (text/code) symbols
    while IFS=' ' read -r sym_addr sym_type sym_name rest; do
        # Skip non-text symbols
        case "$sym_type" in
            T|t|W|w) ;;
            *) continue ;;
        esac

        # Skip empty names
        [[ -z "$sym_name" ]] && continue

        # If there's extra text (demangled names with spaces), append it
        if [[ -n "$rest" ]]; then
            sym_name="$sym_name $rest"
        fi

        local sym_dec
        sym_dec=$((16#$sym_addr))

        if (( sym_dec <= addr_dec )); then
            best_name="$sym_name"
            best_addr=$sym_dec
        else
            # Past our address — the previous symbol is the match
            break
        fi
    done < "$SYM_FILE"

    # Filter garbage linker boundary symbols (data_start, _edata, etc.)
    if [[ -n "$best_name" ]] && is_garbage_symbol "$best_name"; then
        best_name=""
    fi

    # Resolved to a real function — print and return
    if [[ -n "$best_name" ]]; then
        local offset=$(( addr_dec - best_addr ))
        if (( LOAD_BASE > 0 )); then
            printf "0x%s (file: 0x%s) → %s+0x%x\n" "$orig_addr_hex" "$addr_hex" "$best_name" "$offset"
        else
            printf "0x%s → %s+0x%x\n" "$addr_hex" "$best_name" "$offset"
        fi
        return
    fi

    # Unresolved — try to identify the shared library from memory maps
    local runtime_addr_dec
    if (( LOAD_BASE > 0 )); then
        local raw_hex="${addr_input#0x}"
        raw_hex="${raw_hex#0X}"
        runtime_addr_dec=$((16#$raw_hex))
    else
        runtime_addr_dec=$((16#$addr_hex))
    fi

    for map_entry in "${MEMORY_MAPS[@]+"${MEMORY_MAPS[@]}"}"; do
        local map_start map_end map_start_hex map_path
        read -r map_start map_end map_start_hex map_path <<< "$map_entry"
        if (( runtime_addr_dec >= map_start && runtime_addr_dec < map_end )); then
            local lib_offset=$(( runtime_addr_dec - map_start ))
            local lib_basename
            lib_basename=$(basename "$map_path")
            if (( LOAD_BASE > 0 )); then
                printf "0x%s (file: 0x%s) → (%s+0x%x)\n" "$orig_addr_hex" "$addr_hex" "$lib_basename" "$lib_offset"
            else
                printf "0x%s → (%s+0x%x)\n" "$addr_hex" "$lib_basename" "$lib_offset"
            fi
            return
        fi
    done

    # No match at all
    if (( LOAD_BASE > 0 )); then
        printf "0x%s (file: 0x%s) → (unknown)\n" "$orig_addr_hex" "$addr_hex"
    else
        printf "0x%s → (unknown)\n" "$addr_hex"
    fi
}

# =============================================================================
# Debug info (.debug file) and addr2line support
# When available, addr2line gives file:line info and resolves inlined frames.
# =============================================================================

# Map platform names to cross-compile prefixes for addr2line
platform_to_cross_prefix() {
    case "$1" in
        pi)         echo "aarch64-linux-gnu-" ;;
        pi32)       echo "arm-linux-gnueabihf-" ;;
        ad5m|cc1)   echo "arm-none-linux-gnueabihf-" ;;
        k1)         echo "mipsel-buildroot-linux-musl-" ;;
        k2)         echo "mipsel-k1-linux-gnu-" ;;
        u1)         echo "arm-buildroot-linux-musleabihf-" ;;
        x86)        echo "x86_64-linux-gnu-" ;;
        *)          echo "" ;;
    esac
}

# Find a working addr2line for the target platform
find_addr2line() {
    local platform="$1"
    local cross_prefix
    cross_prefix=$(platform_to_cross_prefix "$platform")

    # Try cross-compile addr2line first (exact match for target arch)
    if [[ -n "$cross_prefix" ]]; then
        local cross_a2l="${cross_prefix}addr2line"
        if command -v "$cross_a2l" &>/dev/null; then
            echo "$cross_a2l"
            return
        fi
    fi

    # Try llvm-addr2line (arch-independent, works on any host)
    if command -v llvm-addr2line &>/dev/null; then
        echo "llvm-addr2line"
        return
    fi

    # On macOS, llvm-addr2line may be in Xcode toolchain
    local xcode_a2l="/Library/Developer/CommandLineTools/usr/bin/llvm-addr2line"
    if [[ -x "$xcode_a2l" ]]; then
        echo "$xcode_a2l"
        return
    fi

    # Try plain addr2line (only works for native-arch binaries)
    if command -v addr2line &>/dev/null; then
        echo "addr2line"
        return
    fi

    echo ""
}

# Try to download .debug file from R2 (same location as .sym)
# Debug files are stored compressed (.debug.zst) since v0.12.0
DEBUG_FILE=""
if [[ -z "${HELIX_SYM_FILE:-}" ]]; then
    DEBUG_FILE="${CACHE_DIR}/v${VERSION}/${PLATFORM}.debug"
    if [[ ! -f "$DEBUG_FILE" ]]; then
        # Try compressed (.debug.zst) first, fall back to uncompressed (.debug)
        DBG_ZST_URL="${R2_BASE_URL}/v${VERSION}/${PLATFORM}.debug.zst"
        DBG_URL="${R2_BASE_URL}/v${VERSION}/${PLATFORM}.debug"
        echo "Downloading debug info for v${VERSION}/${PLATFORM}..." >&2
        if curl -fsSL -o "${DEBUG_FILE}.zst" "$DBG_ZST_URL" 2>/dev/null; then
            echo "Decompressing debug info..." >&2
            if command -v zstd >/dev/null 2>&1; then
                zstd -d --rm -q "${DEBUG_FILE}.zst"
                echo "Cached: $DEBUG_FILE ($(du -h "$DEBUG_FILE" 2>/dev/null | cut -f1))" >&2
            else
                echo "Warning: zstd not installed, cannot decompress .debug.zst" >&2
                echo "Install with: brew install zstd (macOS) or apt install zstd (Linux)" >&2
                rm -f "${DEBUG_FILE}.zst"
                DEBUG_FILE=""
            fi
        elif curl -fsSL -o "$DEBUG_FILE" "$DBG_URL" 2>/dev/null; then
            # Legacy uncompressed format (pre-v0.12.0)
            echo "Cached: $DEBUG_FILE ($(du -h "$DEBUG_FILE" 2>/dev/null | cut -f1))" >&2
        else
            echo "No .debug file available (nm-based resolution only)" >&2
            rm -f "$DEBUG_FILE" "${DEBUG_FILE}.zst"
            DEBUG_FILE=""
        fi
    fi
fi

# Also check for a local unstripped binary (developer builds)
LOCAL_BINARY=""
for candidate in \
    "build/bin/helix-screen" \
    "build/${PLATFORM}/bin/helix-screen"; do
    if [[ -f "$candidate" ]]; then
        LOCAL_BINARY="$candidate"
        break
    fi
done

# Find addr2line tool
ADDR2LINE=""
ADDR2LINE_TARGET=""  # The file to pass to addr2line -e
if [[ -n "$DEBUG_FILE" ]] || [[ -n "$LOCAL_BINARY" ]]; then
    ADDR2LINE=$(find_addr2line "$PLATFORM")
    if [[ -n "$ADDR2LINE" ]]; then
        # Prefer .debug file (matches the exact release version)
        if [[ -n "$DEBUG_FILE" ]]; then
            ADDR2LINE_TARGET="$DEBUG_FILE"
        else
            ADDR2LINE_TARGET="$LOCAL_BINARY"
        fi
        echo "Using $ADDR2LINE with $(basename "$ADDR2LINE_TARGET")" >&2
    fi
fi

# resolve_with_addr2line <file_offset_hex>
# Returns "function_name at file:line" or empty string on failure
resolve_with_addr2line() {
    local offset_hex="$1"
    [[ -z "$ADDR2LINE" ]] && return

    local result
    result=$("$ADDR2LINE" -e "$ADDR2LINE_TARGET" -f -C -i "0x${offset_hex}" 2>/dev/null || true)
    [[ -z "$result" ]] && return

    # addr2line returns pairs of lines: function name, then file:line
    # With -i (inline), there may be multiple pairs
    local func="" location="" output=""
    while IFS= read -r line; do
        if [[ -z "$func" ]]; then
            func="$line"
        else
            location="$line"
            # Skip unknown results
            if [[ "$func" != "??" ]] && [[ "$location" != *"??:0"* ]]; then
                if [[ -n "$output" ]]; then
                    output="${output} → ${func} at ${location}"
                else
                    output="${func} at ${location}"
                fi
            fi
            func=""
        fi
    done <<< "$result"

    echo "$output"
}

# ─────────────────────────────────────────────────────────────────────────────
# Batched addr2line.
#
# addr2line maps the .debug file and pulls DWARF in lazily, so a process per
# address re-reads the same file N times and each child grows without bound —
# on the pi .debug (~2.6GB) a single child was measured at 9.5GB RSS, and a
# killed run orphans children that keep growing invisibly (the binary name
# truncates to "aarch64-linux-g", so pkill -f addr2line misses them).
#
# One process for every address instead. -p (pretty) makes the output safe to
# split: each address begins a line that does NOT start with " (inlined by) ",
# and inline frames continue with that prefix. If the parse doesn't yield
# exactly one block per address (a non-GNU addr2line with different pretty
# formatting), fall back to the per-address path rather than misalign frames.
#
# Resolving only the "reliable" frames was measured and is NOT worth it: every
# frame in a typical trace lands in the same few CUs, so addr2line pages in the
# same DWARF either way (16.9GB peak / ~175s for 10 addresses vs 43). Skipping
# the stack-scan candidates costs their file:line — where the real call spine
# often hides — and saves nothing. Resolve them all.
A2L_RESULTS=()
A2L_BATCHED=false
_a2l_count=$#
if [[ -n "$ADDR2LINE" ]] && (( _a2l_count > 0 )); then
    _offsets=()
    for addr in "$@"; do
        _h="${addr#0x}"; _h="${_h#0X}"
        _d=$((16#$_h))
        (( LOAD_BASE > 0 )) && _d=$(( _d - LOAD_BASE ))
        _offsets+=("$(printf '0x%x' "$_d")")
    done

    _a2l_out=$("$ADDR2LINE" -e "$ADDR2LINE_TARGET" -f -C -i -p "${_offsets[@]}" 2>/dev/null || true)

    if [[ -n "$_a2l_out" ]]; then
        _cur=""
        _started=0
        while IFS= read -r _line; do
            if [[ "$_line" == " (inlined by) "* ]]; then
                _frag="${_line# (inlined by) }"
                [[ "$_frag" == "??"* ]] && continue
                if [[ -n "$_cur" ]]; then _cur="${_cur} → ${_frag}"; else _cur="$_frag"; fi
            else
                (( _started )) && A2L_RESULTS+=("$_cur")
                _started=1
                _cur=""
                [[ "$_line" == "??"* ]] || _cur="$_line"
            fi
        done <<< "$_a2l_out"
        (( _started )) && A2L_RESULTS+=("$_cur")

        if (( ${#A2L_RESULTS[@]} == _a2l_count )); then
            A2L_BATCHED=true
        else
            echo "Note: batched addr2line returned ${#A2L_RESULTS[@]} blocks for ${_a2l_count} addresses;" \
                 "falling back to per-address resolution." >&2
            A2L_RESULTS=()
        fi
    fi
fi

echo "Resolving ${#@} address(es) against v${VERSION}/${PLATFORM}..."
if (( LOAD_BASE > 0 )); then
    if [[ "$AUTO_DETECT_BASE" == "true" ]]; then
        printf "ASLR load base: 0x%x (auto-detected from _start/main)\n" "$LOAD_BASE"
    else
        printf "ASLR load base: 0x%x (will subtract from addresses)\n" "$LOAD_BASE"
    fi
fi
echo ""

# When a stack-scan boundary was recorded, label the two sections. Frames
# before PRIMARY_COUNT are the reliable PC/RA + fp-walk chain; frames at/after
# are stack-scanned candidates where real return addresses are interleaved with
# stale ones — read them as "the call spine is in here", not as a clean trace.
_addr_idx=0
if (( PRIMARY_COUNT >= 0 )); then
    echo "── reliable frames (PC/RA + frame-pointer walk) ──"
fi

# Collect app-code frames (de-duplicated, in order) as a "call spine" summary.
# On MIPS/ARM the reliable trace is short and the real chain is buried in the
# stack-scan candidates among libstdc++/fmt/spdlog noise — this surfaces it.
SPINE=()
_spine_last=""
# Every resolved symbol (all frames, not just app code) — used by the
# crash-context analysis below to detect which thread the crash is on.
ALL_SYMS=()
# A frame is "app code" if it names our namespaces/entry points and is not the
# crash handler itself. Tuned to the symbols this codebase actually emits.
_is_app_frame() {
    local s="$1"
    case "$s" in
        *crash_signal_handler*) return 1 ;;
    esac
    case "$s" in
        *helix::*|*"Application::"*|*gcode_viewer*|*ui_*|*Modal*|*Panel*|*Printer*|*"main+0x"*|*_draw_cb*|*_cb\(*) return 0 ;;
        *) return 1 ;;
    esac
}

for addr in "$@"; do
    if (( PRIMARY_COUNT >= 0 && _addr_idx == PRIMARY_COUNT )); then
        echo ""
        echo "── stack-scan candidates (noisy: real return addresses interleaved with stale ones) ──"
    fi
    _addr_idx=$(( _addr_idx + 1 ))
    _res=$(resolve_address "$addr")
    echo "$_res"
    # Strip "0x… (file: 0x…) → " prefix down to the symbol for spine matching.
    _sym="${_res#*→ }"
    ALL_SYMS+=("$_sym")
    if _is_app_frame "$_sym" && [[ "$_sym" != "$_spine_last" ]]; then
        SPINE+=("$_sym")
        _spine_last="$_sym"
    fi

    # Supplement with addr2line source info when available
    if [[ "$A2L_BATCHED" == "true" ]]; then
        if (( _addr_idx <= ${#A2L_RESULTS[@]} )); then
            a2l_result="${A2L_RESULTS[$(( _addr_idx - 1 ))]}"
            if [[ -n "$a2l_result" ]]; then
                echo "    ${a2l_result}"
            fi
        fi
    elif [[ -n "$ADDR2LINE" ]]; then
        # Compute file offset (subtract ASLR base)
        local_hex="${addr#0x}"
        local_hex="${local_hex#0X}"
        local_dec=$((16#$local_hex))
        if (( LOAD_BASE > 0 )); then
            local_dec=$(( local_dec - LOAD_BASE ))
        fi
        file_hex=$(printf '%x' "$local_dec")

        a2l_result=$(resolve_with_addr2line "$file_hex")
        if [[ -n "$a2l_result" ]]; then
            echo "    ${a2l_result}"
        fi
    fi
done

# App-code call spine: the high-signal frames, de-duplicated in trace order.
if [[ ${#SPINE[@]} -gt 0 ]]; then
    echo ""
    echo "── app-code call spine (filtered from above; crash site first) ──"
    for s in "${SPINE[@]}"; do
        echo "  $s"
    done
fi

# ─────────────────────────────────────────────────────────────────────────────
# Crash-context analysis: name the thread the crash is REALLY on (from the
# resolved frames), and warn when the crash header's LVGL event_target/event_code
# are misleading. Those fields record the LAST LVGL event on the MAIN thread —
# ambient context, NOT the crash locus — so a background-thread crash (libhv WS
# loop, HTTP worker) sends analysis down a phantom "LVGL widget UAF" rabbit hole.
# (Added after bundle UK9QCFY3: a libhv onclose UAF masqueraded as an lv_label /
# event_code 53 crash for most of an investigation.)
_bg_ws_re='EventLoopThread::loop_thread|hloop_run|hloop_process_events|hio_handle_events|hio_handle_read|hio_close|eventfd_read_cb|websocket_parser_execute|onCustomEvent'
_bg_http_re='HttpExecutor|http::HttpExecutor|curl_easy|curl_multi'
_bg_thread_re='std::thread|std::__thread|BusThread|camera_stream'
_main_re='lv_timer_handler|lv_refr|_lv_display_refr|lv_obj_redraw|indev_proc|Application::run| main\+0x'

_crash_thread=""
if [[ ${#ALL_SYMS[@]} -gt 0 ]]; then
    _joined=$(printf '%s\n' "${ALL_SYMS[@]}")
    if grep -Eq "$_bg_ws_re" <<<"$_joined"; then
        _crash_thread="libhv WebSocket / event-loop  ·  BACKGROUND thread"
    elif grep -Eq "$_bg_http_re" <<<"$_joined"; then
        _crash_thread="HTTP worker  ·  BACKGROUND thread"
    elif grep -Eq "$_bg_thread_re" <<<"$_joined"; then
        _crash_thread="spawned worker  ·  BACKGROUND thread"
    elif grep -Eq "$_main_re" <<<"$_joined"; then
        _crash_thread="main / LVGL thread"
    fi
fi

# Pull the ambient LVGL/queue fields from the crash header, if we have one.
_ev_code="" _ev_class="" _ev_addr="" _q_prev="" _r_err=""
if [[ -n "${CRASH_FILE:-}" && -f "${CRASH_FILE:-/nonexistent}" ]]; then
    _ev_code=$(grep -m1 '^event_code:' "$CRASH_FILE" 2>/dev/null | cut -d: -f2 | tr -d '[:space:]' || true)
    _ev_class=$(grep -m1 '^event_target_class:' "$CRASH_FILE" 2>/dev/null | cut -d: -f2 | tr -d '[:space:]' || true)
    _ev_addr=$(grep -m1 '^event_target:' "$CRASH_FILE" 2>/dev/null | cut -d: -f2 | tr -d '[:space:]' || true)
    _q_prev=$(grep -m1 '^queue_prev:' "$CRASH_FILE" 2>/dev/null | cut -d: -f2- || true)
    _r_err=$(grep -m1 '^recent_error:' "$CRASH_FILE" 2>/dev/null | cut -d: -f2- || true)
fi

if [[ -n "$_crash_thread" || -n "$_ev_code$_ev_class$_q_prev$_r_err" ]]; then
    echo ""
    echo "── crash-context analysis ──"
    [[ -n "$_crash_thread" ]] && echo "  Crash thread (inferred from resolved frames): $_crash_thread"

    _is_bg=0
    [[ "$_crash_thread" == *BACKGROUND* ]] && _is_bg=1

    if [[ -n "$_ev_code" || -n "$_ev_class" ]]; then
        _ev_name=$(lvgl_event_name "$_ev_code")
        _line="  LVGL header: event_target_class=${_ev_class:-?}"
        [[ -n "$_ev_addr" ]] && _line="$_line (${_ev_addr})"
        _line="$_line  event_code=${_ev_code:-?}"
        [[ -n "$_ev_name" ]] && _line="$_line → ${_ev_name}"
        echo "$_line"
        if (( _is_bg )); then
            echo "  ⚠ MISLEADING: event_target_class / event_code are the LAST LVGL event on the"
            echo "    MAIN thread — ambient state, NOT the crash locus. The crash is on a background"
            echo "    thread (see above). Ignore these fields; trust the call spine."
        else
            echo "  These LVGL fields may be relevant (crash thread is main/LVGL or undetermined)."
        fi
    fi
    if [[ -n "$_q_prev" || -n "$_r_err" ]]; then
        _tag="main-thread context — may be relevant"
        (( _is_bg )) && _tag="ambient main-thread context — likely UNRELATED to this bg-thread crash"
        [[ -n "$_q_prev" ]] && echo "  queue_prev:${_q_prev}    [${_tag}]"
        [[ -n "$_r_err" ]] && echo "  recent_error:${_r_err}    [${_tag}]"
    fi
fi
