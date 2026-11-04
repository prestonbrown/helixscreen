// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "default_layout_resolver.h"

#include "json_utils.h"
#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <set>

namespace helix::layout {

using json = nlohmann::json;
namespace ju = helix::json_util;

int CapabilitySnapshot::get(const std::string& name) const {
    auto it = subjects.find(name);
    return it == subjects.end() ? 0 : it->second;
}

const BreakpointChain& breakpoint_fallback_chain(UiBreakpoint breakpoint) {
    static constexpr BreakpointChain kChains[] = {
        {"micro", "tiny", "small"},     // bp=0 Micro
        {"tiny", "small", nullptr},     // bp=1 Tiny
        {"small", nullptr},             // bp=2 Small
        {"medium", nullptr},            // bp=3 Medium
        {"large", nullptr},             // bp=4 Large
        {"xlarge", "large", nullptr},   // bp=5 XLarge
        {"xxlarge", "xlarge", "large"}, // bp=6 XXLarge
    };
    static_assert(std::size(kChains) == to_int(UiBreakpoint::XXLarge) + 1,
                  "kChains must cover every UiBreakpoint tier");
    static constexpr BreakpointChain kNoChain{};

    const int bp_idx = to_int(breakpoint);
    if (bp_idx < 0 || bp_idx >= static_cast<int>(std::size(kChains))) {
        return kNoChain;
    }
    return kChains[bp_idx];
}

std::string choose_placement_key(const json& by_bp, UiBreakpoint breakpoint, int grid_cols,
                                 int grid_rows) {
    const bool have_grid = grid_cols > 0 && grid_rows > 0;
    const std::string suffix =
        have_grid ? "@" + std::to_string(grid_cols) + "x" + std::to_string(grid_rows)
                  : std::string{};

    for (const char* name : breakpoint_fallback_chain(breakpoint)) {
        if (!name) {
            break;
        }
        const std::string tier = name;
        if (have_grid && by_bp.contains(tier + suffix)) {
            return tier + suffix;
        }
        if (by_bp.contains(tier)) {
            return tier;
        }
    }
    return {};
}

namespace {

/// The array member @p key of @p j, or nullptr.
const json* array_member(const json& j, const char* key) {
    const json* v = ju::find_member(j, key);
    return v && v->is_array() ? v : nullptr;
}

/// An anchor or slot entry's identity for overlay matching: "id:<id>" or
/// "slot:<name>", or "" for an entry that has neither.
std::string entry_key(const json& entry) {
    const std::string slot = ju::safe_string(entry, "slot");
    if (!slot.empty()) {
        return "slot:" + slot;
    }
    const std::string id = ju::safe_string(entry, "id");
    return id.empty() ? std::string{} : "id:" + id;
}

/// Overlay entries replace the base entry with the same id or slot, are
/// appended when the base has none, and `"remove": true` deletes the match.
void merge_entries(json& base, const json& overlay) {
    if (!base.is_array()) {
        base = json::array();
    }
    for (const auto& entry : overlay) {
        const std::string key = entry_key(entry);
        if (key.empty()) {
            continue;
        }
        auto it = base.begin();
        while (it != base.end() && entry_key(*it) != key) {
            ++it;
        }
        if (ju::safe_bool(entry, "remove")) {
            if (it != base.end()) {
                base.erase(it);
            }
        } else if (it != base.end()) {
            *it = entry;
        } else {
            base.push_back(entry);
        }
    }
}

/// Per-key replacement of an object member: overlay `disabled` tiers and
/// `candidate_lists` names win over the base's.
void merge_object_member(json& base, const json& overlay, const char* key) {
    const json* o = ju::find_member(overlay, key);
    if (!o || !o->is_object()) {
        return;
    }
    json& b = base[key];
    if (!b.is_object()) {
        b = json::object();
    }
    for (auto it = o->begin(); it != o->end(); ++it) {
        b[it.key()] = it.value();
    }
}

json apply_overlay(const json& table, const json& overlay) {
    json doc = table;
    if (const json* a = array_member(overlay, "anchors")) {
        merge_entries(doc["anchors"], *a);
    }
    merge_object_member(doc, overlay, "disabled");
    merge_object_member(doc, overlay, "candidate_lists");

    const json* ov = ju::find_member(overlay, "variants");
    if (!ov || !ov->is_object()) {
        return doc;
    }
    json& variants = doc["variants"];
    if (!variants.is_object()) {
        variants = json::object();
    }
    for (auto it = ov->begin(); it != ov->end(); ++it) {
        json& v = variants[it.key()];
        // A bare-array variant is the anchors-only form of the object form.
        if (v.is_array()) {
            v = json{{"anchors", v}};
        } else if (!v.is_object()) {
            v = json::object();
        }
        const json& o = it.value();
        if (o.is_array()) {
            merge_entries(v["anchors"], o);
        } else if (o.is_object()) {
            if (const json* a = array_member(o, "anchors")) {
                merge_entries(v["anchors"], *a);
            }
            merge_object_member(v, o, "disabled");
        }
    }
    return doc;
}

/// One `[col, row, colspan, rowspan]` rectangle, or false when malformed.
bool read_rect(const json& at, int out[4]) {
    if (!at.is_array() || at.size() != 4) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!at[i].is_number_integer()) {
            return false;
        }
        out[i] = ju::as_int(at[i]);
    }
    return out[0] >= 0 && out[1] >= 0 && out[2] >= 1 && out[3] >= 1;
}

