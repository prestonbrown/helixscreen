# AMS Remap Capability Model — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Give each capability question about tool remapping exactly one spelling, so a
new firmware declares its behaviour in one file instead of three that can disagree.

**Architecture:** Five overlapping spellings collapse to three, split along what they
actually ask rather than along the order they were added. `RemapStrategy` becomes the
single declaration of *whether and how* a user's tool→lane choice is carried out, gained
a readiness axis (`remap_ready()`) that nothing modelled before, and is asked through
pure free functions. The half of `ToolMappingCapabilities` that answered a genuinely
different question — "does this backend own a tool→slot table?" — becomes its own named
virtual. The struct itself, its virtual, its dead `description` field and both
`honors_user_tool_mapping()` overloads are deleted.

**Tech Stack:** C++17, Catch2 v3, no wire format or persisted state involved.

**Spec:** `docs/devel/plans/ams-remap-capability-spec.md` (copied into this branch).
Three of the spec's claims are corrected below; where plan and spec disagree, the plan
wins and says why.

## Global Constraints

- **Vendor knowledge stays behind an abstraction** (CLAUDE.md): the test is that adding a
  second firmware with the same capability touches exactly one file. Generic code asks a
  capability question and never names a vendor.
- **Naming follows the capability, never the vendor.**
- No RTTI: `dynamic_cast`/`typeid` are lint-gated. Backend identity is answered by virtuals.
- `#include "hv/json.hpp"`, never `<nlohmann/json.hpp>`.
- Predicates carry no `get_` prefix (`requires_preprint_send()`, `has_physical_tray()`);
  value getters keep it (`get_remap_strategy()`). Follow that split for new names.
- Every behaviour change must be named in the task that makes it, with the backend it
  affects. Silent behaviour changes are the defect this branch exists to remove.
- `make test-run` green before each commit; `make regen-doc-links` after doc edits.

---

## What the code actually says (corrections to the spec)

The spec was written against a reading of the call sites. Three of its claims do not
survive contact with the code, and each changes the work.

### Correction 1 — `caps.supported` at `ams_state.cpp:71` is a THIRD question

The spec maps it to `can_remap(*backend)` and says this "preserves today's behaviour
exactly." It does not. That call site gates `build_ams_topology()`, whose own comment
says it fires "when the active backend multiplexes tools. Otherwise leave ToolState in
its extruder-enumerated state."

`AmsBackendSnapmaker` **never overrides `get_tool_mapping_capabilities()`** — it inherits
the base `{false, false, ""}` — while overriding `get_tool_mapping()` and
`set_tool_mapping()`. So the U1 gets no `ToolTopology` today, which is correct: its four
extruders are independent, and `ToolState`'s extruder enumeration is the right model.
Under the spec's mapping `can_remap(U1)` is **true** (strategy `SnapmakerNative`), so the
U1 would start building an AMS topology it has never had, on shipped hardware.

`.supported` therefore becomes its own virtual, `owns_tool_mapping_table()`, and the
topology gate keeps its current per-backend answers exactly.

### Correction 2 — `requires_preprint_send()` survives

The spec retires it and derives it from `RemapStrategy::SnapmakerNative`. But
`ams_backend.h:2174` records that the two were deliberately separated: the U1's pre-send
is **always-on, even with no remap**, to suppress a spurious-feed runout, and the comment
states outright that this "is a backend capability, not a remap-strategy proxy."
Re-deriving it from the strategy re-conflates print-start sequencing with remap
capability — the exact failure mode this branch is fixing, pointed the other way.

It is also unnecessary: `can_remap()` reads `strategy != None`, and `SnapmakerNative`
already encodes the pre-print route, so nothing in the remap question needs to ask about
pre-sends. `requires_preprint_send()` keeps its two print-start call sites and its
existing agreement test with `build_preprint_gcode()`.

### Correction 3 — `description` is dead, so `remap_description()` is not needed

The spec keeps the description as "the only field with independent content." Nothing in
`src/` reads `caps.description`; the only readers are nine assertions across three test
files, all of which assert on the string the backend itself sets. It is deleted, not
migrated.

### What that leaves

