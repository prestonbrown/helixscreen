// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_SNAPMAKER

#include "multiace_status.h"

#include "ace_slot_parse.h"
#include "core/text_io.h"
#include "lane_translation.h"
#include "printer_discovery.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace helix::multiace {

using json = nlohmann::json;

namespace {

const json* field(const json& obj, const char* key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

std::string str_or_empty(const json& obj, const char* key) {
    const json* v = field(obj, key);
    return (v && v->is_string()) ? v->get<std::string>() : std::string{};
}

std::optional<int> int_of(const json* v) {
    if (v && v->is_number_integer()) {
        return v->get<int>();
    }
    return std::nullopt;
}

std::optional<float> float_of(const json* v) {
    if (v && v->is_number()) {
        return v->get<float>();
    }
    return std::nullopt;
}

std::optional<bool> bool_of(const json* v) {
    if (v && v->is_boolean()) {
        return v->get<bool>();
    }
    return std::nullopt;
}

/// The head a per-head dict key names: exactly "0".."3".
std::optional<int> head_key(const std::string& key) {
    if (key.size() != 1 || key[0] < '0' || key[0] >= '0' + kHeadCount) {
        return std::nullopt;
    }
    return key[0] - '0';
}

/// A per-head dict of @p T, read entry by entry. A key that names no head, or
/// a value of the wrong type, leaves that head at @p fallback(h).
template <typename T, typename Read, typename Fallback>
std::optional<std::array<T, kHeadCount>> per_head(const json& ace, const char* key, Read read,
                                                  Fallback fallback) {
    const json* dict = field(ace, key);
    if (!dict || !dict->is_object()) {
        return std::nullopt;
    }
    std::array<T, kHeadCount> out{};
    for (int h = 0; h < kHeadCount; ++h) {
        out[static_cast<size_t>(h)] = fallback(h);
    }
    for (auto it = dict->begin(); it != dict->end(); ++it) {
        const auto h = head_key(it.key());
        if (!h) {
            continue;
        }
        if (auto value = read(it.value())) {
            out[static_cast<size_t>(*h)] = *value;
        }
    }
    return out;
}

Mode mode_from_string(const std::string& s) {
    if (s == "normal") {
        return Mode::NORMAL;
    }
    if (s == "multi") {
        return Mode::MULTI;
    }
    if (s == "head") {
        return Mode::HEAD;
    }
    return Mode::UNKNOWN;
}

HeadSource parse_head_source(const json& src) {
    HeadSource out;
    out.ace = int_of(field(src, "ace_index")).value_or(-1);
    out.slot = int_of(field(src, "slot")).value_or(-1);
    out.type = str_or_empty(src, "type");
    out.color_hex = str_or_empty(src, "color");
    out.brand = str_or_empty(src, "brand");
    out.subtype = str_or_empty(src, "subtype");
    out.load_failed = bool_of(field(src, "load_failed")).value_or(false);
    return out;
}

Bay parse_bay(const json& slot, int position, const std::vector<int>& gate_status) {
    Bay bay;
    bay.index = int_of(field(slot, "index")).value_or(position);
    bay.material = ace::read_slot_material(slot).value_or("");
    bay.subtype = str_or_empty(slot, "subtype");
    bay.brand = str_or_empty(slot, "brand");
    bay.sku = str_or_empty(slot, "sku");
    bay.uid = str_or_empty(slot, "uid");
    bay.rfid = int_of(field(slot, "rfid")).value_or(0);

    bay.status = ace::slot_status_from_string(str_or_empty(slot, "status"));
    if (bay.status == SlotStatus::UNKNOWN && bay.index >= 0 &&
        bay.index < static_cast<int>(gate_status.size())) {
        // The gate flag answers presence when the status word does not:
        // 1 = filament at the gate, 0 = none, anything else unknown.
        const int gate = gate_status[static_cast<size_t>(bay.index)];
        if (gate == 1) {
            bay.status = SlotStatus::AVAILABLE;
        } else if (gate == 0) {
            bay.status = SlotStatus::EMPTY;
        }
    }

    if (const json* color = field(slot, "color")) {
        bay.color = ace::parse_slot_color(*color);
        // The raw device triple [0,0,0] is the tag's black only beside a
        // material; alone it states no colour.
        if (bay.color && *bay.color == 0 && bay.material.empty()) {
            bay.color.reset();
        }
    }
    return bay;
}

Unit parse_unit(const json& u, int position) {
    Unit unit;
    unit.idx = int_of(field(u, "idx")).value_or(position);
    unit.connected = bool_of(field(u, "connected")).value_or(false);
    unit.protocol = str_or_empty(u, "protocol");
    unit.model = str_or_empty(u, "model");
    unit.firmware = str_or_empty(u, "firmware");
    unit.status = str_or_empty(u, "status");
    unit.connect_error = str_or_empty(u, "connect_error");
    unit.temp = float_of(field(u, "temp"));
    unit.humidity = float_of(field(u, "humidity"));
    unit.feed_assist = int_of(field(u, "feed_assist")).value_or(-1);
    unit.dryer = ace::ace_pro_dryer_info();
    ace::apply_dryer_state(u, unit.dryer);

    if (const json* gates = field(u, "gate_status"); gates && gates->is_array()) {
        for (const auto& g : *gates) {
            unit.gate_status.push_back(g.is_number_integer() ? g.get<int>() : -1);
        }
    }
    if (const json* slots = field(u, "slots"); slots && slots->is_array()) {
        int n = 0;
        for (const auto& s : *slots) {
            if (s.is_object()) {
                unit.bays.push_back(parse_bay(s, n, unit.gate_status));
            }
            ++n;
        }
    }
    return unit;
}

} // namespace

bool present(const helix::PrinterDiscovery& hw) {
    const auto& names = hw.ace_object_names();
    return hw.has_snapmaker() && std::find(names.begin(), names.end(), "ace") != names.end();
}

StatusDelta parse_ace_status(const json& ace) {
    StatusDelta d;
    if (!ace.is_object()) {
        return d;
    }

    d.api_version = int_of(field(ace, "api_version"));
    if (const json* m = field(ace, "mode"); m && m->is_string()) {
        d.mode = mode_from_string(m->get<std::string>());
    }

    d.head_ace = per_head<int>(
        ace, "head_ace", [](const json& v) { return int_of(&v); }, [](int h) { return h; });
    const auto read_bool = [](const json& v) { return bool_of(&v); };
    const auto no = [](int) { return false; };
    d.head_feeder = per_head<bool>(ace, "head_feeder", read_bool, no);
    d.head_manual = per_head<bool>(ace, "head_manual", read_bool, no);
    d.head_source = per_head<std::optional<HeadSource>>(
        ace, "head_source",
        [](const json& v) -> std::optional<std::optional<HeadSource>> {
            if (v.is_null()) {
                return std::optional<HeadSource>{};
            }
            if (v.is_object()) {
                return std::optional<HeadSource>{parse_head_source(v)};
            }
            return std::nullopt;
        },
        [](int) { return std::optional<HeadSource>{}; });

    if (const json* heads = field(ace, "ace_heads"); heads && heads->is_array()) {
        std::vector<int> out;
        for (const auto& h : *heads) {
            if (h.is_number_integer()) {
                out.push_back(h.get<int>());
            }
        }
        d.ace_heads = std::move(out);
    }

    if (const json* aces = field(ace, "aces"); aces && aces->is_array()) {
        std::vector<Unit> units;
        int position = 0;
        for (const auto& u : *aces) {
            if (u.is_object()) {
                units.push_back(parse_unit(u, position));
            }
            ++position;
        }
        d.aces = std::move(units);
    }

    d.swap_in_progress = bool_of(field(ace, "swap_in_progress"));
    d.unload_all_active = bool_of(field(ace, "unload_all_active"));
    d.quad_replenish = bool_of(field(ace, "quad_replenish"));
    d.quad_first = bool_of(field(ace, "quad_first"));
    if (const json* p = field(ace, "swap_phase"); p && p->is_string()) {
        d.swap_phase = p->get<std::string>();
    }
    if (const json* r = field(ace, "last_swap_result")) {
        if (r->is_null()) {
            d.last_swap_result = LastSwap{};
        } else if (r->is_object()) {
            LastSwap last;
            last.head = int_of(field(*r, "head")).value_or(-1);
            last.ace = int_of(field(*r, "ace")).value_or(-1);
            last.slot = int_of(field(*r, "slot")).value_or(-1);
            last.status = str_or_empty(*r, "status");
            d.last_swap_result = last;
        }
    }
    if (const json* seq = field(ace, "event_seq"); seq && seq->is_number_integer()) {
        d.event_seq = seq->get<int64_t>();
    }
    if (const json* spools = field(ace, "spools"); spools && spools->is_object()) {
        d.spools = *spools;
    }
    if (const json* binding = field(ace, "spool_binding"); binding && binding->is_object()) {
        d.spool_binding = *binding;
    }
    return d;
}

std::optional<uint32_t> head_source_color(const HeadSource& source) {
    if (source.color_hex.empty()) {
        return std::nullopt;
    }
    const auto reading = helix::ams::read_lane_color(source.color_hex);
    if (reading.kind != helix::ams::ColorReadingKind::Observed) {
        return std::nullopt;
    }
    return reading.rgb;
}

HeadFeed head_feed(Mode mode, bool manual, bool feeder) {
    if (manual) {
        return HeadFeed::MANUAL;
    }
    if (mode == Mode::HEAD && feeder) {
        return HeadFeed::FEEDER;
    }
    return HeadFeed::ACE;
}

std::vector<std::pair<int, int>> candidate_bays(Mode mode, int head,
                                                const std::array<int, kHeadCount>& head_ace,
                                                const std::array<HeadFeed, kHeadCount>& feed,
                                                int unit_count) {
    std::vector<std::pair<int, int>> out;
    if (head < 0 || head >= kHeadCount || feed[static_cast<size_t>(head)] != HeadFeed::ACE) {
        return out;
    }
    if (mode == Mode::HEAD) {
        const int ace = head_ace[static_cast<size_t>(head)];
        if (ace >= 0 && ace < unit_count) {
            for (int s = 0; s < kBaysPerUnit; ++s) {
                out.emplace_back(ace, s);
            }
        }
    } else if (mode == Mode::MULTI) {
        for (int k = 0; k < unit_count; ++k) {
            out.emplace_back(k, head);
        }
    }
    return out;
}

std::optional<int> head_for_bay(Mode mode, int ace, int slot,
                                const std::array<int, kHeadCount>& head_ace,
                                const std::array<HeadFeed, kHeadCount>& feed) {
    if (ace < 0 || slot < 0 || slot >= kBaysPerUnit) {
        return std::nullopt;
    }
    if (mode == Mode::HEAD) {
        for (int h = 0; h < kHeadCount; ++h) {
            if (feed[static_cast<size_t>(h)] == HeadFeed::ACE &&
                head_ace[static_cast<size_t>(h)] == ace) {
                return h;
            }
        }
        return std::nullopt;
    }
    if (mode == Mode::MULTI && feed[static_cast<size_t>(slot)] == HeadFeed::ACE) {
        return slot;
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>> bay_of(int global) {
    if (global < kHeadCount) {
        return std::nullopt;
    }
    const int offset = global - kHeadCount;
    return std::make_pair(offset / kBaysPerUnit, offset % kBaysPerUnit);
}

std::optional<Event> parse_event_line(std::string_view line) {
    static constexpr std::string_view kRespond = "// ";
    static constexpr std::string_view kPrefix = "multiace_event ";
    if (line.substr(0, kRespond.size()) == kRespond) {
        line.remove_prefix(kRespond.size());
    }
    if (line.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    line.remove_prefix(kPrefix.size());

    std::vector<std::string_view> tokens;
    while (!line.empty()) {
        const size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) {
            break;
        }
        line.remove_prefix(start);
        const size_t end = line.find_first_of(" \t\r\n");
        tokens.push_back(line.substr(0, end));
        line.remove_prefix(end == std::string_view::npos ? line.size() : end);
    }
    if (tokens.empty() || tokens.front().find('=') != std::string_view::npos) {
        return std::nullopt;
    }

    Event ev;
    ev.name = std::string(tokens.front());
    for (size_t i = 1; i < tokens.size(); ++i) {
        const auto eq = tokens[i].find('=');
        if (eq == std::string_view::npos || eq == 0) {
            return std::nullopt;
        }
        std::string key(tokens[i].substr(0, eq));
        std::string value(tokens[i].substr(eq + 1));
        if (key == "seq") {
            const auto seq = text_io::parse_int<int64_t>(value);
            if (!seq) {
                return std::nullopt;
            }
            ev.seq = *seq;
        }
        ev.fields[std::move(key)] = std::move(value);
    }
    return ev;
}

std::string load_gcode(int head, std::optional<std::pair<int, int>> bay) {
    if (!bay) {
        return fmt::format("ACE_LOAD_HEAD HEAD={}", head);
    }
    return fmt::format("ACE_LOAD_HEAD HEAD={} ACE={} SLOT={}", head, bay->first, bay->second);
}

std::string unload_gcode(int head) {
    return fmt::format("ACE_UNLOAD_HEAD HEAD={}", head);
}

std::string unload_all_gcode() {
    return "ACE_UNLOAD_ALL_HEADS";
}

std::string dry_gcode(int ace, int temp_c, int duration_min) {
    return fmt::format("ACE_DRY ACE={} TEMP={} DURATION={}", ace, temp_c, duration_min);
}

std::string stop_dry_gcode(int ace) {
    return fmt::format("ACE_STOP_DRYING ACE={}", ace);
}

} // namespace helix::multiace

#endif // HELIX_HAS_SNAPMAKER
