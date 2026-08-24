# Code Review Rubric

The quality bar for HelixScreen changes, written down so reviews are consistent whether a
human, `/code-review`, or a reviewer subagent is doing them.

Two rules govern how to use it:

**Don't re-check what a gate already checks.** `scripts/quality-checks.sh` runs on every commit
and in CI. Reviewer attention spent on copyright headers or `printf` calls is attention not
spent on the things that actually ship crashes. The machine-checked list is at the bottom.

**Rank by what has actually broken.** The tiers below are ordered by real production impact in
this codebase, not by how easy an issue is to spot. Every Tier 1 item has a crash and an issue
number behind it.

---

## Severity

| Level | Meaning | Action |
|-------|---------|--------|
| **Blocker** | Crashes, corrupts state, or bricks a device. Tier 1, or any Tier 2 item on a critical path. | Do not merge |
| **Major** | Wrong behaviour a user would notice, or a silent-failure trap that will bite later | Fix before merge, or file with a reproducer |
| **Minor** | Inconsistency, missed pattern, or a readability problem | Note it; fix if the diff is already open |
| **Note** | Observation for later; no action implied | Mention once, don't relitigate |

A finding needs a **concrete failure scenario** — inputs or state that produce the wrong
output. "This looks fragile" is not a finding. If you can't say what breaks, it's a Note.

---

## Tier 1 — Crash families

These have each cost at least one field crash. On 32-bit, memory-constrained targets
(K1/AD5M/CC1) races that are theoretical on a desktop reproduce reliably.

Full rules: [`THREADING.md`](THREADING.md).

| Check | What to look for | Refs |
|-------|------------------|------|
| **LVGL off the main thread** | Any `lv_*` or `lv_subject_set_*` reachable from a WebSocket/libhv, HTTP, or timer callback without `ui_queue_update()` / `tok.defer()`. Remember `lv_subject_set_int()` counts — it fires observers. | — |
| **Sync deletion inside a queued callback** | `safe_delete()`, `lv_obj_delete()`, `lv_obj_clean()` inside `queue_update` / `async_call` / `lifetime_.defer` / `tok.defer` / `observe_int_sync` bodies. `lifetime_.defer` does **not** escape the batch. | #776, #190, #80 |
| **Dynamic subject's `SubjectLifetime` fetched but not passed to `observe_*`** | `get_temp_subject(name, lt)` / `get_fan_speed_subject(name, lt)` / `get_extruder_temp_subject(name, lt)` where `lt` never reaches the factory's `lifetime` argument (it defaults to `{}`, so this compiles silently). Local vs member storage of the token is **not** the tell — the owner keeps its own copy. | #705 |
| **`ObserverGuard::release()` in normal cleanup** | `release()` anywhere except a `StaticSubjectRegistry::register_deinit` callback. The reasoning "release skips the remove so it's safer" is the exact misconception behind 17 reports. | #579 |
| **Detached one-shot `std::thread`** | `std::thread(...).detach()` without try/catch. `pthread_create` returns `EAGAIN` under thread exhaustion → `std::terminate` through an LVGL C frame, and the crash looks unrelated. | #724, #837 |
| **Deletion during input dispatch** | `lv_obj_delete()` on container children inside `LV_EVENT_CLICKED` / `LV_EVENT_RELEASED`. | — |
| **Missing shutdown self-registration** | An `init_subjects()` that doesn't register its own `deinit_subjects()`, or a teardown callback that reaches a lazy `get_global_*()` getter and resurrects a singleton. | L101 |
| **`lv_obj_is_valid()` in a hot path** | Recursive O(n) walk; stack-overflows on Pi. Use null checks plus a lifetime token. | L076 |
| **Cached child pointers outliving the widget tree** | A panel or manager caches raw `lv_obj_t*` members (or containers keyed by widget) — the subtree is deleted by a rebuild/teardown, then a later callback writes through the stale pointer. Null checks pass; the pointer isn't null, it's freed. Two guards do **not** cover it: `AsyncLifetimeGuard` guards `this`, not the widget tree, and a queued `UpdateQueue` callback can drain after the tree died. Fix shape: an `LV_EVENT_DELETE` hook on the owning root nulls the cached pointers. | 882edde88, 13db7c92e |

