#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Release archives must not carry the build machine's uid/gid, and the
# installer must not restore ownership out of an archive it extracts as root.
#
# A tarball created by a bare `tar -czvf` stamps every entry with the builder's
# numeric uid/gid (1001 on the GitHub Actions runner, 1000 on a local build
# host).  Root extracts with --same-owner by default on both GNU and BusyBox
# tar, so such an install lands owned by a uid that does not exist in the
# printer's /etc/passwd.
#
# Ownership cannot be asserted end-to-end from the test suite: --same-owner is
# a no-op for a non-root extractor, so a functional extract test would pass
# whether or not the fix is present.  These are therefore static gates over the
# call sites, each paired with a mutation check proving the gate can fail.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
CROSS_MK="$WORKTREE_ROOT/mk/cross.mk"
RELEASE_SH="$WORKTREE_ROOT/scripts/lib/installer/release.sh"

setup() {
    load helpers
}

# ---------------------------------------------------------------------------
# Archive creation: releases ship zips only
# ---------------------------------------------------------------------------

# Only recipe lines count — a make recipe line starts with a literal tab, so
# this skips prose in comments that happens to quote a tar command.
#
# The tab is written with printf rather than \t because BSD grep (macOS) has no
# -P, and only PCRE understands \t; an ERE over a literal tab works everywhere.
_gzip_tar_recipe_lines() {
    grep -nE "^$(printf '\t').*tar .*-c[a-z]*z[a-z]*f" "$1"
}

# A tarball would carry the builder's numeric uid/gid, which root restores on
# extract. The zip the release ships records no owner that any of the
# installer's extractors (info-zip without -X, BusyBox unzip, python zipfile)
# applies, so the release must not grow a tarball again without the owner flags
# that guard it.
@test "release packaging creates no gzip tarball" {
    local offending
    offending=$(_gzip_tar_recipe_lines "$CROSS_MK" || true)
    if [ -n "$offending" ]; then
        printf 'release recipe creates a tarball:\n%s\n' "$offending" >&2
        return 1
    fi
    # The zip recipe is still there, so an empty result means no tarball rather
    # than a cross.mk this grep no longer understands.
    grep -qE "^$(printf '\t').*zip -qr \.\./helixscreen-" "$CROSS_MK"
}

@test "the no-tarball gate catches a tar -czvf recipe line" {
    local fixture="$BATS_TEST_TMPDIR/cross.mk"
    cat > "$fixture" << 'EOF'
# a comment quoting tar -czvf is prose, not a recipe
	@cd $(RELEASE_DIR)/helixscreen && zip -qr ../helixscreen-pi.zip .
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf b.tar.gz helixscreen
EOF
    local offending
    offending=$(_gzip_tar_recipe_lines "$fixture" || true)
    [ "$(echo "$offending" | wc -l)" -eq 1 ]
    echo "$offending" | grep -q 'b.tar.gz'
}

# ---------------------------------------------------------------------------
# Archive extraction: never restore uid/gid out of the archive
# ---------------------------------------------------------------------------
#
# `-o` is the ONLY portable spelling.  BusyBox 1.33.2 (Creality K2) documents
# `-o  Don't restore user:group` but has no --no-same-owner long option, so
# passing --no-same-owner there fails the extraction outright.  GNU tar accepts
# `-o` as an alias for --no-same-owner when extracting.

@test "installer tar extractions pass -o (do not restore user:group)" {
    local offending
    # Every extract in the release module: `tar -xzf`, `tar xf -`, etc.
    offending=$(grep -nE 'tar +-?x[a-z]*f' "$RELEASE_SH" | grep -vE 'tar +-?x[a-z]*o[a-z]*f|tar +-?xo|-o ' || true)
    if [ -n "$offending" ]; then
        printf 'extraction without -o:\n%s\n' "$offending" >&2
        return 1
    fi
}

@test "deploy targets extract on the device with -o" {
    local offending
    offending=$(grep -n 'tar -xf -' "$CROSS_MK" || true)
    if [ -n "$offending" ]; then
        printf 'remote extract without -o:\n%s\n' "$offending" >&2
        return 1
    fi
}

@test "the extraction gate fails when -o is dropped" {
    local fixture="$BATS_TEST_TMPDIR/release.sh"
    cat > "$fixture" << 'EOF'
    tar -xzof "$archive" && extract_ok=true
    tar -xzf "$archive" && extract_ok=true
EOF
    local offending
    offending=$(grep -nE 'tar +-?x[a-z]*f' "$fixture" | grep -vE 'tar +-?x[a-z]*o[a-z]*f|tar +-?xo|-o ' || true)
    [ -n "$offending" ]
    # The -xzof line must NOT be flagged; the -xzf line must be.
    [ "$(echo "$offending" | wc -l)" -eq 1 ]
    echo "$offending" | grep -q 'xzf'
}

# ---------------------------------------------------------------------------
# Portability of the flags themselves
# ---------------------------------------------------------------------------

@test "the local tar accepts -o on extract" {
    local d="$BATS_TEST_TMPDIR/portable"
    mkdir -p "$d/src"
    echo payload > "$d/src/file.txt"
    tar -czf "$d/a.tar.gz" -C "$d" src
    mkdir -p "$d/out"
    # Must not error: this is the flag the installer relies on everywhere.
    tar -xzof "$d/a.tar.gz" -C "$d/out"
    [ -f "$d/out/src/file.txt" ]
}

