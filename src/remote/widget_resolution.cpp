// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "widget_resolution.h"

#include "lvgl.h"

// lv_event_dsc_t's filter is not exposed by the public API, and
// has_own_click_handler() needs it to tell a click target from scaffolding.
#include "lvgl/src/misc/lv_event_private.h"

#include <cstdlib>

namespace helix {

bool is_value_control(lv_obj_t* o) {
    return lv_obj_check_type(o, &lv_switch_class) || lv_obj_check_type(o, &lv_checkbox_class) ||
           lv_obj_check_type(o, &lv_slider_class) || lv_obj_check_type(o, &lv_arc_class) ||
           lv_obj_check_type(o, &lv_dropdown_class) || lv_obj_check_type(o, &lv_textarea_class);
}

bool has_own_click_handler(lv_obj_t* o) {
    if (!o) {
        return false;
    }
    // Mirrors the dispatch test in lv_event.c's event_send_core: mask off the
    // PREPROCESS flag, LV_EVENT_ALL matches everything, and entries already
    // marked for deletion will never fire.
    uint32_t count = lv_obj_get_event_count(o);
    for (uint32_t i = 0; i < count; ++i) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(o, i);
        if (!dsc || (dsc->filter & LV_EVENT_MARKED_DELETING) != 0) {
            continue;
        }
        uint32_t filter = dsc->filter & ~(LV_EVENT_PREPROCESS | LV_EVENT_MARKED_DELETING);
        if (filter == LV_EVENT_ALL || filter == LV_EVENT_CLICKED) {
            return true;
        }
    }
    return false;
}

namespace {

// Collect visible value-controls in a subtree (excluding the root itself).
// Hidden subtrees are skipped for the same reason describe_screen skips them:
// they are not on screen, so they are not targets.
//
// Recursion stops at any descendant that is itself a click target. Such a
// child is a destination in its own right, not scaffolding wrapping the
// control the caller meant, so tunnelling past it would resolve a click onto
// something the caller never addressed (#1179).
void collect_value_controls(lv_obj_t* parent, std::vector<lv_obj_t*>& out) {
    if (!parent) {
        return;
    }
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(parent, i);
        if (!child || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        if (is_value_control(child)) {
            out.push_back(child);
            continue; // a control's own internals are never a separate target
        }
        if (has_own_click_handler(child)) {
            continue;
        }
        collect_value_controls(child, out);
    }
}

} // namespace

// `click row_foo` on a composite settings row used to send CLICKED to the row
// container — which does nothing, because the control the user meant is the
// switch nested inside it. Prefer a value-control descendant when the target
// itself isn't one; fall back to the literal target (a category row that opens
// an overlay is clickable and has no value-control, and must stay that way).
//
// Descent is only ever a repair for a target that does nothing on its own. A
// target with its own click handler is already the thing the caller addressed,
// so it is acted on literally — this is what keeps `click <backdrop>` a
// dismissal instead of resolving into whatever the overlay happens to contain
// (#1179).
lv_obj_t* resolve_actionable(lv_obj_t* target, lv_obj_t** descended_to,
                             std::vector<lv_obj_t*>* ambiguous) {
    *descended_to = nullptr;
    if (!target || is_value_control(target) || has_own_click_handler(target)) {
        return target;
    }
    std::vector<lv_obj_t*> found;
    collect_value_controls(target, found);
    if (found.size() == 1) {
        *descended_to = found[0];
        return found[0];
    }
    if (found.size() > 1 && ambiguous) {
        *ambiguous = found;
    }
    // Zero candidates, or too many to choose between: act on the target itself
    // if it is clickable at all, so buttons and overlay-opening rows are
    // unaffected by this resolution step.
    return target;
}

// --- readable path locators ---------------------------------------------

bool parse_indexed_name(const std::string& token, std::string& out_name, int& out_index) {
    if (token.size() < 4 || token.back() != ']') { // "a[0]" is the shortest form
        return false;
    }
    const size_t open = token.rfind('[');
    if (open == std::string::npos || open == 0) {
        return false; // no '[' at all, or nothing before it to name
    }
    const size_t first = open + 1;
    const size_t last = token.size() - 1; // one past the final digit
    if (last <= first || last - first > 9) {
        return false; // "[]", or more digits than any child count can need
    }
    for (size_t i = first; i < last; ++i) {
        if (token[i] < '0' || token[i] > '9') {
            return false; // "[x]", "[-1]"
        }
    }
    out_name = token.substr(0, open);
    out_index = std::atoi(token.c_str() + first);
    return true;
}

