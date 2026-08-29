# AMS Remap Capability Model — Design Spec

**Status:** design settled, plan to follow when the chip branch lands.
**Goal:** one declaration of whether and how a backend can carry out a user's tool→lane choice, with readiness modelled once instead of nowhere.

## The problem

Four spellings answer overlapping versions of one question, and two of them disagree on real hardware.

| # | Spelling | Kind | Where |
|---|---|---|---|
| 1 | `ToolMappingCapabilities{supported, editable, description}` | **runtime** query, mutex-guarded on 2 backends | `include/ams_types.h:1894`, `AmsBackend::get_tool_mapping_capabilities()` |
| 2 | `helix::printer::honors_user_tool_mapping(caps, applies_via_preprint)` | free function over #1 + #5 | `include/ams_types.h:1923` |
| 3 | `AmsBackend::honors_user_tool_mapping()` | **zero-arg member** wrapping #2 with its own virtuals | `include/ams_backend.h:1831` |
| 4 | `RemapStrategy{None,Native,GcodeRewrite,SnapmakerNative}` | **static** per-class declaration | `include/ams_backend.h:1967`, `get_remap_strategy()` |
| 5 | `requires_preprint_send()` | static per-class declaration | `include/ams_backend.h:2178` |

#2 and #3 share a name with different arity, so grepping the name conflates two call shapes.

### Divergence 1 — real, on shipped hardware (AD5X)

`src/printer/ams_backend_ad5x_ifs.cpp:2669` locks a mutex and returns `{false,false,""}` when `!has_ifs_vars_`.
`include/ams_backend_ad5x_ifs.h:467` returns `RemapStrategy::Native` unconditionally.

Before the firmware's `_IFS_VARS` macro is discovered, `honors_user_tool_mapping()` says **false** and `get_remap_strategy() != None` says **true**. No compile error, no test, and callers reach for whichever spelling is nearest.

### Divergence 2 — mock vs the backend it emulates

`AmsBackendMock` in `tool_changer_mode_` declares caps `{false,false,""}` (`ams_backend_mock.cpp:3356`) and strategy `None` (`:3371`). The real `AmsBackendToolChanger` declares `{true,true,"Tool reassignment via ASSIGN_TOOL"}` and `Native`. The mock teaches the opposite of the shape it stands in for.

### Why it rotted this way

`editable` was the original question and is documented as only half an answer (`ams_types.h:1900`): the U1 reports `editable=false` and still honors every pick, via a pre-print send. Rather than fix the question, #2 was added to combine it with #5, and #4 grew separately to describe delivery. `ams_state.h:376` records the bug the half-answer already caused: auto-match keyed on `editable` alone put the U1 in the wrong bucket and let colour proximity rewrite the print's routing with no way to decline.

## The model

`RemapStrategy` already encodes both *whether* and *how*. Make it the single declaration and add the axis nothing currently models — readiness.

```cpp
enum class RemapStrategy {
    None,             // cannot honor a user tool->lane choice
    Native,           // writes the machine's persistent mapping table
    GcodeRewrite,     // rewrites tool commands in the job
    SnapmakerNative,  // firmware pre-print send (SET_PRINT_EXTRUDER_MAP)
};

// What this backend is capable of, by construction. Static.
virtual RemapStrategy remap_strategy() const { return RemapStrategy::None; }

// Is that capability usable RIGHT NOW? THE only place readiness lives.
// Default true; override only where discovery gates it.
virtual bool remap_ready() const { return true; }

// UI hint text — the only field of ToolMappingCapabilities with independent content.
virtual std::string remap_description() const { return {}; }
```

Capability questions become free functions — pure, testable, one definition each:

```cpp
bool remap_is_persistent(RemapStrategy s);  // Native || GcodeRewrite
bool remap_via_preprint(RemapStrategy s);   // SnapmakerNative
bool can_remap(const AmsBackend& b);        // b.remap_strategy() != None && b.remap_ready()
```

**Retired:** `ToolMappingCapabilities` (struct and virtual), both `honors_user_tool_mapping` overloads, `requires_preprint_send()`.

**CLAUDE.md's test — "adding a second firmware with the same capability must touch exactly one file":** a new backend declares `remap_strategy()`, plus `remap_ready()` only if gated. One file. Today it means touching the capability struct, the strategy enum and the preprint predicate separately.

## Call-site migration

Each existing read maps to exactly one new question. The mapping is not uniform — this is where the current model's ambiguity has to be resolved deliberately, not mechanically.