| Question | One spelling | Was |
|---|---|---|
| Does this backend own a tool→slot table? | `owns_tool_mapping_table()` | `caps.supported` |
| Can a user's tool→lane choice be carried out, and how? | `get_remap_strategy()` + `remap_ready()`, asked via `can_remap()` / `remap_is_persistent()` | `caps.editable`, `honors_user_tool_mapping()` ×2, `get_remap_strategy()` |
| Does print-start need a firmware pre-send? | `requires_preprint_send()` | unchanged |

Deleted: `ToolMappingCapabilities`, `AmsBackend::get_tool_mapping_capabilities()`,
`helix::printer::honors_user_tool_mapping(caps, bool)`,
`AmsBackend::honors_user_tool_mapping()`.

### Equivalence, per backend

`can_remap(b)` must reproduce `honors_user_tool_mapping()` on every real backend:

| Backend | `honors` today | strategy / ready | `can_remap` | Same? |
|---|---|---|---|---|
| AFC, Happy Hare, CFS, QIDI, ToolChanger | `{true,true}` → true | `Native` / true | true | yes |
| Snapmaker U1 | preprint → true | `SnapmakerNative` / true | true | yes |
| ACE | `{false,false}` → false | `None` / true | false | yes |
| AD5X IFS, `_IFS_VARS` present | `{true,true}` → true | `Native` / true | true | yes |
| AD5X IFS, before discovery | `{false,false}` → false | `Native` / **false** | false | yes |
| Base `AmsBackend` | false | `None` / true | false | yes |
| **Mock, filament-system mode** | `{true,true}` → **true** | `None` / true | **false** | **no — Task 4** |
| **Mock, tool-changer mode** | `{false,false}` → **false** | `None` / true | **true after Task 4** | **no — Task 4** |

The mock is the only disagreement, and it is the defect — in BOTH directions. Its
filament-system mode declares editable mapping and `RemapStrategy::None` at once. Its
tool-changer mode declares "not supported" where the real `AmsBackendToolChanger` declares
`{true,true}` and `Native`, so correcting it flips `can_remap()` false→true there and with
it `effective_auto_match()`, `color_card_opens_remap()` and the pre-flight button under
`--test`. Both rows move toward the backend being emulated; Task 4 resolves them together.

---

## File Structure

| File | Responsibility after this branch |
|---|---|
| `include/ams_backend.h` | Declares all three questions. `RemapStrategy` enum, `get_remap_strategy()`, new `remap_ready()`, new `owns_tool_mapping_table()`, unchanged `requires_preprint_send()`. `get_tool_mapping_capabilities()` and `honors_user_tool_mapping()` gone. |
| `include/ams_types.h` | `ToolMappingCapabilities` and the free `honors_user_tool_mapping()` gone. |
| `include/ams_remap.h` (new) | The pure free functions: `can_remap()`, `remap_is_persistent()`. Header-only, no backend state. |
| `include/ams_backend_*.h`, `src/printer/ams_backend_*.cpp` | Each backend declares its own three answers, nothing more. |
| `src/printer/ams_state.cpp` | Asks `owns_tool_mapping_table()` for topology, `can_remap()` for auto-match. |
| `src/ui/ui_print_start_controller.cpp` | Asks `can_remap()` and `remap_is_persistent()`. |
| `tests/unit/test_remap_strategy.cpp` | The per-backend pin file, extended to cover readiness and the new agreement invariant. |

---

## Task 1: The capability API, pinned per backend

**Files:**
- Create: `include/ams_remap.h`
- Modify: `include/ams_backend.h`, `include/ams_backend_ad5x_ifs.h`,
  `include/ams_backend_afc.h`, `include/ams_backend_cfs.h`,
  `include/ams_backend_happy_hare.h`, `include/ams_backend_qidi.h`,
  `include/ams_backend_toolchanger.h`, `include/ams_backend_ace.h`,
  `include/ams_backend_snapmaker.h`, `include/ams_backend_mock.h`
- Test: `tests/unit/test_remap_strategy.cpp`

