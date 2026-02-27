#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - Build Cross-Compilation Toolchain Docker Images
#
# Usage: ./docker/build-toolchains.sh [pi|ad5m|ad5x|all]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
RESET='\033[0m'

build_pi() {
    echo -e "${CYAN}${BOLD}Building Raspberry Pi (aarch64) toolchain...${RESET}"
    docker build -t helixscreen/toolchain-pi -f Dockerfile.pi .
    echo -e "${GREEN}✓ helixscreen/toolchain-pi built successfully${RESET}"
}

build_ad5m() {
    echo -e "${CYAN}${BOLD}Building Adventurer 5M (armv7-a) toolchain...${RESET}"
    docker build -t helixscreen/toolchain-ad5m -f Dockerfile.ad5m .
    echo -e "${GREEN}✓ helixscreen/toolchain-ad5m built successfully${RESET}"
}

build_ad5x() {
    echo -e "${CYAN}${BOLD}Building Adventurer 5X (mips) toolchain...${RESET}"
    docker build -t helixscreen/toolchain-ad5x -f Dockerfile.ad5x .
    echo -e "${GREEN}✓ helixscreen/toolchain-ad5x built successfully${RESET}"
}

build_cc1() {
    echo -e "${CYAN}${BOLD}Building Centauri Carbon 1 (armv7-a) toolchain...${RESET}"
    docker build -t helixscreen/toolchain-cc1 -f Dockerfile.cc1 .
    echo -e "${GREEN}✓ helixscreen/toolchain-cc1 built successfully${RESET}"
}

usage() {
    echo -e "${BOLD}HelixScreen Cross-Compilation Toolchain Builder${RESET}"
    echo ""
    echo "Usage: $0 [target]"
    echo ""
    echo "Targets:"
    echo "  pi      Build Raspberry Pi toolchain only"
    echo "  ad5m    Build Adventurer 5M toolchain only"
    echo "  ad5x    Build Adventurer 5X toolchain only"
    echo "  cc1     Build Centauri Carbon 1 toolchain only"
    echo "  all     Build all toolchains (default)"
    echo ""
    echo "After building, use:"
    echo "  make pi-docker      Cross-compile for Raspberry Pi"
    echo "  make ad5m-docker    Cross-compile for Adventurer 5M"
    echo "  make ad5x-docker    Cross-compile for Adventurer 5X"
    echo "  make cc1-docker     Cross-compile for Centauri Carbon 1"
}

case "${1:-all}" in
    pi)
        build_pi
        ;;
    ad5m)
        build_ad5m
        ;;
    ad5x)
        build_ad5x
        ;;
    cc1)
        build_cc1
        ;;
    all)
        echo -e "${BOLD}========================================${RESET}"
        echo -e "${BOLD}Building HelixScreen Cross-Compilation Toolchains${RESET}"
        echo -e "${BOLD}========================================${RESET}"
        echo ""
        build_pi
        echo ""
        build_ad5m
        echo ""
        build_cc1
        echo ""
        echo -e "${BOLD}========================================${RESET}"
        echo -e "${GREEN}${BOLD}All toolchains built successfully!${RESET}"
        echo -e "${BOLD}========================================${RESET}"
        echo ""
        echo -e "Available images:"
        docker images helixscreen/toolchain-*
        echo ""
        echo -e "Usage:"
        echo -e "  ${YELLOW}make pi-docker${RESET}      Cross-compile for Raspberry Pi"
        echo -e "  ${YELLOW}make ad5m-docker${RESET}    Cross-compile for Adventurer 5M"
        echo -e "  ${YELLOW}make ad5x-docker${RESET}    Cross-compile for Adventurer 5X"
        echo -e "  ${YELLOW}make cc1-docker${RESET}     Cross-compile for Centauri Carbon 1"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        echo -e "${RED}Unknown target: $1${RESET}"
        usage
        exit 1
        ;;
esac
