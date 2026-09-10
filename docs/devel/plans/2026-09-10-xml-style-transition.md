# XML Style Transitions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a CSS-style `transition` attribute to the XML style parser so state-driven motion can be declared in XML instead of written in C++, and use it for the nav bar's active/inactive icon swap.

**Architecture:** `lib/helix-xml/` parses `transition` into an engine-owned `lv_style_transition_dsc_t` hung off the `lv_xml_style_t` record, and calls LVGL's existing `lv_style_set_transition`. LVGL already interpolates on state change; nothing new animates anything. A global scale walks the scope lists and retimes in place so the app's animations preference can switch motion off instantly.

**Tech Stack:** C (engine, MIT, `LV_LOG_*`), C++ (app, GPL-3.0-or-later, spdlog), Unity + ctest for the engine suite, Catch2 for the app suite.

**Spec:** `docs/devel/plans/2026-09-10-xml-style-transition-design.md`

## Global Constraints

- Engine files under `lib/helix-xml/` are C, carry `SPDX-License-Identifier: MIT`, and log through `LV_LOG_WARN` / `LV_LOG_INFO`. Never spdlog.
- App files carry `SPDX-License-Identifier: GPL-3.0-or-later` and log through spdlog. Never `printf`/`LV_LOG_*`.
- `lib/helix-xml/` is ours and is edited directly. It is a separate repo, so its commits are made inside `lib/helix-xml/` and the parent repo records a submodule pin bump.
- Engine code must not name any HelixScreen concept. `settings_animations_enabled` never appears under `lib/helix-xml/`.
- No development archaeology in comments. State the constraint, not the history.
- Engine suite: `make test-xml`. App suite: `make test && ./build/bin/helix-tests "[tag]"`.
- Before compiling, check for a peer build: `pgrep -x -d' ' 'make|cc1plus'`.

## File Structure

| File | Responsibility |
|---|---|
| `lib/helix-xml/src/xml/lv_xml_base_types.h/.c` | Gains `lv_xml_style_prop_anim_type()`, the interpolatability classifier, moved here from `lv_xml_component.c` because it is a converter and this file is where converters live |
| `lib/helix-xml/src/xml/lv_xml_style.h` | `lv_xml_style_t` gains ownership of the dsc, its prop array, and the pre-scale duration |
| `lib/helix-xml/src/xml/lv_xml_style.c` | Parses the shorthand and longhand attributes, builds and installs the dsc, frees the previous one on re-registration |
| `lib/helix-xml/src/xml/lv_xml_component.c` | Frees the dsc in the scope teardown walk; implements the scale walk because the scope lists are static here |
| `lib/helix-xml/tests/cases/test_style.c` | Engine tests for parsing, rejection, replacement and scaling |
| `src/application/application.cpp` | Wires the scale to the animations preference once at startup |
| `ui_xml/navigation_bar.xml` | First consumer: overlaid icons, state binding, transition |

---

### Task 1: Move and correct the interpolatability classifier

`style_prop_anim_get_type` is `static` in `lv_xml_component.c` and serves `<animation>`. The style parser needs the same judgement, and forking a twin would let the two drift. Move it to the converters file and fix two wrong verdicts.

**Files:**
- Modify: `lib/helix-xml/src/xml/lv_xml_component.c`
- Modify: `lib/helix-xml/src/xml/lv_xml_base_types.h`
- Modify: `lib/helix-xml/src/xml/lv_xml_base_types.c`
- Test: `lib/helix-xml/tests/cases/test_base_types.c`

**Interfaces:**
- Produces: `lv_xml_style_prop_anim_type_t` with values `LV_XML_STYLE_PROP_ANIM_INT`, `_OPA`, `_COLOR`, `_UNKNOWN`; and `lv_xml_style_prop_anim_type_t lv_xml_style_prop_anim_type(lv_style_prop_t prop)`.

- [ ] **Step 1: Write the failing test**

In `lib/helix-xml/tests/cases/test_base_types.c`, add:

```c
static void test_prop_anim_type_matches_what_lvgl_can_actually_interpolate(void)
{
    /* Numeric and opacity props ride LVGL's generic lerp. */
    TEST_ASSERT_EQUAL_INT(LV_XML_STYLE_PROP_ANIM_INT,
                          lv_xml_style_prop_anim_type(LV_STYLE_TRANSFORM_SCALE_X));
    TEST_ASSERT_EQUAL_INT(LV_XML_STYLE_PROP_ANIM_OPA,
                          lv_xml_style_prop_anim_type(LV_STYLE_TEXT_OPA));

    /* LVGL mixes these through lv_color_mix(). */
    TEST_ASSERT_EQUAL_INT(LV_XML_STYLE_PROP_ANIM_COLOR,
                          lv_xml_style_prop_anim_type(LV_STYLE_BG_COLOR));
    TEST_ASSERT_EQUAL_INT(LV_XML_STYLE_PROP_ANIM_COLOR,
                          lv_xml_style_prop_anim_type(LV_STYLE_RECOLOR));
    TEST_ASSERT_EQUAL_INT(LV_XML_STYLE_PROP_ANIM_COLOR,
                          lv_xml_style_prop_anim_type(LV_STYLE_IMAGE_RECOLOR));

    /* Absent from LVGL's colour-mix block: these bleed channels through .num,
     * so the classifier must refuse them. */
    TEST_ASSERT_EQUAL_INT(LV_XML_STYLE_PROP_ANIM_UNKNOWN,
                          lv_xml_style_prop_anim_type(LV_STYLE_ARC_COLOR));
    TEST_ASSERT_EQUAL_INT(LV_XML_STYLE_PROP_ANIM_UNKNOWN,
                          lv_xml_style_prop_anim_type(LV_STYLE_LINE_COLOR));

    /* Pointer-valued: blending the low 32 bits yields a pointer the draw pass
     * will follow. */
    TEST_ASSERT_EQUAL_INT(LV_XML_STYLE_PROP_ANIM_UNKNOWN,
                          lv_xml_style_prop_anim_type(LV_STYLE_BG_IMAGE_SRC));
    TEST_ASSERT_EQUAL_INT(LV_XML_STYLE_PROP_ANIM_UNKNOWN,
                          lv_xml_style_prop_anim_type(LV_STYLE_TEXT_FONT));
}
```

Register it in that file's `main()` with `RUN_TEST(test_prop_anim_type_matches_what_lvgl_can_actually_interpolate);`

- [ ] **Step 2: Run it and confirm it fails**

Run: `make test-xml`
Expected: compile error, `lv_xml_style_prop_anim_type` undeclared.

- [ ] **Step 3: Move the function and correct it**

In `lib/helix-xml/src/xml/lv_xml_base_types.h`, add above the closing guard:

```c
typedef enum {
    LV_XML_STYLE_PROP_ANIM_INT,
    LV_XML_STYLE_PROP_ANIM_OPA,
    LV_XML_STYLE_PROP_ANIM_COLOR,
    LV_XML_STYLE_PROP_ANIM_UNKNOWN
} lv_xml_style_prop_anim_type_t;

/**
 * Classify how LVGL interpolates a style property during a transition.
 * Properties LVGL cannot interpolate safely report _UNKNOWN: pointer-valued
 * properties would have their low 32 bits blended into a pointer the draw pass
 * follows, and colours outside lv_obj_style.c's mix block bleed channels.
 */
lv_xml_style_prop_anim_type_t lv_xml_style_prop_anim_type(lv_style_prop_t prop);
```

Move the body from `lv_xml_component.c` into `lv_xml_base_types.c`, renaming it and its enum values. Apply two corrections while moving:
- add `case LV_STYLE_RECOLOR:` and `case LV_STYLE_IMAGE_RECOLOR:` to the COLOR group
- remove `LV_STYLE_ARC_COLOR` and `LV_STYLE_LINE_COLOR` from the COLOR group so they fall to `_UNKNOWN`

- [ ] **Step 4: Update the `<animation>` caller**

In `lv_xml_component.c`, delete the old static function and its enum, and change the `<animation>` call site to use `lv_xml_style_prop_anim_type()` and the new enum names. Add `#include "lv_xml_base_types.h"` if not already present.

- [ ] **Step 5: Run the suite**

Run: `make test-xml`
Expected: PASS, including the pre-existing `<animation>` tests.

- [ ] **Step 6: Commit**

```bash
cd lib/helix-xml
git add src/xml/lv_xml_base_types.h src/xml/lv_xml_base_types.c src/xml/lv_xml_component.c tests/cases/test_base_types.c
git commit -m "refactor(style): share the interpolatability classifier and correct two verdicts"
```

---

### Task 2: Give `lv_xml_style_t` ownership of a transition dsc

Nothing frees what a style property points at. The dsc and its prop array need an owner with the same lifetime as the style, and re-registration must replace rather than orphan.

**Files:**
- Modify: `lib/helix-xml/src/xml/lv_xml_style.h`
- Modify: `lib/helix-xml/src/xml/lv_xml_style.c`
- Modify: `lib/helix-xml/src/xml/lv_xml_component.c`
- Test: `lib/helix-xml/tests/cases/test_style.c`

**Interfaces:**
- Produces: `lv_xml_style_t` fields `trans_dsc`, `trans_props`, `trans_authored_time`; and `void lv_xml_style_transition_clear(lv_xml_style_t * xs);`

- [ ] **Step 1: Add the ownership fields**

In `lib/helix-xml/src/xml/lv_xml_style.h`:

```c
typedef struct _lv_xml_style_t {
    const char * name;
    const char * long_name;
    lv_style_t style;
    /* Engine-owned transition. LVGL stores only a pointer in the style, and
     * lv_style_reset() does not follow it, so the record owns both allocations.
     * trans_authored_time is the pre-scale duration: scaling reads it rather
     * than the live value, so repeated scaling does not compound. */
    lv_style_transition_dsc_t * trans_dsc;
    lv_style_prop_t * trans_props;
    uint32_t trans_authored_time;
} lv_xml_style_t;

/**
 * Drop the style's transition, freeing the engine-owned descriptor and its
 * property array. Safe on a style that has none.
 */
void lv_xml_style_transition_clear(lv_xml_style_t * xs);
```

- [ ] **Step 2: Write the failing test**

In `lib/helix-xml/tests/cases/test_style.c`, add a pointer-reading helper beside `style_prop_num`:

```c
static const void * style_prop_ptr(const lv_xml_style_t * xs, lv_style_prop_t prop)
{
    lv_style_value_t v;
    lv_style_res_t res = lv_style_get_prop((lv_style_t *)&xs->style, prop, &v);
    TEST_ASSERT_EQUAL_INT_MESSAGE(LV_STYLE_RES_FOUND, (int)res,
                                  "the property is not present in the registered style at all");
    return v.ptr;
}
```

and the test:

```c
static void test_clearing_a_transition_removes_the_property_and_is_idempotent(void)
{
    ASSERT_XML_REGISTERS("trans_clear",
                         "<component>"
                         "  <styles>"
                         "    <style name=\"t\" transition_props=\"opa\" transition_duration=\"120\"/>"
                         "  </styles>"
                         "  <view extends=\"lv_obj\" name=\"clear_root\"/>"
                         "</component>");

    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("trans_clear");
    TEST_ASSERT_NOT_NULL(scope);
    lv_xml_style_t * s = lv_xml_get_style_by_name(scope, "t");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(s->trans_dsc);
    TEST_ASSERT_TRUE(style_has_prop(s, LV_STYLE_TRANSITION));

    lv_xml_style_transition_clear(s);
    TEST_ASSERT_NULL(s->trans_dsc);
    TEST_ASSERT_NULL(s->trans_props);
    TEST_ASSERT_FALSE(style_has_prop(s, LV_STYLE_TRANSITION));

    /* A second clear must not double free. */
    lv_xml_style_transition_clear(s);
    TEST_ASSERT_NULL(s->trans_dsc);
}
```

Register it in `main()`.

- [ ] **Step 3: Run it and confirm it fails**

Run: `make test-xml`
Expected: FAIL. `transition_props` is not parsed yet, so `trans_dsc` is NULL and the first `TEST_ASSERT_NOT_NULL` fires. This test is completed by Task 3; implement the clear function now.

- [ ] **Step 4: Implement the clear function**

In `lib/helix-xml/src/xml/lv_xml_style.c`:

```c
void lv_xml_style_transition_clear(lv_xml_style_t * xs)
{
    if(xs == NULL || xs->trans_dsc == NULL) return;

    lv_style_remove_prop(&xs->style, LV_STYLE_TRANSITION);
    lv_free(xs->trans_dsc);
    lv_free(xs->trans_props);
    xs->trans_dsc = NULL;
    xs->trans_props = NULL;
    xs->trans_authored_time = 0;
}
```

- [ ] **Step 5: Free it during scope teardown**

In `lib/helix-xml/src/xml/lv_xml_component.c`, in the style walk inside `component_scope_free`:

```c
    lv_xml_style_t * style;
    LV_LL_READ(&scope->style_ll, style) {
        lv_free((char *)style->name);
        lv_free((char *)style->long_name);
        lv_xml_style_transition_clear(style);
        lv_style_reset(&style->style);
    }
    lv_ll_clear(&scope->style_ll);
```

- [ ] **Step 6: Commit**

```bash
cd lib/helix-xml
git add src/xml/lv_xml_style.h src/xml/lv_xml_style.c src/xml/lv_xml_component.c tests/cases/test_style.c
git commit -m "feat(style): give the style record ownership of a transition descriptor"
```

---

### Task 3: Parse the longhand attributes

**Files:**
- Modify: `lib/helix-xml/src/xml/lv_xml_style.c`
- Test: `lib/helix-xml/tests/cases/test_style.c`

**Interfaces:**
- Consumes: `lv_xml_style_transition_clear` (Task 2), `lv_xml_style_prop_anim_type` (Task 1).
- Produces: attributes `transition_props`, `transition_duration`, `transition_easing`, `transition_delay`. Easing names: `linear`, `ease_in`, `ease_out`, `ease_in_out`, `overshoot`, `bounce`, `step`.

- [ ] **Step 1: Write the failing tests**