**Interfaces:**
- Produces: `AmsBackend::remap_ready() const -> bool` (virtual, default `true`);
  `AmsBackend::owns_tool_mapping_table() const -> bool` (virtual, default `false`);
  `helix::printer::can_remap(const AmsBackend&) -> bool`;
  `helix::printer::remap_is_persistent(AmsBackend::RemapStrategy) -> bool`.
- Consumes: existing `AmsBackend::RemapStrategy` and `get_remap_strategy()`, unchanged.

This task is **purely additive**: the old API stays live and every existing call site
keeps compiling. Nothing migrates until Tasks 2 and 3.

- [ ] **Step 1: Write the failing per-backend pin test**

Append to `tests/unit/test_remap_strategy.cpp`. It is already the authoritative
per-backend pin file and already carries an agreement invariant of this shape.

```cpp
// Readiness is the axis the capability struct hid: a backend can be built to
// remap and not be able to yet. Only AD5X IFS has such a gate today, and it is
// the divergence this file exists to stop: get_remap_strategy() said Native
// unconditionally while the retired caps query said {false,false} until
// _IFS_VARS was discovered.
TEST_CASE("Backends declare remap readiness, defaulting to ready", "[ams][strategy]") {
    SECTION("Backends with no discovery gate are ready on construction") {
        auto afc = make_afc_probe();
        REQUIRE(afc->remap_ready());
        auto sm = make_snapmaker_probe();
        REQUIRE(sm->remap_ready());
        auto ace = make_ace_probe();
        REQUIRE(ace->remap_ready()); // ready, but RemapStrategy::None: nothing to carry out
    }

    SECTION("AD5X IFS is not ready until _IFS_VARS is discovered") {
        auto ad5x = make_ad5x_probe();
        REQUIRE(ad5x->get_remap_strategy() == AmsBackend::RemapStrategy::Native);
        REQUIRE_FALSE(ad5x->remap_ready());
        REQUIRE_FALSE(helix::printer::can_remap(*ad5x));

        // The gate, and ONLY the gate, is what moves.
        Ad5xIfsTestAccess::set_has_ifs_vars(*ad5x, true);
        REQUIRE(ad5x->remap_ready());
        REQUIRE(helix::printer::can_remap(*ad5x));
        REQUIRE(ad5x->get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
}

// The whole point of the branch: one declaration, and generic code cannot reach
// a second one that disagrees with it.
TEST_CASE("can_remap agrees with the declared strategy and readiness",
          "[ams][strategy]") {
    struct Case {
        const char* name;
        AmsBackend::RemapStrategy strategy;
        bool ready;
        bool expected;
    };
    const Case cases[] = {
        {"Native and ready", AmsBackend::RemapStrategy::Native, true, true},
        {"Native, not ready", AmsBackend::RemapStrategy::Native, false, false},
        {"SnapmakerNative and ready", AmsBackend::RemapStrategy::SnapmakerNative, true, true},
        {"GcodeRewrite and ready", AmsBackend::RemapStrategy::GcodeRewrite, true, true},
        {"None but ready", AmsBackend::RemapStrategy::None, true, false},
        {"None and not ready", AmsBackend::RemapStrategy::None, false, false},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        StubBackend stub(c.strategy, c.ready);
        CHECK(helix::printer::can_remap(stub) == c.expected);
    }
}

TEST_CASE("remap_is_persistent separates the write-a-table routes from the pre-send",
          "[ams][strategy]") {
    using RS = AmsBackend::RemapStrategy;
    CHECK(helix::printer::remap_is_persistent(RS::Native));
    CHECK(helix::printer::remap_is_persistent(RS::GcodeRewrite));
    CHECK_FALSE(helix::printer::remap_is_persistent(RS::SnapmakerNative));
    CHECK_FALSE(helix::printer::remap_is_persistent(RS::None));
}

// The table gate is a different question from the remap gate, and the U1 is
// where they part company: it carries out every remap the user picks, through
// the pre-print send, and owns no tool->slot table for ToolState to adopt.
TEST_CASE("owns_tool_mapping_table is answered independently of remap capability",
          "[ams][strategy]") {
    auto sm = make_snapmaker_probe();
    CHECK(helix::printer::can_remap(*sm));
    CHECK_FALSE(sm->owns_tool_mapping_table());

    auto afc = make_afc_probe();
    CHECK(helix::printer::can_remap(*afc));
    CHECK(afc->owns_tool_mapping_table());

    auto ad5x = make_ad5x_probe();
    CHECK_FALSE(ad5x->owns_tool_mapping_table()); // gated on _IFS_VARS, like readiness
    Ad5xIfsTestAccess::set_has_ifs_vars(*ad5x, true);
    CHECK(ad5x->owns_tool_mapping_table());
}
```

