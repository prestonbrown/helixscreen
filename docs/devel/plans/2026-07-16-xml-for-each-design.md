# `<for_each>`: Iterate a Collection into Repeated Fragments

**Date:** 2026-07-16
**Status:** DRAFT — motivation + approaches captured; the core "collection model" decision is OPEN and MUST be confirmed before an implementation plan. **Do not implement from this doc as-is.**
**Scope:** MAJOR, and the highest-effort of the three parser enhancements — depending on the chosen model it touches the LVGL subject core. Feature 3 of `docs/devel/plans/2026-07-16-xml-parser-enhancements-design.md`. Feature 1 (`${expr}`) and Feature 2 (`<if>/<else>`) are SHIPPED.

## The question this feature must answer first

**What does `<for_each>` give you that shipped `<repeat count="subject">` + `${i}` does not?**

We already ship, as of Features 1–2:

```xml
<repeat count="fan_count">
  <fan_widget index="${i}"/>
</repeat>
```

`<repeat count="subject">` reactively rebuilds N fragments when a count subject changes; `${i}` splices the loop index into any attribute; the C++ side owns per-item data and computed layout (CLAUDE.md rule 10's carve-out). So the *count-driven, index-addressed* case is **already covered**. Before building `<for_each>`, we have to be honest that it is only worth it if it removes a real, recurring pain that `<repeat count>+${i}` leaves behind.

The pain it leaves behind: **binding per-item field data.** With `<repeat>`, to show `fan[i].name` a fragment must bind a subject that is addressable by index — which means the C++ must pre-register index-named subjects (`fan_0_name`, `fan_1_name`, …) or the fragment must call an index-taking accessor that only C++ can wire. There is no way, in XML today, to say "for each item in this collection, bind this fragment to that item's fields." `<for_each>` exists to close exactly that gap — iterate a **collection** and expose **each item's fields** to the fragment's bindings without index-named-subject boilerplate.

If, when we pick this up, the honest answer is "our real collections (fans, sensors, AMS slots, tools) are already served well enough by `<repeat count>` + the existing dynamic per-item subject accessors (`get_fan_speed_subject(name, lifetime)` etc.)," then `<for_each>` is **YAGNI** and we should not build it. That determination is part of the OPEN decision below — this spec assumes the gap is real enough to warrant the feature, but the first plan step is to validate that against 2–3 concrete call sites.

## Motivating use cases (validate these before building)

- AMS slots — a variable-length list of slots, each with material/color/name/state fields (`AmsState`).
- Fans / temperature sensors / extruders — dynamic collections with per-item subjects that are already destroyed/recreated on reconnect (the "dynamic subject lifetime" hazard in CLAUDE.md).
- Tools on a toolchanger (`ToolState`).

Each is today a C++ create-and-wire loop or a `<repeat count>` with C++-wired per-index subjects. The test for `<for_each>`'s worth: does it let the XML own the per-item binding for at least one of these with LESS C++ than the `<repeat count>` version, without sacrificing the dynamic-subject lifetime safety those accessors provide?

## The core decision (OPEN — pick one before planning)

LVGL subjects are scalar (`int` / `string` / `pointer` / `color` / `group`). There is no list subject. So "iterate a collection and bind each item's fields" requires deciding **where the collection lives and how a fragment instance reaches item[i]'s fields.** Three models, cheapest to heaviest:

### (A) Index-named-subject convention — thinnest, mostly already possible
`<for_each count="fan_count" as="fan">` is sugar over `<repeat count="fan_count">` that, inside the body, rewrites `fan.speed` → the subject named `fan_${i}_speed`. No new C++ concept; the C++ still registers `fan_0_speed`, `fan_1_speed`, … as ordinary subjects.
- **Pros:** trivial to build on the shipped `xml_frag_*` + `${expr}` machinery; no subject-core work; fully reactive via existing per-subject observers.
- **Cons:** the C++ still owns all the index-named-subject registration boilerplate — so it barely beats `<repeat count>+${i}`. Mostly syntactic sugar. Likely **not worth a MAJOR effort** on its own.

### (B) C++ collection-adapter interface queried by the parser — RECOMMENDED
Define a narrow C++ interface, e.g. `IXmlCollection { size_t count(); lv_subject_t* field(size_t index, const char* field_name, SubjectLifetime&); }`, and a registry `lv_xml_register_collection(scope, "fans", IXmlCollection*)`. `<for_each items="fans" as="fan">` resolves `count()` (reactively — the adapter also exposes a count subject to trigger rebuild), and inside the body `fan.speed` resolves through `field(i, "speed", lifetime)`. Existing state objects (`AmsState`, `PrinterFanState`) implement `IXmlCollection` by delegating to their EXISTING dynamic per-item subject accessors — so the dynamic-subject lifetime safety (the `SubjectLifetime` token discipline) is preserved and centralized, not reinvented.
- **Pros:** XML owns per-item binding with no index-named-subject boilerplate; reuses `xml_frag_*` for the fragment machinery and the existing dynamic-subject accessors for data + lifetime; the "collection" is an adapter over C++ state (the parent spec's "option b"), NOT a new subject kind — so the subject core is untouched. This is the sweet spot.
- **Cons:** new interface + registry + parser resolution path; the reactive-rebuild + per-item-observer lifetime interplay is the crash-class surface (must compose with `frag_ll` teardown and the dynamic-subject `SubjectLifetime` rules — see Risks).
- **Effort:** moderate. Reuses Feature 2's `xml_frag_*` record/teardown wholesale; adds a resolver layer for `item.field`.

### (C) Native collection/list subject in the subject core — heaviest
Add a genuine list/collection subject type to LVGL's subject core, with element subjects nested under it. `<for_each>` iterates the list subject directly.
- **Pros:** most general; collections become first-class reactive citizens.
- **Cons:** modifies vendored LVGL subject internals (a patch we'd carry forever, against the pinned 9.5.0), largest surface, largest crash risk. The parent spec explicitly listed "touches subject core" as an original non-goal. **Not recommended** unless (B) proves insufficient for a concrete case.

**Recommendation: (B).** Validate the gap against 2–3 real call sites first; if the gap is real, build (B) reusing the `xml_frag_*` core; keep (A) as the fallback if the adapter interface proves heavier than the pain it removes, and (C) firmly out of scope.

## Design sketch (assuming (B) — to be firmed up at planning time)

### Syntax
```xml
<for_each items="fans" as="fan">
  <fan_card
     bind_text="fan.name"
     bind_value="fan.speed"/>
</for_each>
```
- `items` — the registered collection name (resolved in scope, like a subject).
- `as` — the per-item binding prefix used inside the body (`fan.field`).
- Body is captured and replayed once per item, exactly as `<repeat>`/`<if>` bodies are — via the shared `xml_frag_*` capture/replay/teardown.

### Reuse of the shipped `xml_frag_*` core
- Capture the `<for_each>` body into an `xml_frag_capture_t` (add `is_for_each`, `items_raw`, `as_raw` fields alongside `<if>`'s `is_if`/`cond_raw`).
- On close, resolve `count()` from the adapter; expand the body `count` times through `xml_frag_expand` (the existing range replay — full range per item).
- Reactive rebuild: observe the adapter's count subject via the same instance-lifetime `frag_ll` record + view-root `LV_EVENT_DELETE` machinery `<repeat>` uses (this is the count-subject path, not the multi-subject `lv_xml_expr_bind` path).
- Per-item field binding: during each iteration `i`, a resolver maps `as.field` → `adapter->field(i, "field", lifetime)`. The **per-item observer lifetime** is the hard part (see Risks) — each item's field subjects are dynamic and must be tracked with `SubjectLifetime` tokens that live as long as that item's fragment instance, torn down in lockstep on rebuild.

### What stays in C++
Same carve-out as `<repeat>`: measured layout, computed callbacks, and the collection's data/population stay in C++. `<for_each>` replaces only the create-and-wire loop AND the per-item field-binding boilerplate.

## Testing (crash-class, mirrors the `<repeat>`/`<if>` suites)
- Static count → N fragments, each bound to the right item's fields.
- Reactive: count grows/shrinks → fragments added/removed; per-item bindings track the right item after a shrink (no off-by-one, no stale binding to a removed item).
- **Per-item observer lifetime under ASAN:** item removed → its field observers detached before its subjects are freed (the dynamic-subject `SubjectLifetime` hazard — this is THE crash-class test).
- Unregister-while-instance-alive (the ESP32 reclaim path, as with `<if>`).
- Empty collection → nothing created, component loads.
- Nested `<for_each>` — deferred (same buffered-depth limitation as nested `<repeat>`/`<if>`).

## Non-goals (deferred)
- Native list/collection subject (model C) — out of scope unless (B) proves insufficient.
- Nested `<for_each>`/`<repeat>`/`<if>` — same v1 limitation.
- Filtering/sorting in XML — the C++ adapter presents the already-ordered/filtered view.

## Risks
- **Redundancy with `<repeat count>+${i}`** — the biggest risk is building a MAJOR feature that a `<repeat count>` + existing dynamic accessors already cover. Gate: validate against real call sites in the first plan step; be willing to conclude YAGNI.
- **Per-item dynamic-subject lifetime** — item field subjects are dynamic (destroyed/recreated on reconnect); tracking per-item `SubjectLifetime` tokens aligned with per-item fragment instances, torn down in lockstep on rebuild, is the crash-class core (parallel-vectors discipline from CLAUDE.md, but driven by the parser rather than a C++ panel).
- **Adapter interface churn** — designing `IXmlCollection` too narrow (misses a real field-access pattern) or too broad (reinvents subjects). Keep it minimal: `count` + reactive count subject + `field(index, name, lifetime)`.
- **Subject-core temptation** — resist drifting from (B) into (C); if a case seems to need (C), re-examine whether the adapter can express it first.

## Next step (when we pick this up — NOT tonight)
1. Validate the gap: pick 2–3 real collections (AMS slots, fans) and write out what the `<repeat count>+${i}` version costs vs. what `<for_each>` (model B) would cost. Decide go/no-go on that evidence.
2. If go: confirm model (B), then run superpowers:writing-plans → subagent-driven execution, reusing the `xml_frag_*` core the way `<if>` did.