```c
static void test_longhand_transition_builds_a_descriptor_on_the_style(void)
{
    ASSERT_XML_REGISTERS("trans_long",
                         "<component>"
                         "  <styles>"
                         "    <style name=\"t\" transition_props=\"opa|transform_scale_x\""
                         "           transition_duration=\"180\" transition_easing=\"ease_out\""
                         "           transition_delay=\"20\"/>"
                         "  </styles>"
                         "  <view extends=\"lv_obj\" name=\"long_root\"/>"
                         "</component>");

    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("trans_long");
    lv_xml_style_t * s = lv_xml_get_style_by_name(scope, "t");
    TEST_ASSERT_NOT_NULL(s);

    const lv_style_transition_dsc_t * d = style_prop_ptr(s, LV_STYLE_TRANSITION);
    TEST_ASSERT_EQUAL_PTR(s->trans_dsc, d);
    TEST_ASSERT_EQUAL_UINT32(180, d->time);
    TEST_ASSERT_EQUAL_UINT32(20, d->delay);
    TEST_ASSERT_EQUAL_PTR(lv_anim_path_ease_out, d->path_xcb);

    TEST_ASSERT_EQUAL_INT(LV_STYLE_OPA, d->props[0]);
    TEST_ASSERT_EQUAL_INT(LV_STYLE_TRANSFORM_SCALE_X, d->props[1]);
    TEST_ASSERT_EQUAL_INT(0, d->props[2]);   /* NULL terminator LVGL scans for */
}

static void test_a_non_interpolatable_transition_prop_is_rejected_by_name(void)
{
    log_capture_start();
    ASSERT_XML_REGISTERS("trans_bad",
                         "<component>"
                         "  <styles>"
                         "    <style name=\"t\" transition_props=\"opa|bg_image_src\""
                         "           transition_duration=\"100\"/>"
                         "  </styles>"
                         "  <view extends=\"lv_obj\" name=\"bad_root\"/>"
                         "</component>");
    log_capture_stop();

    TEST_ASSERT_TRUE(log_contains("bg_image_src"));

    /* The whole transition is refused, not silently trimmed to the good half:
     * a partially applied transition would be a surprise at the call site. */
    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("trans_bad");
    lv_xml_style_t * s = lv_xml_get_style_by_name(scope, "t");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NULL(s->trans_dsc);
    TEST_ASSERT_FALSE(style_has_prop(s, LV_STYLE_TRANSITION));
}

static void test_re_registering_a_style_replaces_its_transition(void)
{
    ASSERT_XML_REGISTERS("trans_twice",
                         "<component>"
                         "  <styles>"
                         "    <style name=\"t\" transition_props=\"opa\" transition_duration=\"100\"/>"
                         "    <style name=\"t\" transition_props=\"opa\" transition_duration=\"250\"/>"
                         "  </styles>"
                         "  <view extends=\"lv_obj\" name=\"twice_root\"/>"
                         "</component>");

    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("trans_twice");
    lv_xml_style_t * s = lv_xml_get_style_by_name(scope, "t");
    TEST_ASSERT_NOT_NULL(s);

    const lv_style_transition_dsc_t * d = style_prop_ptr(s, LV_STYLE_TRANSITION);
    TEST_ASSERT_EQUAL_PTR(s->trans_dsc, d);
    TEST_ASSERT_EQUAL_UINT32(250, d->time);
}
```

Register all three in `main()`, and register Task 2's clear test too.

- [ ] **Step 2: Run and confirm they fail**

Run: `make test-xml`
Expected: FAIL, `trans_dsc` NULL and `LV_STYLE_TRANSITION` absent.

- [ ] **Step 3: Implement the longhand parse**

In `lv_xml_style.c`, add the easing converter above `lv_xml_register_style`:

```c
static lv_anim_path_cb_t transition_easing_to_cb(const char * txt)
{
    if(lv_streq(txt, "linear"))      return lv_anim_path_linear;
    if(lv_streq(txt, "ease_in"))     return lv_anim_path_ease_in;
    if(lv_streq(txt, "ease_out"))    return lv_anim_path_ease_out;
    if(lv_streq(txt, "ease_in_out")) return lv_anim_path_ease_in_out;
    if(lv_streq(txt, "overshoot"))   return lv_anim_path_overshoot;
    if(lv_streq(txt, "bounce"))      return lv_anim_path_bounce;
    if(lv_streq(txt, "step"))        return lv_anim_path_step;
    return NULL;
}
```

and a duration parser that accepts a `ms` suffix:

```c
static uint32_t transition_time_to_ms(const char * txt)
{
    return (uint32_t)lv_xml_atoi(txt);   /* lv_xml_atoi stops at the 'm' of "200ms" */
}
```

Collect the four attributes during the existing attribute loop into a local struct rather than applying them inline, because the descriptor cannot be built until every part is known. After the loop, build and install:

```c
static void style_transition_install(lv_xml_style_t * xs, const char * props_str,
                                     uint32_t time, uint32_t delay, lv_anim_path_cb_t path,
                                     const char * style_name)
{
    lv_xml_style_transition_clear(xs);
    if(props_str == NULL) return;

    char buf[256];
    lv_strncpy(buf, props_str, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    /* Count first so the array is allocated once, then fill. */
    uint32_t cnt = 1;
    for(const char * p = buf; *p; p++) if(*p == '|') cnt++;

    lv_style_prop_t * arr = lv_malloc((cnt + 1) * sizeof(lv_style_prop_t));
    LV_ASSERT_MALLOC(arr);
    if(arr == NULL) return;

    uint32_t n = 0;
    char * bufp = buf;
    const char * tok = lv_xml_split_str(&bufp, '|');
    while(tok) {
        lv_style_prop_t prop = lv_xml_style_prop_to_enum(tok);
        if(prop == LV_STYLE_PROP_INV) {
            LV_LOG_WARN("`%s` is not a style property, in transition of style `%s`", tok, style_name);
            lv_free(arr);
            return;
        }
        if(lv_xml_style_prop_anim_type(prop) == LV_XML_STYLE_PROP_ANIM_UNKNOWN) {
            LV_LOG_WARN("`%s` cannot be interpolated, in transition of style `%s`", tok, style_name);
            lv_free(arr);
            return;
        }
        arr[n++] = prop;
        tok = lv_xml_split_str(&bufp, '|');
    }
    arr[n] = 0;

    lv_style_transition_dsc_t * dsc = lv_malloc(sizeof(*dsc));
    LV_ASSERT_MALLOC(dsc);
    if(dsc == NULL) { lv_free(arr); return; }

    lv_style_transition_dsc_init(dsc, arr, path ? path : lv_anim_path_linear, time, delay, NULL);

    xs->trans_dsc = dsc;
    xs->trans_props = arr;
    xs->trans_authored_time = time;
    lv_style_set_transition(&xs->style, dsc);
}
```

Call it after the attribute loop, before `lv_xml_register_style` returns.

- [ ] **Step 4: Run the tests**

Run: `make test-xml`
Expected: PASS, including Task 2's clear test.

- [ ] **Step 5: Commit**

```bash
cd lib/helix-xml
git add src/xml/lv_xml_style.c tests/cases/test_style.c
git commit -m "feat(style): parse the longhand transition attributes"
```

---

### Task 4: Parse the shorthand, and document both forms

**Files:**
- Modify: `lib/helix-xml/src/xml/lv_xml_style.c`
- Modify: `docs/devel/LVGL9_XML_GUIDE.md`
- Test: `lib/helix-xml/tests/cases/test_style.c`

**Interfaces:**
- Consumes: `style_transition_install` (Task 3).
- Produces: attribute `transition="<props> <duration> [easing] [delay]"`.

- [ ] **Step 1: Write the failing tests**

```c
static void test_shorthand_transition_parses_props_duration_easing_and_delay(void)
{
    ASSERT_XML_REGISTERS("trans_short",
                         "<component>"
                         "  <styles>"
                         "    <style name=\"t\" transition=\"opa|transform_scale_x 200ms ease_out 30ms\"/>"
                         "  </styles>"
                         "  <view extends=\"lv_obj\" name=\"short_root\"/>"
                         "</component>");

    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("trans_short");
    lv_xml_style_t * s = lv_xml_get_style_by_name(scope, "t");
    const lv_style_transition_dsc_t * d = style_prop_ptr(s, LV_STYLE_TRANSITION);

    TEST_ASSERT_EQUAL_UINT32(200, d->time);
    TEST_ASSERT_EQUAL_UINT32(30, d->delay);
    TEST_ASSERT_EQUAL_PTR(lv_anim_path_ease_out, d->path_xcb);
    TEST_ASSERT_EQUAL_INT(LV_STYLE_OPA, d->props[0]);
    TEST_ASSERT_EQUAL_INT(LV_STYLE_TRANSFORM_SCALE_X, d->props[1]);
    TEST_ASSERT_EQUAL_INT(0, d->props[2]);
}

static void test_shorthand_defaults_easing_to_linear_and_delay_to_zero(void)
{
    ASSERT_XML_REGISTERS("trans_short_min",
                         "<component>"
                         "  <styles>"
                         "    <style name=\"t\" transition=\"opa 90\"/>"
                         "  </styles>"
                         "  <view extends=\"lv_obj\" name=\"short_min_root\"/>"
                         "</component>");

    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("trans_short_min");
    lv_xml_style_t * s = lv_xml_get_style_by_name(scope, "t");
    const lv_style_transition_dsc_t * d = style_prop_ptr(s, LV_STYLE_TRANSITION);

    TEST_ASSERT_EQUAL_UINT32(90, d->time);
    TEST_ASSERT_EQUAL_UINT32(0, d->delay);
    TEST_ASSERT_EQUAL_PTR(lv_anim_path_linear, d->path_xcb);
}

static void test_longhand_overrides_the_matching_shorthand_field(void)
{
    /* XML attribute order is not dependable, so precedence is stated by rule:
     * each longhand attribute wins over that field of the shorthand. */
    ASSERT_XML_REGISTERS("trans_mixed",
                         "<component>"
                         "  <styles>"
                         "    <style name=\"t\" transition=\"opa 200ms ease_out\""
                         "           transition_duration=\"55\"/>"
                         "  </styles>"
                         "  <view extends=\"lv_obj\" name=\"mixed_root\"/>"
                         "</component>");

    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("trans_mixed");
    lv_xml_style_t * s = lv_xml_get_style_by_name(scope, "t");
    const lv_style_transition_dsc_t * d = style_prop_ptr(s, LV_STYLE_TRANSITION);

    TEST_ASSERT_EQUAL_UINT32(55, d->time);
    TEST_ASSERT_EQUAL_PTR(lv_anim_path_ease_out, d->path_xcb);   /* untouched half survives */
}
```