`tests/unit/test_remap_strategy.cpp` already defines a nullptr-constructed probe per
backend (`AfcProbe`, `Ad5xIfsProbe`, `SnapmakerProbe`, ..., plus a `BaseProbe` stubbing
the pure virtuals). Use those, not new ones — the sketch above writes `make_*_probe()`
only as shorthand.

`has_ifs_vars_` is **private**, so a derived probe cannot set it. The seam already
exists: `Ad5xIfsTestAccess::set_has_ifs_vars(backend, bool)` in
`tests/test_helpers/ad5x_ifs_test_access.h`, used across
`tests/unit/test_ams_backend_ad5x_ifs.cpp`. Include that header and use it.

`StubBackend` is new: a `BaseProbe` subclass taking a strategy and a readiness flag in
its constructor and returning them from the two virtuals, so the `can_remap` table can
cover combinations no real backend declares (`GcodeRewrite`, `None`-but-not-ready).

While in this file, fix its header comment: it says `GcodeRewrite — ... (Snapmaker U1)`,
which has been wrong since the U1 moved to `SnapmakerNative`. No backend declares
`GcodeRewrite` today; ACE is the one it was written for and it declares `None`.

- [ ] **Step 2: Run it and watch it fail to compile**

Run: `make test -j6 2>&1 | tail -20`
Expected: `remap_ready`/`owns_tool_mapping_table`/`can_remap` are not members / not declared.

- [ ] **Step 3: Add the two virtuals to `AmsBackend`**

In `include/ams_backend.h`, beside `get_remap_strategy()`:

```cpp
    /**
     * @brief Is the declared remap strategy usable RIGHT NOW?
     *
     * The axis `get_remap_strategy()` cannot express: a backend can be BUILT to
     * remap and not be able to yet, because the firmware object it writes
     * through has not been discovered. THE one place readiness lives — a
     * second gate elsewhere is how the two answers drifted apart before.
     *
     * Default true: a backend with no discovery step is ready on construction.
     * Override only where a gate genuinely exists (AD5X IFS: `_IFS_VARS`).
     *
     * @note Backends holding a mutex over discovery state must take it here.
     */
    [[nodiscard]] virtual bool remap_ready() const {
        return true;
    }

    /**
     * @brief Does this backend own a tool->slot table worth building a
     *        ToolTopology from?
     *
     * Distinct from remap capability, and the two part company on real
     * hardware. The Snapmaker U1 carries out every remap the user picks, via
     * its pre-print send, and owns no such table: its extruders are
     * independent, so ToolState's extruder enumeration is the correct model
     * and an AMS topology would be a fiction. Answer this only about the
     * table.
     *
     * Default false. Override true where get_tool_mapping() returns a real
     * per-tool table.
     */
    [[nodiscard]] virtual bool owns_tool_mapping_table() const {
        return false;
    }
```

- [ ] **Step 4: Create `include/ams_remap.h`**

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend.h"

