// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_unit_art.h"

#include "ui_utils.h"

#include "ams_types.h"
#include "ams_unit_shell.h"
#include "core/data_root_resolver.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace helix::ui {

namespace {

constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint32_t kCarriageSlideMs = 600;

struct LaneImages {
    lv_obj_t* color;
    lv_obj_t* shade;
};

struct ArtData {
    UnitArtTier tier = UnitArtTier::Thumb;
    const char* built_id = nullptr;
    int built_lanes = 0;
    std::vector<LaneImages> lanes;
    lv_obj_t* layers = nullptr; // WIDGET_PTR_OK: child of this widget, gone with it
    lv_obj_t* flat = nullptr;   // WIDGET_PTR_OK: child of this widget, gone with it
    lv_draw_buf_t* flat_buf = nullptr;
    std::vector<lv_draw_buf_t*> retired; // swapped out while widgets may still draw them
    uint64_t flat_key = 0;

    const tray::CarriageTrack* track = nullptr;
    lv_obj_t* carriage = nullptr;      // WIDGET_PTR_OK: child of this widget, gone with it
    lv_obj_t* carriage_flat = nullptr; // WIDGET_PTR_OK: child of this widget, gone with it
    lv_draw_buf_t* carriage_buf = nullptr;
    int32_t carriage_w = 0, carriage_h = 0;
    int carriage_gate = 0;
    bool carriage_placed = false;
};

ArtData* data_of(lv_obj_t* art) {
    return static_cast<ArtData*>(lv_obj_get_user_data(art));
}

lv_obj_t* add_layer(lv_obj_t* art, const std::string& path) {
    lv_obj_t* image = lv_image_create(art);
    lv_obj_set_size(image, lv_pct(100), lv_pct(100));
    lv_obj_center(image);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(image, path.c_str());
    lv_obj_set_clickable(image, false);
    return image;
}

void build_layers(lv_obj_t* art, ArtData* data, const tray::UnitShell* shell, int lane_count) {
    // Runs under subject observers, where a synchronous delete corrupts the event batch.
    helix::ui::safe_clean_children(art);
    data->lanes.clear();
    data->flat_key = 0;
    if (data->flat_buf) {
        data->retired.push_back(data->flat_buf);
        data->flat_buf = nullptr;
    }
    if (data->carriage_buf) {
        data->retired.push_back(data->carriage_buf);
        data->carriage_buf = nullptr;
    }
    data->carriage = nullptr;
    data->carriage_flat = nullptr;
    data->carriage_gate = 0;
    data->carriage_placed = false;
    data->track = tray::unit_shell_carriage(*shell, lane_count);
    data->layers = lv_obj_create(art);
    lv_obj_remove_style_all(data->layers);
    lv_obj_set_size(data->layers, lv_pct(100), lv_pct(100));
    lv_obj_set_clickable(data->layers, false);
    data->flat = lv_image_create(art);
    lv_obj_set_size(data->flat, lv_pct(100), lv_pct(100));
    lv_obj_set_clickable(data->flat, false);
    lv_obj_set_hidden(data->flat, true);
    const std::string variant =
        shell->art_lane_variants != 0 ? std::to_string(lane_count) + "/" : std::string();
    const std::string base =
        helix::asset_component_uri(std::string("assets/images/units/") + shell->id + "/" + variant +
                                   (data->tier == UnitArtTier::Full ? "full" : "thumb"));
    add_layer(data->layers, base + "_back.png");
    for (int lane = 0; lane < lane_count; ++lane) {
        const std::string stem = base + "_lane" + std::to_string(lane);
        LaneImages images{add_layer(data->layers, stem + ".png"),
                          add_layer(data->layers, stem + "_shade.png")};
        // DECLARATIVE_OK: each lane's colour is per-widget live data.
        lv_obj_set_style_image_recolor_opa(images.color, LV_OPA_COVER, LV_PART_MAIN);
        data->lanes.push_back(images);
    }
    add_layer(data->layers, base + "_front.png");
    if (data->track) {
        data->carriage = add_layer(data->layers, base + "_carriage.png");
        data->carriage_flat = lv_image_create(art);
        lv_obj_set_size(data->carriage_flat, lv_pct(100), lv_pct(100));
        lv_obj_set_clickable(data->carriage_flat, false);
        lv_obj_set_hidden(data->carriage_flat, true);
    }
    data->built_id = shell->id;
    data->built_lanes = lane_count;
}

void set_lane_visible(lv_obj_t* image, bool visible) {
    // DECLARATIVE_OK: per-lane state with no subject behind it.
    if (visible)
        lv_obj_set_hidden(image, false);
    else
        lv_obj_set_hidden(image, true);
}

void destroy_buf(lv_draw_buf_t* buf) {
    lv_image_cache_drop(buf);
    lv_draw_buf_destroy(buf);
}

lv_obj_t* live_carriage(const ArtData* data) {
    return data->carriage_buf ? data->carriage_flat : data->carriage;
}

void set_translate_x(void* obj, int32_t value) {
    // DECLARATIVE_OK: per-frame animation of a pre-rendered layer.
    lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
}

void set_translate_y(void* obj, int32_t value) {
    // DECLARATIVE_OK: per-frame animation of a pre-rendered layer.
    lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
}

void place_carriage(lv_obj_t* art, ArtData* data, bool slide) {
    lv_obj_t* obj = live_carriage(data);
    if (!obj)
        return;
    lv_anim_delete(obj, set_translate_x);
    lv_anim_delete(obj, set_translate_y);
    const tray::CarriageOffset to = tray::carriage_offset(
        *data->track, data->carriage_gate, lv_obj_get_width(art), lv_obj_get_height(art));
    if (!slide) {
        set_translate_x(obj, to.x);
        set_translate_y(obj, to.y);
        return;
    }
    const auto start = [&](lv_anim_exec_xcb_t exec, int32_t from, int32_t end) {
        if (from == end)
            return;
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, obj);
        lv_anim_set_exec_cb(&anim, exec);
        lv_anim_set_values(&anim, from, end);
        lv_anim_set_duration(&anim, kCarriageSlideMs);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_start(&anim);
    };
    start(set_translate_x, lv_obj_get_style_translate_x(obj, LV_PART_MAIN), to.x);
    start(set_translate_y, lv_obj_get_style_translate_y(obj, LV_PART_MAIN), to.y);
}

