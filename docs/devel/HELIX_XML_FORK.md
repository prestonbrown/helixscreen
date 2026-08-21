# LVGL XML: Fork Origin, Licensing, and Divergence

**Date:** 2026-02-05 | **Updated:** 2026-08-08
**Status:** SETTLED — `lib/helix-xml/` is a permanent hard fork. There is no upstream to track.

**Key paths:** `lib/helix-xml/` (engine) · `lib/helix-xml/src/xml/parsers/` (widget parsers) · `ui_xml/` (300+ layouts)
**Upstream of `lib/helix-xml/`:** https://github.com/prestonbrown/helix-xml — ours, MIT, where the engine's issues live

This doc explains where the XML engine came from, what we are allowed to do with it, and how we
relate to the version LVGL now sells. For XML *syntax*, read `LVGL9_XML_GUIDE.md` and
`LVGL9_XML_ATTRIBUTES_REFERENCE.md`.

---

## Timeline

| Date | Event |
|------|-------|
| 2024-11-22 | XML first added to LVGL (`fc5939dcf`) |
| 2025-02 | LVGL ends its collaboration with SquareLine Studio |
| 2026-01-26 | **Our fork point** — `a15dcbeb5`, `v9.4.0-358-ga15dcbeb5`, the last commit with XML in core |
| 2026-01-27 | XML removed from LVGL core (`7c1e0684f`, PR #9565) |
| 2026-02-18 | LVGL v9.5.0 released (`85aa60d18`) — first release with no XML. We upgrade to it the same day |
| 2026-02-18 | We extract the engine into `lib/helix-xml/`, on branch `feature/helix-xml` |
| 2026-02-23 | `8d079ff70` rolls helix-xml forward to `a15dcbeb5`, the true last-XML tree |
| 2026-05-26 | LVGL Pro Editor v1.2.1 — "first-class support" for the 9.5-era XML features |
| 2026-07-09 | LVGL publishes Pro licensing tiers (Community / Product $20k / Platform) |
| 2026-07-27 | LVGL publishes ["The Future of the XML Engine"](https://lvgl.io/blog/announcement-lv_xml-removal-from-v9-5) — the public rationale, six months after the code was actually removed |

## What we forked, exactly

Not "LVGL 9.4". The precise base is:

```
commit  a15dcbeb56db765db8853261df3013ba037b17fc
date    2026-01-26
describe v9.4.0-358-ga15dcbeb5
parent-of 7c1e0684f "feat(xml): remove the XML parser and loader (#9565)"
```

That is master 358 commits past the v9.4.0 tag — *after* 9.4 shipped, one commit *before* XML was
deleted. It is the highest-water-mark MIT XML engine that has ever existed. Several docs and one
header still say "extracted from LVGL 9.4"; that is a useful shorthand but it undersells the base
by 358 commits, which is where slots, `bind_style_prop`, the imagebutton parser, blur styles,
local style selectors, color/opa animation, and the `state_changed` trigger came from.

The LVGL submodule itself is pinned at **v9.5.0** (`85aa60d18`) with XML and expat excluded from
the build (`Makefile:297`). So we run new LVGL core with the last MIT XML engine bolted on.

## Licensing

**Our position is clean, and LVGL has said so in public.**

- `LICENCE.txt` at `a15dcbeb5` is plain MIT, `Copyright (c) 2025 LVGL Kft`. Verified: it is
  byte-identical to the one at v9.5.0.
- No separate or additional license file ever covered the XML sources while they were in the repo.
  The removal commit deleted `src/libs/expat/LICENSE.txt` along with the sources — it did not add
  a restrictive one.
- From the July 2026 announcement, verbatim: *"Versions up to 9.4 keep their MIT XML engine
  forever. MIT can't be revoked, and we wouldn't try."*

So the grant we received is permanent and unconditional beyond attribution. We may use, modify,
relicense-in-combination, and ship the engine indefinitely.

**What we owe:** MIT requires the copyright notice and permission text to travel with the code.
Our attribution hygiene is currently thin, and now that LVGL has drawn a public line around this
code it is worth tightening:

| Gap | Where |
|-----|-------|
| No `LICENSE` or `NOTICE` file in `lib/helix-xml/` | Only `src/libs/expat/LICENSE.txt` exists there |
| SPDX headers on ~7 of ~110 files | The LVGL-derived `.c`/`.h` files carry none |
| Root `COPYRIGHT` third-party list omits helix-xml and expat | `COPYRIGHT:32-45` |
| Root `README.md` never mentions the fork | Badge says "LVGL-9.5", no disclosure |

**Mixed licensing inside the directory.** `lv_xml_expr.{c,h}` are HelixScreen-authored and marked
`GPL-3.0-or-later`, sitting in an otherwise MIT/LVGL-derived tree. That combination is legal —
HelixScreen ships as GPLv3 and MIT is GPL-compatible — but it means `lib/helix-xml/` as a whole is
**not** redistributable as MIT. Anyone lifting the directory wholesale would be taking GPL code.
New HelixScreen-authored files in that tree should keep the GPL header; files derived from LVGL
should keep MIT.

## What upstream did instead

LVGL moved the engine into **LVGL Pro** under a custom, non-open-source "LVGL license". The core
C library stays MIT. Their stated reasons:

1. **Release cadence.** The XML engine has to keep pace with the Editor — days, not the 2–3
   releases a year the core library ships with year-long support windows.
2. **Business.** Verbatim: *"An MIT-licensed XML engine would mean any company could take the
   format and the runtime, build a competing commercial editor on top of our work, and contribute
   nothing back to the library you use for free."*

The commercial shape, as of July 2026:

- **Editor** — free for personal, educational, and open-source use. Exports plain C that depends
  only on MIT LVGL.
- **Runtime XML loading on device** — *"a licensed feature… no free tier for on-device use."*
  Licensees get precompiled libraries or source.
- **Tiers** — Community (free, non-commercial), Product ($20,000 per product, ≤5 seats, 5 years),
  Platform (custom).

Runtime XML loading is exactly what HelixScreen does — `lv_xml_register_component_from_file()` at
boot, plus `HELIX_HOT_RELOAD` re-registration. Under their model that is the paid path, at a price
that is not remotely proportionate to a solo-maintained GPL project. Nothing about that changes
what we already have; it only forecloses ever adopting theirs.

## The relationship now: there isn't one

This is the part that is easy to get wrong. **`lib/helix-xml/` has no upstream.** There is no
branch to rebase onto, no patch series to carry, no "next sync". Post-`7c1e0684f` XML work lives
in a proprietary repo we have neither access to nor the right to copy from.

Practical consequences:

- Any XML bug is **our** bug. `PLUGIN_DEVELOPMENT.md` used to say "LVGL 9.5 does not provide
  `lv_xml_unregister_subject()`" as though it were an upstream limitation — LVGL 9.5 has no XML at
  all; that gap is ours to fill or not.
- Upstream XML documentation links are dead. `docs.lvgl.io/master/details/xml/` now redirects to
  Pro docs describing a different, closed engine. Do not cite it as authoritative for our syntax.
- `lib/helix-xml/` is a submodule pointing at
  [prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml) — **ours**, so it inverts
  this repo's usual submodule rule. Edit the files in place, commit and push *inside*
  `lib/helix-xml/`, then commit the bumped pointer here. Never write a `patches/*.patch` for it.
  It is also excluded from clang-format, so match surrounding style by hand.
- It is pure C and must not include or call app-layer C++.
- Being its own repo, it has its own tests and its own CI - neither of which the parent's
  `make test` touches. `lib/helix-xml/tests/` is a standalone CMake + Unity suite that builds
  the engine against a *pinned upstream* LVGL v9.5.0, not our patched `lib/lvgl`, so it stands
  up from a bare clone: `cmake -S tests -B build`, `cmake --build build`,
  `ctest --test-dir build --output-on-failure`. From this repo, `make test-xml`. Its CI
  (`.github/workflows/ci.yml` inside the submodule) runs a gcc + clang matrix, ASAN/UBSAN, and
  a conf-guards job that proves the `#if LV_USE_XML` / `LV_USE_TRANSLATION` / `LV_USE_OBJ_NAME`
  guards still hold. Details and the here-vs-there test split: `TESTING.md` § "helix-xml Engine
  Tests".
- In a worktree, `lib/` is symlinked to the main tree — so editing `lib/helix-xml/` from a
  worktree edits the main checkout, exactly as for `lib/lvgl/`. Make engine changes in the main
  tree, or unlink first (`scripts/setup-worktree.sh --unlink`).

### Clean-room rule

If we ever implement something upstream also has, the rule is:

**Allowed:** reading LVGL's *published documentation* (lvgl.io/docs/pro), blog posts, and public
release notes; observing the XML *format* — tag and attribute names, argument shapes. Formats and
interfaces are the interoperable surface, not the protected work.

**Not allowed:** reading LVGL Pro source, decompiling the Editor or its bundled runtimes, lifting
any post-`7c1e0684f` XML implementation, or pasting their documentation prose into ours.

**Required:** when a feature is implemented from their docs, the commit body names the doc URL and
the date it was read. That record is what makes "clean room" a fact rather than a claim.

This is engineering policy, not legal advice. If a parity feature ever becomes load-bearing, get a
real opinion first.

---

## Feature comparison, July 2026

Built from LVGL's public Pro documentation only — no Pro source was consulted.

### Parity (we already have it)

`<component>` / `<widget>` / `<screen>` roots · `<consts>` · `<styles>` with `selector=`, local
style selector suffixes (`style_bg_opa-indicator-pressed`), and `bind_style_prop` · gradients
(horizontal, vertical, linear, radial, conical, with `<stop>`) · `<translations>` ·
`<timeline>`/`<animation>`/`<include_timeline>` + `play_timeline_event` · all nine event tags
(`event_cb`, `subject_toggle_event`, `subject_set_int/float/string_event`,
`subject_increment_event`, `screen_load_event`, `screen_create_event`, `play_timeline_event`) ·
`bind_text` / `bind_value` / `bind_flag_if_*` / `bind_state_if_*` · `<api><prop default=>` ·
XML-driven UI tests (`lv_xml_test.c`).

Two of these are dormant: no `ui_xml/` file uses `<timeline>` or `<animation>` today.

### Gaps (they have it, we don't)

| Feature | Notes |
|---------|-------|
| `<api><param>` — multi-argument props, used as `prop-param="…"` | We hardcode exactly one instance of this shape (`bind_text-fmt` on labels and spangroups). The generic mechanism is absent; `process_prop_element` drops every non-`prop` child, and `resolve_params` has no hyphen-suffix path. A vestigial `type = "compound"` marker is the only trace |
| `<enumdef>` / `<enum>` | Widget-scoped named enums. We have 37 C++-registered custom widgets whose enum-ish attributes are unvalidated strings |
| `<element access="add\|get\|set">` | Declares widget sub-structures in the API block |
| Real `<slot>` declarations | See below — ours is a lookalike, not a slot system |
| `<targets>` / `if_target` | Build-time UI variants keyed on display size and memory budget |
| `<preview>` contexts, Figma plugin, online viewer, C export | Editor-side tooling. Not applicable |

**On slots specifically:** the roll commit claimed slot support, and the `<component-slotname>`
*usage* syntax does parse (`lv_xml.c:2147-2163`). But there is no `<slot>` declaration, no
validation against the component's API, no default content, and no multi-slot dispatch. It
degrades to `lv_obj_find_by_name(parent, slot_name)` — and when that returns NULL you get the
generic "unknown tag / STALE BINARY" error, which is a genuinely bad diagnostic. This is the one
gap that actively costs us something today.

### Ahead (we have it, they don't)

| Feature | Why it exists |
|---------|---------------|
| `cond=` expression language on `bind_flag_if` / `bind_state_if` / `bind_style_if`, and `<subject_expr>` derived subjects | Full Pratt parser: arithmetic, comparison, boolean, word forms. Upstream's binding model is still one subject and one comparison operator per bind — there is no way to express `a or b gt c` in their XML |
| `<repeat count=>` with reactive rebuild, `$i`, `${expr}` composition | Kills the C++ create-and-wire loop (CLAUDE.md rule 8) |
| `<if cond=>` / `<else/>` structural conditionals | Builds only the matching branch |
| `parts="main,indicator,knob"` on style binds | One line instead of three |
| `hidden_if_prop_eq` / `hidden_if_not_eq` / `hidden_if_empty` | Prop-driven visibility without a subject |
| Inline PCDATA text auto-synthesizing `translation_tag` | `<text_muted>Foo</text_muted>`; whitespace collapsed to stay byte-identical with `scripts/translations/extractor.py` |
| `float` and `color` subjects | Upstream Pro docs, July 2026: *"Currently, only integer and string types are supported."* |
| `bind_src` accepting STRING subjects | Upstream only accepted POINTER |
| `HELIX_HOT_RELOAD` re-registration | Edit XML, see it in ~500ms, no restart |
| `<subject name= type= value=>` convention | Ours; upstream uses tag-per-type (`<int name=…>`). Both parse — `type=` overrides the tag name (`lv_xml_component.c:567`) |

## Should we chase parity?

**No — not as a goal.** Parity would be a real cost paid for a benefit we cannot collect.

1. **The compatibility payoff doesn't exist.** Format parity is only worth money if it buys tool
   interop, and it can't: their runtime is paid, their Editor can't render our 37 C++-registered
   widgets, and our files already diverge on subject declarations, `cond=`, `parts=`, `<repeat>`,
   and `<if>`. We would be matching a format we can never round-trip through their tools.
2. **Most of the gap is editor-shaped.** `<targets>`, `<preview>`, Figma, C export and the memory
   model exist to serve a desktop IDE and a compile-to-C pipeline. We have neither.
3. **`<targets>` would be a regression.** It selects asset and layout variants at build or init
   time from a display-size table. We resolve at runtime, reactively, off a single `ui_breakpoint`
   subject (`include/ui_breakpoint.h`) that XML consumes via `bind_flag_if_eq` and `cond=`. Ours
   handles rotation and runtime resolution change; theirs doesn't need to.
4. **We're ahead where it counts.** Expressions, `<repeat>`, and `<if>/<else>` are the features
   that actually removed C++ from our UI layer. Upstream shows no sign of adding them, so parity
   is a one-way ratchet away from our own design.
5. **Chasing it invites the one mistake we can't undo.** A parity backlog creates standing
   pressure to "just check how they did it." Nothing in this repo is worth that.

**Adopt three things anyway — on their own merits, not for parity:**

- **`<api><param>`.** Generic multi-argument props. We already need it; `bind_text-fmt` is the
  hardcoded proof. Cheapest of the three and unblocks well-typed custom-widget APIs.
- **Real `<slot>` declarations.** Fixes an active footgun. The current silent-NULL path produces a
  misleading "STALE BINARY" error, and `lv_xml_create()` doesn't implement the fallback at all —
  the two entry points behave differently, which is worse than not having slots.
- **`<enumdef>`.** Turns 37 widgets' worth of unvalidated string attributes into checked values,
  and gives the XML linter something to check against.

Each is independently justifiable. If we build them and they happen to look like upstream's,
that's convergent design on an obvious problem — record the doc URL in the commit and move on.

These are tracked as [helix-xml#1](https://github.com/prestonbrown/helix-xml/issues/1) (slots),
[#2](https://github.com/prestonbrown/helix-xml/issues/2) (`<param>`), and
[#3](https://github.com/prestonbrown/helix-xml/issues/3) (`<enumdef>`). The engine's roadmap lives
on GitHub, not in `ROADMAP.md`.

**Explicitly not adopting:** `<targets>`/`if_target`, `<preview>`, C export, `<element>`. And the
dormant `<timeline>`/`<animation>` code should be either used or deleted rather than maintained on
the theory that upstream has it.

---

## References

- [The Future of the XML Engine](https://lvgl.io/blog/announcement-lv_xml-removal-from-v9-5) — LVGL, 2026-07-27
- [LVGL Pro licensing](https://lvgl.io/blog/announcement-lvgl-pro-licensing) — 2026-07-09
- [LVGL Pro Editor v1.2.1](https://lvgl.io/blog/release-lvgl-pro-v1-2-1) — 2026-05-26, the 9.5-era XML features
- [LVGL Pro XML syntax docs](https://lvgl.io/docs/pro/syntax) — the only upstream XML reference we may read
- [PR #9565: remove the XML parser and loader](https://github.com/lvgl/lvgl/pull/9565)
- [LVGL LICENCE.txt](https://github.com/lvgl/lvgl/blob/master/LICENCE.txt)
- [LVGL ends its collaboration with SquareLine Studio](https://forum.lvgl.io/t/lvgl-ends-its-collaboration-with-squareline-studio/14638)

Internal: `LVGL9_XML_GUIDE.md` (syntax) · `LVGL9_XML_ATTRIBUTES_REFERENCE.md` (attributes) ·
`BUILD_SYSTEM.md` (how helix-xml is built) · `patches/README.md` (LVGL patches; XML patches are
baked in) · `ROADMAP.md` § "XML Engine Extraction & LVGL 9.5 Upgrade"