struct Candidate {
    std::string id;
    std::vector<std::string> requires_;
};

Candidate read_candidate(const json& c) {
    Candidate out;
    if (c.is_string()) {
        out.id = c.get<std::string>();
        return out;
    }
    out.id = ju::safe_string(c, "id");
    if (const json* req = array_member(c, "requires")) {
        for (const auto& r : *req) {
            if (r.is_string()) {
                out.requires_.push_back(r.get<std::string>());
            }
        }
    }
    return out;
}

/// A cell's candidate list: inline, or named in the table's `candidate_lists`.
const json* cell_candidates(const json& cell, const json& doc) {
    const json* c = ju::find_member(cell, "candidates");
    if (!c) {
        return nullptr;
    }
    if (c->is_array()) {
        return c;
    }
    if (c->is_string()) {
        const json* lists = ju::find_member(doc, "candidate_lists");
        const json* named =
            lists ? ju::find_member(*lists, c->get_ref<const std::string&>().c_str()) : nullptr;
        return named && named->is_array() ? named : nullptr;
    }
    return nullptr;
}

class Resolver {
  public:
    Resolver(const json& doc, UiBreakpoint bp, int cols, int rows, const CapabilitySnapshot& caps)
        : doc_(doc), bp_(bp), cols_(cols), rows_(rows), caps_(caps) {}

    void collect_tier_disabled(const json* owner) {
        const json* d = owner ? ju::find_member(*owner, "disabled") : nullptr;
        if (!d || !d->is_object()) {
            return;
        }
        const std::string key = choose_placement_key(*d, bp_, cols_, rows_);
        const json* ids = key.empty() ? nullptr : array_member(*d, key.c_str());
        if (!ids) {
            return;
        }
        for (const auto& id : *ids) {
            if (id.is_string() && find_widget_def(id.get_ref<const std::string&>())) {
                tier_disabled_.insert(id.get<std::string>());
            }
        }
    }

    void place_anchor(const json& anchor) {
        const std::string id = ju::safe_string(anchor, "id");
        const auto* def = id.empty() ? nullptr : find_widget_def(id);
        if (!def || placed_ids_.count(id) || tier_disabled_.count(id)) {
            return;
        }
        const json* placements = ju::find_member(anchor, "placements");
        if (!placements || !placements->is_object()) {
            return;
        }
        const std::string key = choose_placement_key(*placements, bp_, cols_, rows_);
        const json* p = key.empty() ? nullptr : ju::find_member(*placements, key.c_str());
        if (!p || !p->is_object()) {
            return;
        }
        // Anchor-level config first, then placement-level so one tier can override.
        json cfg = json::object();
        if (const json* ac = ju::find_member(anchor, "config"); ac && ac->is_object()) {
            cfg = *ac;
        }
        if (const json* pc = ju::find_member(*p, "config"); pc && pc->is_object()) {
            for (auto it = pc->begin(); it != pc->end(); ++it) {
                cfg[it.key()] = it.value();
            }
        }
        place(id, ju::safe_int(*p, "col", 0), ju::safe_int(*p, "row", 0),
              ju::safe_int(*p, "colspan", def->colspan), ju::safe_int(*p, "rowspan", def->rowspan),
              std::move(cfg));
    }

