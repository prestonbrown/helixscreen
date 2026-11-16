// SPDX-License-Identifier: GPL-3.0-or-later

#include "chamber_air_settings.h"

#include "config.h"

#include <algorithm>
#include <cmath>

namespace helix::chamber_air {

namespace {

std::string key(const char* field) {
    return Config::get_instance()->df() + "chamber_air/" + field;
}

} // namespace

ModeTargets clamp_targets(ModeTargets t) {
    t.target_c = std::isfinite(t.target_c) ? std::clamp(t.target_c, kMinTargetC, kMaxTargetC)
                                           : ModeTargets{}.target_c;
    t.exhaust_pct = std::clamp(t.exhaust_pct, kMinTargetPct, kMaxTargetPct);
    t.recirculate_pct = std::clamp(t.recirculate_pct, kMinTargetPct, kMaxTargetPct);
    return t;
}

ModeTargets load_targets() {
    Config* cfg = Config::get_instance();
    if (!cfg) {
        return ModeTargets{};
    }
    const ModeTargets d{};
    ModeTargets t;
    t.target_c = cfg->get<double>(key("target_c"), d.target_c);
    t.exhaust_pct = cfg->get<int>(key("exhaust_pct"), d.exhaust_pct);
    t.recirculate_pct = cfg->get<int>(key("recirculate_pct"), d.recirculate_pct);
    return clamp_targets(t);
}

void save_targets(const ModeTargets& in) {
    Config* cfg = Config::get_instance();
    if (!cfg) {
        return;
    }
    const ModeTargets t = clamp_targets(in);
    cfg->set<double>(key("target_c"), t.target_c);
    cfg->set<int>(key("exhaust_pct"), t.exhaust_pct);
    cfg->set<int>(key("recirculate_pct"), t.recirculate_pct);
    cfg->save();
}

} // namespace helix::chamber_air
