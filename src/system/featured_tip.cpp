// SPDX-License-Identifier: GPL-3.0-or-later

#include "featured_tip.h"

#include "json_utils.h"

namespace helix::featured_tip {

std::optional<FeaturedTip> parse(const nlohmann::json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    FeaturedTip tip;
    tip.id = json_util::safe_string(value, "id");
    tip.title = json_util::safe_string(value, "title");
    if (tip.id.empty() || tip.title.empty()) {
        return std::nullopt;
    }
    tip.details = json_util::safe_string(value, "details");
    tip.source = json_util::safe_string(value, "source");
    tip.posted_at = json_util::safe_double(value, "posted_at");
    return tip;
}

bool is_fresh(const FeaturedTip& tip, double now_s) {
    return tip.posted_at <= 0.0 || now_s - tip.posted_at <= kMaxAgeSeconds;
}

PrintingTip to_printing_tip(const FeaturedTip& tip) {
    PrintingTip out;
    out.id = "featured:" + tip.id;
    out.title = tip.title;
    out.heading = tip.source;
    out.content = tip.details.empty() ? tip.title : tip.title + "\n\n" + tip.details;
    out.category = "featured";
    return out;
}

} // namespace helix::featured_tip