    void fill_slot(const json& slot) {
        const json* alternatives = array_member(slot, "alternatives");
        if (!alternatives) {
            return;
        }
        note_candidates(*alternatives);

        const json* placements = ju::find_member(slot, "placements");
        if (!placements || !placements->is_object()) {
            return;
        }
        const std::string key = choose_placement_key(*placements, bp_, cols_, rows_);
        const json* region = key.empty() ? nullptr : ju::find_member(*placements, key.c_str());
        if (!region || !region->is_object()) {
            return;
        }
        const int rc = ju::safe_int(*region, "col", 0);
        const int rr = ju::safe_int(*region, "row", 0);
        const int rw = ju::safe_int(*region, "colspan", 0);
        const int rh = ju::safe_int(*region, "rowspan", 0);

        // An alternative whose cells run off the measured grid gives way to a
        // later one that stays on it. When none does, the choice falls back to
        // the region alone: the caller auto-places a winner it cannot seat,
        // where resolving nothing would switch every candidate off.
        const bool have_grid = cols_ > 0 && rows_ > 0;
        for (const bool within_grid : {true, false}) {
            if (!within_grid && !have_grid) {
                break;
            }
            if (fill_from_first_qualifying(*alternatives, rc, rr, rw, rh,
                                           within_grid && have_grid)) {
                return;
            }
        }
        // The last alternative wins whenever it fits, so reaching here means
        // none fits the region.
        spdlog::debug("[DefaultLayoutResolver] slot '{}' at {}: no alternative fits region "
                      "{},{} {}x{}",
                      ju::safe_string(slot, "slot"), key, rc, rr, rw, rh);
    }

    std::vector<ResolvedPlacement> take_placed() {
        return std::move(placed_);
    }

    std::vector<std::string> disabled() const {
        std::set<std::string> out = tier_disabled_;
        for (const auto& id : named_) {
            if (!placed_ids_.count(id)) {
                out.insert(id);
            }
        }
        return {out.begin(), out.end()};
    }

    const std::map<std::string, int>& consulted() const {
        return consulted_;
    }

  private:
    int consult(const std::string& name) {
        const int v = caps_.get(name);
        consulted_[name] = v;
        return v;
    }

    /// Placed-ness and the tier list are checked before any subject so a
    /// subject is only consulted when its value can change the outcome.
    bool qualifies(const Candidate& c) {
        const auto* def = c.id.empty() ? nullptr : find_widget_def(c.id);
        if (!def || placed_ids_.count(c.id) || tier_disabled_.count(c.id)) {
            return false;
        }
        if (def->hardware_gate_subject && consult(def->hardware_gate_subject) == 0) {
            return false;
        }
        for (const auto& r : c.requires_) {
            if (consult(r) == 0) {
                return false;
            }
        }
        return true;
    }

    /// True when any candidate in @p list qualifies.
    bool any_qualifies(const json* list) {
        if (!list) {
            return false;
        }
        for (const auto& c : *list) {
            if (qualifies(read_candidate(c))) {
                return true;
            }
        }
        return false;
    }

    /// Fill the region at (@p rc, @p rr) from the first alternative that fits
    /// it, and the grid too when @p check_grid, and whose lead cell has a
    /// qualifying candidate (the last alternative has no lead requirement).
    /// False when none is taken.
    bool fill_from_first_qualifying(const json& alternatives, int rc, int rr, int rw, int rh,
                                    bool check_grid) {
        const size_t n = alternatives.size();
        for (size_t i = 0; i < n; ++i) {
            const json* cells = array_member(alternatives[i], "cells");
            if (!cells || cells->empty() || !fits(*cells, rw, rh) ||
                (check_grid && !fits(*cells, cols_ - rc, rows_ - rr))) {
                continue;
            }
            const bool last = i + 1 == n;
            if (!last && !any_qualifies(cell_candidates((*cells)[0], doc_))) {
                continue;
            }
            for (const auto& cell : *cells) {
                int at[4];
                read_rect(*ju::find_member(cell, "at"), at);
                const json* list = cell_candidates(cell, doc_);
                if (!list) {
                    continue;
                }
                for (const auto& c : *list) {
                    Candidate cand = read_candidate(c);
                    if (qualifies(cand)) {
                        place(cand.id, rc + at[0], rr + at[1], at[2], at[3], json::object());
                        break;
                    }
                }
            }
            return true;
        }
        return false;
    }

