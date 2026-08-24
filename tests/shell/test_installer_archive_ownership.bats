#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Release archives must not carry the build machine's uid/gid, and the
# installer must not restore ownership out of an archive it extracts as root.
#
# Shipping tarballs were built with a bare `tar -czvf`, so every entry carried
# the builder's numeric uid/gid (1001 on the GitHub Actions runner, 1000 on a
# local build host).  Root extracts with --same-owner by default on both GNU
# and BusyBox tar, so an install landed owned by a uid that does not exist in
# the printer's /etc/passwd.
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
# Archive creation: release tarballs must be built with owner/group zeroed
# ---------------------------------------------------------------------------

# Only recipe lines count — a make recipe line starts with a literal tab, so
# this skips prose in comments that happens to quote a tar command.
#
# The tab is written with printf rather than \t because BSD grep (macOS) has no
# -P, and only PCRE understands \t; an ERE over a literal tab works everywhere.
_release_tar_recipe_lines() {
    grep -nE "^$(printf '\t').*tar .*-czvf" "$1"
}

@test "every release tarball is created with TAR_OWNER_FLAGS" {
    local total offending
    total=$(_release_tar_recipe_lines "$CROSS_MK" | wc -l)
    # Every platform must still be packaged; a drop to zero means the grep
    # stopped matching rather than the tree becoming clean.
    [ "$total" -ge 10 ]

    offending=$(_release_tar_recipe_lines "$CROSS_MK" | grep -v 'TAR_OWNER_FLAGS' || true)
    if [ -n "$offending" ]; then
        printf 'release tar without TAR_OWNER_FLAGS:\n%s\n' "$offending" >&2
        return 1
    fi
}

@test "TAR_OWNER_FLAGS is defined in cross.mk" {
    grep -q '^TAR_OWNER_FLAGS' "$CROSS_MK"
}

@test "the release-tar gate fails when the flags are dropped from one site" {
    local fixture="$BATS_TEST_TMPDIR/cross.mk"
    cat > "$fixture" << 'EOF'
TAR_OWNER_FLAGS := --owner=0 --group=0 --numeric-owner
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar $(TAR_OWNER_FLAGS) -czvf a.tar.gz helixscreen
	@cd $(RELEASE_DIR) && COPYFILE_DISABLE=1 tar -czvf b.tar.gz helixscreen
EOF
    # Exactly the b.tar.gz line must be caught.
    local offending
    offending=$(_release_tar_recipe_lines "$fixture" | grep -v 'TAR_OWNER_FLAGS' || true)
    [ -n "$offending" ]
    echo "$offending" | grep -q 'b.tar.gz'
    echo "$offending" | refute_grep 'a.tar.gz'
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

# Print every archive entry's owner as "uid/gid", from either tar's listing.
#
# GNU tar prints uid/gid as a single field 2 ("0/0"); BSD tar (macOS) prints a
# link count there and puts uid and gid in fields 3 and 4. Keying on whether
# field 2 contains a slash normalises both without probing which tar this is.
_archive_numeric_owners() {
    tar -tvzf "$1" --numeric-owner |
        awk '{ if ($2 ~ /\//) print $2; else print $3 "/" $4 }'
}

@test "TAR_OWNER_FLAGS produces an archive owned by 0:0" {
    local d="$BATS_TEST_TMPDIR/owner"
    mkdir -p "$d/helixscreen/bin"
    echo binary > "$d/helixscreen/bin/helix-screen"

    # Resolve the same flags the Makefile resolves, by the same probe.
    local flags
    if tar --owner=0 --group=0 -cf /dev/null -T /dev/null >/dev/null 2>&1; then
        flags="--owner=0 --group=0 --numeric-owner"
    else
        flags="--uid 0 --gid 0 --numeric-owner"
    fi

    # shellcheck disable=SC2086
    tar $flags -czf "$d/rel.tar.gz" -C "$d" helixscreen

    # No entry may carry a non-zero uid/gid.
    local bad
    bad=$(_archive_numeric_owners "$d/rel.tar.gz" | grep -v '^0/0$' || true)
    if [ -n "$bad" ]; then
        printf 'archive entries with non-zero owner: %s\n' "$bad" >&2
        return 1
    fi
}

@test "a bare tar -czf DOES leak the builder uid (proves the previous test bites)" {
    # Guards against the owner assertion passing vacuously on a system where
    # the build user happens to be uid 0 (e.g. a root CI container).
    if [ "$(id -u)" = "0" ]; then
        skip "running as root: a bare tar cannot leak a non-zero uid here"
    fi

    local d="$BATS_TEST_TMPDIR/leak"
    mkdir -p "$d/helixscreen"
    echo x > "$d/helixscreen/file"
    tar -czf "$d/rel.tar.gz" -C "$d" helixscreen

    local bad
    bad=$(_archive_numeric_owners "$d/rel.tar.gz" | grep -v '^0/0$' || true)
    [ -n "$bad" ]
}