namespace helix {
namespace printer {

/**
 * @brief Does this strategy write a routing table that outlives the send?
 *
 * Native and GcodeRewrite both leave the mapping somewhere durable — the
 * machine's own table, or the job file. SnapmakerNative does not: the firmware
 * is told once, before PRINT_START, and nothing persists. Callers that gate the
 * generic set_tool_mapping() apply path want this; callers asking whether the
 * user's pick will be honored at all want can_remap(). Pure.
 */
[[nodiscard]] inline bool remap_is_persistent(AmsBackend::RemapStrategy s) {
    return s == AmsBackend::RemapStrategy::Native ||
           s == AmsBackend::RemapStrategy::GcodeRewrite;
}

/**
 * @brief Can this backend carry out an explicit user tool->lane choice, now?
 *
 * The one question generic code should ask. Both halves matter: a backend built
 * to remap but not yet ready answers false, which is what the retired
 * capability struct got right and the strategy enum alone got wrong.
 */
[[nodiscard]] inline bool can_remap(const AmsBackend& backend) {
    return backend.get_remap_strategy() != AmsBackend::RemapStrategy::None &&
           backend.remap_ready();
}

} // namespace printer
} // namespace helix
```

- [ ] **Step 5: Declare the per-backend answers**

Add only what differs from the defaults. `remap_ready()` is `true` by default, so only
AD5X IFS overrides it; `owns_tool_mapping_table()` is `false` by default, so only the
table-owning backends override it. Every value below must equal what
`get_tool_mapping_capabilities()` returns on that backend today — read each one and
match it, do not assume:

| Backend | `remap_ready()` | `owns_tool_mapping_table()` |
|---|---|---|
| AFC, Happy Hare, CFS, QIDI, ToolChanger | default | `true` |
| AD5X IFS | `has_ifs_vars_` (take `mutex_`) | `has_ifs_vars_` (take `mutex_`) |
| ACE | default | `false` (default — no override) |
| Snapmaker | default | `false` (default — no override) |
| Mock | default | mirrors its mode; see Task 4 |

AD5X IFS: both live in `src/printer/ams_backend_ad5x_ifs.cpp` beside the existing
`get_tool_mapping_capabilities()`, taking `mutex_` the same way it does. Declare them in
`include/ams_backend_ad5x_ifs.h`.

- [ ] **Step 6: Run the test to verify it passes**

Run: `make test -j6 && ./build/bin/helix-tests "[ams][strategy]"`
Expected: PASS, with no change to any existing case in that file.

- [ ] **Step 7: Prove the new tests can fail**

Flip `remap_ready()` on AD5X IFS to `return true;`, rebuild, and confirm the readiness
and `can_remap` cases go red. Restore. Record the observed failure in the report — a
green suite is not evidence.

- [ ] **Step 8: Commit**

```bash
git add include/ams_backend.h include/ams_remap.h include/ams_backend_*.h \
        src/printer/ams_backend_ad5x_ifs.cpp tests/unit/test_remap_strategy.cpp
git commit -m "feat(ams): declare remap readiness and table ownership per backend"
```

---

## Task 2: Move the topology gate onto `owns_tool_mapping_table()`

**Files:**
- Modify: `src/printer/ams_state.cpp:68-75` (`build_ams_topology`)
- Test: `tests/unit/test_ams_tool_topology.cpp` (or the existing file covering
  `build_ams_topology` — find it before creating one)

**Interfaces:**
- Consumes: `AmsBackend::owns_tool_mapping_table()` from Task 1.

This is the correction the spec got wrong, so it is its own task with its own gate.

- [ ] **Step 1: Find the existing coverage**

Run: `grep -rn "set_ams_topology\|ams_topology_active\|build_ams_topology" tests/`
Read what is already asserted before writing anything. If a test already pins which
backends produce a topology, extend it; if none does, write the one below.

- [ ] **Step 2: Write the failing test**

```cpp
// The gate that decides whether ToolState adopts an AMS tool->slot table or
// keeps enumerating extruders. It is NOT the remap gate: the U1 remaps through
// its pre-print send and owns no table, so a topology built from can_remap()
// would hand ToolState a fiction for a machine with four real extruders.
TEST_CASE("Only table-owning backends push a ToolTopology to ToolState",
          "[ams][topology]") {
    SECTION("A lane-multiplexing backend pushes one") { /* AFC probe -> topology present */ }
    SECTION("Snapmaker pushes none despite being able to remap") {
        // can_remap() is true here and owns_tool_mapping_table() is false.
        // Asserting both together is the point: they must not be the same gate.
    }
    SECTION("AD5X IFS pushes none until _IFS_VARS is discovered") { /* ... */ }
}
```

Fill each section using the fixture the file found in Step 1 already uses.

- [ ] **Step 3: Run it and watch it fail**

Expected: the Snapmaker section fails only if the production change has already been made
wrongly; against today's code it should PASS (today's `.supported` is false for the U1).
That is the point — this test pins behaviour that must **not** change. Say so in the
report: this is a characterization test, and a green first run is the correct result.
It fails only if someone later routes this gate through `can_remap()`.

- [ ] **Step 4: Make the production change**

```cpp
    if (!backend->owns_tool_mapping_table())
        return std::nullopt;