- [ ] **Step 2: Run and confirm they fail**

Run: `make test-xml`
Expected: FAIL, `transition` is not a known style property.

- [ ] **Step 3: Implement the shorthand**

Parse `transition` into the same local struct the longhand fills, before longhand fields are applied, so longhand assignment naturally overwrites. Tokens split on whitespace: first token is the prop list, second the duration, then an optional easing name and an optional delay. A token that parses as neither easing nor number warns and the shorthand is refused.

- [ ] **Step 4: Run the tests**

Run: `make test-xml`
Expected: PASS.

- [ ] **Step 5: Document both forms**

Add a "Transitions" section to `docs/devel/LVGL9_XML_GUIDE.md` covering the two spellings, the easing names, and three rules that are not guessable:
- put `transition` on the **base** style, because only the state being entered is scanned
- the state style must set the property on the animated widget itself, since equal endpoints are a silent no-op and `text_opa` is inheritable
- `LV_STATE_CHECKED` carries a theme background on buttons; prefer it on labels or override the background

- [ ] **Step 6: Commit**

```bash
cd lib/helix-xml
git add src/xml/lv_xml_style.c tests/cases/test_style.c
git commit -m "feat(style): accept the CSS-style transition shorthand"
cd ../..
git add docs/devel/LVGL9_XML_GUIDE.md
git commit -m "docs(xml): document declarative style transitions"
```

---

### Task 5: Global transition scale

No separate registry: every engine-owned dsc hangs off an `lv_xml_style_t`, and every style hangs off a scope. Walking `component_scope_ll` **and** `pending_free_scope_ll` reaches all of them, and the second list matters because a retired scope with borrowed styles can still back live widgets.

**Files:**
- Modify: `lib/helix-xml/src/xml/lv_xml_style.h`
- Modify: `lib/helix-xml/src/xml/lv_xml_component.c`
- Test: `lib/helix-xml/tests/cases/test_style.c`

**Interfaces:**
- Produces: `void lv_xml_set_transition_scale(uint32_t scale_256);` and `uint32_t lv_xml_get_transition_scale(void);`

- [ ] **Step 1: Write the failing test**

```c
static void test_transition_scale_retimes_registered_descriptors_without_compounding(void)
{
    ASSERT_XML_REGISTERS("trans_scale",
                         "<component>"
                         "  <styles>"
                         "    <style name=\"t\" transition=\"opa 200ms\"/>"
                         "  </styles>"
                         "  <view extends=\"lv_obj\" name=\"scale_root\"/>"
                         "</component>");

    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("trans_scale");
    lv_xml_style_t * s = lv_xml_get_style_by_name(scope, "t");
    const lv_style_transition_dsc_t * d = style_prop_ptr(s, LV_STYLE_TRANSITION);
    TEST_ASSERT_EQUAL_UINT32(200, d->time);

    lv_xml_set_transition_scale(128);            /* half speed */
    TEST_ASSERT_EQUAL_UINT32(100, d->time);

    /* Scaling reads the authored value, so applying a second scale does not
     * compound onto the first. */
    lv_xml_set_transition_scale(64);
    TEST_ASSERT_EQUAL_UINT32(50, d->time);

    lv_xml_set_transition_scale(0);              /* motion off */
    TEST_ASSERT_EQUAL_UINT32(0, d->time);

    lv_xml_set_transition_scale(256);            /* restored */
    TEST_ASSERT_EQUAL_UINT32(200, d->time);
}

static void test_a_style_registered_after_the_scale_is_set_is_born_scaled(void)
{
    lv_xml_set_transition_scale(0);

    ASSERT_XML_REGISTERS("trans_late",
                         "<component>"
                         "  <styles>"
                         "    <style name=\"t\" transition=\"opa 300ms\"/>"
                         "  </styles>"
                         "  <view extends=\"lv_obj\" name=\"late_root\"/>"
                         "</component>");

    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("trans_late");
    lv_xml_style_t * s = lv_xml_get_style_by_name(scope, "t");
    const lv_style_transition_dsc_t * d = style_prop_ptr(s, LV_STYLE_TRANSITION);

    TEST_ASSERT_EQUAL_UINT32(0, d->time);
    TEST_ASSERT_EQUAL_UINT32(300, s->trans_authored_time);

    lv_xml_set_transition_scale(256);
}
```