---

## Tier 2 — Silent failures

No crash, no compile error, no failing test. These are the ones that pass review by looking
reasonable, and the reason adversarial reading matters.

| Check | What to look for | Refs |
|-------|------------------|------|
| **Lossy mutation of a member returned by a getter** | Compacting, filtering, or reordering a member container changes the semantics of *every* accessor that returns it. Audit all consumers, not just the path you're changing — especially any that index positionally by a domain id. | L100 |
| **Recycled widget with retained state** | `PanelWidgetManager` reuses instances across rebuilds. An imperative apply that only runs in `on_size_changed()` leaves a reused instance stale when the new size matches a stale flag. Hoist it and call from `attach()` too. | L099, #1109 |
| **Test inputs that don't match runtime** | A pure decision function is only as good as whether its test inputs are what it actually receives. Assert against the value held at the real call site, not a convenient one. | L093 |
| **Click target that silently absorbs** | `lv_obj` / `ui_card` / `ui_dialog` are clickable by default and `clickable="false"` does not inherit. Symptom: "the thumbnail works but the text area is dead." | L071, #1101 |
| **JSON null vs missing** | A default-constructed `nlohmann::json` is null, and `.value()` throws on null. Guard with `is_object()` or initialize to `json::object()`. | L087 |
| **Thread/network test left untagged** | Tests using `std::thread` / `condition_variable` / `hv::EventLoop` must be `[slow]`, or they deadlock parallel shards. | L052 |
| **Claim not verified against current code** | A root cause inherited from an issue report, a commit message, or a stale comment. Grep the current tree and `git log -S` before accepting a mechanism — confident, well-argued reporter archaeology has pointed at the wrong cause more than once. | L095 |
| **Log-only `on_error` claiming the error report** | An `execute_gcode` / `set_temperature` / `set_led` caller whose `on_error` only `spdlog`s or resets state, without `caller_surfaces_errors=false`. It records the rejection for dedup and silences `GcodeErrorRouter`'s `!!` copy — the only surface that would have told the user. Deriving intent *after* an internal callback wrapper has the same effect. | `RPC_ERROR_OWNERSHIP.md` |
| **Print-state enum mismatch** | `static_cast<PrintState>(lv_subject_get_int(...))` compiles against whichever subject the author named — and `PrintJobState` and `PrintState` share no numbering past index 0, so a COMPLETE job reads back Paused and a PRINTING one Preparing. Binding the wrong subject to a typed observer fails the same way. Compiles, runs, answers a different question; shipped twice in one refactor. Use `get_print_lifecycle()` / `get_print_job_state()` and the typed factories (see `architecture/05-printer-state.md` § "Reading print state: typed accessors, not hand-cast ints"). | `check_print_state_cast.py` (partial) |

---

## Tier 3 — Architecture and consistency

| Check | What to look for |
|-------|------------------|
| **Declarative UI** | New UI that mutates an XML widget from C++ instead of binding a subject. The gate ratchets the count, but it can only see the specific `find_by_name` + setter shape — read for the intent. Structural exceptions are listed in CLAUDE.md; existing imperative code is **not** precedent. |
| **Centralized sends** | Temperature targets go through `TemperatureController::set_target()`, never `MoonrakerAPI::set_temperature()` directly. |
| **Design tokens** | `theme_manager_get_color("token")` and `#space_md`, not raw hex or pixel counts. |
| **Pattern reuse** | Does an existing helper already do this? Check the exemplars in CLAUDE.md § Patterns before accepting a hand-rolled version. |
| **Tests that would fail if the feature were removed** | Not `REQUIRE(true)`, not happy-path only. Edge cases and error conditions. |
| **Docs updated with behaviour** | If the change alters behaviour or architecture, the relevant `docs/devel/` doc moves with it. |

