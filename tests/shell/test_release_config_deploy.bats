#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# End-to-end coverage of the config-deploy step: a config file that exists in
# the release archive must reach INSTALL_DIR/config/ on every update path
# (prestonbrown/helixscreen#1468 shipped config/creality-backend.init and an
# on-device report questioned whether new config files survive an in-place
# update). These tests run the REAL extract_release over a real zip and a real
# pre-existing install; only architecture validation and the NoNewPrivileges
# probe are stubbed, since both are properties of the device, not the flow.

RELEASE_SH="scripts/lib/installer/release.sh"
COMMON_SH="scripts/lib/installer/common.sh"
HOST_PROFILE_SH="scripts/lib/installer/host_profile.sh"

setup() {
    source tests/shell/helpers.bash

    unset _HELIX_RELEASE_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    # shellcheck disable=SC1090
    source "$COMMON_SH"
    # shellcheck disable=SC1090
    source "$HOST_PROFILE_SH"
    # shellcheck disable=SC1090
    source "$RELEASE_SH"

    # Device properties, not flow properties: the fake payload's binary is a
    # text file, and the NoNewPrivileges probe must be steerable per test.
    validate_binary_architecture() { return 0; }

    export SUDO=""
    TMP_DIR="$BATS_TEST_TMPDIR/tmp"
    INSTALL_DIR="$BATS_TEST_TMPDIR/usr/data/helixscreen"
    mkdir -p "$TMP_DIR"
    export TMP_DIR INSTALL_DIR
}

teardown() {
    # The in-place-merge test makes the install parent read-only; restore it
    # so bats can clean the test tree up.
    chmod 755 "$BATS_TEST_TMPDIR/usr/data" 2>/dev/null || true
    return 0
}

# Build a flat-layout release zip at $TMP_DIR/helixscreen.zip. $1 = "with" to
# ship config/creality-backend.init, anything else to omit it (the shape of a
# release predating #1468, e.g. the v1.0.0 stable payload).
build_release_zip() {
    local with_backend="$1" stage="$BATS_TEST_TMPDIR/stage/helixscreen"
    rm -rf "$BATS_TEST_TMPDIR/stage"
    mkdir -p "$stage/bin" "$stage/ui_xml" "$stage/assets" "$stage/config"

    echo "fake-binary" > "$stage/bin/helix-screen"
    echo "xml" > "$stage/ui_xml/home.xml"
    echo "asset" > "$stage/assets/marker"
    echo '{"from":"bundle"}' > "$stage/config/settings.json"
    echo "# bundled env" > "$stage/config/helixscreen.env"
    echo "rules" > "$stage/config/99-helixscreen-backlight.rules"
    printf '#!/bin/sh\n# init\n' > "$stage/config/helixscreen.init"
    if [ "$with_backend" = "with" ]; then
        printf '#!/bin/sh\n# backend trio launcher\n' > "$stage/config/creality-backend.init"
    fi

    ( cd "$stage" && zip -qr "$TMP_DIR/helixscreen.zip" . )
    _ARCHIVE_FORMAT="zip"
    export _ARCHIVE_FORMAT
}

# An install from a release that predates the backend script: config/ holds
# the operator's files but no creality-backend.init.
make_existing_install() {
    mkdir -p "$INSTALL_DIR/bin" "$INSTALL_DIR/ui_xml" "$INSTALL_DIR/assets" "$INSTALL_DIR/config"
    echo "old-binary" > "$INSTALL_DIR/bin/helix-screen"
    echo "old xml" > "$INSTALL_DIR/ui_xml/home.xml"
    echo "old asset" > "$INSTALL_DIR/assets/marker"
    echo '{"from":"user"}' > "$INSTALL_DIR/config/settings.json"
    echo "# user env" > "$INSTALL_DIR/config/helixscreen.env"
    echo "user rules" > "$INSTALL_DIR/config/99-helixscreen-backlight.rules"
}

@test "deploy: a new config file in the archive lands on the atomic-swap update path" {
    make_existing_install
    build_release_zip with
    _has_no_new_privs() { return 1; }

    run extract_release k1
    [ "$status" -eq 0 ]

    [ -f "$INSTALL_DIR/config/creality-backend.init" ]
    grep -q "backend trio launcher" "$INSTALL_DIR/config/creality-backend.init"
    # Operator files win over the bundle's copies.
    grep -q '"user"' "$INSTALL_DIR/config/settings.json"
    grep -q "# user env" "$INSTALL_DIR/config/helixscreen.env"
    [ -f "$INSTALL_DIR/config/helixscreen.init" ]
}

@test "deploy: a new config file lands on the read-only-parent in-place merge path" {
    make_existing_install
    build_release_zip with
    _has_no_new_privs() { return 0; }
    chmod 555 "$BATS_TEST_TMPDIR/usr/data"

    run extract_release k1
    [ "$status" -eq 0 ]

    [ -f "$INSTALL_DIR/config/creality-backend.init" ]
    grep -q "backend trio launcher" "$INSTALL_DIR/config/creality-backend.init"
    grep -q '"user"' "$INSTALL_DIR/config/settings.json"
    chmod 755 "$BATS_TEST_TMPDIR/usr/data"
}

@test "deploy: a new config file lands on a fresh install" {
    build_release_zip with
    _has_no_new_privs() { return 1; }

    run extract_release k1
    [ "$status" -eq 0 ]

    [ -f "$INSTALL_DIR/config/creality-backend.init" ]
    grep -q "backend trio launcher" "$INSTALL_DIR/config/creality-backend.init"
}

@test "deploy: an archive predating the file leaves it absent without touching user data" {
    # The stable channel can serve a payload that predates a config file the
    # installer knows about (the v1.0.0-vs-#1468 case). The deploy step must
    # not conjure the file, and the operator's config must still be restored,
    # which is also the proof this test actually ran the flow.
    make_existing_install
    build_release_zip without
    _has_no_new_privs() { return 1; }

    run extract_release k1
    [ "$status" -eq 0 ]

    [ ! -e "$INSTALL_DIR/config/creality-backend.init" ]
    grep -q '"user"' "$INSTALL_DIR/config/settings.json"
    grep -q "# user env" "$INSTALL_DIR/config/helixscreen.env"
}

@test "deploy: install_k1_creality_backend finds the file the update deployed" {
    # The wiring the on-device report could not see: the same lookup path the
    # installer uses, against the tree the swap actually produced.
    make_existing_install
    build_release_zip with
    _has_no_new_privs() { return 1; }

    run extract_release k1
    [ "$status" -eq 0 ]

    # competing_uis.sh resolves the asset relative to INSTALL_DIR; mirror the
    # lookup exactly (no mocking of the deploy step).
    [ -f "${INSTALL_DIR}/config/creality-backend.init" ]
}
