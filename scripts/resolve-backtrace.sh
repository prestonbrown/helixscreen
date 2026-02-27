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
        to_delete=$(ls -1dt "$CACHE_DIR"/v*/ 2>/dev/null | tail -n +"$((CACHE_RETAIN_COUNT + 1))")
        if [[ -n "$to_delete" ]]; then
            echo "Pruning local cache (keeping $CACHE_RETAIN_COUNT most recent)..." >&2
            echo "$to_delete" | while read -r dir; do
                echo "  Removing $(basename "$dir")" >&2
                rm -rf "$dir"
            done
        fi
    fi
}

LOAD_BASE=0
AUTO_DETECT_BASE=false
CRASH_FILE=""
ISSUE_NUMBER=""
ISSUE_REPO=""

# Linker/runtime boundary symbols that are NOT real functions.
# Resolving to these means the address wasn't in any real function.
# Uses a pipe-delimited string for O(1)-ish matching (compatible with bash 3.2+).
readonly GARBAGE_SYMBOLS="|data_start|_edata|_end|__bss_start|__bss_start__|__bss_end__|__data_start|__dso_handle|__libc_csu_init|__libc_csu_fini|_fini|_init|_fp_hw|_IO_stdin_used|__init_array_start|__init_array_end|__fini_array_start|__fini_array_end|__FRAME_END__|__GNU_EH_FRAME_HDR|__TMC_END__|__ehdr_start|__exidx_start|__exidx_end|_GLOBAL_OFFSET_TABLE_|_DYNAMIC|_PROCEDURE_LINKAGE_TABLE_|completed.0|"

# Check if a symbol name is a garbage linker boundary symbol
is_garbage_symbol() {
    [[ "$GARBAGE_SYMBOLS" == *"|$1|"* ]]
}

# Memory map entries parsed from crash file (array of "start_dec end_dec path" strings)
MEMORY_MAPS=()

usage() {
    echo "Usage: $(basename "$0") [options] <version> <platform> <addr1> [addr2] ..."
    echo "       $(basename "$0") --crash-file <crash.txt> [platform]"
    echo "       $(basename "$0") --issue <number> [--repo owner/repo]"
    echo ""
    echo "Resolves raw backtrace addresses to function names using symbol maps."
    echo ""
    echo "Options:"
    echo "  --base <hex>         ELF load base (ASLR offset) to subtract from addresses"
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

    # Extract backtrace addresses from "| N | \`0xADDR\` | ... |"
    ADDRS=()
    while IFS= read -r addr; do
        if [[ -n "$addr" ]]; then
            ADDRS+=("$addr")
        fi
    done < <(echo "$ISSUE_BODY" | grep -oE '\| `0x[0-9a-fA-F]+`' | grep -oE '0x[0-9a-fA-F]+' || true)

    # Also extract register addresses (PC, LR) as they may point to app code
    while IFS= read -r addr; do
        if [[ -n "$addr" ]]; then
            # Don't add duplicates
            local_dup=false
            for existing in "${ADDRS[@]+"${ADDRS[@]}"}"; do
                if [[ "$existing" == "$addr" ]]; then
                    local_dup=true
                    break
                fi
            done
            if [[ "$local_dup" == "false" ]]; then
                ADDRS+=("$addr")
            fi
        fi
    done < <(echo "$ISSUE_BODY" | grep -E '\*\*(LR|PC)\*\*' | grep -oE '`0x[0-9a-fA-F]+`' | head -1 | grep -oE '0x[0-9a-fA-F]+' || true)

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

    # Extract version
    VERSION=$(grep "^version:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
    if [[ -z "$VERSION" ]]; then
        echo "Error: No version found in crash file" >&2
        exit 1
    fi

    # Extract platform from file, or use command-line override
    FILE_PLATFORM=$(grep "^platform:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
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
    if (( LOAD_BASE == 0 )); then
        file_base=$(grep "^load_base:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
        if [[ -n "$file_base" ]]; then
            base_hex="${file_base#0x}"
            base_hex="${base_hex#0X}"
            LOAD_BASE=$((16#$base_hex))
            echo "Using load_base from crash file: $file_base" >&2
        fi
    fi

    # Extract backtrace addresses
    ADDRS=()
    while IFS= read -r line; do
        addr=$(echo "$line" | cut -d: -f2 | tr -d '[:space:]')
        if [[ -n "$addr" ]]; then
            ADDRS+=("$addr")
        fi
    done < <(grep "^bt:" "$CRASH_FILE" || true)

    if [[ ${#ADDRS[@]} -eq 0 ]]; then
        echo "Error: No backtrace addresses found in crash file" >&2

        # Fall back to registers
        reg_pc=$(grep "^reg_pc:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
        reg_lr=$(grep "^reg_lr:" "$CRASH_FILE" | cut -d: -f2 | tr -d '[:space:]' || true)
        if [[ -n "$reg_pc" ]]; then
            echo "Using PC/LR registers as fallback" >&2
            ADDRS+=("$reg_pc")
            [[ -n "$reg_lr" ]] && ADDRS+=("$reg_lr")
        else
            exit 1
        fi
    fi

    # Extract memory map entries for shared library resolution
    while IFS= read -r line; do
        map_line="${line#map:}"
        # /proc/self/maps format: start-end perms offset dev inode pathname
        # e.g. 7f1234000-7f1235000 r-xp 00000000 08:01 12345 /usr/lib/libfoo.so
        # We only care about executable mappings (perms contain 'x')
        if [[ "$map_line" =~ ^([0-9a-fA-F]+)-([0-9a-fA-F]+)[[:space:]]+(r|-)(w|-)(x)(p|s) ]]; then
            map_start_hex="${BASH_REMATCH[1]}"
            map_end_hex="${BASH_REMATCH[2]}"
            map_start_dec=$((16#$map_start_hex))
            map_end_dec=$((16#$map_end_hex))
            # Extract the pathname (last field, may contain spaces)
            map_path=$(echo "$map_line" | awk '{print $NF}')
            # Skip anonymous mappings, stack, heap, vdso, etc.
            if [[ "$map_path" == /* ]]; then
                MEMORY_MAPS+=("${map_start_dec} ${map_end_dec} ${map_start_hex} ${map_path}")
            fi
        fi
    done < <(grep "^map:" "$CRASH_FILE" || true)

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
        SYM_URL="${R2_BASE_URL}/v${VERSION}/${PLATFORM}.sym"
        if ! curl -fsSL -o "$SYM_FILE" "$SYM_URL"; then
            echo "Error: Failed to download symbol map from $SYM_URL" >&2
            echo "  Check version/platform or set HELIX_SYM_FILE for a local file." >&2
            rm -f "$SYM_FILE"
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
    local best_addr_hex=""

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
            best_addr_hex="$sym_addr"
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

echo "Resolving ${#@} address(es) against v${VERSION}/${PLATFORM}..."
if (( LOAD_BASE > 0 )); then
    if [[ "$AUTO_DETECT_BASE" == "true" ]]; then
        printf "ASLR load base: 0x%x (auto-detected from _start/main)\n" "$LOAD_BASE"
    else
        printf "ASLR load base: 0x%x (will subtract from addresses)\n" "$LOAD_BASE"
    fi
fi
echo ""

for addr in "$@"; do
    resolve_address "$addr"

    # Supplement with addr2line source info when available
    if [[ -n "$ADDR2LINE" ]]; then
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
