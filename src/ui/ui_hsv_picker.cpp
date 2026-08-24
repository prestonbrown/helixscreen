// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_hsv_picker.h"

#include "ui_update_queue.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "memory_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

using namespace helix;

namespace {

// Default sizes — smaller on constrained devices (saves ~107KB ARGB8888)
static int32_t default_sv_size() {
    static const int32_t size = helix::get_system_memory_info().is_constrained_device() ? 128 : 200;
    return size;
}
static int32_t default_hue_height() {
    static const int32_t size = helix::get_system_memory_info().is_constrained_device() ? 16 : 24;
    return size;
}
constexpr int32_t DEFAULT_GAP = 8;
constexpr int32_t INDICATOR_RADIUS = 6;
constexpr int32_t INDICATOR_BORDER = 2;

// === Color Conversion Utilities ===

/**
 * @brief Convert HSV to RGB
 * @param h Hue 0-360
 * @param s Saturation 0-100
 * @param v Value 0-100
 * @return RGB packed as 0x00RRGGBB
 */
static uint32_t hsv_to_rgb(float h, float s, float v) {
    s /= 100.0f;
    v /= 100.0f;

    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;

    float r1, g1, b1;
    if (h < 60) {
        r1 = c;
        g1 = x;
        b1 = 0;
    } else if (h < 120) {
        r1 = x;
        g1 = c;
        b1 = 0;
    } else if (h < 180) {
        r1 = 0;
        g1 = c;
        b1 = x;
    } else if (h < 240) {
        r1 = 0;
        g1 = x;
        b1 = c;
    } else if (h < 300) {
        r1 = x;
        g1 = 0;
        b1 = c;
    } else {
        r1 = c;
        g1 = 0;
        b1 = x;
    }

    uint8_t r = static_cast<uint8_t>((r1 + m) * 255.0f);
    uint8_t g = static_cast<uint8_t>((g1 + m) * 255.0f);
    uint8_t b = static_cast<uint8_t>((b1 + m) * 255.0f);

    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

/**
 * @brief Convert RGB to HSV
 * @param rgb RGB packed as 0x00RRGGBB
 * @param h Output hue 0-360
 * @param s Output saturation 0-100
 * @param v Output value 0-100
 */
static void rgb_to_hsv(uint32_t rgb, float& h, float& s, float& v) {
    float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    float g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    float b = static_cast<float>(rgb & 0xFF) / 255.0f;

    float max_val = std::max({r, g, b});
    float min_val = std::min({r, g, b});
    float delta = max_val - min_val;

    v = max_val * 100.0f;

    if (max_val == 0.0f) {
        s = 0.0f;
        h = 0.0f;
        return;
    }

    s = (delta / max_val) * 100.0f;

    if (delta == 0.0f) {
        h = 0.0f;
        return;
    }

    if (max_val == r) {
        h = 60.0f * std::fmod((g - b) / delta, 6.0f);
    } else if (max_val == g) {
        h = 60.0f * ((b - r) / delta + 2.0f);
    } else {
        h = 60.0f * ((r - g) / delta + 4.0f);
    }

    if (h < 0.0f)
        h += 360.0f;
}

// === HSV Picker Data Structure ===

struct HsvPickerData {
    // Current HSV values
    float hue;        // 0-360
    float saturation; // 0-100
    float value;      // 0-100

    // Widget dimensions
    int32_t sv_size;
    int32_t hue_height; // Height of horizontal hue bar
    int32_t gap;

    // Child widgets
    lv_obj_t* sv_image;      // Saturation-Value square
    lv_obj_t* hue_image;     // Hue bar
    lv_obj_t* sv_indicator;  // Crosshair on SV square
    lv_obj_t* hue_indicator; // Line on hue bar

    // Draw buffers
    lv_draw_buf_t* sv_buf;
    lv_draw_buf_t* hue_buf;

    // Callback
    HsvPickerCallback callback;
    void* callback_user_data;
};

// === Rendering Functions ===

/**
 * @brief Render the Saturation-Value square for the current hue
 */
static void render_sv_square(HsvPickerData* data) {
    if (!data || !data->sv_buf)
        return;

    uint8_t* buf = data->sv_buf->data;
    uint32_t stride = data->sv_buf->header.stride;
    int32_t size = data->sv_size;

    for (int32_t y = 0; y < size; y++) {
        lv_color32_t* row =
            reinterpret_cast<lv_color32_t*>(buf + static_cast<uint32_t>(y) * stride);
        float val =
            100.0f * (1.0f - static_cast<float>(y) / (size - 1)); // Value: top=100, bottom=0

        for (int32_t x = 0; x < size; x++) {
            float sat =
                100.0f * static_cast<float>(x) / (size - 1); // Saturation: left=0, right=100
            uint32_t rgb = hsv_to_rgb(data->hue, sat, val);

            row[x].red = (rgb >> 16) & 0xFF;
            row[x].green = (rgb >> 8) & 0xFF;
            row[x].blue = rgb & 0xFF;
            row[x].alpha = 255;
        }
    }
}

/**
 * @brief Render the Hue bar (horizontal rainbow)
 */
static void render_hue_bar(HsvPickerData* data) {
    if (!data || !data->hue_buf)
        return;

    uint8_t* buf = data->hue_buf->data;
    uint32_t stride = data->hue_buf->header.stride;
    int32_t width = data->sv_size; // Same width as SV square
    int32_t height = data->hue_height;

    for (int32_t y = 0; y < height; y++) {
        lv_color32_t* row =
            reinterpret_cast<lv_color32_t*>(buf + static_cast<uint32_t>(y) * stride);

        for (int32_t x = 0; x < width; x++) {
            float hue = 360.0f * static_cast<float>(x) / (width - 1); // Hue: left=0, right=360
            uint32_t rgb = hsv_to_rgb(hue, 100.0f, 100.0f);           // Full saturation/value

            row[x].red = (rgb >> 16) & 0xFF;
            row[x].green = (rgb >> 8) & 0xFF;
            row[x].blue = rgb & 0xFF;
            row[x].alpha = 255;
        }
    }
}

/**
 * @brief Update indicator positions based on current HSV
 */
static void update_indicators(HsvPickerData* data) {
    if (!data)
        return;

    // SV indicator position
    if (data->sv_indicator) {
        int32_t x = static_cast<int32_t>(data->saturation / 100.0f * (data->sv_size - 1));
        int32_t y = static_cast<int32_t>((1.0f - data->value / 100.0f) * (data->sv_size - 1));
        lv_obj_set_pos(data->sv_indicator, x - INDICATOR_RADIUS, y - INDICATOR_RADIUS);
    }

    // Hue indicator position (horizontal bar: vertical line indicator)
    if (data->hue_indicator) {
        int32_t x = static_cast<int32_t>(data->hue / 360.0f * (data->sv_size - 1));
        lv_obj_set_x(data->hue_indicator, x - 2); // Center the 4px wide indicator
    }
}

/**
 * @brief Notify callback of color change
 */
static void notify_color_changed(HsvPickerData* data) {
    if (data && data->callback) {
        uint32_t rgb = hsv_to_rgb(data->hue, data->saturation, data->value);
        data->callback(rgb, data->callback_user_data);
    }
}

// === Event Handlers ===

static void sv_touch_handler(lv_event_t* e) {
    lv_obj_t* sv_img = lv_event_get_target_obj(e);
    lv_obj_t* picker = lv_obj_get_parent(sv_img);
    HsvPickerData* data = static_cast<HsvPickerData*>(lv_obj_get_user_data(picker));
    if (!data)
        return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    // Get widget's absolute screen position
    lv_area_t coords;
    lv_obj_get_coords(sv_img, &coords);

    // Convert screen coordinates to local widget coordinates
    int32_t x = point.x - coords.x1;
    int32_t y = point.y - coords.y1;

    // Clamp to bounds
    x = std::clamp<int32_t>(x, 0, data->sv_size - 1);
    y = std::clamp<int32_t>(y, 0, data->sv_size - 1);

    // Calculate saturation and value
    data->saturation = 100.0f * x / (data->sv_size - 1);
    data->value = 100.0f * (1.0f - static_cast<float>(y) / (data->sv_size - 1));

    update_indicators(data);
    notify_color_changed(data);
}

static void hue_touch_handler(lv_event_t* e) {
    lv_obj_t* hue_img = lv_event_get_target_obj(e);
    lv_obj_t* picker = lv_obj_get_parent(hue_img);
    HsvPickerData* data = static_cast<HsvPickerData*>(lv_obj_get_user_data(picker));
    if (!data)
        return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    // Get widget's absolute screen position
    lv_area_t coords;
    lv_obj_get_coords(hue_img, &coords);

    // Convert screen coordinates to local widget coordinates (horizontal bar)
    int32_t x = point.x - coords.x1;

    // Clamp to bounds
    x = std::clamp<int32_t>(x, 0, data->sv_size - 1);

    // Calculate hue (left=0, right=360)
    data->hue = 360.0f * x / (data->sv_size - 1);

    // Re-render SV square with new hue
    render_sv_square(data);
    // Defer invalidation to avoid calling during render phase
    helix::ui::async_call(
        data->sv_image, [](void* d) { lv_obj_invalidate(static_cast<lv_obj_t*>(d)); },
        data->sv_image);

    update_indicators(data);
    notify_color_changed(data);
}

static void picker_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    // Transfer ownership to unique_ptr for RAII cleanup (exception-safe)
    std::unique_ptr<HsvPickerData> data(static_cast<HsvPickerData*>(lv_obj_get_user_data(obj)));
    lv_obj_set_user_data(obj, nullptr);

    if (data) {
        if (data->sv_buf) {
            lv_draw_buf_destroy(data->sv_buf);
        }
        if (data->hue_buf) {
            lv_draw_buf_destroy(data->hue_buf);
        }
        // data automatically freed by ~unique_ptr()
    }
}

// === XML Widget Handlers ===

static void* ui_hsv_picker_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* picker = lv_obj_create(static_cast<lv_obj_t*>(parent));

