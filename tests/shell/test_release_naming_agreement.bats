#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Three scripts split a release archive name by hand. scripts/generate-manifest.sh
# and scripts/dev-release.sh read the platform key out of a release zip
# (helixscreen-<platform>.zip); scripts/lib/installer/release.sh reads the
# version out of a versioned tarball handed to --local. The installer's copy
# cannot be shared, because install.sh is bundled into a single file that
# carries no sourcing path back into the repo.
#
# One rule written twice agrees by convention until it silently does not, and
# each spelling reads as correct on its own. These tests pull the real
# expression out of each file and run it over one matrix, so a change to one of
# them that the other does not follow fails here.
#
# The matrix pairs every hyphen shape that reaches a filename: platform keys
# that carry hyphens (android-arm64, snapmaker-u1, x86-desktop) against versions
# that carry them (1.1.0-beta.1, 1.1.0-rc.2, and the purely numeric prerelease
# identifier 1.1.0-2 that tests/fixtures/version_precedence.txt admits).

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

PLATFORMS=(pi pi32 ad5m ad5x cc1 k1 k2 x86 x86-desktop snapmaker-u1 android-arm64)
VERSIONS=(0.99.118 1.1.0 1.1.0-beta.1 1.1.0-rc.2 1.1.0-2 1.1.0-11)

# Each helper reads the expression out of the file that owns it. Copying the
# expression into this file instead would assert that a copy matches itself.
manifest_regex() {
    sed -n 's/.*=~ \(\^helixscreen-(\.+)\\\.zip\$\) \]\].*/\1/p' scripts/generate-manifest.sh | head -1
}

dev_release_expr() {
    grep -o "s/\^helixscreen-.*\.zip\$/\\\\1/p" scripts/dev-release.sh | head -1
}

installer_expr() {
    grep -o "s/\.\*helixscreen-.*/p" scripts/lib/installer/release.sh | head -1
}

@test "each parser's expression is still findable in its own file" {
    # A rename or refactor that moves an expression turns every agreement
    # assertion below into a vacuous pass, so prove the extraction found
    # something before trusting what it reports.
    [ -n "$(manifest_regex)" ]    || fail "no zip regex found in scripts/generate-manifest.sh"
    [ -n "$(dev_release_expr)" ]  || fail "no zip sed expression found in scripts/dev-release.sh"
    [ -n "$(installer_expr)" ]    || fail "no sed expression found in scripts/lib/installer/release.sh"
}

@test "both zip parsers recover the same platform across the matrix" {
    local mre dev plat name got_plat got_dev
    mre="$(manifest_regex)"
    dev="$(dev_release_expr)"

    for plat in "${PLATFORMS[@]}"; do
        name="helixscreen-${plat}.zip"

        [[ "$name" =~ $mre ]] || fail "generate-manifest.sh does not match $name"
        got_plat="${BASH_REMATCH[1]}"
        [ "$got_plat" = "$plat" ] \
            || fail "generate-manifest.sh read platform [$got_plat] from $name, want [$plat]"

        got_dev="$(echo "$name" | sed -n -E "$dev")"
        [ "$got_dev" = "$plat" ] \
            || fail "dev-release.sh read platform [$got_dev] from $name, want [$plat]"
    done
}

@test "the installer recovers the version of a versioned tarball across the matrix" {
    local ins plat ver name got_ver
    ins="$(installer_expr)"
    for plat in "${PLATFORMS[@]}"; do
        for ver in "${VERSIONS[@]}"; do
            name="helixscreen-${plat}-v${ver}.tar.gz"
            got_ver="$(echo "$name" | sed -n "$ins")"
            [ "$got_ver" = "v${ver}" ] \
                || fail "installer read version [$got_ver] from $name, want [v${ver}]"
        done
    done
}

@test "no parser splits on a pattern that stops at the first hyphen" {
    # `helixscreen-[^-]*-` reads snapmaker-u1 as snapmaker, and `[0-9.]*` stops
    # at the hyphen opening a prerelease. Either one returns a truncated key
    # that still looks like an answer.
    run grep -rnE 'helixscreen-\[\^-\]|helixscreen-\(\[\^-\]' scripts/
    [ "$status" -ne 0 ] || fail "a parser stops at the first hyphen:"$'\n'"$output"
}

@test "the installer reads no version out of the unversioned zip" {
    local ins
    ins="$(installer_expr)"
    [ -z "$(echo 'helixscreen-pi.zip' | sed -n "$ins")" ] \
        || fail "the installer invented a version from an unversioned name"
}