    static bool fits(const json& cells, int region_w, int region_h) {
        for (const auto& cell : cells) {
            const json* at = ju::find_member(cell, "at");
            int r[4];
            if (!at || !read_rect(*at, r) || r[0] + r[2] > region_w || r[1] + r[3] > region_h) {
                return false;
            }
        }
        return true;
    }

    void note_candidates(const json& alternatives) {
        for (const auto& alt : alternatives) {
            const json* cells = array_member(alt, "cells");
            if (!cells) {
                continue;
            }
            for (const auto& cell : *cells) {
                const json* list = cell_candidates(cell, doc_);
                if (!list) {
                    continue;
                }
                for (const auto& c : *list) {
                    Candidate cand = read_candidate(c);
                    if (!cand.id.empty() && find_widget_def(cand.id)) {
                        named_.insert(cand.id);
                    }
                }
            }
        }
    }

    void place(const std::string& id, int col, int row, int colspan, int rowspan, json config) {
        placed_.push_back({id, col, row, colspan, rowspan, std::move(config)});
        placed_ids_.insert(id);
    }

    const json& doc_;
    UiBreakpoint bp_;
    int cols_, rows_;
    const CapabilitySnapshot& caps_;
    std::vector<ResolvedPlacement> placed_;
    std::set<std::string> placed_ids_;
    std::set<std::string> tier_disabled_;
    std::set<std::string> named_;
    std::map<std::string, int> consulted_;
};

std::string make_signature(std::string_view tier, int cols, int rows,
                           const std::vector<std::string>& chain, std::string_view preset,
                           const std::map<std::string, int>& consulted) {
    std::string sig(tier);
    sig += '|' + std::to_string(cols) + '|' + std::to_string(rows) + '|';
    for (size_t i = 0; i < chain.size(); ++i) {
        sig += (i ? "," : "") + chain[i];
    }
    sig += '|';
    sig += preset;
    sig += '|';
    bool first = true;
    for (const auto& [name, value] : consulted) {
        sig += (first ? "" : ",") + name + '=' + std::to_string(value);
        first = false;
    }
    return sig;
}

} // namespace

ResolvedDefault resolve_default_layout(const json& table, const json* overlay,
                                       std::string_view tier, int cols, int rows,
                                       const std::vector<std::string>& variant_chain,
                                       std::string_view preset, const CapabilitySnapshot& caps) {
    ResolvedDefault out;
    const int bp = breakpoint_from_name(std::string(tier).c_str());
    if (!table.is_object() || bp < 0) {
        out.signature = make_signature(tier, cols, rows, variant_chain, preset, {});
        return out;
    }

    json merged;
    if (overlay && overlay->is_object()) {
        merged = apply_overlay(table, *overlay);
    }
    const json& doc = merged.is_object() ? merged : table;

    // Most-specific variant with a non-empty anchor list wins; the top-level
    // table is the final fallback. A bare-array variant carries no `disabled`.
    const json* list = array_member(doc, "anchors");
    const json* disabled_owner = &doc;
    std::string variant_used = "base";
    if (const json* variants = ju::find_member(doc, "variants");
        variants && variants->is_object()) {
        for (const auto& dir : variant_chain) {
            const json* v = ju::find_member(*variants, dir.c_str());
            if (!v) {
                continue;
            }
            if (v->is_array() && !v->empty()) {
                list = v;
                disabled_owner = nullptr;
                variant_used = dir;
                break;
            }
            const json* va = v->is_object() ? array_member(*v, "anchors") : nullptr;
            if (!va || va->empty()) {
                continue;
            }
            list = va;
            disabled_owner = v;
            variant_used = dir;
            break;
        }
    }

    Resolver r(doc, as_breakpoint(bp), cols, rows, caps);
    if (list) {
        r.collect_tier_disabled(disabled_owner);
        for (const auto& entry : *list) {
            if (entry.is_object() && !ju::find_member(entry, "slot")) {
                r.place_anchor(entry);
            }
        }
        for (const auto& entry : *list) {
            if (entry.is_object() && ju::find_member(entry, "slot")) {
                r.fill_slot(entry);
            }
        }
    }

    out.placed = r.take_placed();
    out.disabled = r.disabled();
    out.signature = make_signature(tier, cols, rows, variant_chain, preset, r.consulted());
    out.ok = !out.placed.empty();
    spdlog::debug("[DefaultLayoutResolver] tier={} grid={}x{} table={} placed={} disabled={} "
                  "signature={}",
                  tier, cols, rows, variant_used, out.placed.size(), out.disabled.size(),
                  out.signature);
    return out;
}