```
replacing the `get_tool_mapping_capabilities()` call and its `.supported` read. Update the
function comment above it, which still describes the old query.

- [ ] **Step 5: Run the full suite**

Run: `make test -j6 && make test-run`
Expected: green, with no change in which backends produce a topology.

- [ ] **Step 6: Commit**

```bash
git add src/printer/ams_state.cpp tests/unit/<the test file>
git commit -m "refactor(ams): gate the tool topology on table ownership, not remap capability"
```

---

## Task 3: Migrate every remap-capability read

**Files:**
- Modify: `src/printer/ams_state.cpp:1030,1040`,
  `src/ui/ui_print_start_controller.cpp:656,722`,
  `include/ui_print_start_controller.h:281-295`,
  `src/ui/modals/ui_preflight_check_modal.cpp:77`,
  `src/ui/ui_print_select_detail_view.cpp:1486`,
  `tests/test_helpers/print_start_controller_test_access.h:50`
- Test: `tests/unit/test_effective_auto_match.cpp`,
  `tests/unit/test_print_start_filament_gate.cpp`

**Interfaces:**
- Consumes: `can_remap()`, `remap_is_persistent()` from Task 1.
- Produces: `should_warn_remap_unsupported(const AmsBackend&)` — signature changes from
  `(caps, bool)` to one backend reference. `print_start_controller_test_access.h` and its
  callers move with it.

Each site below reads a specific thing; the mapping is not uniform. Do them one at a
time and state, for each, whether behaviour changes and on which backend.

| Site | Reads today | Becomes | Behaviour |
|---|---|---|---|
| `ams_state.cpp:1030` | `backend->honors_user_tool_mapping()` | `helix::printer::can_remap(*backend)` | identical on every real backend (see the equivalence table) |
| `ams_state.cpp:1040` | `.editable` | `remap_is_persistent(b->get_remap_strategy()) && b->remap_ready()` | identical. Inside the HELD `kPreprintSeedFollowsUserSetting` branch — **preserve that branch exactly**, do not simplify it away |
| `ui_print_start_controller.cpp:656` | free `honors_user_tool_mapping(caps, preprint)` | `!can_remap(b)` | identical; signature narrows to one argument |
| `ui_print_start_controller.cpp:722` | `.editable` | `remap_is_persistent(...) && remap_ready()` | identical |
| `ui_preflight_check_modal.cpp:77` | `get_remap_strategy() != None` | `can_remap(*backend)` | **CHANGES: gains readiness.** On an AD5X before `_IFS_VARS`, the pre-flight "Remap…" affordance now correctly reports unavailable instead of offering a write that cannot land |
| `ui_print_select_detail_view.cpp:1486` | `get_remap_strategy() != None` | `can_remap(*backend)` | **CHANGES: gains readiness**, same case, same reason |
| `ui_panel_print_select.cpp:2827,2890` | `get_remap_strategy()` | unchanged | the `switch` on strategy is the correct shape and stays |

- [ ] **Step 1: Add the AD5X readiness case to the pre-flight tests**

Before touching production, pin the behaviour change. Find the test covering
`ui_preflight_check_modal`'s remap affordance and add a case: an AD5X backend without
`_IFS_VARS` must not offer the remap. Watch it fail against today's code — that failure
IS the bug being fixed, and a report that does not show it has not proven anything.

- [ ] **Step 2: Migrate the two `ams_state.cpp` sites, run `[ams]`**
- [ ] **Step 3: Migrate `should_warn_remap_unsupported` and its test access, run the gate tests**

The signature change reaches `include/ui_print_start_controller.h:281-295` (whose doc
comment names `requires_preprint_send` as a parameter that no longer exists) and
`tests/test_helpers/print_start_controller_test_access.h:50`.
`tests/unit/test_print_start_filament_gate.cpp:39-41` builds `ToolMappingCapabilities`
by **positional** brace-init — it is rewritten to drive real backends instead.

- [ ] **Step 4: Migrate the two `!= None` sites, run the pre-flight tests**

Expected: the case from Step 1 now passes.

- [ ] **Step 5: Full suite**

Run: `make test-run`

- [ ] **Step 6: Commit**

```bash
git commit -m "refactor(ams): ask one question about remap capability at every call site"
```

---

## Task 4: Make the mock declare what it emulates

**Files:**
- Modify: `src/printer/ams_backend_mock.cpp:3351-3383`, `include/ams_backend_mock.h`
- Test: `tests/unit/test_ams_tool_mapping.cpp:186`, `tests/unit/test_remap_strategy.cpp:305`,
  `tests/unit/test_print_select_detail_subjects.cpp:763`

The mock currently returns `RemapStrategy::None` for **every** mode except Snapmaker,
while its capability query says `{true, true, "Mock tool-to-slot mapping"}` for the
filament-system mode. Two consequences, and the second is why this matters beyond tidiness:

1. It is the one backend where `can_remap()` disagrees with `honors_user_tool_mapping()`.
2. Every `get_remap_strategy() != None` gate is therefore **dead under `--test`** unless
   `HELIX_MOCK_AMS=snapmaker`. The remap picker reached from the pre-flight modal and
   from the detail-view chip tap cannot be exercised in the mock for AFC, CFS, Happy
   Hare, QIDI or the tool changer — five backends' worth of UI that no local run touches.
   This is the second time the mock has taught the opposite of the backend it stands in
   for; the first was the tool-changer capability contradiction below.

- [ ] **Step 1: Fix `get_remap_strategy()` to mirror the emulated backend**

```cpp
AmsBackendMock::RemapStrategy AmsBackendMock::get_remap_strategy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapmaker_mode_) {
        return RemapStrategy::SnapmakerNative;
    }
    // Every other mode stands in for a backend that owns its own tool->slot
    // table and writes it directly: AFC, CFS, Happy Hare, QIDI, and the tool
    // changer all declare Native. Returning None here made every
    // "can this backend remap" gate dead under --test, so the remap picker
    // those five reach was unreachable in the mock.
    return RemapStrategy::Native;
}
```

`tool_changer_mode_` gets `Native` too: the real `AmsBackendToolChanger` declares
`Native` and `{true, true, "Tool reassignment via ASSIGN_TOOL"}`, while the mock declared
`None` and `{false, false, ""}` — the opposite of the thing it emulates, on both
spellings.

- [ ] **Step 2: Declare `owns_tool_mapping_table()` on the mock**

`true` in filament-system and tool-changer modes (both stand in for table owners),
`false` in Snapmaker mode (matching the real U1, which owns none).

- [ ] **Step 3: Update the tests that pinned the contradiction**

`test_ams_tool_mapping.cpp:186` ("Mock backend tool mapping - tool changer mode") and
`test_remap_strategy.cpp:305` assert the old shape. Rewrite them to assert the new one
**and say in a comment why the mock matches the real backend** — a bare expectation flip
with no reason is how this comes back. `test_print_select_detail_subjects.cpp:763`
carries a comment ("The plain mock reports RemapStrategy::None") that becomes false;
that test may now take a different branch, so read what it is actually covering before
adjusting it.

- [ ] **Step 4: Verify the picker is now reachable in the mock**

This is the payoff, so prove it rather than asserting it:

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
HELIX_MOCK_AMS=afc ./build/bin/helix-screen --test -vv \
  --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
```
Then drive to a print file's detail view with `ctl navigate` / `ctl click`, tap a
filament chip, and confirm the remap picker opens — `ctl ls` for the picker's widget
names, not a screenshot. Kill the instance when done. Record the command sequence and
what came back in the report.

