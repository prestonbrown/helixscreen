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

/// Mixed with tips: a featured line older than this when first seen is
/// skipped, so a screen that boots after the chat has moved on does not
/// replay it, and a shown line holds the tile for one rotation.
inline constexpr double kMaxAgeSeconds = 600.0;

/// Featured lines replacing tips (the default): the latest line holds the tile
/// until a newer one arrives or it is this old, so a writer that stopped
/// posting does not pin one line to the screen forever.
inline constexpr double kReplaceMaxAgeSeconds = 7.0 * 24 * 3600;

/// Tips-widget config key: true (or absent) lets featured lines replace the
/// tips while one is current; false mixes each one in for a single rotation.
inline constexpr const char* kConfigFeaturedOnly = "featured_only";

struct FeaturedTip {
    std::string id;
    std::string title;
    std::string details;
    std::string source;
    double posted_at = 0.0; ///< Unix seconds; 0 when the writer gave none
};

/// The record, or nullopt when it has no id or no title. Never throws.
std::optional<FeaturedTip> parse(const nlohmann::json& value);

/// Whether featured lines replace the tips under this widget @p config. Default true.
bool featured_only(const nlohmann::json& config);

/// The oldest a line may be and still show: kReplaceMaxAgeSeconds when
/// @p featured_only, else kMaxAgeSeconds.
double max_age_seconds(bool featured_only);

/// Whether @p tip is still worth showing at @p now_s (Unix seconds) under
/// @p max_age_s. A record without a time is fresh; one from the future (clock
/// skew) is too.
bool is_fresh(const FeaturedTip& tip, double now_s, double max_age_s = kMaxAgeSeconds);

/// The tips widget's tip for @p tip: the line as the title, and for the tap,
/// the line again above its details under the writer's name.
PrintingTip to_printing_tip(const FeaturedTip& tip);

} // namespace helix::featured_tip