namespace {

void add_gate(const std::string& id, std::set<std::string>& out) {
    const auto* def = id.empty() ? nullptr : find_widget_def(id);
    if (def && def->hardware_gate_subject) {
        out.insert(def->hardware_gate_subject);
    }
}

void add_candidate_subjects(const json& c, std::set<std::string>& out) {
    const Candidate cand = read_candidate(c);
    add_gate(cand.id, out);
    out.insert(cand.requires_.begin(), cand.requires_.end());
}

void add_entry_list_subjects(const json& list, std::set<std::string>& out) {
    for (const auto& entry : list) {
        if (!entry.is_object()) {
            continue;
        }
        if (!ju::find_member(entry, "slot")) {
            add_gate(ju::safe_string(entry, "id"), out);
            continue;
        }
        const json* alternatives = array_member(entry, "alternatives");
        if (!alternatives) {
            continue;
        }
        for (const auto& alt : *alternatives) {
            const json* cells = array_member(alt, "cells");
            if (!cells) {
                continue;
            }
            for (const auto& cell : *cells) {
                // Named lists are walked once, from `candidate_lists`.
                const json* inline_list = array_member(cell, "candidates");
                if (!inline_list) {
                    continue;
                }
                for (const auto& c : *inline_list) {
                    add_candidate_subjects(c, out);
                }
            }
        }
    }
}

} // namespace

std::vector<std::string> subjects_named_by(const json& table) {
    std::set<std::string> out;
    if (!table.is_object()) {
        return {};
    }
    if (const json* a = array_member(table, "anchors")) {
        add_entry_list_subjects(*a, out);
    }
    if (const json* variants = ju::find_member(table, "variants");
        variants && variants->is_object()) {
        for (const auto& v : *variants) {
            if (v.is_array()) {
                add_entry_list_subjects(v, out);
            } else if (const json* a = array_member(v, "anchors")) {
                add_entry_list_subjects(*a, out);
            }
        }
    }
    if (const json* lists = ju::find_member(table, "candidate_lists");
        lists && lists->is_object()) {
        for (const auto& list : *lists) {
            if (!list.is_array()) {
                continue;
            }
            for (const auto& c : list) {
                add_candidate_subjects(c, out);
            }
        }
    }
    return {out.begin(), out.end()};
}

std::string arrangement_hash(const json& pages_payload) {
    std::string text;
    size_t page_count = 0;
    if (const json* pages = array_member(pages_payload, "pages")) {
        page_count = pages->size();
        for (const auto& page : *pages) {
            text += '[';
            if (const json* widgets = array_member(page, "widgets")) {
                for (const auto& w : *widgets) {
                    const std::string id = ju::safe_string(w, "id");
                    const int col = ju::safe_int(w, "col", -1);
                    const int row = ju::safe_int(w, "row", -1);
                    if (id.empty() || !ju::safe_bool(w, "enabled", true) || col < 0 || row < 0) {
                        continue;
                    }
                    text += id + ',' + std::to_string(col) + ',' + std::to_string(row) + ',' +
                            std::to_string(ju::safe_int(w, "colspan", 0)) + ',' +
                            std::to_string(ju::safe_int(w, "rowspan", 0)) + ';';
                }
            }
            text += ']';
        }
    }
    text += '#' + std::to_string(page_count);

    // FNV-1a 64: stable across platforms and library versions, which a hash
    // stored in settings.json has to be.
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char ch : text) {
        h ^= ch;
        h *= 0x100000001b3ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

} // namespace helix::layout
