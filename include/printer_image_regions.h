// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace helix {

/// A point on a printer image, normalized 0..1 over the source PNG. The
/// prerendered tiers and the exact-size cache are aspect-preserving, centred
/// resizes of that PNG, so the same point holds at every rendered size.
struct NormPoint {
    float x = 0.f;
    float y = 0.f;
};

/// Hand-tagged parts of one printer image (assets/images/printers/regions.json).
struct ImageRegions {
    int src_w = 0; ///< source PNG width, guards against a re-cropped image
    int src_h = 0;
    NormPoint nozzle;
    NormPoint bed_left;  ///< near edge of the plate, left end as pictured
    NormPoint bed_right; ///< near edge of the plate, right end as pictured
    /// The plate's far edge as pictured; plate_quad() estimates it when untagged.
    struct FarEdge {
        NormPoint right;
        NormPoint left;
    };
    std::optional<FarEdge> bed_far;
    std::optional<NormPoint> part_fan;
    std::optional<NormPoint> chamber; ///< empty spot inside the enclosure
    std::optional<NormPoint> light;
    /// The see-through regions of the enclosure (front, side, top), each clockwise
    /// from its top-left. Chamber heat and the light show only inside these.
    std::vector<std::array<NormPoint, 4>> windows;
};

/// Parse a regions document. Entries missing `size`, `nozzle` or `bed`, or
/// holding a malformed point, are skipped with a warning; a malformed
/// document yields an empty map.
std::unordered_map<std::string, ImageRegions> parse_image_regions(const std::string& json_text);

/// Regions for an image's key (printer_image_region_key()) as displayed at
/// `natural_w` x `natural_h`: the user's own tags when they were placed on an
/// image of that size, else the shipped entry, else nullptr. Both files are
/// read once, on first call.
const ImageRegions* lookup_image_regions(std::string_view key, int natural_w, int natural_h);

/// The user's own tags for `key`, or nullptr when there are none or they were
/// placed on an image of another size: another screen tier, or re-cut art. A
/// replaced custom photo of the same aspect has the same size; importing it
/// clears its tags instead (PrinterImageManager::import_image).
const ImageRegions* lookup_user_image_regions(std::string_view key, int natural_w, int natural_h);

/// Whether regions.json tags `key`, whatever the user has saved over it.
bool has_shipped_image_regions(std::string_view key);

/// The plate's top surface as pictured: near-left, near-right, far-right, far-left.
/// Without a tagged far edge, the near edge raised and narrowed by a typical
/// three-quarter view stands in for it.
std::array<NormPoint, 4> plate_quad(const ImageRegions& r);

/// Store the user's tags for `key` in <config dir>/printer_image_regions.json,
/// replacing any earlier entry for it. False, with nothing changed, when the
/// file cannot be written, or exists and cannot be read. A file that reads but
/// does not parse is first moved aside to printer_image_regions.json.bad.
bool save_user_image_regions(const std::string& key, const ImageRegions& regions);

/// Delete the user's tags for `key`, returning the image to its shipped points
/// or to docked chips. True, writing nothing, when there are none. False, with
/// nothing changed, when the file cannot be written.
bool reset_user_image_regions(const std::string& key);

/// The regions key for an image path the printer image widget displays. A
/// shipped image is keyed by its file stem ("creality-k1c"), a custom image by
/// "custom:<name>"; a "-<size>" tier suffix on a .bin is dropped. Empty for
/// anything outside the shipped printers and custom images directories.
std::string printer_image_region_key(std::string_view image_path);

/// The things the tagger asks for, in the order it asks.
enum class TagPrompt {
    Nozzle,
    PartFan,
    BedLeft,
    BedRight,
    BedFarRight,
    BedFarLeft,
    Chamber,
    Light,
    Done
};

/// One pass of tapping an image's parts: a tap answers the current prompt, the
/// optional ones (part fan, the plate's far corners, chamber, light) can be skipped, and undo steps
/// back.
class ImageTagSession {
  public:
    static constexpr int kPromptCount = 8;

    TagPrompt prompt() const {
        return static_cast<TagPrompt>(step_);
    }
    int step() const {
        return step_;
    }
    bool done() const {
        return step_ == kPromptCount;
    }
    bool can_skip() const;
    bool can_undo() const {
        return step_ > 0;
    }
    void tap(NormPoint p);
    /// False, changing nothing, on a required prompt.
    bool skip();
    void undo();
    /// What was tapped so far, for the image it was tapped on.
    ImageRegions regions(int natural_w, int natural_h) const;

  private:
    std::optional<NormPoint> points_[kPromptCount];
    int step_ = 0;
};

} // namespace helix