    if (!picker) {
        spdlog::error("[HsvPicker] Failed to create container");
        return nullptr;
    }

    // Initialize data using RAII pattern
    auto data_ptr = std::make_unique<HsvPickerData>();
    data_ptr->hue = 0.0f;
    data_ptr->saturation = 100.0f;
    data_ptr->value = 100.0f;
    data_ptr->sv_size = default_sv_size();
    data_ptr->hue_height = default_hue_height();
    data_ptr->gap = DEFAULT_GAP;
    data_ptr->callback = nullptr;
    data_ptr->callback_user_data = nullptr;

    // Transfer ownership to LVGL widget
    lv_obj_set_user_data(picker, data_ptr.release());

    // Container styling - NO flex, use explicit positioning
    lv_obj_set_style_bg_opa(picker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(picker, 0, 0);
    lv_obj_set_style_pad_all(picker, 0, 0);
    lv_obj_remove_flag(picker, LV_OBJ_FLAG_SCROLLABLE);

    // Cleanup handler
    lv_obj_add_event_cb(picker, picker_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::debug("[HsvPicker] Created picker container");
    return static_cast<void*>(picker);
}

static void ui_hsv_picker_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* picker = static_cast<lv_obj_t*>(item);

    if (!picker)
        return;

    HsvPickerData* data = static_cast<HsvPickerData*>(lv_obj_get_user_data(picker));
    if (!data)
        return;

    // Parse custom attributes
    for (int i = 0; attrs[i] && attrs[i + 1]; i += 2) {
        if (strcmp(attrs[i], "sv_size") == 0) {
            data->sv_size = lv_xml_atoi(attrs[i + 1]);
        } else if (strcmp(attrs[i], "hue_height") == 0) {
            data->hue_height = lv_xml_atoi(attrs[i + 1]);
        } else if (strcmp(attrs[i], "gap") == 0) {
            data->gap = lv_xml_atoi(attrs[i + 1]);
            lv_obj_set_style_pad_row(picker, data->gap, 0);
        }
    }

    // Apply standard obj properties
    lv_xml_obj_apply(state, attrs);

    // Create SV square buffer
    data->sv_buf = lv_draw_buf_create(data->sv_size, data->sv_size, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!data->sv_buf) {
        spdlog::error("[HsvPicker] Failed to create SV buffer");
        return;
    }

    // Create Hue bar buffer (horizontal: width=sv_size, height=hue_height)
    data->hue_buf =
        lv_draw_buf_create(data->sv_size, data->hue_height, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!data->hue_buf) {
        spdlog::error("[HsvPicker] Failed to create hue buffer");
        return;
    }

    // Render initial gradients
    render_sv_square(data);
    render_hue_bar(data);

    // Set container size explicitly (SV square + gap + hue bar)
    int32_t total_height = data->sv_size + data->gap + data->hue_height;
    lv_obj_set_size(picker, data->sv_size, total_height);

    // Create SV image widget (saturation-value square) at top
    data->sv_image = lv_image_create(picker);
    lv_image_set_src(data->sv_image, data->sv_buf);
    lv_obj_set_size(data->sv_image, data->sv_size, data->sv_size);
    lv_obj_set_pos(data->sv_image, 0, 0); // Top-left
    lv_obj_add_flag(data->sv_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(data->sv_image, 4, 0);
    lv_obj_add_event_cb(data->sv_image, sv_touch_handler, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(data->sv_image, sv_touch_handler, LV_EVENT_CLICKED, nullptr);

    // Create Hue bar image widget (horizontal rainbow) BELOW SV square
    data->hue_image = lv_image_create(picker);
    lv_image_set_src(data->hue_image, data->hue_buf);
    lv_obj_set_size(data->hue_image, data->sv_size, data->hue_height);
    lv_obj_set_pos(data->hue_image, 0, data->sv_size + data->gap); // Below SV square
    lv_obj_add_flag(data->hue_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(data->hue_image, 4, 0);
    lv_obj_add_event_cb(data->hue_image, hue_touch_handler, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(data->hue_image, hue_touch_handler, LV_EVENT_CLICKED, nullptr);

    // Create SV indicator (circular crosshair)
    data->sv_indicator = lv_obj_create(data->sv_image);
    lv_obj_set_size(data->sv_indicator, INDICATOR_RADIUS * 2, INDICATOR_RADIUS * 2);
    lv_obj_set_style_radius(data->sv_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(data->sv_indicator, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(data->sv_indicator, INDICATOR_BORDER, 0);
    lv_obj_set_style_border_color(data->sv_indicator, lv_color_white(), 0);
    lv_obj_set_style_shadow_width(data->sv_indicator, 2, 0);
    lv_obj_set_style_shadow_color(data->sv_indicator, lv_color_black(), 0);
    lv_obj_remove_flag(data->sv_indicator, LV_OBJ_FLAG_CLICKABLE);

    // Create Hue indicator (vertical line for horizontal bar)
    data->hue_indicator = lv_obj_create(data->hue_image);
    lv_obj_set_size(data->hue_indicator, 4, data->hue_height);
    lv_obj_set_style_bg_opa(data->hue_indicator, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(data->hue_indicator, INDICATOR_BORDER, 0);
    lv_obj_set_style_border_color(data->hue_indicator, lv_color_white(), 0);
    lv_obj_set_style_shadow_width(data->hue_indicator, 2, 0);
    lv_obj_set_style_shadow_color(data->hue_indicator, lv_color_black(), 0);
    lv_obj_remove_flag(data->hue_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_y(data->hue_indicator, 0);

    // Position indicators at initial HSV
    update_indicators(data);

    spdlog::debug("[HsvPicker] Applied (sv_size={}, hue_height={}, gap={})", data->sv_size,
                  data->hue_height, data->gap);
}

} // anonymous namespace

// === Public API ===

void ui_hsv_picker_register() {
    lv_xml_register_widget("ui_hsv_picker", ui_hsv_picker_xml_create, ui_hsv_picker_xml_apply);
    spdlog::trace("[HsvPicker] Registered <ui_hsv_picker> widget");
}

void ui_hsv_picker_set_color_rgb(lv_obj_t* obj, uint32_t rgb) {
    if (!obj)
        return;
    HsvPickerData* data = static_cast<HsvPickerData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    rgb_to_hsv(rgb, data->hue, data->saturation, data->value);

    // Re-render SV square with new hue
    render_sv_square(data);
    if (data->sv_image) {
        // Defer invalidation to avoid calling during render phase
        helix::ui::async_call(
            data->sv_image, [](void* d) { lv_obj_invalidate(static_cast<lv_obj_t*>(d)); },
            data->sv_image);
    }

    update_indicators(data);
}

uint32_t ui_hsv_picker_get_color_rgb(lv_obj_t* obj) {
    if (!obj)
        return 0;
    HsvPickerData* data = static_cast<HsvPickerData*>(lv_obj_get_user_data(obj));
    if (!data)
        return 0;

    return hsv_to_rgb(data->hue, data->saturation, data->value);
}

void ui_hsv_picker_set_callback(lv_obj_t* obj, HsvPickerCallback callback, void* user_data) {
    if (!obj)
        return;
    HsvPickerData* data = static_cast<HsvPickerData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    data->callback = callback;
    data->callback_user_data = user_data;
}

void ui_hsv_picker_set_hsv(lv_obj_t* obj, float hue, float sat, float val) {
    if (!obj)
        return;
    HsvPickerData* data = static_cast<HsvPickerData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    data->hue = std::clamp(hue, 0.0f, 360.0f);
    data->saturation = std::clamp(sat, 0.0f, 100.0f);
    data->value = std::clamp(val, 0.0f, 100.0f);

    render_sv_square(data);
    if (data->sv_image) {
        // Defer invalidation to avoid calling during render phase
        helix::ui::async_call(
            data->sv_image, [](void* d) { lv_obj_invalidate(static_cast<lv_obj_t*>(d)); },
            data->sv_image);
    }

    update_indicators(data);
}

void ui_hsv_picker_get_hsv(lv_obj_t* obj, float* hue, float* sat, float* val) {
    if (!obj)
        return;
    HsvPickerData* data = static_cast<HsvPickerData*>(lv_obj_get_user_data(obj));
    if (!data)
        return;

    if (hue)
        *hue = data->hue;
    if (sat)
        *sat = data->saturation;
    if (val)
        *val = data->value;
}
