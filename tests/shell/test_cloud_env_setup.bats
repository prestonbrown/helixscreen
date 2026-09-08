#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/cloud/env-setup.sh installs the build's apt packages on a cloud VM
# whose network policy cannot reach Launchpad. The base image ships PPA
# sources that point there, and apt-get update aborts on the first 403 with
# nothing installed, so the script sets those sources aside first.

load helpers

setup() {
  cd "$BATS_TEST_DIRNAME/../.." || return 1
  SOURCES="$BATS_TEST_TMPDIR/sources.list.d"
  KEPT="$BATS_TEST_TMPDIR/kept"
  mkdir -p "$SOURCES"
  printf 'Types: deb\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble\n' > "$SOURCES/ubuntu.sources"
  printf 'Types: deb\nURIs: https://ppa.launchpadcontent.net/deadsnakes/ppa/ubuntu/\nSuites: noble\n' > "$SOURCES/deadsnakes-ubuntu-ppa-noble.sources"
  printf 'deb http://ppa.launchpad.net/ondrej/php/ubuntu noble main\n' > "$SOURCES/ondrej-php.list"
  export SOURCES KEPT
}

run_disable() {
  bash -c '
    log() { :; }
    HELIX_APT_SOURCES_DIR="$1"
    eval "$(sed -n "/^disable_blocked_apt_sources() {/,/^}/p" scripts/cloud/env-setup.sh)"
    disable_blocked_apt_sources "$2"
  ' _ "$SOURCES" "$KEPT"
}

@test "Launchpad sources are set aside, in both formats" {
  run run_disable
  [ "$status" -eq 0 ]
  [ ! -e "$SOURCES/deadsnakes-ubuntu-ppa-noble.sources" ] || fail "deadsnakes .sources still active"
  [ ! -e "$SOURCES/ondrej-php.list" ] || fail "ondrej .list still active"
  [ -f "$KEPT/deadsnakes-ubuntu-ppa-noble.sources" ] || fail "deadsnakes not kept"
  [ -f "$KEPT/ondrej-php.list" ] || fail "ondrej not kept"
}

@test "Ubuntu's own sources stay" {
  run run_disable
  [ -f "$SOURCES/ubuntu.sources" ] || fail "ubuntu.sources was set aside"
}

@test "apt-get update failing does not skip the install" {
  # The lists already on disk are enough to install from; an update that
  # aborts on a blocked mirror must not take the packages with it.
  run sed -n '/^install_apt_packages() {/,/^}/p' scripts/cloud/env-setup.sh
  contains 'apt-get update ||' "$output"
  contains 'apt-get install' "$output"
}
