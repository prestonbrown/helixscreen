# Doc citation staleness audit — how many bootstrapped anchors point at the wrong line?

**Date:** 2026-08-27
**Branch:** `audit/doc-citation-staleness`
**Scope:** every `` `file:N` `` citation the anchor gate tracks, minus
`docs/devel/RPC_ERROR_OWNERSHIP.md` and `docs/devel/architecture/15-known-debt.md`
(being repaired concurrently on another branch).

## Three ways a citation goes wrong

The anchor gate has two independent blind spots, and this audit is about the second and
third. Naming all three keeps them from being confused for each other:

1. **Ambiguous anchor, wrong guess.** The primary hash matches several lines, context does
   not narrow it, and the `nearest` fallback picks the wrong one. *Mechanical* — the gate
   can be made to catch this, and is being hardened for it separately.
2. **Unique anchor, resolves cleanly, points somewhere meaningless.** The sidecar was
   **bootstrapped from the citations already in the docs**, and some were already wrong.
   The bootstrapper derived an anchor from whatever line the stale citation happened to
   land on, so the gate now faithfully maintains a wrong number and reports green forever.
   Undetectable by construction — only a semantic comparison of prose against the cited
   line finds it. **This is the audit's main target.**
3. **A citation form the gate cannot parse at all.** `LINE_REF_RE` only matches a
   backticked `` `path.ext:N` ``. Every other way this tree names a line — the bare
   follow-on `` `:857` ``, the bare range `` `1755-1796` `` riding after a full citation,
   the unbackticked `(:720)` — is invisible to every check and drifts freely with every
   edit to the file. Nothing has ever verified one. **458 of them remain** (below).

## Numbers

| | |
|---|---|
| Gate-visible citations in the corpus | **718** unique `(doc, path, line)` keys / 850 occurrences |
| **Class-2 confirmed wrong and fixed** | **59** (8.3% of the corpus) — every one hand-verified against both the prose and the source |
| **Class-3 references repaired and promoted to gate-visible** | **4** |
| Class-3 bare `` `:N` `` follow-ons corrected in passing | 8 |
| Estimated *total* class-2 wrong, including what the heuristic missed | **~110–140 (15–20%)**, wide interval — see below |
| Class-3 references still unverifiable by any gate | **458** |
| Left unfixed, deliberately | 4 (below) |

**This is a project, not a cleanup — but a bounded one.** 8.3% is established fact. The
15–20% figure is an extrapolation from a 40-citation random audit of the population the
heuristic *passed*, in which 4 were also wrong (3 clearly, 1 borderline). A 4/40 sample
carries a 95% interval of roughly 3–24%, so treat "15–20%" as the right order of
magnitude and nothing finer. What is not in doubt: the remainder is large enough that a
second pass is worth doing, and small enough that it is a day of work, not a rewrite.

## The heuristic

`cite_semantic_triage.py` (kept in the session scratchpad, not committed — see
"Recommendation" for what should be promoted). For every citation it:

1. extracts the **sentence** containing the citation (not the paragraph — an em-dash is
   deliberately *not* a sentence boundary, because `` `path.cpp:196` — `init_subjects()` ``
   is this repo's commonest citation shape);
2. finds backticked identifiers within 60 characters of the citation, **nearest first**.
   Nearest-first is the whole trick: a sentence names several symbols and only the one
   bracketing the citation is a claim *about* it. Without proximity ranking,
   ``` `MoonrakerClient`; the registered callback (`moonraker_manager.cpp:616`) ``` reads
   as a claim that `MoonrakerClient` lives at line 616, and the tool fires on a correct
   citation;
3. asks whether the nearest identifier appears within ±12 lines of the cited line;
4. separately flags a **low-information cited line** — a lone `}`, a blank, a comment
   banner. Nothing there can support any claim, so the number is unfalsifiable;
5. reports, for each missed identifier, where it *does* live in that file — which is not
   just evidence, it is the proposed repair.

`check_doc_refs.py` already does a tight version of step 3 (`SYMBOL_CITE_A_RE` /
`SYMBOL_CITE_B_RE`, ±5 lines) for two exact syntactic shapes. This relaxes the shape and
adds step 4.

### Precision, measured

110 citations flagged; 8 skipped as another branch's files; **58 of the remaining 102
confirmed real — 57% precision.** Per signal:

| Signal | Flagged | Confirmed | Precision |
|---|---|---|---|
| Cited line is low-information (`}`, blank, banner) | 39 | 29 | **75–82%** |
| Nearest symbol absent, and found elsewhere in the file | 39 | 23 | 61–80% |
| Nearest symbol absent, not found anywhere in the file | 32 | 6 | **21%** |

**The scoring had it backwards.** The cheapest signal is the best one: "the cited line is
a brace, a blank, or a `// ====` banner" needs no identifier extraction, no NLP, no
proximity model, and it is right three times out of four. The symbol-proximity signal
that the ranking treated as strongest is the weakest of the three at 21% — it fires
constantly on table rows (a cell's symbol paired with a neighbouring cell's citation) and
on sentences whose subject is a type while the citation points at a call site.

### The dominant failure shape

Far and away the commonest defect: **the citation points at the `}` of the *previous*
function, two lines above the one the sentence names.** Code grew above it, the number
stayed, and the `}` is a perfectly good unique-enough anchor, so the gate locks it in.

- `ams_state.cpp:710` (`}` of `init_backend_from_hardware`) cited for
  `init_backends_from_hardware()` at **712** — in two different chapters
- `gcode_layer_renderer.cpp:1387` (`}`) cited for `pick_object_at()` at **1389** — in two
  chapters, while a *third* doc cited **1363** for the same function
- `ui_print_select_detail_view.cpp:1840` (`}`) cited for `start_tail_summary_scan()` at
  **1842** — twice
- `panel_widget_registry.cpp:147`→**149**, `display_manager.cpp:630`→**632**,
  `printer_state.cpp:130`→**133**

The second commonest: a citation landing inside the *preceding* declaration's doc comment
(`printer_detector.h:444` for the `is_*_printer()` family at **466**;
`theme_manager.cpp:2257` for `is_on_elevated_surface()` at **2261**).

The third, and the largest in magnitude, is ordinary drift: `printer_detector.h:531`→**552**
and `:576`→**597** (both exactly 21 lines stale), `ams_state.h:1186`→**1247**,
`application.cpp:2962`→**3110**, `moonraker_manager.cpp:450`→**481**,
`z_offset_persistence.cpp:69`→**13**.

## Class 3: the forms no gate can parse

Caught live during this audit. A merge inserted ~20 lines at
`ams_backend_snapmaker.cpp:1508` and ~8 more at `:1934`. `make regen-doc-links` shifted
five citations in `FILAMENT_BACKEND_SNAPMAKER_U1.md` correctly. A sixth did not move,
because it is a **bare range with no path prefix**:

```
(`ams_backend_snapmaker.cpp:1692-1739`, `1755-1796`).
```

The first range shifted +20. The second could not be attributed to a file, so nothing
checked it and it kept pointing at `port_present_changed = true;` mid-block. The prose
describes `apply_overrides()` layering user fields over firmware truth; that function is
**1780-1823**. (A naive +20 would give `1775-1816`, but 1775 is a blank line — which the
anchor tool refuses to anchor anyway — and 1816 lands inside the `clear_async` lambda.
The function's own span is the right answer.)

**Census over the whole scanned doc set:**

| Form | Count |
|---|---|
| backticked `` `:N` `` / `` `:N-M` `` follow-on | 444 |
| backticked bare `` `N-M` `` sharing a line with a citation | 14 |
| unbackticked `(:N)` shorthand | 0 — the single instance was `QIDI_BOX_HEATER.md:205`, removed by the rewrite |
| real `` `path:N` `` citation inside a fenced block | 0 |
| **total unparseable** | **458**, across 25 docs (463 before this audit's repairs) |
| *gate-visible, for comparison* | *850* |

**35% of every line reference in the docs is unverifiable by any gate** (458 against 854
tracked). Worst
concentrations: `11-startup-shutdown.md` (69), `07-filament-ams.md` (42),
`12-system-services.md` (38), `03-threading-lifetime.md` (36). The count is an upper
bound — a few are teaching examples rather than live references (`CLAUDE.md:108` uses
`` `:123` `` to *describe* the syntax) — but the great majority are real.

The rot rate in this class looks bad, on a small sample. Of the 12 examined by hand
across this audit, **11 were wrong**: the one above, three more bare ranges in the same
file, all three follow-ons in `PAGE_SCROLL_BUTTONS.md`'s list of `on_root_shown` call
sites (`:1744`/`:1979`/`:2085` → `1870`/`2105`/`2211`), and four others corrected
alongside the citations they trail. That is a biased sample — I looked where a full
citation was already suspect — but nothing in this class has *ever* been checked, so
there is no reason to expect it to be healthier than the checked population.

Three further bare ranges in `FILAMENT_BACKEND_SNAPMAKER_U1.md` were verified and
repaired, each promoted to a full path-qualified citation so the gate now tracks it:

| Doc line | Was | Now | Why |
|---|---|---|---|
| :110 | `860-869` | `ams_backend_snapmaker.cpp:869-878` | started 9 lines early in unrelated `lane_data` commentary; the SUB_TYPE guard + write are 869-878 |
| :178 | `1694-1699` | `ams_backend_snapmaker.cpp:1764-1769` | pointed inside the parse-convergence comment; `mark_slot_unloaded` and its grace-window comment are 1764-1769 — **70 lines stale** |
| :282 | `942-948` | `ams_backend_snapmaker.cpp:951-957` | stopped at the section banner; both `not_supported` bypass entry points are 951-957 |

Two others in that file (`1171-1204` for the RFID apply loop, `1321-1354` for the
error-latch guard) were checked and are correct; they were left in bare form, because
converting 458 references by hand is the wrong fix. The generator change below is the
right one.

## What was fixed

59 citations across 21 files. Grouped by what the citation was *for*:

**Constants and thresholds** — `LONG_PRESS_THRESHOLD_MS` (825→844), `MISMATCH_MIN_CONFIDENCE`
(531→552), `AUTOSAVE_MIN_CONFIDENCE` (576→597), `MAX_SLOTS`/`MAX_UNITS` (60→62, 75→77).

**Function definitions** — `pick_object_at` (1363/1387→1389), `try_get_layer_segments`
(1513→1546), `sync_from_backend` (1186→1247 in the header, 1356→1398 in the cpp),
`get_bed_temp_subject` (359→336), `init_backends_from_hardware` (710→712),
`AmsBackend::create(AmsType, api, client)` (557→607), `register_callbacks` (85→102),
`init_widget_registrations` (147→149), `DisplayManager::shutdown` (630→632),
`deinit_subjects` (130→133), `on_objects_clicked` (1947→1998),
`render_selection_brackets` (1640→1642), `start_tail_summary_scan` (1840→1842),
`is_using_2d_mode` (386→389), `is_on_elevated_surface` (2257→2261), `set_dark_mode`
(394→434), `is_*_printer` family (444→466), `Provider` table (69→13),
`get_list_options` (243→208), `start_tail_summary_scan` and `is_support_segment`
neighbours.

**Call sites and wiring** — `overlay_panel.xml` registration (414→428),
`notify_timelapse_event` registration (2962→3110), `notify_history_changed` + `token.defer()`
(240/250→274), `set_slot_info(persist=false)` (362→426), `SoundManager` client wiring
(436→467), HTTP-base redirect under `--test` (181→199), `AmsBackendMock` simulator
subscription (450→481), `push_overlay(root, false)` keypad site (164→195),
`register_overlay_instance(..., persistent)` (1315→1329), `PANEL_WIDGET_TILE_FLAG` set
site (881→885), `attach()` call (925→940), `zoffset::required_status_objects` in the
subscription builder (1367→1464), `persistence_provider_name` logging (1510→1613),
`discover_from_config` main-thread queue (741→856), evdev touch open (489→497), the PCH
patch-stamp prerequisite (208→214), `set_gcode` streaming null-out (132→134), the NVI
entry-point block (44→53), `PrinterState`'s thirteen domain members (2252→2285-2328), the
AMS detection ladder (536→593), the verbatim-quoted notification unpack (293→311),
`ui_panel_home` scroll snapshot, `on_root_shown`'s four call sites (1402/1744/1979/2085 →
1431/1870/2105/2211 — **all four wrong**), the `print_lifecycle_prev` write
(1432→1513), the coordinate-space doc block (42→36), the Snapmaker pre-print send block
(315→319), Snapmaker U1 detection (413→414), and the AD5X `head_filament_` untrustworthy
note (808→80).

Plus the four class-3 repairs above (`apply_overrides` 1755→1780, SUB_TYPE 860→869,
grace window 1694→1764, bypass 942→951), each promoted from a bare range to a
path-qualified citation — which is why the tracked corpus grew from 714 to 718.

`make regen-doc-links` was run after the edits and **converges** — the second consecutive
run reports `0 rewritten, 0 bootstrapped` for anchors and `0 rewritten` for links.
`scripts/check_doc_refs.py` is green on all six of its checks.

## What was NOT fixed, and why

1. **`docs/devel/QIDI_BOX_HEATER.md:205`** — "HelixScreen currently sends bare `T<n>`
   (`ams_backend_qidi.cpp:688`) and `UNLOAD_T<n>` (:720)". Both numbers are wrong, but
   re-pointing them would make the doc *worse*: the code no longer sends those macros at
   all. `AmsBackendQidi::do_change_tool()` (1157) now says in its own comment "The bare
   T<n> macros don't exist for the box's own slots on Q2 firmware", and resolves
   tool→slot to drive the verified load path instead; unload goes through
   `build_unload_gcode()` (1034), which emits `M603` or `EXTRUDER_UNLOAD`. The doc is
   describing a decision (#1022 Phase 3) that has since been made in the code. This needs
   a prose rewrite by whoever owns that decision, not a line-number repair.

2. **`docs/devel/RPC_ERROR_OWNERSHIP.md`** (:151, :153, :155, :283) and
   **`docs/devel/architecture/15-known-debt.md`** (:68, :99, :106, :171) — another branch
   is repairing these concurrently. Left untouched. Two are worth naming so they are not
   lost: `15-known-debt.md:106` cites `moonraker_discovery_sequence.cpp:741` for "the
   configfile discovery step", but 741 is `detect_webcam()` inside the **server.info**
   step; the configfile step is at ~840–860. And `RPC_ERROR_OWNERSHIP.md:153` cites
   `ams_subscription_backend.cpp:529-533` for
   `caller_surfaces_errors.value_or(on_error != nullptr)`, but 529 is the `if (!api_)`
   synthetic-error branch.

3. **~40–80 class-2 citations the heuristic did not surface.** The 40-sample audit says
   they exist; finding them needs either a better signal or a pass a human actually reads.
   Not attempted here.

4. **458 class-3 references.** Verifying them by hand means resolving each against the
   citation it trails and reading the prose — the same cost per item as the main audit,
   for a population 65% as large. Do the generator change first; it converts most of them
   into ordinary tracked citations that the existing gate then keeps honest for free, and
   turns "verify 458 by hand, once" into "the gate tells you which ones broke".

## Recommendation

**Promote the low-information-line check into `doc_cite_anchors.py` as a hard gate.** It
is the highest-precision signal found (75–82%), it needs none of the identifier
extraction, and it is a ten-line predicate: *refuse to anchor a citation whose target
line, whitespace-stripped, is a lone brace, a blank, a bare `#endif`, or a comment banner
made only of `=`/`-`/`*`.* The `blank-line` rule already in the tool is exactly this idea
applied to one case; the bootstrap found 32 of those and "every one was a genuine defect".
The same is true of `}` — the audit found 29 of them and 29 were defects or near-misses.
`scripts/doc_cite_anchor_baseline.txt` is the ready-made escape hatch for a citation
where a brace is genuinely the point.

Measured effect of the repairs: sidecar rows still anchored to a low-information line fell
from roughly 30 to **12 of 754 (1.6%)** — and 7 of those 12 are the same
`temperature_service.cpp:667` row duplicated across stale `.claude/worktrees/*/CLAUDE.md`
snapshots. The genuinely distinct remainder is about five, each hand-checked and
defensible (a `/**` that opens the doc block a reading-list entry names, the `{` that
opens a JSON file, the `// ====` banner that opens the FULL-LOAD path). A gate would hold
it there rather than let it climb back.

Do **not** promote the symbol-proximity heuristic as a gate at 21–61% precision. It earned
its keep as a triage tool for a one-time human pass and should stay one.

**Second, and larger: teach `iter_citations()` the two surviving unparseable forms.** 458
references — **35% of every line reference in the docs** — are invisible to every gate,
and 11 of the 12 examined by hand were wrong. The change is small and mechanical:

- **`` `:N` `` and `` `:N-M` `` (444)** — resolve against the file named by the nearest
  preceding full citation *on the same line*, then anchor like any other citation. The
  same-line rule matters: `check_doc_refs`' `SYMBOL_CITE_B_RE` already learned the hard
  way that `\s*` spanning newlines pairs a trailing citation with the next bullet's
  symbol. Attribution across lines would repeat that bug.
- **bare `` `N-M` `` after a citation (14)** — same rule, same owner. This is the form
  that produced the live failure above.
- **unbackticked `(:N)`** — zero remain. The corpus had exactly one, in
  `QIDI_BOX_HEATER.md`, and the prose rewrite there removed it. The form needs no
  resolver support.

A doc-side alternative exists for the last two — write them path-qualified, as the four
repaired here now are — but at 458 sites that is a bulk hand-edit, which this repo's own
convention says to replace with a generator plus a gate. Do it in the resolver.

**Status: the resolver change shipped.** `doc_cite_anchors.py --bare-refs` now pairs each
bare `` `:N` `` with the nearest preceding full citation on its own line *whose file
actually contains that line*, and emits the result as a **review list rather than
anchoring anything**. That last decision is the important one: a census put the class at
459 with 11 of 12 hand-checked wrong, so bootstrapping them where they sit would have
frozen ~400 bad citations and made them look maintained — the original bootstrap's mistake
at four times the scale. `include_bare` defaults off, so the bootstrapper cannot reach a
derived citation at all, and a test pins the sidecar row count either side of it.

---

# Remaining debt: 257 bare refs whose prose nobody has checked

The unanchorable entries have been worked (49 → 5). What is left is the population the
gate can now *see* but cannot judge.

## The count, and that it grows on its own

| | |
|---|---|
| Bare `` `:N` `` refs in the docs | **437** across 26 docs |
| **Attribute to exactly one file — the reviewable set** | **257** (252 the only file cited on their line, 5 singled out by which file is long enough) |
| No antecedent on their line | 168 — not a failure; the file is named in prose or an earlier paragraph |
| Two or more candidate files | 12 — not a failure; the resolver refuses rather than guesses |
| Still unanchorable after the repair pass | **5** |

**This population grows without anyone touching a doc.** During the single task that
promoted nine refs, merging main introduced **four new blank-landing bare refs** in files
that had been clean an hour earlier. That is the mechanism, not an anecdote — see below.

## The one argument that matters

**Full citations self-heal on every regen. Bare ones re-rot on every merge.**

This audit proved it on itself. Between one pass and the next, `src/application/application.cpp`
grew 23 lines, and **three of six targets moved within a single working session** —
`start_auto_send` 3662→3685, `ObserverGuard::invalidate_all` 5206→5229,
`theme_manager_deinit` 5247→5270. The full citations on those same lines were re-pinned
automatically and needed no attention. The bare ones needed a human, twice, for the same
six references.

Every bare ref left in the tree is therefore not static debt but a slow leak: the count of
*wrong* ones rises monotonically with commit volume until someone converts or checks them.

## Projection for a first slice — a projection, not a measurement

Preston declined the ~45 minutes it would take to turn this into a counted number, which
is reasonable: it would not change what anyone does today. So the following is
**extrapolated from two other populations and has never been measured on this one.**

| | |
|---|---|
| Expected flags from the triage heuristic | **50–70** of 257 |
| Expected real defects among them | **30–45** |
| Effort | one working session, same shape as the 49 |

Basis: the flag rate on full citations was 110/846 ≈ **13%**, and **57%** of those flags
were real. Bare refs have never been checked by anything, so a higher flag rate was
assumed — 20–27%. Nothing else supports the range.

The prior that they are *worse* than the 49 rests on two measured results, both in this
document: `RELEASE_1_0_CHECKLIST.md:206` carried five bare refs of which four were stale
while only the blank-line one was ever flagged, and `ui_update_queue.h:438` was wrong in
four separate places across two chapters. Both were invisible to every gate.

## Precision by signal — what makes a future slice cheap

Measured on 102 hand-verified flags (the full-citation audit). Order the work by this
table rather than by document:

| Signal | Precision |
|---|---|
| Cited line is low-information (`}`, blank, `// ====` banner) | **75–82%** |
| Named symbol absent near the cite, but found elsewhere in the file | 61–80% |
| Named symbol absent, and not in the file at all | **21%** |

The cheapest signal is the best one and needs no NLP. The symbol-proximity signal that
looks strongest is the weakest — it fires on markdown table rows (a cell's symbol paired
with a neighbouring cell's citation) and on sentences whose subject is a type while the
citation points at a call site.

## Proposed first slice — so nobody re-derives the method

1. Adapt the triage heuristic (`cite_semantic_triage.py`, session scratchpad — see "The
   heuristic" above) to take the resolver's `--bare-refs` pairings as its input. The tool
   already takes doc + path + line; feeding it the pairings is the only change.
2. Score all 257 and print the histogram. **Report the histogram before working anything**
   — that converts the projection above into a counted number for free, and it is the step
   that was declined only because it was bundled with the work.
3. Work score ≥3 top-down. Hand-verify every one; the heuristic's false-positive rate is
   43% and a "fix" that moves a correct citation is worse than the rot.
4. Promote anything whose antecedent is genuinely a different file (as the nine were)
   rather than only renumbering it.

## The 5 residual, and why each is not a repair

- **3 syntax placeholders** — `scripts/CLAUDE.md:96,98` use `` `file.cpp:70` `` and
  `` `:857` `` as *examples of citation syntax* while describing the gate. `file.cpp` is a
  made-up name. The resolver should exclude these outright; nobody should "fix" them.
- **`09-home-widgets.md:196`** — `panel_widget_manager.cpp:149` is `try {`, and the prose
  says "the per-widget try/catch (`:149`)". Correct target, unanchorable shape. Leave.
- **`11-startup-shutdown.md:193` — needs an author decision, not a repair.** The prose
  promises "the splash-handoff **and** 11s-failsafe blocks (`:4154`–4205)", one range for
  two blocks that have since separated: the 11s failsafe constant is at
  `src/application/application.cpp:4040`-4046, the handoff repaint at `:4189`-4205.
  Re-pointing the range to either one silently drops the other. The fix is to split it
  into two references, which changes what the sentence promises — an author's call.

Rough coverage arithmetic if the 257 are converted: 854 tracked today, ~1300 after. The
existing anchor machinery then keeps all of them honest with no further human passes,
which is the whole point of having built it.
