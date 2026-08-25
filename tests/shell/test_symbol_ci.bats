#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for symbol extraction CI workflow
# Split from test_symbol_extraction.bats for parallel execution.

load helpers

# Overridable so these gates can be proven to FIRE against hand-broken copies.
YML="${HELIX_TEST_RELEASE_YML:-.github/workflows/release.yml}"
CROSS_MK="${HELIX_TEST_CROSS_MK:-mk/cross.mk}"

# release.yml with full-line YAML comments dropped. Every assertion below runs
# against this rather than the raw file: release.yml documents its own R2 layout
# in prose, so a bare grep for a path finds the comment describing the upload
# long before it finds the upload, and deleting the real step stays green.
yml_code() {
    grep -vE '^[[:space:]]*#' "$YML"
}

# Platforms the release workflow builds, from the matrix axis itself.
ci_platforms() {
    yml_code \
        | sed -n 's/^[[:space:]]*platform:[[:space:]]*\[\(.*\)\].*/\1/p' \
        | tr ',' '\n' | tr -d ' ' | grep -v '^$' | sort -u
}

# Platforms mk/cross.mk can package. release-all is an aggregate and
# release-clean/-clean-assets ship nothing, so none of them is a platform.
make_platforms() {
    grep -oE '^release-[a-z0-9-]+:' "$CROSS_MK" \
        | sed -e 's/:$//' -e 's/^release-//' \
        | grep -vxE 'all|clean|clean-assets' \
        | grep -vxE "$CI_EXEMPT" \
        | sort -u
}

# Make targets that are deliberately not release platforms. k1-dynamic is an
# alternate toolchain (glibc/dynamic) for hardware the matrix already ships as
# k1, not a separate product — CI has never built it. Anything added here needs
# the same kind of reason, and the guard below checks the target still exists so
# the exemption cannot outlive its target.
CI_EXEMPT='k1-dynamic'

@test "release.yml build matrix and mk/cross.mk agree on the platform set" {
    # The previous version of this test looped over five hardcoded names against
    # a matrix that had grown to nine — cc1, ad5x, x86 and snapmaker-u1 could
    # have vanished from CI without a word. Set equality means either side
    # gaining or losing a platform fails.
    [ -f "$YML" ]

    local ci mk
    ci=$(ci_platforms)
    mk=$(make_platforms)

    [ -n "$ci" ] || fail "could not parse the platform matrix out of $YML"
    [ -n "$mk" ] || fail "could not parse release-* targets out of $CROSS_MK"

    local only_ci only_mk
    only_ci=$(comm -23 <(printf '%s\n' "$ci") <(printf '%s\n' "$mk"))
    only_mk=$(comm -13 <(printf '%s\n' "$ci") <(printf '%s\n' "$mk"))

    [ -z "$only_ci" ] || fail "release.yml builds platforms mk/cross.mk cannot package: $(echo $only_ci)"
    [ -z "$only_mk" ] || fail "mk/cross.mk packages platforms release.yml never builds: $(echo $only_mk)
(if that is deliberate, add it to CI_EXEMPT with the reason)"
}

@test "every CI_EXEMPT entry still names a real release target" {
    # An exemption for a target that no longer exists silently widens the gate.
    local e
    for e in $(printf '%s\n' "$CI_EXEMPT" | tr '|' ' '); do
        grep -qE "^release-${e}:" "$CROSS_MK" || fail "CI_EXEMPT lists '$e' but $CROSS_MK has no release-${e} target"
    done
}

@test "release.yml uploads symbol artifacts via matrix" {
    # Symbol upload uses matrix.platform variable, not literal platform names
    yml_code | grep -q 'name: symbols-\${{ matrix.platform }}'
    yml_code | grep -q 'helix-screen\.sym'
}

@test "release.yml uploads symbol maps to R2" {
    # Assert the upload COMMAND, not the path. `symbols/v.*\.sym` used to match
    # the prose comment at the top of the file that describes the R2 layout, so
    # the real aws-s3 step could have been deleted outright and this stayed green.
    yml_code \
        | sed -e :a -e '/\\$/N; s/\\\n[[:space:]]*/ /; ta' \
        | grep -qE '(s3cp|aws s3 cp)[^#]*s3://[^"]*/symbols/v\$\{RELEASE_VERSION\}/\$\{platform\}\.sym\.zst'
}