- [ ] **Step 5: Full suite, then commit**

```bash
git commit -m "fix(mock): declare the remap strategy of the backend being emulated"
```

---

## Task 5: Delete the retired API

**Files:**
- Modify: `include/ams_types.h:1880-1928`, `include/ams_backend.h:1800-1835`
- Modify: all nine backends — remove `get_tool_mapping_capabilities()` and its declaration
- Test: `tests/unit/test_ams_tool_mapping.cpp`, `tests/unit/test_effective_auto_match.cpp`,
  `tests/unit/test_print_start_filament_gate.cpp`,
  `tests/unit/test_ams_backend_afc_capabilities.cpp:49`, `tests/unit/test_ams_backend_cfs.cpp:1794`

Nothing should still reference these by the time this task runs. If anything does, that
is a missed site from Tasks 2-4 — go fix it there rather than adapting it here.

- [ ] **Step 1: Confirm the call sites are gone**

```bash
grep -rn "ToolMappingCapabilities\|get_tool_mapping_capabilities\|honors_user_tool_mapping" \
     src/ include/ tests/ docs/
```
Everything left must be a declaration or a test asserting on the retired API.

**Trap — `.supported` collides:** `DryerInfo::supported` is unrelated and heavily used
(`test_ams_backend_{ace,happy_hare,qidi}.cpp`, `ams_state.cpp:2029,2052,2395`). Never
sweep `.supported` by text; go by receiver type.