/// Render the carriage once at gate 0, so moving it is a plain blit of one image.
void flatten_carriage(lv_obj_t* art, ArtData* data, int32_t width, int32_t height) {
    if (!data->carriage)
        return;
    if (data->carriage_buf && data->carriage_w == width && data->carriage_h == height)
        return;
    lv_anim_delete(data->carriage, nullptr);
    set_translate_x(data->carriage, 0);
    set_translate_y(data->carriage, 0);
    lv_obj_set_hidden(data->carriage, false);
    lv_obj_update_layout(art);
    lv_draw_buf_t* snap = lv_snapshot_take(data->carriage, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_hidden(data->carriage, true);
    if (!snap)
        return;
    lv_image_set_src(data->carriage_flat, snap);
    if (data->carriage_buf)
        destroy_buf(data->carriage_buf);
    data->carriage_buf = snap;
    data->carriage_w = width;
    data->carriage_h = height;
    lv_obj_set_hidden(data->carriage_flat, false);
    place_carriage(art, data, false);
}

/// Replace the stack of decoded layers with one image of the current lane state.
/// Each layer would otherwise be decoded from its PNG on every redraw.
void flatten(lv_obj_t* art, ArtData* data, uint64_t key) {
    lv_obj_update_layout(art);
    const int32_t width = lv_obj_get_width(art);
    const int32_t height = lv_obj_get_height(art);
    // The image is one bitmap at the widget's size, so a new size is a new image.
    key = (key ^ static_cast<uint64_t>(width)) * kFnvPrime;
    key = (key ^ static_cast<uint64_t>(height)) * kFnvPrime;
    if (width <= 0 || height <= 0 || key == data->flat_key)
        return;
    lv_obj_set_hidden(data->layers, false);
    lv_obj_set_hidden(data->flat, true);
    // The carriage moves on its own, so it stays out of the flat image.
    if (data->carriage) {
        lv_obj_set_hidden(data->carriage, true);
        lv_obj_set_hidden(data->carriage_flat, true);
    }
    lv_draw_buf_t* snap = lv_snapshot_take(art, LV_COLOR_FORMAT_ARGB8888);
    if (!snap) {
        if (data->carriage)
            lv_obj_set_hidden(live_carriage(data), false);
        return;
    }
    lv_image_set_src(data->flat, snap);
    if (data->flat_buf)
        destroy_buf(data->flat_buf);
    data->flat_buf = snap;
    data->flat_key = key;
    lv_obj_set_hidden(data->flat, false);
    lv_obj_set_hidden(data->layers, true);
    flatten_carriage(art, data, width, height);
    if (data->carriage_buf)
        lv_obj_set_hidden(data->carriage_flat, false);
}

void delete_cb(lv_event_t* e) {
    lv_obj_t* art = lv_event_get_target_obj(e);
    if (ArtData* data = data_of(art)) {
        if (data->flat_buf)
            lv_draw_buf_destroy(data->flat_buf);
        if (data->carriage_buf)
            lv_draw_buf_destroy(data->carriage_buf);
        for (lv_draw_buf_t* buf : data->retired)
            lv_draw_buf_destroy(buf);
    }
    delete data_of(art);
    lv_obj_set_user_data(art, nullptr);
}

lv_obj_t* make_widget(lv_obj_t* parent, UnitArtTier tier) {
    lv_obj_t* art = lv_obj_create(parent);
    lv_obj_remove_style_all(art);
    lv_obj_set_scrollable(art, false);
    lv_obj_set_clickable(art, false);
    auto* data = new ArtData{};
    data->tier = tier;
    lv_obj_set_user_data(art, data);
    // DECLARATIVE_OK: a custom widget's own teardown hook.
    lv_obj_add_event_cb(art, delete_cb, LV_EVENT_DELETE, nullptr);
    return art;
}

void* xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    return make_widget(static_cast<lv_obj_t*>(lv_xml_state_get_parent(state)), UnitArtTier::Thumb);
}

void xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    auto* art = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    lv_xml_obj_apply(state, attrs);
    const char* tier = lv_xml_get_value_of(attrs, "tier");
    if (tier && std::strcmp(tier, "full") == 0)
        data_of(art)->tier = UnitArtTier::Full;
}

} // namespace

UnitArtLane unit_art_lane(const SlotInfo& slot) {
    const bool filled = slot.status != SlotStatus::EMPTY && slot.status != SlotStatus::UNKNOWN;
    return {filled, slot.color_rgb};
}

bool unit_art_available(const AmsSystemInfo& info, int unit_index, bool has_dryer) {
    const tray::UnitShell* shell = tray::unit_shell_for(info, unit_index, has_dryer);
    return shell && tray::unit_shell_has_art_for(
                        *shell, info.units[static_cast<size_t>(unit_index)].slot_count);
}

void register_unit_art_widget() {
    lv_xml_register_widget("unit_art", xml_create, xml_apply);
}

lv_obj_t* unit_art_create(lv_obj_t* parent, UnitArtTier tier) {
    return make_widget(parent, tier);
}

void unit_art_set_unit(lv_obj_t* art, const AmsSystemInfo& info, int unit_index, bool has_dryer) {
    ArtData* data = art ? data_of(art) : nullptr;
    if (!data)
        return;
    if (!unit_art_available(info, unit_index, has_dryer)) {
        lv_obj_set_hidden(art, true);
        return;
    }
    const tray::UnitShell* shell = tray::unit_shell_for(info, unit_index, has_dryer);
    const AmsUnit& unit = info.units[static_cast<size_t>(unit_index)];
    if (data->built_id != shell->id || data->built_lanes != unit.slot_count)
        build_layers(art, data, shell, unit.slot_count);
    lv_obj_set_hidden(art, false);

    uint64_t key = 1469598103934665603ull;
    for (size_t i = 0; i < data->lanes.size(); ++i) {
        const UnitArtLane lane =
            i < unit.slots.size() ? unit_art_lane(unit.slots[i]) : UnitArtLane{};
        key = (key ^ (lane.filled ? static_cast<uint64_t>(lane.color) + 1 : 0)) * kFnvPrime;
        set_lane_visible(data->lanes[i].color, lane.filled);
        set_lane_visible(data->lanes[i].shade, lane.filled);
        if (lane.filled) {
            // DECLARATIVE_OK: the live filament colour of this lane.
            lv_obj_set_style_image_recolor(data->lanes[i].color, lv_color_hex(lane.color),
                                           LV_PART_MAIN);
        }
    }
    flatten(art, data, key);

    if (data->track) {
        const int gate = info.current_slot - unit.first_slot_global_index;
        const bool on_this_unit = gate >= 0 && gate < unit.slot_count;
        const bool moved = on_this_unit && gate != data->carriage_gate;
        if (on_this_unit)
            data->carriage_gate = gate;
        if (moved || !data->carriage_placed) {
            place_carriage(art, data,
                           moved && data->carriage_placed &&
                               DisplaySettingsManager::instance().get_animations_enabled());
        }
        data->carriage_placed = true;
    }
}

bool unit_art_is_flattened(lv_obj_t* art) {
    ArtData* data = art ? data_of(art) : nullptr;
    return data && data->flat_buf && !lv_obj_is_hidden(data->flat);
}

lv_obj_t* unit_art_carriage_obj(lv_obj_t* art) {
    ArtData* data = art ? data_of(art) : nullptr;
    return data ? live_carriage(data) : nullptr;
}

lv_obj_t* unit_art_lane_color_obj(lv_obj_t* art, int lane) {
    ArtData* data = art ? data_of(art) : nullptr;
    if (!data || lane < 0 || lane >= static_cast<int>(data->lanes.size()))
        return nullptr;
    return data->lanes[static_cast<size_t>(lane)].color;
}

} // namespace helix::ui
