#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for check_doc_refs.py --devel: the docs/devel dead-path and
# dead-link gate added for the architecture overhaul.

load helpers

setup() {
    CHECK="$BATS_TEST_DIRNAME/../../scripts/check_doc_refs.py"
    FIX="$BATS_TEST_TMPDIR/devel"
    mkdir -p "$FIX"
}

@test "devel: clean doc with file ref, :line ref, and live link passes" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    # The `:42` suffix names a line; the gate strips it and checks the file.
    for i in $(seq 50); do echo "line $i"; done > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/good.md" <<'EOF'
See `src/zz_fixture_real.cpp` and `src/zz_fixture_real.cpp:42` and
[the other doc](other.md). Glob `src/*.cpp` and `<placeholder>` are skipped.
EOF
    echo x > "$FIX/other.md"
    run python3 "$CHECK" --devel "$FIX/good.md"
    [ "$status" -eq 0 ]
}

@test "devel: dead file path fails with file:line report" {
    cat > "$FIX/bad.md" <<'EOF'
Cites `src/zz_fixture_missing_thing.cpp` which does not exist.
EOF
    run python3 "$CHECK" --devel "$FIX/bad.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"zz_fixture_missing_thing.cpp"* ]]
}

@test "devel: a Makefile citation is path-checked despite having no extension" {
    cat > "$FIX/mk.md" <<'EOF'
Cites `mk/zz_missing/Makefile#all` which does not exist.
EOF
    run python3 "$CHECK" --devel "$FIX/mk.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"zz_missing/Makefile"* ]]
}

@test "devel: dead relative markdown link fails" {
    echo x > "$FIX/target.md"
    cat > "$FIX/badlink.md" <<'EOF'
See [gone](nope.md) and [anchor ok](target.md#section).
EOF
    run python3 "$CHECK" --devel "$FIX/badlink.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"nope.md"* ]]
}

@test "devel: plans and printer-research subdirs are exempt" {
    mkdir -p "$FIX/plans"
    cat > "$FIX/plans/old.md" <<'EOF'
`src/zz_fixture_missing_thing.cpp`
EOF
    run python3 "$CHECK" --devel "$FIX"
    [ "$status" -eq 0 ]
}

@test "devel: C++ lambda with unnamed pointer param is not a link" {
    cat > "$FIX/lambda.md" <<'EOF'
```cpp
.on_destroy = [](lv_obj_t*) {
    g_api->log_debug("cleanup");
}
```
EOF
    run python3 "$CHECK" --devel "$FIX/lambda.md"
    [ "$status" -eq 0 ]
    [[ "$output" != *"lv_obj_t*"* ]]
}

@test "devel: an anchor citation names the file before the '#'" {
    # A citation names a place inside the file - `path#symbol`. The fragment is
    # not part of the path, so the gate must strip it and check the file.
    mkdir -p "$BATS_TEST_TMPDIR/src"
    printf 'void widget_attach(void) {}\n' > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/anchor_ok.md" <<'MD'
See `src/zz_fixture_real.cpp#widget_attach` for the entry point.
MD
    run python3 "$CHECK" --devel "$FIX/anchor_ok.md"
    [ "$status" -eq 0 ]
}

@test "devel: an anchor citation to a missing file fails the path check" {
    # The fragment must not smuggle a dead path past the check: strip it, and
    # what is left is a file that has to exist.
    cat > "$FIX/anchor_bad.md" <<'MD'
Cites `src/zz_fixture_missing_thing.cpp#some_symbol` which does not exist.
MD
    run python3 "$CHECK" --devel "$FIX/anchor_bad.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"zz_fixture_missing_thing.cpp#some_symbol"* ]]
}

@test "stale: report mode runs clean on a doc with no file cites" {
    cat > "$FIX/nocites.md" <<'EOF'
Plain prose, no backticked paths at all.
EOF
    run python3 "$CHECK" --stale --devel "$FIX/nocites.md"
    [ "$status" -eq 0 ]
    [[ "$output" == *"No citation is older than its doc"* ]]
}

@test "devel: a RANGE citation to a missing file fails the path check" {
    # `file.cpp:63-65` used not to match PATH_RE at all, so a citation naming a
    # block was exempt from the one check that proves its file exists — while
    # the identical `file.cpp:70` form failed. That asymmetry is what let
    # RPC_ERROR_OWNERSHIP.md carry a range pointing ~355 lines from the gate it
    # described, invisible to every gate at once.
    cat > "$FIX/range.md" <<'MD'
Cites `src/zz_fixture_missing_thing.cpp:63-65` which does not exist.
MD
    run python3 "$CHECK" --devel "$FIX/range.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"zz_fixture_missing_thing.cpp:63-65"* ]]
}

@test "devel: a RANGE citation to a real file still passes" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    for i in $(seq 80); do echo "line $i"; done > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/rangeok.md" <<'MD'
Cites `src/zz_fixture_real.cpp:63-65`, a real block.
MD
    run python3 "$CHECK" --devel "$FIX/rangeok.md"
    [ "$status" -eq 0 ]
}

