# Doc citation staleness audit — how many bootstrapped anchors point at the wrong line?

**Date:** 2026-08-27
**Branch:** `audit/doc-citation-staleness`
**Scope:** every `` `file:N` `` citation the anchor gate tracks, minus
`docs/devel/RPC_ERROR_OWNERSHIP.md` and `docs/devel/architecture/15-known-debt.md`
(being repaired concurrently on another branch).

## The problem this audits

`scripts/doc_cite_anchors.py` pins each `` `file:N` `` citation to a content anchor, so
the number self-heals when code moves. It works. But it was **bootstrapped from the
citations already in the docs**, and some of those were already wrong. The bootstrapper
derived an anchor from whatever line the stale citation happened to land on, so the gate
now faithfully maintains a wrong number and reports green forever.

This class is undetectable by the gate by construction: the anchor is unique and resolves
cleanly. It just resolves to a line that does not support the sentence citing it. Only a
semantic comparison of prose against the cited line finds it.

## Numbers

| | |
|---|---|
| Citations in the corpus | **715** unique `(doc, path, line)` keys / 846 occurrences |
| **Confirmed wrong and fixed** | **59** (8.3% of the corpus) — every one hand-verified against both the prose and the source |
| Untracked bare `` `:N` `` follow-ons also repaired | 8 |
| Estimated *total* wrong, including what the heuristic missed | **~110–140 (15–20%)**, wide interval — see below |
| Left unfixed, deliberately | 3 (below) |

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

3. **~40–80 citations the heuristic did not surface.** The 40-sample audit says they
   exist; finding them needs either a better signal or a pass a human actually reads. Not
   attempted here.

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

Do **not** promote the symbol-proximity heuristic as a gate at 21–61% precision. It earned
its keep as a triage tool for a one-time human pass and should stay one.

**Second, close the bare-`:N` blind spot.** There are **443** follow-on references of the
form `` `:71` `` / `` `:294` `` in the docs — more than half the size of the tracked
corpus — and the gate cannot see a single one. They rot identically: of the eight this
audit touched incidentally, **all eight were wrong**, including all three follow-ons in
`PAGE_SCROLL_BUTTONS.md`'s list of `on_root_shown` call sites. The fix is small: resolve a
bare `` `:N` `` against the file named by the nearest preceding full citation on the same
line, and anchor it like any other. That roughly doubles the gate's coverage for a modest
change to `iter_citations()`.