- [ ] **Step 2: Run and confirm they fail**

Run: `make test-xml`
Expected: compile error, `lv_xml_set_transition_scale` undeclared.

- [ ] **Step 3: Implement**

Declare in `lv_xml_style.h`:

```c
/**
 * Scale every declared transition duration. 256 runs them as authored, 0
 * disables motion. Applies to descriptors already registered and to any
 * registered afterwards.
 */
void lv_xml_set_transition_scale(uint32_t scale_256);
uint32_t lv_xml_get_transition_scale(void);
```

Implement in `lv_xml_component.c`, which owns the scope lists:

```c
static uint32_t transition_scale = 256;

uint32_t lv_xml_get_transition_scale(void)
{
    return transition_scale;
}

static void scope_retime_transitions(lv_ll_t * list)
{
    lv_xml_component_scope_t * scope;
    LV_LL_READ(list, scope) {
        lv_xml_style_t * style;
        LV_LL_READ(&scope->style_ll, style) {
            if(style->trans_dsc == NULL) continue;
            style->trans_dsc->time =
                (uint32_t)(((uint64_t)style->trans_authored_time * transition_scale) >> 8);
        }
    }
}

void lv_xml_set_transition_scale(uint32_t scale_256)
{
    transition_scale = scale_256;
    /* Retired scopes still back live widgets when their styles are borrowed. */
    scope_retime_transitions(&component_scope_ll);
    scope_retime_transitions(&pending_free_scope_ll);
}
```

In `style_transition_install` (Task 3), apply the current scale at build time so a style registered later is born scaled:

```c
    uint32_t scaled = (uint32_t)(((uint64_t)time * lv_xml_get_transition_scale()) >> 8);
    lv_style_transition_dsc_init(dsc, arr, path ? path : lv_anim_path_linear, scaled, delay, NULL);
    xs->trans_authored_time = time;
```

- [ ] **Step 4: Run the tests**

Run: `make test-xml`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
cd lib/helix-xml
git add src/xml/lv_xml_style.h src/xml/lv_xml_style.c src/xml/lv_xml_component.c tests/cases/test_style.c
git commit -m "feat(style): scale declared transition durations globally"
```

---

### Task 6: Wire the scale to the animations preference

**Files:**
- Modify: `src/application/application.cpp`
- Modify: `lib/helix-xml` (submodule pin bump in the parent repo)
- Test: `tests/unit/test_xml_transition_pref.cpp` (create)

**Interfaces:**
- Consumes: `lv_xml_set_transition_scale` (Task 5), `helix::ui::animations_pref_subject` from `include/ui_animations_pref.h`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_xml_transition_pref.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "helix_test_fixture.h"
#include "ui_animations_pref.h"

extern "C" {
#include "xml/lv_xml_style.h"
}

TEST_CASE_METHOD(LVGLTestFixture, "animations pref drives the XML transition scale", "[xml][transition]")
{
    lv_subject_t* pref = helix::ui::animations_pref_subject(nullptr);
    REQUIRE(pref != nullptr);

    /* lv_subject_set_int notifies observers synchronously on the calling
     * thread, so no timer pump is needed here. */
    lv_subject_set_int(pref, 1);
    REQUIRE(lv_xml_get_transition_scale() == 256);

    lv_subject_set_int(pref, 0);
    REQUIRE(lv_xml_get_transition_scale() == 0);
}
```

`HelixTestFixture::reset_all` forces the pref to 0, so the test sets it explicitly in both directions rather than assuming a starting value. If the wiring in Step 3 marshals through `UpdateQueue` instead of observing directly, this test needs a drain; wire it as a direct main-thread observer and it does not.

- [ ] **Step 2: Run and confirm it fails**

Run: `make test && ./build/bin/helix-tests "[xml][transition]"`
Expected: FAIL, scale stays 256 when the pref goes to 0.

- [ ] **Step 3: Implement the wiring**

In `src/application/application.cpp`, after XML components are registered and the display settings subjects exist, observe the preference and push the scale. Use the project's observer factory rather than a raw callback, and honor the `ui_animations_pref.h` convention that a null subject means animations are on.

- [ ] **Step 4: Run the test**

Run: `./build/bin/helix-tests "[xml][transition]"`
Expected: PASS.

- [ ] **Step 5: Bump the submodule pin and commit**

```bash
git add lib/helix-xml src/application/application.cpp tests/unit/test_xml_transition_pref.cpp
git commit -m "feat(ui): drive XML transition scale from the animations preference"
```

---

### Task 7: Animate the nav bar icons

**Files:**
- Modify: `ui_xml/navigation_bar.xml`
- Test: `tests/unit/test_nav_icon_size_ladder.cpp` (extend)

**Interfaces:**
- Consumes: the `transition` attribute (Task 4), the scale wiring (Task 6).