@test "scope: .claude/worktrees is skipped, .claude/skills and .claude/rules are not" {
    # The harness keeps its live agent checkouts under .claude/worktrees/, each
    # a COMPLETE copy of this repo — CLAUDE.md, docs/ and all. A by-name prune
    # cannot express "skip that but keep .claude/skills", so the walk descended
    # into them and every doc and citation appeared once per running agent. Six
    # copies of CLAUDE.md's `temperature_service.cpp:667` turned main red, and
    # only after the resolver stopped having a nearest-line fallback to swallow
    # them with. Nothing asserted the walk's scope, which is why it survived.
    REPO="$BATS_TEST_TMPDIR/scoperepo"
    mkdir -p "$REPO/.claude/worktrees/agent-deadbeef" "$REPO/.claude/skills/thing" \
             "$REPO/.claude/rules" "$REPO/docs/devel" "$REPO/src"
    echo 'Root doc.' > "$REPO/CLAUDE.md"
    echo 'A live agent copy of the root doc.' \
        > "$REPO/.claude/worktrees/agent-deadbeef/CLAUDE.md"
    printf -- '---\nname: thing\n---\nA real skill doc.\n' \
        > "$REPO/.claude/skills/thing/SKILL.md"
    printf -- '---\npaths:\n  - "src/**/*"\n---\nA path-scoped rule.\n' \
        > "$REPO/.claude/rules/rule.md"
    cd "$REPO"

    run python3 "$CHECK" --list
    [ "$status" -eq 0 ]
    # The skills doc is deliberately in scope...
    contains ".claude/skills/thing/SKILL.md" "$output"
    contains ".claude/rules/rule.md" "$output"
    # ...and the agent worktree is deliberately not.
    lacks "agent-deadbeef" "$output"
    # The repo's own CLAUDE.md is still scanned, exactly once.
    [ "$(printf '%s\n' "$output" | grep -c 'scanned: CLAUDE.md$')" -eq 1 ]
}

@test "scope: a doc inside .claude/worktrees cannot fail the gate" {
    # The end-to-end shape of the bug: a citation that resolves against the
    # AGENT's tree, not the one being checked, reported against a path nobody
    # can act on. Deleting those worktrees is not available as a fix — they hold
    # other agents' uncommitted work.
    REPO="$BATS_TEST_TMPDIR/scoperepo2"
    mkdir -p "$REPO/.claude/worktrees/agent-deadbeef" "$REPO/docs/devel"
    echo 'Clean.' > "$REPO/CLAUDE.md"
    echo 'Cites `src/zz_definitely_missing.cpp` which does not exist.' \
        > "$REPO/.claude/worktrees/agent-deadbeef/CLAUDE.md"
    cd "$REPO"

    run python3 "$CHECK" --refs
    [ "$status" -eq 0 ]
    [[ "$output" != *"zz_definitely_missing"* ]]
}

