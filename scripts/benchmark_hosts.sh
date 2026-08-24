#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# benchmark_hosts.sh - Compare hardware specs and optionally build times across hosts
#
# Usage:
#   ./scripts/benchmark_hosts.sh                    # Compare localhost only
#   ./scripts/benchmark_hosts.sh thelio.local       # Compare localhost + thelio
#   ./scripts/benchmark_hosts.sh thelio.local zeus.local  # Multiple remotes
#   ./scripts/benchmark_hosts.sh --build            # Include build time benchmark
#   ./scripts/benchmark_hosts.sh --build thelio.local zeus.local

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

RUN_BUILD=false
HOSTS=()

# Parse arguments
for arg in "$@"; do
    if [[ "$arg" == "--build" ]]; then
        RUN_BUILD=true
    else
        HOSTS+=("$arg")
    fi
done

print_header() {
    echo -e "\n${CYAN}════════════════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}════════════════════════════════════════════════════════════════${NC}\n"
}

print_section() {
    echo -e "${YELLOW}── $1 ──${NC}"
}

get_local_specs() {
    print_header "LOCAL MACHINE ($(hostname))"

    if [[ "$(uname)" == "Darwin" ]]; then
        # macOS
        print_section "System"
        system_profiler SPHardwareDataType 2>/dev/null | grep -E "Model Name|Chip" | sed 's/^[ ]*/  /'

        print_section "CPU"
        echo "  Model: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo 'Unknown')"
        echo "  Total Cores: $(sysctl -n hw.ncpu)"
        echo "  Physical Cores: $(sysctl -n hw.physicalcpu)"

        # Apple Silicon specific
        if sysctl -n hw.perflevel0.physicalcpu &>/dev/null; then
            echo "  Performance Cores: $(sysctl -n hw.perflevel0.physicalcpu)"
            echo "  Efficiency Cores: $(sysctl -n hw.perflevel1.physicalcpu)"
        fi

        print_section "Memory"
        RAM_BYTES=$(sysctl -n hw.memsize)
        RAM_GB=$(echo "$RAM_BYTES / 1024 / 1024 / 1024" | bc)
        echo "  RAM: ${RAM_GB} GB"

    else
        # Linux
        print_section "CPU"
        echo "  Model: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d':' -f2 | xargs)"
        echo "  Total Threads: $(nproc)"
        echo "  Physical Cores: $(grep -c '^processor' /proc/cpuinfo 2>/dev/null || nproc)"
        lscpu 2>/dev/null | grep -E "Socket|Core|Thread|MHz|cache" | sed 's/^/  /'

        print_section "Memory"
        RAM_KB=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
        RAM_GB=$((RAM_KB / 1024 / 1024))
        echo "  RAM: ${RAM_GB} GB"
    fi
}

get_remote_specs() {
    local host="$1"
    print_header "REMOTE: $host"

    if ! ssh -o ConnectTimeout=5 "$host" "echo connected" &>/dev/null; then
        echo -e "${RED}  ✗ Cannot connect to $host${NC}"
        return 1
    fi

    ssh "$host" '
        echo "── CPU ──"
        if [ -f /proc/cpuinfo ]; then
            echo "  Model: $(grep "model name" /proc/cpuinfo | head -1 | cut -d":" -f2 | xargs)"
        fi
        echo "  Total Threads: $(nproc)"

        if command -v lscpu &>/dev/null; then
            lscpu 2>/dev/null | grep -E "Socket|Core|Thread|MHz|cache" | sed "s/^/  /"
        fi

        echo ""
        echo "── Memory ──"
        if [ -f /proc/meminfo ]; then
            RAM_KB=$(awk "/MemTotal/ {print \$2}" /proc/meminfo)
            RAM_GB=$((RAM_KB / 1024 / 1024))
            echo "  RAM: ${RAM_GB} GB"
        fi
    '
}

run_build_benchmark() {
    local host="$1"

    if [[ "$host" == "local" ]]; then
        print_section "Build Benchmark (local)"
        cd "$PROJECT_DIR"
        make clean &>/dev/null || true

        echo "  Starting clean build with make -j..."
        BUILD_OUTPUT=$( { time make -j; } 2>&1 )

        REAL_TIME=$(echo "$BUILD_OUTPUT" | grep '^real' | awk '{print $2}')
        USER_TIME=$(echo "$BUILD_OUTPUT" | grep '^user' | awk '{print $2}')
        SYS_TIME=$(echo "$BUILD_OUTPUT" | grep '^sys' | awk '{print $2}')

        echo -e "  ${GREEN}✓ Build complete${NC}"
        echo "  Real time: $REAL_TIME"
        echo "  User time: $USER_TIME"
        echo "  Sys time:  $SYS_TIME"
    else
        print_section "Build Benchmark ($host)"
        echo -e "  ${YELLOW}Note: Remote builds require project to exist at same path${NC}"
        echo "  Skipping remote build (manual setup required)"
    fi
}

calculate_relative_performance() {
    print_header "RELATIVE PERFORMANCE ESTIMATE"

    echo "Based on CPU architecture and core counts:"
    echo ""
    echo "Rough compilation speed factors (higher = faster):"
    echo "  - Single-threaded IPC matters most for incremental builds"
    echo "  - Core count matters most for clean builds (make -j)"
    echo "  - RAM > 16GB rarely bottlenecks compilation"
    echo ""
    echo "Typical relative speeds for C++ compilation:"
    echo "  Intel i7-9700K (8c/8t @ 3.6GHz):     ~1.0x baseline"
    echo "  AMD Ryzen 9 9950X (16c/32t @ 5.7GHz): ~3-4x (modern IPC + 2x cores)"
    echo "  Intel Xeon E5-2697v4 (36c/72t @ 2.3GHz): ~2-3x (many cores, older IPC)"
    echo ""
    echo "Note: Actual performance varies by workload, I/O, and ccache usage."
}

# Main execution
echo -e "${GREEN}╔══════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║           Hardware & Compilation Benchmark Tool                  ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════════════╝${NC}"

# Always show local specs
get_local_specs

# Show remote specs
for host in "${HOSTS[@]}"; do
    get_remote_specs "$host"
done

# Run build benchmarks if requested
if $RUN_BUILD; then
    echo ""
    run_build_benchmark "local"
fi

# Show performance comparison if we have multiple hosts
if [[ ${#HOSTS[@]} -gt 0 ]]; then
    calculate_relative_performance
fi

echo ""
echo -e "${GREEN}Benchmark complete!${NC}"
