#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The CustomPiOS / CustoPiZer image module (scripts/custompios/helixscreen),
# prestonbrown/helixscreen#1913. The chroot is simulated: dpkg answers with a
# chosen architecture and package set, curl serves fixtures and records each
# URL, apt-get records, and the release's install.sh is a stub recording the
# arguments the module hands it.

MODULE="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/custompios/helixscreen/start_chroot_script"

setup() {
    load helpers
    STUBBIN="$BATS_TEST_TMPDIR/stubbin"
    CALLS="$BATS_TEST_TMPDIR/calls.log"
    SERVE="$BATS_TEST_TMPDIR/serve"
    mkdir -p "$STUBBIN" "$SERVE"
    export CALLS SERVE
    export IMAGE_ARCH=arm64 IMAGE_PKGS="libdrm2 libinput10 libgbm1"
    cat > "$STUBBIN/dpkg" <<'STUB'
#!/bin/sh
[ "$1" = --print-architecture ] && echo "$IMAGE_ARCH"
STUB
    cat > "$STUBBIN/dpkg-query" <<'STUB'
#!/bin/sh
for a in "$@"; do pkg="$a"; done
case " $IMAGE_PKGS " in *" $pkg "*) printf 'install ok installed' ;; esac
STUB
    printf '#!/bin/sh\necho "apt-get $*" >> "$CALLS"\n' > "$STUBBIN/apt-get"
    # curl -o FILE URL: serves $SERVE/<last path element of URL>.
    cat > "$STUBBIN/curl" <<'STUB'
#!/bin/sh
out=""; url=""
while [ $# -gt 0 ]; do
    case "$1" in -o) out="$2"; shift ;; http*) url="$1" ;; esac
    shift
done
echo "curl $url" >> "$CALLS"
cp "$SERVE/${url##*/}" "$out"
STUB
    chmod +x "$STUBBIN"/*
    PATH="$STUBBIN:$PATH"
    export TMPDIR="$BATS_TEST_TMPDIR/tmp"
    mkdir -p "$TMPDIR"
    unset BASE_USER
    for v in VERSION PLATFORM ARCHIVE SHA256 USER OWN_DISPLAY INSTALL_ARGS REPO DEPS; do unset "HELIXSCREEN_$v"; done

    printf '{\n  "url": "x",\n  "tag_name": "v1.2.0",\n  "name": "v1.2.0"\n}\n' > "$SERVE/latest"
    make_release pi v1.2.0
}

# make_release PLATFORM TAG [INSTALLER_TEXT]: a release archive whose install.sh
# records its arguments.
make_release() {
    local pkg="$BATS_TEST_TMPDIR/pkg/$1/helixscreen"
    mkdir -p "$pkg"
    printf '#!/bin/sh\n# %s\necho "install.sh $*" >> "$CALLS"\n' "${3:---target-root and --first-boot-moonraker}" \
        > "$pkg/install.sh"
    tar -czf "$SERVE/helixscreen-$1-$2.tar.gz" -C "$(dirname "$pkg")" helixscreen
}

installer_args() { sed -n 's/^install.sh //p' "$CALLS"; }

@test "the module installs the latest release for the image's architecture, adding only missing libraries" {
    BASE_USER=pi run bash "$MODULE"
    [ "$status" -eq 0 ] || fail "module failed ($status): $output"
    grep -qx "curl https://api.github.com/repos/prestonbrown/helixscreen/releases/latest" "$CALLS"
    grep -qx "curl https://github.com/prestonbrown/helixscreen/releases/download/v1.2.0/helixscreen-pi-v1.2.0.tar.gz" "$CALLS"
    grep -qx "apt-get install -y --no-install-recommends libegl1 libgles2" "$CALLS" || fail "$(cat "$CALLS")"
    args=$(installer_args)
    [[ "$args" == "--target-root / --platform pi --local "*"/helixscreen-pi-v1.2.0.tar.gz --user pi" ]] ||
        fail "install.sh got: $args"

    : > "$CALLS"
    make_release pi32 v1.1.5
    IMAGE_ARCH=armhf IMAGE_PKGS="libdrm2 libinput10 libgbm1 libegl1 libgles2" \
        HELIXSCREEN_VERSION=v1.1.5 HELIXSCREEN_OWN_DISPLAY=1 HELIXSCREEN_INSTALL_ARGS="--skip-kiauh-registration --dry-run" \
        run bash "$MODULE"
    [ "$status" -eq 0 ] || fail "module failed ($status): $output"
    if grep -q "releases/latest\|^apt-get" "$CALLS"; then fail "pinned, fully provisioned image: $(cat "$CALLS")"; fi
    args=$(installer_args)
    [[ "$args" == "--target-root / --platform pi32 --local "*"-pi32-v1.1.5.tar.gz --own-display --skip-kiauh-registration --dry-run" ]] ||
        fail "install.sh got: $args"
}

@test "an image with no release platform is refused unless HELIXSCREEN_PLATFORM names one" {
    IMAGE_ARCH=riscv64 run bash "$MODULE"
    [ "$status" -ne 0 ]
    contains "no HelixScreen release for a riscv64 image" "$output"
    [ -z "$(installer_args)" ]

    IMAGE_ARCH=riscv64 HELIXSCREEN_PLATFORM=pi run bash "$MODULE"
    [ "$status" -eq 0 ] || fail "module failed ($status): $output"
    [[ "$(installer_args)" == "--target-root / --platform pi "* ]]
}

@test "an archive that fails its checksum, predates image installs or is missing is refused before installing" {
    HELIXSCREEN_VERSION=v1.2.0 HELIXSCREEN_SHA256=0000000000000000000000000000000000000000000000000000000000000000 \
        run bash "$MODULE"
    [ "$status" -ne 0 ]
    contains "does not match HELIXSCREEN_SHA256" "$output"
    [ -z "$(installer_args)" ]

    sum=$(sha256sum "$SERVE/helixscreen-pi-v1.2.0.tar.gz" | cut -d' ' -f1)
    HELIXSCREEN_VERSION=v1.2.0 HELIXSCREEN_SHA256="$sum" run bash "$MODULE"
    [ "$status" -eq 0 ] || fail "a matching checksum was refused: $output"

    : > "$CALLS"
    make_release pi v1.0.9 "--first-boot only"
    HELIXSCREEN_VERSION=v1.0.9 run bash "$MODULE"
    [ "$status" -ne 0 ]
    contains "predates image installs" "$output"
    [ -z "$(installer_args)" ]

    HELIXSCREEN_ARCHIVE="$BATS_TEST_TMPDIR/nowhere.tar.gz" run bash "$MODULE"
    [ "$status" -ne 0 ]
    contains "HELIXSCREEN_ARCHIVE not found" "$output"

    : > "$CALLS"
    HELIXSCREEN_ARCHIVE="$SERVE/helixscreen-pi-v1.2.0.tar.gz" run bash "$MODULE"
    [ "$status" -eq 0 ] || fail "module failed ($status): $output"
    if grep -q "^curl" "$CALLS"; then fail "a provided archive was downloaded anyway: $(cat "$CALLS")"; fi
    [[ "$(installer_args)" == *"--local $SERVE/helixscreen-pi-v1.2.0.tar.gz" ]]
}