@test "index: a local .md the docs index links becomes a scan target" {
    cd "$BATS_TEST_TMPDIR"
    mkdir -p docs/audits
    printf '# Index\n\n[Audit](audits/A.md) and [User](user/U.md)\n' > docs/README.md
    echo x > docs/audits/A.md
    run python3 -c "
import os, sys
os.chdir('$BATS_TEST_TMPDIR')
sys.path.insert(0, '$BATS_TEST_DIRNAME/../../scripts')
import check_doc_refs as r
print(r.indexed_docs())
"
    [ "$status" -eq 0 ]
    # Listed and present on disk -> a target. Listed but absent -> not invented.
    contains "docs/audits/A.md" "$output"
    [[ "$output" != *"docs/user/U.md"* ]]
}

@test "index: a URL in the docs index is not treated as a local doc" {
    cd "$BATS_TEST_TMPDIR"
    mkdir -p docs
    printf '# Index\n\n[Up](https://example.com/x.md) and [Mail](mailto:a@b.md)\n' > docs/README.md
    run python3 -c "
import os, sys
os.chdir('$BATS_TEST_TMPDIR')
sys.path.insert(0, '$BATS_TEST_DIRNAME/../../scripts')
import check_doc_refs as r
print(r.indexed_docs())
"
    [ "$status" -eq 0 ]
    [ "$output" = "[]" ]
}

make_patch_fixture() {
    # A scratch tree shaped like the repo's patch shelf: one wired LVGL patch
    # with a creation hunk and a modification hunk.
    mkdir -p "$BATS_TEST_TMPDIR/patches" "$BATS_TEST_TMPDIR/mk"
    cat > "$BATS_TEST_TMPDIR/patches/zz_creates.patch" <<'EOF'
diff --git a/src/zz_patch_created.h b/src/zz_patch_created.h
new file mode 100644
--- /dev/null
+++ b/src/zz_patch_created.h
@@ -0,0 +1 @@
+int zz_patch_created(void);
diff --git a/src/zz_modified_not_created.h b/src/zz_modified_not_created.h
--- a/src/zz_modified_not_created.h
+++ b/src/zz_modified_not_created.h
@@ -1 +1 @@
-old
+new
EOF
    printf '$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/zz_creates.patch "fixture patch"\n' \
        > "$BATS_TEST_TMPDIR/mk/patches.mk"
}

@test "devel: a file a wired patch creates resolves on an unapplied tree" {
    # The citation's existence is patch state, not doc state: a fresh clone
    # before the first build, and a tree between reset-patches and the next
    # apply, do not carry the file, while every built tree does. The gate
    # resolves such citations against what patches/ create.
    make_patch_fixture
    cat > "$FIX/pc.md" <<'EOF'
See `lib/lvgl/src/zz_patch_created.h` for the guard it installs.
EOF
    run python3 "$CHECK" --devel "$FIX/pc.md"
    [ "$status" -eq 0 ]
}

@test "devel: a file a patch only modifies is still required to exist" {
    # Only a creation hunk licenses the citation: a patch that merely edits a
    # file says nothing about that file existing on an unapplied tree.
    make_patch_fixture
    cat > "$FIX/pm.md" <<'EOF'
See `lib/lvgl/src/zz_modified_not_created.h` and `lib/lvgl/src/zz_never_anywhere.h`.
EOF
    run python3 "$CHECK" --devel "$FIX/pm.md"
    [ "$status" -eq 1 ]
    contains "zz_modified_not_created.h" "$output"
    contains "zz_never_anywhere.h" "$output"
}