---

## What NOT to flag

Noise makes a review ignorable, and these all look like problems but aren't:

- **Anything the gates check.** See below.
- **The structural exceptions to the declarative rules** — custom XML widget implementations
  (files calling `lv_xml_register_widget`), `LV_EVENT_DELETE` / draw hooks / `SIZE_CHANGED` /
  gestures, measured layout and computed fonts, C++-created canvas widgets, per-item payloads
  on generated collections, the `ctl` remote-control server, CLI stdout. Full list in CLAUDE.md.
- **Pre-existing imperative UI in a file the change happens to touch.** It's tracked debt
  (#1140) with a ratchet. Don't demand a refactor as the price of an unrelated fix, and don't
  treat its presence as license to add more.
- **Line counts, formatting, import order** — clang-format owns these.
- **Style preferences with no failure scenario.** Match the surrounding code.

---

## How to verify a finding

```bash
./scripts/quality-checks.sh              # everything the gates check
make -j                                  # it has to build
make test-run                            # parallel; excludes [slow] and hidden
./build/bin/helix-tests "[tag]" "~[.]"   # one area
make test-asan                           # suspected UAF or leak
make test-tsan                           # suspected race
python3 scripts/check_imperative_ui.py --list <file>
```

For UI changes, run it: `helix-screen ctl` drives navigation, clicks, and screenshots
programmatically (`docs/devel/HELIXCTL.md`), so "I couldn't check it visually" isn't a reason
to skip verification. Use `--sim-speed 4..10` to reach an active print in seconds.

---

## Already machine-checked — do not spend review time here

`scripts/quality-checks.sh`, on every commit and in CI:

| Gate | Enforces |
|------|----------|
| `check_l081_anti_pattern.py` | No bare `tok.expired()`/`expired_no_lvgl()` then `this` access on a bg thread |
| `check_subscription_null_safety.py` | Subscription-handler JSON reads are guarded (baseline 0) |
| `check_imperative_ui.py` | XML-owned widgets driven from C++ (ratcheting baseline) |
| `check_doc_refs.py` | Docs cite files that exist (CLAUDE.md files, skills, all of `docs/devel/`); `docs/devel/` index is complete |
| `check_gcode_error_ownership.py` | Log-only error callbacks declare `caller_surfaces_errors=false` (baseline 0) |
| `check_translation_format_specifiers.py` | Translated strings keep their placeholders |
| `check_modal_chrome_budget.py` | A modal's chrome matches the content cap it budgets against: everything but a divider and the button row lives inside the scroll container; a second button row switches to the tall-chrome token; no card raised above the shared 85% cap (`MODAL_CHROME_OK` opt-out) |
| `check_raw_print_job_state.py` | Every read of the raw print wire (`PrintJobState::…`, `get_print_job_state()`, `get_print_state_enum_subject()`) says why it is not on the lifecycle — `// RAW_PRINT_STATE_OK: <reason>` (baseline 0) |
| `check_print_state_cast.py` | No hand-casting `lv_subject_get_int()` into `PrintState`/`PrintJobState`; the typed accessors pair each subject with its own enum (`PRINT_STATE_CAST_OK` opt-out) |
| spdlog-only | No `printf`/`cout`/`LV_LOG_` outside CLI subcommands |
| design tokens | Hardcoded colors ratcheted; no private `_lv_*` APIs |
| copyright, icon fonts, XML validity, shellcheck | Headers, codepoint sync, well-formed XML |

`tests/shell/test_code_lint.bats` additionally forbids `_for_testing` methods on production
classes, raw `set_temperature` sends from migrated temp views, the "centidegree" misnomer, and
inline decidegree conversion.

A rule that keeps getting missed in review is a rule that wants a gate. Adding one to
`quality-checks.sh` is usually cheaper than catching it by eye forever.