**Trap — `EndlessSpoolCapabilities::editable()` is a different axis and a METHOD**
(`ams_types.h:1715`), live in `src/ui/ui_ams_context_menu.cpp`. Do not touch it.

- [ ] **Step 2: Delete the struct, the free function, the virtual and the member wrapper**
- [ ] **Step 3: Delete each backend's override**
- [ ] **Step 4: Rewrite the tests that covered the retired API**

`test_ams_tool_mapping.cpp` opens with type tests over the struct's fields
(`:23-58`) — those cases die with the struct. The behavioural cases below them move onto
the new API. Do not keep a case alive by rewriting it into an assertion about a field
that no longer exists.

- [ ] **Step 5: Full suite + `make test-hidden`**

`test-hidden` is the only suite that catches exit/static-destruction crashes; check the
exit code, not just the printed result.

- [ ] **Step 6: Commit**

```bash
git commit -m "refactor(ams): retire ToolMappingCapabilities and both honors_user_tool_mapping spellings"
```

---

## Task 6: Docs

**Files:**
- Modify: `docs/devel/FILAMENT_MANAGEMENT.md:2233`,
  `docs/devel/FILAMENT_BACKEND_SNAPMAKER_U1.md:213-223`,
  `docs/devel/FILAMENT_BACKEND_TOOLCHANGER.md:87`,
  `docs/devel/FILAMENT_BACKEND_QIDI_BOX.md:63,100`

- [ ] **Step 1: Find every doc naming the retired API**

```bash
grep -rn "ToolMappingCapabilities\|honors_user_tool_mapping\|get_tool_mapping_capabilities\|supports_tool_mapping" docs/
```

- [ ] **Step 2: Rewrite the prose**

These need real edits, not re-pinning: they describe a capability model that no longer
exists. Say what the three questions are and which one each backend answers.

- [ ] **Step 3: Add the model to the architecture guide**

`docs/devel/architecture/` should carry the three-question table, since "which question do
I ask" is the thing a contributor gets wrong. Route it from `docs/devel/CLAUDE.md` if it
lands as a new section.

- [ ] **Step 4: Re-pin citations**

Run: `make regen-doc-links`
A cited line whose **own text changed** is a hard error and must be fixed by hand — the
sentence citing it may no longer be true.

- [ ] **Step 5: Commit**

```bash
git commit -m "docs(ams): describe the three capability questions, not the retired struct"
```

---

## Not in scope

- Renaming `get_remap_strategy()` to `remap_strategy()`. The spec proposed it; it is
  cosmetic, touches ~15 sites, and buys nothing. The name stays.
- `EndlessSpoolCapabilities`. Different axis, untouched.
- The `HELIX_HAS_*` per-platform backend gating on `feature/ams-backend-platform-gates`
  (unmerged, based on devel/1.1). It compiles backends in or out; if it lands first, the
  per-backend pin tests in Task 1 will need the same `#if` guards its own tests use.
