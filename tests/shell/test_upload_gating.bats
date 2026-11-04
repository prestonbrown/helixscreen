#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Diagnostic-upload gate wiring (prestonbrown/helixscreen#1410).
#
# The runtime behaviour is covered by tests/unit/test_diag_upload_gate.cpp.
# What can break SILENTLY is the plumbing this file pins: the compile default
# (official packaging builds upload, everything else defaults off), the
# docker-boundary forwarding, the build-features stamp, and the deploy-time
# env stamp that keeps our rigs uploading. Lose any of those and the build is
# green while a whole class of binaries quietly stops reporting — or a fork's
# build quietly keeps hitting our CDN.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

@test "official packaging builds default to uploads on, everything else off" {
    # The yes must sit in the HELIX_PACKAGING branch and the no in its else —
    # the ENABLE_REMOTE_CONTROL block has the same ifeq shape with the
    # polarity flipped, so keying on the variable name alone is not enough.
    run python3 - <<'PY'
import sys
lines = open("Makefile").read().splitlines()
iy = next(i for i, l in enumerate(lines) if "ENABLE_DIAGNOSTIC_UPLOADS ?= yes" in l)
preceding_ifeqs = [l for l in lines[:iy] if l.lstrip().startswith("ifeq")]
assert preceding_ifeqs and "HELIX_PACKAGING),1" in preceding_ifeqs[-1], \
    "yes default not inside the HELIX_PACKAGING branch: %r" % preceding_ifeqs[-1:]
ino = next(i for i, l in enumerate(lines) if "ENABLE_DIAGNOSTIC_UPLOADS ?= no" in l)
assert any(l.strip() == "else" for l in lines[iy:ino]), \
    "no default not in the else branch of the same rule"
PY
    [ "$status" -eq 0 ]
}

@test "the yes default produces the marker define on the compile line" {
    run grep -F -e '-DHELIX_ENABLE_DIAGNOSTIC_UPLOADS' Makefile
    [ "$status" -eq 0 ]
    run grep -F 'CXXFLAGS += $(DIAG_UPLOAD_DEFINES)' Makefile
    [ "$status" -eq 0 ]
}

@test "both link rules record diag_uploads beside the binary" {
    run grep -F 'diag_uploads=%s' mk/rules.mk
    [ "$status" -eq 0 ]
    run grep -F 'diag_uploads=%s' mk/pi-dual-link.mk
    [ "$status" -eq 0 ]
}

@test "every container make invocation forwards the upload default" {
    # HELIX_PACKAGING is a target-specific variable and never crosses the
    # `docker run` boundary, so each inner make must be handed the resolved
    # ENABLE_DIAGNOSTIC_UPLOADS and ENABLE_MOCKS the same way it is handed
    # remote control.
    run python3 - <<'PY'
import sys
bad = [line.rstrip() for line in open("mk/cross.mk")
       if "$(DOCKER_REMOTE_CONTROL)" in line
       and ("$(DOCKER_DIAG_UPLOADS)" not in line or "$(DOCKER_MOCKS)" not in line)
       and line.lstrip().startswith("make ")]
if bad:
    print("docker make lines without the upload or mock default:")
    print("\n".join(bad))
    sys.exit(1)
PY
    [ "$status" -eq 0 ]
}

@test "dev deploys stamp the device upload switch on" {
    # The deploy targets are the dev flow: helixscreen.env is excluded from
    # every rsync/tar, so this stamp is the only thing that turns uploads on
    # for a rig without a per-device change.
    run grep -F 'HELIX_DIAGNOSTIC_UPLOADS 1' mk/cross.mk
    [ "$status" -eq 0 ]
    # and it must live inside sync-device-features, which every deploy reaches
    run python3 -c "assert 'HELIX_DIAGNOSTIC_UPLOADS 1' in open('mk/cross.mk').read().split('define sync-device-features')[1].split('endef')[0]"
    [ "$status" -eq 0 ]
}

@test "release builds carry HELIX_PACKAGING, which the upload default keys on" {
    run grep -F 'HELIX_PACKAGING=1' .github/workflows/release.yml
    [ "$status" -eq 0 ]
}

@test "the loopback URL overrides keep ungated test runs off the CDN" {
    # libhv honours no proxy environment; the env override is the only seam
    # that lets the unit tests point uploads at a counting listener. Removing
    # it turns a future reverted gate back into a real CDN upload.
    run grep -F 'HELIX_BUNDLE_WORKER_URL' src/system/debug_bundle_collector.cpp
    [ "$status" -eq 0 ]
    run grep -F 'HELIX_CRASH_WORKER_URL' src/system/crash_reporter.cpp
    [ "$status" -eq 0 ]
}

@test "the un-harnessed modal gate paths keep their wiring" {
    # The log RPC gate has behavioural coverage (the unix-socket test in
    # test_diag_upload_gate.cpp); these two modal hunks SURVIVED mutate-diff
    # (no harness drives the modals), so their wiring is pinned structurally:
    # the crash modal pre-checks before promising "Sending", and the debug
    # modal maps the refusal to the translated message rather than a raw
    # error string.
    run python3 - <<'PY'
import sys
checks = [
    ("src/ui/notify/ui_crash_report_modal.cpp",
     "helix::diag::uploads_enabled()", "the crash modal pre-check"),
    ("src/ui/ui_debug_bundle_modal.cpp",
     'lv_tr("Upload unavailable in this build")',
     "the debug modal honest-off message"),
]
for path, needle, what in checks:
    if needle not in open(path).read():
        print(f"{path}: lost {what}")
        sys.exit(1)
PY
    [ "$status" -eq 0 ]
}
