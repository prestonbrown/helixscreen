# HelixScreen XML Engine: Parser Enhancements (Post-`<repeat>`)

**Date:** 2026-07-16
**Status:** Design proposal — for a clean session to brainstorm-refine → plan → execute. NOT yet approved.
**Scope:** MAJOR (new parser capabilities in `lib/helix-xml/` core + `src/`). Three **independent** sub-projects — each ships on its own; do NOT bundle into one plan.

## Context

The helix-xml engine (LVGL 9.5 XML → subjects → C++) now has, from two prior efforts:
- **Expressions (Plan 1):** an integer expression evaluator over subjects (`lv_xml_expr.{c,h}`: `lv_xml_expr_compile(src, resolver, ctx)` → `lv_xml_expr_eval` → int; operators `== != < <= > >= && || ! + - * / %` plus word forms `eq ne lt le gt ge and or not`; word forms are house style). Surfaces: `<subject_expr name="x" expr="…"/>` (derived subjects) and inline `cond="…"` on `bind_flag_if`/`bind_state_if`/`bind_style_if`.
- **Looping (Plan 2):** `<repeat count="N">…$i…</repeat>` — literal / `#const` / **subject-bound** count with reactive rebuild; `$i` iteration index; `${name}` embedded composition (`${i}`, `${prop}`) for self-wiring indexed subjects.

This spec proposes three enhancements that build on that foundation. They are ordered by value-per-effort, but are independent.

## Engine facts a plan must respect (verified)

These constrain all three features and were load-bearing in the prior efforts:

- **Pure C, no app layer.** `lib/helix-xml/` is C and MUST NOT include or call app-layer C++ (`include/ui_utils.h`, `helix::ui::*`). Async widget teardown uses LVGL C primitives only: reparent to an off-tree `LV_LAYOUT_NONE` condemned container on `lv_layer_top()`, then `lv_obj_delete_async()`. (`helix::ui::safe_delete_subtree` is unreachable; the `<repeat>` code reimplements it in C — reuse that.) `lib/helix-xml/` is our own submodule (edit in place, push from inside it) and excluded from clang-format — match surrounding style by hand.
- **SAX/expat streaming, no DOM.** A component `<view>` is retained as a raw XML string (`scope->view_def`) and re-parsed per instantiation. Element handlers are `view_start_element_handler` / `view_end_element_handler` / `view_character_data_handler` in `lv_xml.c`. Depth is implicit in `state->parent_ll` length (no integer counter). There is NO lookahead — any construct needing to know about a *later* sibling (e.g. `<else>`) must buffer or track state across events. The `<repeat>` capture/replay engine (buffer body as deep-copied SAX events during a suppressed pass, replay N times through the real handlers) is the proven pattern for "expand a body a variable number of times."
- **`resolve_params` mutates the `attrs` array in place, destructively, non-allocating** (except the `${…}` compose path, which is the only allocating branch — composed strings tracked on a parser-state transient list, freed at parse end in `lv_xml_create_in_scope`). Order in the resolve path: `${…}` compose branch first, then whole-value `$`/`$i`, then `#const` in `resolve_consts`. No path may both compose and repoint the same slot.
- **Reactive rebuild = crash class.** Count observers fire synchronously inside a UpdateQueue drain batch → no synchronous widget deletion (L081/L059), no `lv_obj_is_valid()` (L076). Observer/record lifetime MUST be tied to the **instance** (via an `LV_EVENT_DELETE` cb on the view root, mirroring `free_timelines_event_cb`), NOT the component scope — a scope-lifetime observer outliving its instance is a UAF (the exact bug the `<repeat>` whole-branch review caught). Detach-before-free ordering in `lv_xml_component_unregister`: instance-owned lists before the subjects they observe.
- **LVGL append semantics.** Widgets are appended on create/reparent. A reactively-rebuilt body that has *later* same-parent siblings mis-orders on rebuild — so reactive constructs must be the parent's last child or in their own container (documented for `<repeat>`). Same constraint will apply to `<if>` and `<for_each>`.
- **Tests use `LVGLTestFixture` + `lv_xml_register_component_from_data`** (reactive XML tests), auto-globbed from `tests/unit/*.cpp`. Teardown order: `lv_obj_delete(instance)` before `lv_xml_component_unregister`. Reactive/async paths need `process_lvgl(...)` before asserts (L048); thread tests tagged `[slow]` (L052). ASAN via `reference_asan_native_build.md` (LD_PRELOAD workaround; `make clean-tests` after). Schema regen + commit on any new tag/attr (L089); new special-element tags must be added to BOTH `schema["widgets"]` AND `special_elements` in `extract_schema.py` (a `<repeat>`-era gotcha — the linter's `is_valid_widget()` only checks `special_elements`/`registered_widgets`).

---

## Feature 1 — `${expr}`: expression-valued composition & numeric attributes

**Cheapest; closes a documented follow-up.**

### Motivation
`${i}` splices the loop index as-is, but `${i + 1}` is explicitly deferred and there is no way to compute a value inline. Two concrete needs:
- **Index arithmetic in composition:** `bind_text="slot_${i + 1}_label"` (1-based names), `bind_text="row_${i * cols + j}"`.
- **Computed numeric attributes:** `style_translate_x="${$i * 84}"` for staggered/offset layouts, `width="${base * scale}"` — instead of pre-registering a subject per computed value.

### Design sketch
Unify composition with the evaluator: `${…}` evaluates its contents as an **integer expression** via `lv_xml_expr_compile`/`_eval`, and splices the integer result as text. `${i}` becomes "evaluate `i`" (the loop index), `${i + 1}` "evaluate `i+1`". This requires making `$i` (and `$prop` params) resolvable *inside the evaluator* during expansion — extend the compose-path resolver so identifier `i` resolves to the current iteration index and param names resolve to their values, falling through to subjects.

### Key open questions (resolve in brainstorming)
1. **Name-compose vs numeric-eval disambiguation.** `slot_${i}_label` splices `2` into a subject *name*; `${i*84}` produces a *number* for a numeric attr. The rule is likely "`${…}` always evaluates to an integer, then splices as text" (uniform), but confirm no case wants literal-name composition of a non-index value.
2. **Reactivity.** Composed subject *names* are resolved once (you can't rebind to a different subject reactively without a rebuild — acceptable, matches `<repeat>`). But a computed *numeric attribute* referencing a subject (`width="${scale_subject * 10}"`) would need an observer to stay live. Decide: (a) numeric-attr expressions are resolve-once (simplest, covers index math), or (b) reactive numeric attrs are a separate, harder sub-feature (observer per computed attr). Recommend (a) first; flag (b) as a follow-up.
3. **Non-int results / errors.** Div-by-zero, unknown identifier — reuse the evaluator's existing degrade-to-0-and-warn.

### Non-goals
- Float expressions (engine is integer-only).
- Reactive computed numeric attributes (defer unless brainstorming decides otherwise).

### Testing / docs / tooling
- Unit (`LVGLTestFixture`): `${i + 1}` composes correctly per iteration; `${i * k}` numeric attr yields the computed pixel value; malformed expr degrades + warns; equivalence of `${i}` (old) vs `${i + 0}`.
- Docs: upgrade the `${name}` sigil rows in `LVGL9_XML_GUIDE.md` + `LVGL9_XML_ATTRIBUTES_REFERENCE.md` + the helix-xml skill from "index only" to "integer expression"; remove the "arithmetic is a follow-up" caveats.
- Tooling: no new tag; crossref already skips `${…}`. Verify the linter doesn't choke on expression syntax inside `${…}`.

### Effort: low–moderate. The evaluator exists; the work is wiring it into the compose path + the numeric-attr apply point, and the resolver extension for `$i`/params.

---

## Feature 2 — `<if cond>` / `<else>`: structural conditionals

**Highest genuine UX/perf win; moderate effort; strong synergy with `<repeat>`.**

### Motivation
Conditional UI today is *created-then-hidden* (`bind_flag hidden` or `cond=`), so a printer-type-specific panel builds *every* variant even when only one is shown — wasted widgets + startup cost on small devices (AD5M/CC1). A structural conditional creates the body only when the condition holds and tears it down reactively when it flips:

```xml
<if cond="printer_has_chamber and chamber_supported">
  <chamber_control_card/>
</if>
<else>
  <divider_horizontal/>
</else>
```

### Design sketch
`<if>` is effectively `<repeat count="cond ? 1 : 0">` with reactive rebuild — it **reuses the `<repeat>` machinery almost wholesale**: capture the body as SAX events, evaluate `cond` via the expression evaluator, expand 0-or-1 times, register an observer on the condition's subjects, and on change async-tear-down + re-expand. `<else>` captures a second body expanded when the condition is falsy. Instance-lifetime observer (view-root `LV_EVENT_DELETE`) and async off-tree teardown are already solved by `<repeat>` — copy the pattern exactly.

### Key open questions
1. **`<else>` pairing in a streaming parser (no lookahead).** `<else>` must associate with the immediately-preceding `<if>` at the same depth. Options: track "last `<if>` record at this depth" in parse state; or require `<else>` be a direct sibling and error clearly otherwise. Define the binding rule + the error for a dangling `<else>`.
2. **Reuse vs fork of `<repeat>` internals.** Ideally factor the `<repeat>` capture/expand/rebuild/teardown into a shared internal core that both `<repeat count>` and `<if cond>` call, rather than copy-paste. Assess whether the `<repeat>` code is already close to that shape (it was written single-purpose; a small refactor may be warranted and is in the spirit of the separation-of-concerns goal).
3. **`cond` semantics** = the existing evaluator (word-form house style). `<if>` with a *static* (non-subject) cond expands once at load, no observer.

### Non-goals
- Replacing `bind_flag`/`cond=` for cheap show/hide of *light* subtrees — those stay (no teardown cost). `<if>` is for *expensive/structural* conditional creation.
- `<elif>` / multi-way — a `<switch>`/`<case>` cousin could be a later feature; keep this to if/else.

### Testing / docs / tooling
- Unit: static cond true/false (body present/absent); subject cond flips true→false→true (reactive create/teardown, `process_lvgl` between); `<else>` body appears iff cond falsy; **no-UAF-on-instance-delete** + **re-instantiation** (the `<repeat>` crash-class tests, adapted) under ASAN; the last-child/own-container ordering constraint.
- Docs: new `<if>`/`<else>` section in guide + attr-ref + skill; the ordering ⚠️ callout; when to prefer `<if>` vs `bind_flag`.
- Tooling: register `<if>`/`<else>` as special elements in `extract_schema.py` (BOTH `widgets` and `special_elements`), regen schema; `cond` attr allowed on them.

### Effort: moderate. Most machinery exists; the new work is `<else>` pairing, the shared-core refactor, and the crash-class test adaptation.

---

## Feature 3 — `<for_each>` over a collection subject: data-driven lists

**Ambitious capstone. High effort; touches the subject core — the original design's stated non-goal. Likely its own spec + plan, possibly phased.**

### Motivation
LVGL subjects are scalar (int/float/string/pointer), so genuinely dynamic lists (AMS slots, file lists, print history) still require C++ to register N indexed subjects. `<repeat>` + `${i}` *approximates* this but the data plumbing stays in C++. A collection subject + `<for_each>` would let data drive the list:

```xml
<for_each item="slot" in="ams_slots">
  <slot_card bind_text="slot.material" bind_flag_if="slot.loaded" ref_value="0"/>
</for_each>
```

### Design sketch (multiple viable; brainstorming must choose)
- **Collection representation.** Options: (a) a new LVGL subject kind wrapping a JSON array (leverages libhv's `hv/json.hpp`); (b) an *adapter* subject exposing `count` + an indexed field-accessor over existing C++ state (no subject-core change — closer to the current `${i}` model but with per-field access); (c) a "list subject" holding a `std::vector` of small structs. Option (b) is the least invasive and should be evaluated first — it may deliver most of the value without a new subject kind.
- **Per-item scoping.** `item.field` must resolve to the right element's field during expansion — extend the resolver so `slot.material` maps to element *k*'s `material`. Reuses the `<repeat>` per-iteration index mechanism, generalized from an index to a named item cursor.
- **Reactivity.** Add/remove/reorder of elements. Simplest correct: full async rebuild on any collection change (reuse `<repeat>` subject-bound rebuild). A *diffing* renderer (only touch changed rows) is a large follow-up — do NOT attempt in v1.

### Key open questions
1. Which collection representation (a/b/c) — evaluate (b) adapter-over-C++-state first for lowest risk.
2. Does this need a genuine subject-core change, or can an adapter live entirely in `lib/helix-xml` + `src/`? (Prefer the latter.)
3. Per-item callbacks and measured/pixel layout **stay in C++** (honest residue — same as the `<repeat>` AMS example). Confirm the division so the feature doesn't over-promise "no C++."

### Non-goals
- Diffing/keyed reconciliation in v1 (full rebuild is acceptable).
- Nested `<for_each>` in v1 (align with the deferred nested-`<repeat>`).

### Testing / docs / tooling
- Unit: static collection renders N rows with correct per-item fields; collection change adds/removes rows reactively (rebuild, ASAN-clean, instance-lifetime); empty collection; the crash-class + ordering constraints.
- Docs: new `<for_each>` section; the honest-residue note (data population / callbacks / measured layout stay C++); update `FILAMENT_MANAGEMENT.md` if AMS adopts it.
- Tooling: register `<for_each>` (widgets + special_elements), schema regen; teach crossref about `item.field` references so they don't false-warn `UNKNOWN_SUBJECT_REF`.

### Effort: high. Real design work on the collection representation; possibly phase it (adapter MVP → richer). Treat as its own spec.

---

## Recommended sequencing

Independent sub-projects; each is its own plan. Suggested order:

1. **`${expr}`** — cheapest, closes a known gap, no new tag, low risk. Good warm-up in a clean session.
2. **`<if>`/`<else>`** — best value/effort; reuses `<repeat>`'s solved crash-class machinery; a small shared-core refactor pays off for future reactive constructs.
3. **`<for_each>`** — its own brainstorm + spec + (likely phased) plan; the ambitious one; only after the above prove the reactive-rebuild patterns further.

Each plan must, per the prior efforts: end every task with an independently testable deliverable; TDD the crash-class paths with ASAN repros; ship docs (guide + attr-ref + **helix-xml skill**) and tooling (schema regen + special_elements) *with* the code, not after; and route all reactive teardown through the pure-C async off-tree pattern with instance-lifetime observers.

## Cross-cutting risks (all three)

- **Reactive teardown UAF** (the `<repeat>` bug) — instance-lifetime observers, not scope-lifetime. Non-negotiable; test it.
- **`resolve_params` ordering** — the `${expr}` change touches the only allocating branch; preserve the compose-first ordering and the transient-string free-at-parse-end discipline; no double-compose/leak.
- **SAX no-lookahead** — `<else>` and `<for_each>` item scoping have no DOM to consult; use parse-state tracking, and error clearly on malformed shapes rather than silently mis-parenting.
- **Schema/linter drift** — every new tag needs BOTH schema dicts + a regen; crossref needs teaching for any new reference syntax (`item.field`).

## Prior art to read first

- `lib/helix-xml/src/xml/lv_xml.c` — `xml_repeat_*` functions (capture/replay/expand/rebuild/teardown, instance-lifetime `LV_EVENT_DELETE`), `resolve_params`/`resolve_consts`, `xml_compose_indexed`, `xml_state_free_composed`.
- `lib/helix-xml/src/xml/lv_xml_expr.{c,h}` — the evaluator the `${expr}` and `<if cond>` features reuse.
- CLAUDE.md § "CRITICAL RULES - Declarative UI" (rows 9-10) and § Threading for the crash-class rules.
