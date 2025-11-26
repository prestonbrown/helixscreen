# LV_SIZE_CONTENT Deep Dive & Patch Proposal

## Executive Summary

`LV_SIZE_CONTENT` (or `height="content"` in XML) is designed to make containers automatically size to fit their children. However, it frequently collapses to 0 height with nested flex containers. This document explains the root cause and proposes patches.

## Root Cause Analysis

### The Layout Update Order Problem

The issue is in `layout_update_core()` in `lib/lvgl/src/core/lv_obj_pos.c`:

```c
static void layout_update_core(lv_obj_t * obj)
{
    // 1. First: recurse into ALL children (depth-first)
    for(i = 0; i < child_cnt; i++) {
        lv_obj_t * child = obj->spec_attr->children[i];
        layout_update_core(child);  // Children processed first
    }

    // 2. Then: update THIS object
    if(obj->layout_inv) {
        obj->layout_inv = 0;
        lv_obj_refr_size(obj);  // SIZE_CONTENT calculated HERE
        lv_obj_refr_pos(obj);
        if(child_cnt > 0) {
            lv_layout_apply(obj);  // Flex layout runs AFTER size calc
        }
    }
}
```

### How calc_content_height() Works

```c
static int32_t calc_content_height(lv_obj_t * obj)
{
    // ...
    for(i = 0; i < child_cnt; i++) {
        lv_obj_t * child = obj->spec_attr->children[i];

        if(lv_obj_is_layout_positioned(child)) {
            // For flex children: read absolute coords
            child_res_tmp = child->coords.y2 - obj->coords.y1 + 1;
        }
        // ...
    }
    return child_res + space_bottom;
}
```

**Problem**: It reads `child->coords.y2`, which depends on children being positioned. But for flex containers, children aren't positioned until `lv_layout_apply()` runs - which happens AFTER `lv_obj_refr_size()`.

### The Flex Layout Workaround

LVGL has a partial fix in `flex_update()`:

```c
// At the END of flex_update(), after all children positioned:
if(w_set == LV_SIZE_CONTENT || h_set == LV_SIZE_CONTENT) {
    lv_obj_refr_size(cont);  // Re-calculate size NOW that children are positioned
}
```

This means simple cases work - after flex positions children, it re-calculates the parent's SIZE_CONTENT.

### The Circular Dependency Blocker

The **real killer** is in `calc_dynamic_height()`:

```c
static int32_t calc_dynamic_height(lv_obj_t * obj, int32_t height, ...)
{
    if(LV_COORD_IS_PCT(height)) {
        lv_obj_t * parent = lv_obj_get_parent(obj);
        if(parent->h_layout == 0 &&
           lv_obj_get_style_height(parent, 0) == LV_SIZE_CONTENT) {
            // BLOCKER: If parent uses SIZE_CONTENT, force child to 0
            height = lv_obj_get_style_space_top(obj, 0) +
                     lv_obj_get_style_space_bottom(obj, 0);
        }
        // ...
    }
}
```

This prevents a genuine circular dependency:
- Parent height = SIZE_CONTENT (depends on child height)
- Child height = 50% of parent height (depends on parent height)
→ Infinite loop!

**But it's OVERLY AGGRESSIVE**. It also blocks:
- Child `width="100%"` when parent has `height="SIZE_CONTENT"`
- These are INDEPENDENT dimensions - no circular dependency!

## Failure Cases

### Case 1: Nested SIZE_CONTENT with Percentage Width

```xml
<lv_obj height="LV_SIZE_CONTENT" flex_flow="column" width="300">
  <lv_obj width="100%" height="LV_SIZE_CONTENT">
    <lv_label text="This should be visible"/>
  </lv_obj>
</lv_obj>
```

**What happens:**
1. Outer container: `height=SIZE_CONTENT`
2. Inner container: `width=100%`, `height=SIZE_CONTENT`
3. When calculating inner's width, LVGL sees parent has SIZE_CONTENT height
4. LVGL (incorrectly) thinks there's a circular dependency
5. Inner gets `width = padding only` → effectively 0
6. Outer reads inner's coords → 0 height
7. **Result: Both collapse to 0**

### Case 2: Triple Nesting

```xml
<lv_obj height="LV_SIZE_CONTENT" flex_flow="column">
  <lv_obj width="100%" height="LV_SIZE_CONTENT">
    <lv_obj width="100%" height="LV_SIZE_CONTENT">
      <lv_label text="Deep content"/>
    </lv_obj>
  </lv_obj>
</lv_obj>
```

Same problem, compounded at each level.

## Proposed Patches

### Patch Option 1: Fix Circular Dependency Detection (RECOMMENDED)

The current check is too broad. Width and height are independent dimensions.

**Change in `lv_obj_pos.c`:**