- [ ] **Step 1: Overlay each icon pair**

Add `floating="true"` to both icons in every pair. Without it the flex column gives both children space the moment both are visible, and the motion is a layout reflow rather than a crossfade.

- [ ] **Step 2: Replace the flag bindings with a state binding**

For the home button, replace the two `bind_flag_if_*` children with a state binding on each icon, so both icons stay visible and opacity carries the swap:

```xml
<icon name="nav_icon_home_active" src="home" size="#icon_size" variant="primary" floating="true">
  <bind_state_if_eq subject="active_panel" state="checked" ref_value="0"/>
</icon>
<icon name="nav_icon_home_inactive" src="home_outline" size="#icon_size_nav_inactive" variant="secondary" floating="true">
  <bind_state_if_not_eq subject="active_panel" state="checked" ref_value="0"/>
</icon>
```

Repeat per button with that button's panel index. The controls button keeps its existing disabled icon and its `nav_controls_icons` wrapper.

- [ ] **Step 3: Declare the styles**

In the same file's `<styles>` block:

```xml
<style name="nav_icon_fade"
       text_opa="0" transform_scale_x="205" transform_scale_y="205"
       transform_pivot_x="50%" transform_pivot_y="50%"
       transition="text_opa|transform_scale_x|transform_scale_y 180ms ease_out"/>
<style name="nav_icon_fade_on" selector="checked"
       text_opa="255" transform_scale_x="256" transform_scale_y="256"/>
```

The `transition` sits on the base style because only the state being entered is scanned. The checked style sets the properties on the icon itself, because equal endpoints are a silent no-op and `text_opa` is inheritable.

- [ ] **Step 4: Verify at runtime, both orientations**

```bash
export HELIX_SOCK=/tmp/helix-navtrans.sock HELIX_CONFIG_DIR=/tmp/helix-config-navtrans
mkdir -p "$HELIX_CONFIG_DIR"
SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv -s medium --remote-socket "$HELIX_SOCK" &
./build/bin/helix-screen ctl --socket "$HELIX_SOCK" navigate home
./build/bin/helix-screen ctl --socket "$HELIX_SOCK" geom nav_icon_home_active
./build/bin/helix-screen ctl --socket "$HELIX_SOCK" navigate settings
./build/bin/helix-screen ctl --socket "$HELIX_SOCK" geom nav_icon_home_inactive
```

Both icons must now resolve in both panels, since neither is hidden any more. Repeat with `-s 480x800`.

- [ ] **Step 5: Extend the ladder test**

`tests/unit/test_nav_icon_size_ladder.cpp` pins the active/inactive size relationship. Add a case asserting both icons of a pair exist simultaneously and that the checked one carries the higher `text_opa`, so a regression that returns to hidden-flag swapping fails here.

- [ ] **Step 6: Prove the tests can fail**

Hand-mutate: set the checked style's `text_opa` to `0` and confirm the new case goes red; revert. `make mutate-diff` is blind on XML data files, so this is done by hand and the mutation is named in the commit body.

- [ ] **Step 7: Commit**

```bash
git add ui_xml/navigation_bar.xml tests/unit/test_nav_icon_size_ladder.cpp
git commit -m "feat(ui): crossfade the nav bar icons between active and inactive"
```

---

## Self-Review

**Spec coverage.** Shorthand and longhand: Tasks 3 and 4. Whitelist validation: Task 1, enforced in Task 3. dsc ownership, free-on-overwrite and scope teardown: Task 2. Global scale: Task 5, wired in Task 6. Nav bar consumer: Task 7. Docs: Task 4. Non-goals are untouched: no per-property durations, no `<timeline>` work, no font-size animation, no change to the controls button's disabled state.

**Deviation from the spec, deliberate.** The spec calls for "a registry of the dscs it allocated". Task 5 walks the existing scope lists instead, which reaches the same descriptors without a second structure to keep in sync. It also covers `pending_free_scope_ll`, which a naive registry built at allocation time would have covered only by accident.

**Type consistency.** `lv_xml_style_prop_anim_type()` and `lv_xml_style_prop_anim_type_t` are named identically in Tasks 1 and 3. `lv_xml_style_transition_clear()` is defined in Task 2 and called in Tasks 2, 3 and 5. `trans_dsc` / `trans_props` / `trans_authored_time` are introduced in Task 2 and used unchanged thereafter. `d->time`, `d->delay`, `d->path_xcb` and `d->props` match `lv_style_transition_dsc_t`.

**Verified while writing, so the executor does not have to.** `lv_xml_atoi("200ms")` returns 200: the digit loop in `lib/helix-xml/src/xml/lv_xml_utils.c#lv_xml_atoi_split` breaks on the first non-digit, so the `ms` suffix needs no special handling. `tests/helix_test_fixture.h` exposes no pump helper, and none is needed, because LVGL subject observers notify synchronously.