| Site | Reads | Becomes | Note |
|---|---|---|---|
| `ams_state.cpp:71` | `.supported` | `can_remap(*backend)` | Drives `ToolTopology` construction. `.supported` here means "is mapping data available", which IS the readiness question. On AD5X this preserves today's behaviour exactly, because `remap_ready()` returns `has_ifs_vars_`. |
| `ams_state.cpp:1030` | `honors_user_tool_mapping()` (member) | `can_remap(*backend)` | `effective_auto_match()` primary input. |
| `ams_state.cpp:1040` | `.editable` | `remap_is_persistent(b.remap_strategy()) && b.remap_ready()` | Inside the HELD `kPreprintSeedFollowsUserSetting` branch. Preserve the held-flag semantics; do not simplify it away as part of this refactor. |
| `ui_print_start_controller.cpp:656` | free `honors_user_tool_mapping(caps, preprint)` | `can_remap(b)` | `should_warn_remap_unsupported()` — the "remap not supported" toast. Its signature changes from two values to one backend ref. |
| `ui_print_start_controller.cpp:722` | `.editable` | `remap_is_persistent(...) && remap_ready()` | Gates the generic `set_tool_mapping()` apply path. |
| `ui_print_start_controller.cpp:325, 455` | `requires_preprint_send()` | `remap_via_preprint(b.remap_strategy())` | Pre-print send gates. |
| `ui_filament_mapping_card.cpp:86-88` | `.supported`/`.editable` | **deleted** | Removed by the chip branch's Task 3 before this work starts. |
| `ui_preflight_check_modal.cpp:77` | `get_remap_strategy() != None` | `can_remap(b)` | Gains the readiness check it should always have had. |
| `ui_panel_print_select.cpp:2827, 2890` | `get_remap_strategy()` | `b.remap_strategy()` | Rename only; the `switch` on strategy is the correct shape and stays. |
| `ui_print_select_detail_view.cpp:1626` | `get_remap_strategy() != None` | `can_remap(b)` | Same readiness gain. |
| `ui_ams_context_menu.cpp:821-830` | `.supported`/`.editable` | none — **commented-out dead code** | Delete the dead block rather than migrate it. |

**Behaviour changes this introduces, all intentional:** the three sites that ask `strategy != None` today gain a readiness check, so on an AD5X before `_IFS_VARS` discovery the pre-flight "Remap…" button, the swatch-card tap and the topology build now agree instead of contradicting each other.

## Per-backend declarations after the change

| Backend | `remap_strategy()` | `remap_ready()` | Change from today |
|---|---|---|---|
| ACE | `None` | default `true` | none |
| AFC / CFS / Happy Hare / QIDI / ToolChanger | `Native` | default `true` | none |
| AD5X IFS | `Native` | **`has_ifs_vars_`** (mutex-locked) | **the fix** — readiness moves out of the caps query into the one place it belongs |
| Snapmaker U1 | `SnapmakerNative` | default `true` | `requires_preprint_send()` retires; derived from the strategy |
| Mock | see decision below | | |

### Open decision — the mock's tool-changer shape

Today the mock's `tool_changer_mode_` returns caps `{false,false,""}` and strategy `None`, contradicting the real `AmsBackendToolChanger` (`{true,true,...}` / `Native`).

**Recommendation:** make the mock mirror the real backend — `Native`, ready. It is a mock of a tool changer; teaching the opposite shape is how a UI path goes untested. **This changes test expectations** in `test_ams_tool_mapping.cpp` ("Mock backend tool mapping - tool changer mode", `:186`) and `test_remap_strategy.cpp` (`:305`), so it must be a deliberate, reviewed step — not folded silently into the mechanical rename. If the divergence turns out to be load-bearing for a specific test shape ("a tool changer that cannot remap"), keep it and add a named mock mode for that instead of overloading `tool_changer_mode_`.

## The test that would have caught this

`tests/unit/test_remap_strategy.cpp` is already the authoritative per-backend pin file, and it already carries an agreement invariant of exactly the right shape (`requires_preprint_send()` ⇒ `build_preprint_gcode()` non-empty, `:294-318`). The caps↔strategy axis never got one.

Add a table-driven case over every real backend probe (`Afc/HappyHare/Cfs/Ad5xIfs/ToolChanger/Snapmaker/Ace/Qidi/Base`) asserting declared `remap_strategy()` and `remap_ready()`, and — for the readiness-gated ones — that flipping the gating member flips `can_remap()` and nothing else. A new backend that forgets `remap_ready()` then fails a test rather than shipping a silent contradiction.

## Traps for whoever implements this

1. **`EndlessSpoolCapabilities::editable()` is a different axis and a METHOD**, not a field (`ams_types.h:1715`). Never sweep it. `src/ui/ui_ams_context_menu.cpp` has the dead `ToolMappingCapabilities` block at `:821-830` and live `EndlessSpoolCapabilities::editable()` calls at `:845/:850/:869` in the same function — highest-risk file in the tree for a careless rename.
2. **`.supported` collides with `DryerInfo::supported`**, unrelated, heavily used in `test_ams_backend_{ace,happy_hare,qidi}.cpp`. Any `.supported` grep must filter by receiver type.
3. **`test_print_start_filament_gate.cpp:39-41`** builds `ToolMappingCapabilities` by *positional* brace-init. It gets rewritten to the new API anyway, but until it does, a field reorder swaps meanings silently rather than failing to compile.
4. **Doc citations need re-pinning.** `docs/devel/FILAMENT_BACKEND_SNAPMAKER_U1.md:213-223` cites `include/ams_backend_snapmaker.h:259-266` and `ui_print_start_controller.cpp:317-404` by pinned line number. Run `make regen-doc-links` after. `FILAMENT_MANAGEMENT.md:2233`, `FILAMENT_BACKEND_TOOLCHANGER.md:87` and `FILAMENT_BACKEND_QIDI_BOX.md:63,100` also name the retired API and need prose edits, not just re-pinning.
5. **Not serialized anywhere** — no JSON, no `ctl`, no XML, no crash bundle. The blast radius is C++ and docs only, which is why this is safe to do as one branch.

## Scope

9 backend classes, ~12 live call sites, 8 test files, 4 docs. No wire format, no persisted state, no UI XML.
