#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Three runtimes decide whether a symbol map is absolute-linked:
# scripts/resolve-backtrace.sh, scripts/telemetry-crashes.py and
# server/crash-worker/src/symbol-resolver.ts. They cannot share code, so they
# share tests/fixtures/load_base_rule.json and each is checked against it here
# (the TypeScript one in its own vitest suite). Two hand-written copies of one
# rule agree by convention until they don't, and this rule's failure mode is a
# backtrace that names a plausible wrong function rather than erroring.

load helpers

FIXTURE="tests/fixtures/load_base_rule.json"

@test "the shared rule fixture is well formed" {
    run python3 -c "
import json
d = json.load(open('$FIXTURE'))
assert d['cases'], 'no cases'
for c in d['cases']:
    assert set(c) >= {'name', 'symbols', 'load_base', 'absolute'}, c
print(len(d['cases']))
"
    [ "$status" -eq 0 ]
    [ "$output" -ge 5 ]
}

@test "resolve-backtrace.sh agrees with the shared rule on every case" {
    run python3 - "$FIXTURE" <<'PY'
import json, subprocess, sys, tempfile, os
cases = json.load(open(sys.argv[1]))["cases"]
bad = []
for c in cases:
    if not c["symbols"]:
        continue  # the CLI needs a map to read; the empty case is unit-level
    with tempfile.NamedTemporaryFile("w", suffix=".sym", delete=False) as f:
        for i, a in enumerate(c["symbols"]):
            f.write("%016x T sym%d\n" % (int(a, 16), i))
        sym = f.name
    env = dict(os.environ, HELIX_SYM_FILE=sym)
    probe = "0x%x" % (int(c["symbols"][0], 16) + 0x10)
    r = subprocess.run(
        ["bash", "scripts/resolve-backtrace.sh", "--base", c["load_base"],
         "0.9.9", "k1", probe],
        capture_output=True, text=True, env=env)
    said_absolute = "absolute-linked" in (r.stderr or "")
    if said_absolute != c["absolute"]:
        bad.append("%s: shell said absolute=%s, rule says %s"
                   % (c["name"], said_absolute, c["absolute"]))
    os.unlink(sym)
print("\n".join(bad))
sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "shell disagrees with the shared rule:\n$output"
}

@test "telemetry-crashes.py agrees with the shared rule on every case" {
    run python3 - "$FIXTURE" <<'PY'
import importlib.util, json, sys
spec = importlib.util.spec_from_file_location("tc", "scripts/telemetry-crashes.py")
tc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tc)
bad = []
for c in json.load(open(sys.argv[1]))["cases"]:
    table = tc.SymbolTable([(int(a, 16), "sym%d" % i)
                            for i, a in enumerate(c["symbols"])])
    got = tc.symbols_are_absolute(table, int(c["load_base"], 16))
    if got != c["absolute"]:
        bad.append("%s: python said absolute=%s, rule says %s"
                   % (c["name"], got, c["absolute"]))
print("\n".join(bad))
sys.exit(1 if bad else 0)
PY
    [ "$status" -eq 0 ] || fail "python disagrees with the shared rule:\n$output"
}
