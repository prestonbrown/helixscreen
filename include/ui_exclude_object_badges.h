// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <glm/vec2.hpp>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class PrinterExcludedObjectsState;
namespace gcode {
struct ParsedGCodeFile;
}
} // namespace helix

namespace helix::ui {

/// Hands the name of an object the user tapped (a list row, a map rect, a badge).
using ObjectTapFn = std::function<void(const std::string& name)>;

/// What a tap on an already-excluded object does. Print status excludes for
/// good, so the object stops taking taps; print details holds picks the user
/// can undo before Print, so it keeps taking them.
enum class ExcludeTapMode { ExcludeOnly, Toggle };

/**
 * @brief What one object's numbered badge says, and where it belongs.
 *
 * Every surface that identifies an object by number - the side list chips, the
 * thumbnail map, the badges drawn on the 2D/3D render - keys on defined_index,
 * the object's position in PrinterExcludedObjectsState::get_defined_objects().
 * Number and colour are both derived from it, so the same object reads the
 * same everywhere.
 */
struct ObjectBadge {
    int defined_index = -1; ///< Position in the defined list; also the palette index
    std::string name;
    std::string number; ///< display_ordinal(defined_index), as text
    bool excluded = false;
    bool current = false;
    bool has_anchor = false;
    glm::vec2 anchor{0.0f, 0.0f}; ///< World XY (mm) the badge sits over
    std::optional<float> top_z;   ///< Top of the object's toolpath, when the file was parsed

    bool operator==(const ObjectBadge& o) const {
        return defined_index == o.defined_index && name == o.name && number == o.number &&
               excluded == o.excluded && current == o.current && has_anchor == o.has_anchor &&
               anchor == o.anchor && top_z == o.top_z;
    }
    bool operator!=(const ObjectBadge& o) const {
        return !(*this == o);
    }
};

/**
 * @brief One badge per defined object, in defined order.
 *
 * Anchor priority: Klipper CENTER, the parsed file's CENTER, Klipper's bbox
 * centre, the parsed toolpath bbox centre. An object with none of these keeps
 * its number and colour but has_anchor = false. Parsed objects are looked up by
 * name; their map order is alphabetical and never decides a number.
 *
 * Pure: reads both inputs, touches no LVGL state.
 */
std::vector<ObjectBadge> compute_object_badges(const PrinterExcludedObjectsState& state,
                                               const gcode::ParsedGCodeFile* parsed);

/**
 * @brief The name an object is shown under: its own when @p font draws it.
 *
 * A name with any character @p font cannot draw (a Chinese model name on an
 * English UI, whose fonts carry no CJK) is shown as "Object <n>" instead, n
 * being the badge number for @p defined_index, so the label still matches the
 * object's disc. An empty name gets the same label. Display only: commands
 * sent to the printer always carry the real name.
 *
 * @param defined_index Position in the defined list; negative when unknown, which labels "Object"
 */
std::string object_display_name(const std::string& name, int defined_index, const lv_font_t* font);

/**
 * @brief Position of @p name in @p defined, or -1 when no defined object matches.
 *
 * Compared ASCII upper-case: Klipper stores object names upper-cased, while the
 * viewer and the file keep the slicer's casing.
 */
int defined_object_index(const std::vector<std::string>& defined, const std::string& name);

/// object_display_name() with the index from defined_object_index().
std::string object_display_name(const std::vector<std::string>& defined, const std::string& name,
                                const lv_font_t* font);

/// object_display_name() for a name formatted into a toast or dialog message,
/// both of which draw in the body font.
std::string object_message_name(const std::vector<std::string>& defined, const std::string& name);

// Badge styling, shared by the map view's widgets and the viewer's draw pass.

/// Fill colour for the object at @p defined_index (same as its side-list chip).
lv_color_t object_badge_color(int defined_index);
/// Number colour on a @p fill disc.
lv_color_t object_badge_text_color(lv_color_t fill);
const lv_font_t* object_badge_font();
/// Disc diameter: one line of object_badge_font(), so it scales with the size class.
int32_t object_badge_diameter();
/// Excluded objects fade their badge like the map view fades their rect.
inline lv_opa_t object_badge_opa(bool excluded) {
    return excluded ? LV_OPA_30 : LV_OPA_COVER;
}

/// Every token a badge pass reads, resolved once so drawing looks nothing up.
struct BadgeLook {
    int32_t diameter = 0;
    const lv_font_t* font = nullptr;
    lv_color_t ring_color{}; ///< Outline marking the object printing now
    int32_t ring_width = 0;
    std::vector<lv_color_t> fill; ///< Per badge, parallel to the badge list
    std::vector<lv_color_t> text;
    int theme_generation = -1; ///< theme_manager_get_changed_subject() when resolved
    int breakpoint = -1;       ///< theme_manager_get_breakpoint_subject() when resolved
};
BadgeLook resolve_badge_look(const std::vector<ObjectBadge>& badges);
/// False once the theme or size class has changed since @p look was resolved.
bool badge_look_current(const BadgeLook& look);

/// Draw badge @p i of the list @p look was resolved for, centred on (@p cx, @p cy).
void draw_object_badge(lv_layer_t* layer, const BadgeLook& look, size_t i, const ObjectBadge& badge,
                       int32_t cx, int32_t cy);

} // namespace helix::ui
