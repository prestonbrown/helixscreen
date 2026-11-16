#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The nightly links the ESP32 firmware (#2042).
#
# Host-side checks (esp32_syntax_check.py, check_esp32_app_srcs.py --link) compile
# units or read native objects; only idf.py links the real image. esp32-build.yml
# does that on PRs and pushes, but `idf.py reconfigure` fetches managed components
# from the registry on every run, so the link can break on a day nothing landed.
# The nightly calls the workflow and its status job fails when the firmware does.

# Overridable so these gates can be proven to FIRE against hand-broken copies.
NIGHTLY="${HELIX_TEST_NIGHTLY_YML:-.github/workflows/nightly.yml}"
ESP32="${HELIX_TEST_ESP32_YML:-.github/workflows/esp32-build.yml}"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    source tests/shell/helpers.bash
}

# Prints "ok" or the reason the contract is broken.
check_contract() {
    python3 - "$NIGHTLY" "$ESP32" <<'PY'
import sys, yaml
nightly_path, esp32_path = sys.argv[1], sys.argv[2]
with open(nightly_path) as fh:
    nightly = yaml.safe_load(fh)
with open(esp32_path) as fh:
    esp32 = yaml.safe_load(fh)
# PyYAML resolves the bare key `on` to the boolean True.
trig = esp32.get("on", esp32.get(True)) or {}
if "workflow_call" not in trig:
    sys.exit(print("esp32-build.yml is not callable (no workflow_call trigger)"))
jobs = nightly.get("jobs") or {}
callers = [n for n, j in jobs.items()
           if isinstance(j, dict) and j.get("uses") == "./.github/workflows/esp32-build.yml"]
if not callers:
    sys.exit(print("no nightly job uses ./.github/workflows/esp32-build.yml"))
job = callers[0]
if "release_tag" in ((jobs[job].get("with") or {})):
    sys.exit(print(f"nightly job {job} passes release_tag and would package a release"))
status = jobs.get("nightly-status") or {}
needs = status.get("needs") or []
if job not in needs:
    sys.exit(print(f"nightly-status does not need {job}"))
script = "".join(s.get("run", "") for s in status.get("steps") or [])
if f"needs.{job}.result" not in script:
    sys.exit(print(f"nightly-status never reads needs.{job}.result"))
print("ok")
PY
}

@test "the nightly links the firmware and its status fails when the link does" {
    run check_contract
    [ "$output" = "ok" ] || fail "$output"
}
