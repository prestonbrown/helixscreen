// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_unit_art.h"

#include "ams_types.h"
#include "ams_unit_shell.h"
#include "core/data_root_resolver.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>
#include <vector>

namespace helix::ui {

namespace {

struct LaneImages {
    lv_obj_t* color;
    lv_obj_t* shade;
};

struct ArtData {
    UnitArtTier tier = UnitArtTier::Thumb;
    const char* built_id = nullptr;
    std::vector<LaneImages> lanes;
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

void build_layers(lv_obj_t* art, ArtData* data, const tray::UnitShell* shell) {
    lv_obj_clean(art);
    data->lanes.clear();
    const std::string base =
        helix::asset_component_uri(std::string("assets/images/units/") + shell->id + "/" +
                                   (data->tier == UnitArtTier::Full ? "full" : "thumb"));
    add_layer(art, base + "_back.png");
    for (int lane = 0; lane < shell->art_lanes; ++lane) {
        const std::string stem = base + "_lane" + std::to_string(lane);
        LaneImages images{add_layer(art, stem + ".png"), add_layer(art, stem + "_shade.png")};
        // DECLARATIVE_OK: each lane's colour is per-widget live data.
        lv_obj_set_style_image_recolor_opa(images.color, LV_OPA_COVER, LV_PART_MAIN);
        data->lanes.push_back(images);
    }
    add_layer(art, base + "_front.png");
    data->built_id = shell->id;
}

void set_lane_visible(lv_obj_t* image, bool visible) {
    // DECLARATIVE_OK: per-lane state with no subject behind it.
    if (visible)
        lv_obj_set_hidden(image, false);
    else
        lv_obj_set_hidden(image, true);
}

void delete_cb(lv_event_t* e) {
    lv_obj_t* art = lv_event_get_target_obj(e);
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

bool unit_art_available(const AmsSystemInfo& info, int unit_index) {
    const tray::UnitShell* shell = tray::unit_shell_for(info, unit_index);
    return shell && info.units[static_cast<size_t>(unit_index)].slot_count == shell->art_lanes;
}

void register_unit_art_widget() {
    lv_xml_register_widget("unit_art", xml_create, xml_apply);
}

lv_obj_t* unit_art_create(lv_obj_t* parent, UnitArtTier tier) {
    return make_widget(parent, tier);
}

void unit_art_set_unit(lv_obj_t* art, const AmsSystemInfo& info, int unit_index) {
    ArtData* data = art ? data_of(art) : nullptr;
    if (!data)
        return;
    if (!unit_art_available(info, unit_index)) {
        lv_obj_set_hidden(art, true);
        return;
    }
    const tray::UnitShell* shell = tray::unit_shell_for(info, unit_index);
    if (data->built_id != shell->id)
        build_layers(art, data, shell);
    lv_obj_set_hidden(art, false);

    const AmsUnit& unit = info.units[static_cast<size_t>(unit_index)];
    for (size_t i = 0; i < data->lanes.size(); ++i) {
        const UnitArtLane lane =
            i < unit.slots.size() ? unit_art_lane(unit.slots[i]) : UnitArtLane{};
        set_lane_visible(data->lanes[i].color, lane.filled);
        set_lane_visible(data->lanes[i].shade, lane.filled);
        if (lane.filled) {
            // DECLARATIVE_OK: the live filament colour of this lane.
            lv_obj_set_style_image_recolor(data->lanes[i].color, lv_color_hex(lane.color),
                                           LV_PART_MAIN);
        }
    }
}

lv_obj_t* unit_art_lane_color_obj(lv_obj_t* art, int lane) {
    ArtData* data = art ? data_of(art) : nullptr;
    if (!data || lane < 0 || lane >= static_cast<int>(data->lanes.size()))
        return nullptr;
    return data->lanes[static_cast<size_t>(lane)].color;
}

} // namespace helix::ui
