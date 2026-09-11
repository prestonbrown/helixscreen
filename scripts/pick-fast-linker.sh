#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - pick the fast linker for a native host build
#
#   scripts/pick-fast-linker.sh "$CXX"             -> mold | lld | (nothing)
#   scripts/pick-fast-linker.sh "$CXX" --describe  -> choice|mold_path|mold_version|lld
#
# The Makefile needs the name to hand to -fuse-ld=, and scripts/check-deps.sh
# needs to tell a developer what is installed and what to do about it. One
# function so the two cannot drift: a box the Makefile silently refuses to use
# mold on must be a box check-deps.sh explains.
#
# Default mode writes advisories to stderr, because its caller is make. In
# --describe mode it is silent and the caller composes its own message.

set -u

CXX_CMD="${1:-c++}"
MODE="${2:-}"

# Resolve the mold the COMPILER will exec, which is not always the one on PATH:
# clang resolves ld.mold from its own program directory first, so a 2.x in
# /usr/local beside a distro 1.x in /usr/bin gives `command -v` one answer and
# clang another. It has to be the one that does the link. clang prints an
# absolute path here; gcc prints a bare name when it will fall through to PATH.
read -r -a cxx_argv <<<"$CXX_CMD"
mold_bin=$("${cxx_argv[@]}" -print-prog-name=ld.mold 2>/dev/null || true)
case "$mold_bin" in
    /*) ;;
    *) mold_bin=$(command -v ld.mold 2>/dev/null || true) ;;
esac

mold_version=""
mold_major=""
if [ -n "$mold_bin" ]; then
    mold_version=$("$mold_bin" --version 2>/dev/null |
        sed -n 's/^mold \([0-9][0-9.]*\).*/\1/p')
    mold_major="${mold_version%%.*}"
fi

lld_bin=$(command -v ld.lld 2>/dev/null || true)

# mold is only taken from 2.x. 1.0.3 - what Ubuntu 22.04 ships - drops the
# STB_GNU_UNIQUE symbol the compiler emits for a function-local static inside a
# template. nlohmann's decode()::utf8d, the 400-entry UTF-8 DFA table, is one of
# those: the reference to it resolves to the image base, so every json dump()
# indexes the ELF header instead of the table. The link is silent and the binary
# runs (prestonbrown/helixscreen#1584). Nothing between 1.0.3 and 2.x has been
# checked, so the floor is the major version.
choice=""
if [ -n "$mold_major" ] && [ "$mold_major" -ge 2 ] 2>/dev/null; then
    choice="mold"
elif [ -n "$lld_bin" ]; then
    choice="lld"
fi

if [ "$MODE" = "--describe" ]; then
    printf '%s|%s|%s|%s\n' "$choice" "$mold_bin" "$mold_version" "$lld_bin"
    exit 0
fi

if [ -n "$mold_bin" ] && [ "$choice" != "mold" ]; then
    echo "⚠️  $mold_bin is mold ${mold_version:-?}, which links this tree incorrectly" >&2
    echo "    (prestonbrown/helixscreen#1584) — ignoring it. Install mold 2.x over that" >&2
    echo "    path; a newer one elsewhere on PATH does not help, $CXX_CMD picks this one." >&2
fi
if [ -z "$choice" ]; then
    echo "⚠️  no usable fast linker — links use GNU ld and take ~25s longer each." >&2
    echo "    Install one: sudo apt install mold   (or set FAST_LINK=0 to silence this.)" >&2
fi

printf '%s' "$choice"