# A miniature tree holding the three sources of truth the counts check reads:
# three filament systems, 101 listed printers (one hidden), two theme presets.
make_counts_fixture() {
    REPO="$BATS_TEST_TMPDIR/countsrepo"
    mkdir -p "$REPO/include" "$REPO/assets/config/themes/defaults" "$REPO/docs/user"
    cat > "$REPO/include/ams_types.h" <<'H'
inline constexpr AmsTypeTraits kAmsTypeTraits[] = {
    {AmsType::NONE, "None", "", "unknown", "", AmsBufferRows::None, nullptr},
    {AmsType::AFC, "AFC", "AFC (Armored Turtle)", "afc", "AFC", AmsBufferRows::UnitBuffer,
     TR_NOOP("AFC buffer")},
    {AmsType::QIDI_BOX, "QIDI Box", "QIDI Box", "qidi_box", "qidi_box", AmsBufferRows::None,
     nullptr},
    {AmsType::TOOL_CHANGER, "Tool Changer", "Tool Changer", "tool_changer", "toolchanger",
     AmsBufferRows::None, nullptr},
};
H
    python3 -c "
import json
p = [{'id': 'p%d' % i} for i in range(101)] + [{'id': 'hidden', 'show_in_list': False}]
json.dump({'printers': p}, open('$REPO/assets/config/printer_database.json', 'w'))"
    echo '{}' > "$REPO/assets/config/themes/defaults/a.json"
    echo '{}' > "$REPO/assets/config/themes/defaults/b.json"
    cat > "$REPO/README.md" <<'MD'
- **Multi-material** — 3 filament systems: AFC, QIDI Box, and tool changers
- Over 100 printers, light/dark themes with 2 presets
| Multi-Material | 3 filament systems |
MD
    cd "$REPO"
}

@test "counts: a README that matches what ships passes" {
    make_counts_fixture
    run python3 "$CHECK" --counts
    [ "$status" -eq 0 ]
    contains "4 claims match" "$output"
}

@test "counts: a filament system missing from the README's list fails" {
    make_counts_fixture
    sed -i.orig 's/AFC, QIDI Box, and/AFC and/' README.md
    run python3 "$CHECK" --counts
    [ "$status" -eq 1 ]
    contains "README.md:1: filament system list omits QIDI Box" "$output"
}

@test "counts: a stale filament system count fails, in docs/user too" {
    make_counts_fixture
    echo '- 2 multi-material backends with Spoolman' > docs/user/FAQ.md
    run python3 "$CHECK" --counts
    [ "$status" -eq 1 ]
    contains "docs/user/FAQ.md:1: says 2 filament systems; the registry has 3" "$output"
}

@test "counts: a wrong theme preset count fails" {
    make_counts_fixture
    sed -i.orig 's/with 2 presets/with 17 presets/' README.md
    run python3 "$CHECK" --counts
    [ "$status" -eq 1 ]
    contains "says 17 theme presets" "$output"
}

@test "counts: a printer floor below the current step or above the database fails" {
    make_counts_fixture
    printf -- '- 80+ printers\n- over 101 models\n- 100+ models\n' > docs/user/GUIDE.md
    run python3 "$CHECK" --counts
    [ "$status" -eq 1 ]
    contains 'GUIDE.md:1: says "80+ printers"; the database lists 101 printers' "$output"
    contains 'GUIDE.md:2: says "over 101 models"' "$output"
    lacks "GUIDE.md:3" "$output"
}

@test "counts: a README with no filament system list cannot pass by silence" {
    make_counts_fixture
    printf 'Supports many filament systems.\n' > README.md
    run python3 "$CHECK" --counts
    [ "$status" -eq 1 ]
    contains 'no "<N> filament systems: <list>" line' "$output"
}

@test "counts: the shipped README and docs/user agree with the tree" {
    cd "$BATS_TEST_DIRNAME/../.."
    run python3 "$CHECK" --counts
    [ "$status" -eq 0 ]
}

@test "counts: a source of truth that moves fails instead of going quiet" {
    make_counts_fixture
    rm assets/config/printer_database.json
    run python3 "$CHECK" --counts
    [ "$status" -eq 1 ]
    contains "assets/config/printer_database.json:0: missing" "$output"
}