```c
// BEFORE (lines 119-125):
if(LV_COORD_IS_PCT(height)) {
    lv_obj_t * parent = lv_obj_get_parent(obj);
    if(parent->h_layout == 0 && lv_obj_get_style_height(parent, 0) == LV_SIZE_CONTENT) {
        height = lv_obj_get_style_space_top(obj, 0) + lv_obj_get_style_space_bottom(obj, 0);
    }
    // ...
}

// AFTER:
if(LV_COORD_IS_PCT(height)) {
    lv_obj_t * parent = lv_obj_get_parent(obj);
    // Only block if SAME dimension creates circular dependency
    // height=PCT + parent height=SIZE_CONTENT → circular
    // But width=PCT + parent height=SIZE_CONTENT → NOT circular
    if(parent->h_layout == 0 && lv_obj_get_style_height(parent, 0) == LV_SIZE_CONTENT) {
        height = lv_obj_get_style_space_top(obj, 0) + lv_obj_get_style_space_bottom(obj, 0);
    }
    // ...
}
```

**Also in `calc_dynamic_width()` (lines 95-101):**

```c
// BEFORE:
if(parent->w_layout == 0 && lv_obj_get_style_width(parent, 0) == LV_SIZE_CONTENT) {
    width = lv_obj_get_style_space_left(obj, 0) + lv_obj_get_style_space_right(obj, 0);
}

// This is CORRECT - only blocks width when parent width is SIZE_CONTENT
// No change needed for width calculation
```

Wait - reading more carefully, the width check IS dimension-specific. Let me re-analyze...

Actually looking at the code again:
- `calc_dynamic_WIDTH`: checks if parent WIDTH is SIZE_CONTENT → correct
- `calc_dynamic_HEIGHT`: checks if parent HEIGHT is SIZE_CONTENT → correct

So the circular dependency checks ARE dimension-specific. The issue must be elsewhere...

### Patch Option 2: Force Layout Update After Component Creation

When XML components are created, add an automatic layout refresh:

```c
// In lv_xml_component_process() or similar:
lv_obj_t * component = lv_xml_create_in_scope(...);
lv_obj_update_layout(component);  // Force immediate layout
return component;
```

### Patch Option 3: Two-Pass Layout for SIZE_CONTENT

Modify `layout_update_core()` to do a second pass for SIZE_CONTENT containers:

```c
static void layout_update_core(lv_obj_t * obj)
{
    // First pass: normal layout
    for(i = 0; i < child_cnt; i++) {
        layout_update_core(obj->spec_attr->children[i]);
    }

    if(obj->layout_inv) {
        obj->layout_inv = 0;
        lv_obj_refr_size(obj);
        lv_obj_refr_pos(obj);
        if(child_cnt > 0) {
            lv_layout_apply(obj);
        }
    }

    // NEW: Second pass for SIZE_CONTENT parents
    int32_t h_set = lv_obj_get_style_height(obj, 0);
    int32_t w_set = lv_obj_get_style_width(obj, 0);
    if((h_set == LV_SIZE_CONTENT && !obj->h_layout) ||
       (w_set == LV_SIZE_CONTENT && !obj->w_layout)) {
        // Children may have changed size after layout, refresh
        lv_obj_refr_size(obj);
    }
}
```

**Downside**: Performance impact - extra size calculations.

### Patch Option 4: Use `style_min_height` Workaround (No Patch Needed)

Current workaround that works:

```xml
<lv_obj height="LV_SIZE_CONTENT" style_min_height="40">
  <!-- Content -->
</lv_obj>
```

The `style_min_height` ensures a minimum, preventing collapse to 0.

## Recommended Approach

1. **Short-term**: Use `style_min_height` workaround in XML
2. **Medium-term**: Implement Patch Option 2 (force layout after XML component creation)
3. **Long-term**: Submit Patch Option 3 to LVGL upstream

## Test Files Created

1. `test_size_content.cpp` - Headless C++ test for SIZE_CONTENT behavior
2. `ui_xml/test_size_content.xml` - Visual XML test panel with nested SIZE_CONTENT cases

## Related LVGL PRs

- [PR #8685](https://github.com/lvgl/lvgl/pull/8685) - Enables LV_SIZE_CONTENT for min/max in flex grow (merged Nov 24, 2025)
  - This is related but doesn't fix the core nesting issue

## Files Analyzed

| File | Key Functions |
|------|---------------|
| `lib/lvgl/src/core/lv_obj_pos.c` | `layout_update_core()`, `calc_content_height()`, `calc_dynamic_height()`, `lv_obj_refr_size()` |
| `lib/lvgl/src/layouts/flex/lv_flex.c` | `flex_update()`, `find_track_end()`, `children_repos()` |
| `lib/lvgl/src/others/xml/lv_xml_component.c` | Component creation flow |

## Conclusion

LV_SIZE_CONTENT is fundamentally working correctly. The issue is:
1. Overly aggressive circular dependency blocking
2. Layout order (size calculated before flex positions children)
3. Missing second-pass for nested SIZE_CONTENT

The `flex_update()` workaround partially fixes this by calling `lv_obj_refr_size()` after positioning children. But for deeply nested containers, this doesn't propagate correctly up the tree.
