// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"
#include "tips_manager.h"

#include <optional>
#include <string>

namespace helix::featured_tip {

/// One Moonraker database item another program on the printer host writes to
/// put a line on the tips widget: the joke a chat bot just posted, say.
/// Value: {"id", "title", "details", "source", "posted_at"}. `title` is what
/// the tile shows, `details` what a tap opens. `id` names one event, so a
/// rewrite of the same event shows once.
inline constexpr const char* kNamespace = "helix-screen";
inline constexpr const char* kKey = "featured_tip";

/// A featured line older than this when first seen is skipped: a screen that
/// boots after the chat has moved on does not replay it.
inline constexpr double kMaxAgeSeconds = 600.0;

struct FeaturedTip {
    std::string id;
    std::string title;
    std::string details;
    std::string source;
    double posted_at = 0.0; ///< Unix seconds; 0 when the writer gave none
};

/// The record, or nullopt when it has no id or no title. Never throws.
std::optional<FeaturedTip> parse(const nlohmann::json& value);

/// Whether a record not shown yet is still worth showing at @p now_s (Unix seconds).
/// A record without a time is fresh; one from the future (clock skew) is too.
bool is_fresh(const FeaturedTip& tip, double now_s);

/// The tips widget's tip for @p tip: the line as the title, and for the tap,
/// the line again above its details under the writer's name.
PrintingTip to_printing_tip(const FeaturedTip& tip);

} // namespace helix::featured_tip