namespace {

// The name a widget carries in its own right, resolved through LVGL's '#'
// auto-indexing. Empty when the widget is nameless: lv_obj_get_name_resolved()
// invents a "<class>_#" name for those, so the RAW name is what decides whether
// there is anything addressable here.
//
// A name that would be read back as something else is treated as no name at
// all, and the segment falls back to a child index. That covers an all-digit
// name (indistinguishable from an index) and any name carrying the path's own
// punctuation. Nothing in the tree is currently named that way; the fallback is
// what keeps that from silently mis-addressing a widget if something ever is.
std::string own_name(lv_obj_t* o) {
    const char* raw = lv_obj_get_name(o);
    if (!raw || raw[0] == '\0') {
        return {};
    }
    char resolved[128];
    lv_obj_get_name_resolved(o, resolved, sizeof(resolved));
    std::string name = resolved[0] != '\0' ? resolved : raw;
    if (name.find_first_of("/[]") != std::string::npos) {
        return {};
    }
    if (name.find_first_not_of("0123456789") == std::string::npos) {
        return {};
    }
    return name;
}

bool all_digits(const std::string& s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

// Same-named children of @p parent, in child order.
std::vector<lv_obj_t*> named_children(lv_obj_t* parent, const std::string& name) {
    std::vector<lv_obj_t*> out;
    const uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(parent, i);
        if (child && own_name(child) == name) {
            out.push_back(child);
        }
    }
    return out;
}

// How one widget is addressed within its parent.
std::string segment_for(lv_obj_t* o) {
    const std::string name = own_name(o);
    if (name.empty()) {
        return std::to_string(lv_obj_get_index(o));
    }
    const std::vector<lv_obj_t*> same = named_children(lv_obj_get_parent(o), name);
    if (same.size() <= 1) {
        return name;
    }
    for (size_t i = 0; i < same.size(); ++i) {
        if (same[i] == o) {
            return name + "[" + std::to_string(i) + "]";
        }
    }
    return name; // unreachable: o is one of its own parent's children
}

// Split on '/', rejecting the whole locator if any segment is empty — "s//1"
// and a trailing slash are typos, and silently ignoring them would resolve to
// a widget the caller did not address.
std::vector<std::string> split_segments(const std::string& path) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        const std::string seg =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg.empty()) {
            return {};
        }
        out.push_back(seg);
        if (slash == std::string::npos) {
            return out;
        }
        start = slash + 1;
    }
}

// Resolve one segment against a parent. Hidden children are deliberately NOT
// skipped: a path is exact addressing, and path_of() must round-trip for a
// widget that happens to be hidden. (`ls` filters hidden subtrees when it
// *lists*; that is a different question from whether a locator resolves.)
lv_obj_t* resolve_segment(lv_obj_t* parent, const std::string& seg,
                          std::vector<lv_obj_t*>* ambiguous) {
    if (!parent || seg.empty()) {
        return nullptr;
    }
    if (all_digits(seg)) {
        const unsigned long idx = std::strtoul(seg.c_str(), nullptr, 10);
        if (idx >= lv_obj_get_child_count(parent)) {
            return nullptr;
        }
        return lv_obj_get_child(parent, static_cast<uint32_t>(idx));
    }

    std::string name;
    int wanted = -1;
    const bool indexed = parse_indexed_name(seg, name, wanted);
    if (!indexed) {
        name = seg;
    }

    const std::vector<lv_obj_t*> matches = named_children(parent, name);
    if (indexed) {
        if (wanted < 0 || static_cast<size_t>(wanted) >= matches.size()) {
            return nullptr;
        }
        return matches[static_cast<size_t>(wanted)];
    }
    if (matches.size() == 1) {
        return matches[0];
    }
    if (matches.size() > 1 && ambiguous) {
        *ambiguous = matches;
    }
    return nullptr;
}

} // namespace

std::string path_segment_for(lv_obj_t* o) {
    return segment_for(o);
}

std::string path_of(lv_obj_t* o, lv_obj_t* base) {
    if (!o || o == base) {
        return {};
    }
    std::string suffix;
    lv_obj_t* cur = o;
    while (true) {
        lv_obj_t* parent = lv_obj_get_parent(cur);
        if (!parent) {
            // cur is a screen root: the active screen ("s") or the top layer
            // ("t"). Reaching here with a base set means the base was not an
            // ancestor, so an absolute locator is the honest answer.
            return (cur == lv_layer_top() ? "t" : "s") + suffix;
        }
        suffix = "/" + segment_for(cur) + suffix;
        cur = parent;
        if (base && cur == base) {
            return suffix.substr(1); // relative: drop the leading '/'
        }
    }
}

lv_obj_t* resolve_path(const std::string& path, lv_obj_t* base, std::vector<lv_obj_t*>* ambiguous) {
    if (ambiguous) {
        ambiguous->clear();
    }
    const std::vector<std::string> segs = split_segments(path);
    if (segs.empty()) {
        return nullptr;
    }

    lv_obj_t* cur = nullptr;
    size_t i = 0;
    if (segs[0] == "s" || segs[0] == "t") {
        cur = (segs[0] == "t") ? lv_layer_top() : lv_screen_active();
        i = 1;
    } else {
        cur = base ? base : lv_screen_active();
    }

    for (; i < segs.size() && cur; ++i) {
        cur = resolve_segment(cur, segs[i], ambiguous);
    }
    return cur;
}

int64_t widget_pick_key(lv_obj_t* o, size_t discovery_order) {
    if (!o) {
        return -1;
    }
    lv_obj_t* top_ancestor = o;
    while (lv_obj_t* parent = lv_obj_get_parent(top_ancestor)) {
        if (!lv_obj_get_parent(parent)) {
            break; // parent is the screen/layer root; top_ancestor is its child
        }
        top_ancestor = parent;
    }
    lv_obj_t* root = lv_obj_get_parent(top_ancestor);
    const int64_t layer_rank = (root == lv_layer_top()) ? 1 : 0;
    // int64_t throughout, not long: see the width note on the declaration.
    return (layer_rank << 40) | (static_cast<int64_t>(lv_obj_get_index(top_ancestor)) << 20) |
           static_cast<int64_t>(discovery_order);
}

lv_obj_t* topmost_visible(const std::vector<lv_obj_t*>& matches) {
    lv_obj_t* best = nullptr;
    int64_t best_key = -1;
    for (size_t i = 0; i < matches.size(); ++i) {
        const int64_t key = widget_pick_key(matches[i], i);
        if (key > best_key) {
            best_key = key;
            best = matches[i];
        }
    }
    return best;
}

} // namespace helix
