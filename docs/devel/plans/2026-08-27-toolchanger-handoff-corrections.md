# Corrections to the 2026-08-27 toolchanger/doc-citation handoff

A handoff written on 2026-08-27 listed five open items following the toolchanger
routing work. Four of its factual claims were wrong. All five items are now closed
or landed, but the handoff text may be reused as the basis for further work, so the
false claims are recorded here with the evidence that refuted them.

Deliberately cited by commit SHA and symbol name rather than `file:line` — these are
claims about history, and line numbers would rot.

## The four wrong claims

### 1. "HelixScreen does not send it; klippy.log has zero hits"

**False, and it was hiding the answer.** The `print_task_config.extruder_map_table`
reset is real: `reset_print_info()` called from `print_stats._note_finish()`, the
shared terminal path for `note_complete`, `note_cancel` and `note_error` — which is
why it fired after both a completed and a cancelled print.

The zero-hit grep failed for two independent reasons: the file searched
(`printer_data/logs/klippy.log`) was stale, the live log being `/oem/klippylogs/klippy.log`;
and the reset never logs the gcode name, it logs `[print_task_config] reset print info`.
A third trap sat underneath: BusyBox `grep` on the U1 silently rejects `--include`,
printing a usage error and matching nothing, which reads as a clean zero.

The other four claims in that item held, but were misleading rather than useful —
the module genuinely has no print-end hook, because `print_stats` calls *into* it.

Full trace: `docs/devel/plans/u1-extruder-map-reset-investigation.md` (commit `d725ade70`).

### 2. "The test fails when a concurrent cmdline contains both moonraker and helix-screen"

**Impossible as described.** `find_moonraker_processes()` already skipped any cmdline
containing `helix-screen`, on the same literal needle the test asserted on. The
assertion was tautological against intact production and could not fail that way.

The real defect was worse: the test iterated the live `/proc` table, so its subject
matter was whatever the box happened to be running. On a machine with no matching
process it executed **zero assertions and still reported green**. Delete the
self-exclusion from production and the test survived.

Fixed by extracting `select_moonraker_processes()` as a pure function driven from a
fixture table (commit `5180a8df5`).

### 3. "Only the picker UI is missing"

**Stale.** Commit `eb1540abd` had already shipped one shared remap picker for every
backend and wired the U1 in via the swatch-card tap. No picker needed building.

What was actually missing was smaller and elsewhere: the predicate deciding whether a
backend can carry a user's tool mapping did not count the pre-print route. Fixed by
`honors_user_tool_mapping()` in `ams_types.h`.

### 4. "Flipping capabilities changes effective_auto_match() for every backend"

**The formula was right; the inference was wrong.** AFC, Happy Hare, CFS, QIDI,
ToolChanger and AD5X-IFS-with-`_IFS_VARS` all report `editable=true` and had honoured
the user setting all along. Only Snapmaker, ACE, and AD5X-IFS-before-`_IFS_VARS` were
pinned. The rule is shared; the capabilities are not.

The blast radius that justified a "stop and hand back a plan" instruction did not exist.

## One further claim, half wrong

The handoff's out-of-scope list said in-backtick ranges (`` `f.cpp:63-65` ``) were
unchecked. The commit had **already** fixed that in `LINE_REF_RE`. What remained true
was narrower: `PATH_RE` alone did not match them, so they went unchecked for *path
existence* only. Closed by `43dd97f1f`.

## Status of the five items

| Item | Outcome |
|------|---------|
| Reprint routes to wrong heads | Landed. Not a skipped send — an active identity overwrite erasing the firmware's crossover. `b85718782` |
| Routing-table reset | Solved, see claim 1. `998077184` |
| Tool remap UX (#962) | Landed. `feature/snapmaker-tool-remap-ui` |
| Doc-citation anchors | Landed and hardened. `00f973133` onward |
| Environment-sensitive test | Landed, see claim 2. `5180a8df5` |

## The pattern worth carrying forward

Every one of these was a **false green**: a gate passing while pointing at a comment
banner, a test that could not fail, a mock that swallowed the feature, a grep that
never ran. The doc-citation audit put the first number on it — 8.3% of citations
confirmed wrong by hand, 15-20% estimated.

Each was caught by verifying a claim rather than building on it. Before trusting a
zero or a green, confirm the instrument can report a non-zero: run the scan where the
files actually are, check the command exited 0, mutate the code a test claims to cover.
