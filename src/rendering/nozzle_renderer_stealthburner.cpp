// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/// @file nozzle_renderer_stealthburner.cpp
/// @brief Voron Stealthburner toolhead renderer implementation
///
/// Ported from toolhead_icon_scaled.c - a traced SVG of the Stealthburner
/// using LVGL polygon primitives for vector rendering.

#include "nozzle_renderer_stealthburner.h"

#include "nozzle_renderer_common.h"

#include <cmath>
#include <iterator>

// ============================================================================
// Polygon Data (1000x1000 design space, centered at 500,500)
// Traced from Voron Stealthburner SVG
// ============================================================================

static const lv_point_t pts_housing[] = {
    {583, 928}, {560, 920}, {554, 914}, {538, 872}, {530, 877}, {529, 896}, {504, 900}, {497, 908},
    {492, 910}, {484, 900}, {459, 896}, {456, 892}, {458, 877}, {446, 870}, {430, 916}, {408, 928},
    {380, 926}, {296, 892}, {274, 832}, {290, 774}, {287, 770}, {280, 769}, {278, 738}, {298, 736},
    {302, 726}, {302, 679}, {308, 672}, {316, 575}, {316, 466}, {290, 328}, {290, 296}, {292, 233},
    {304, 203}, {318, 191}, {308, 180}, {286, 169}, {286, 156}, {292, 152}, {335, 160}, {359, 158},
    {389, 134}, {402, 126}, {436, 114}, {463, 112}, {478, 97},  {478, 84},  {483, 78},  {488, 81},
    {484, 90},  {490, 96},  {497, 96},  {502, 91},  {500, 80},  {504, 78},  {508, 85},  {508, 97},
    {524, 112}, {546, 112}, {590, 128}, {682, 204}, {694, 234}, {698, 286}, {698, 333}, {668, 528},
    {668, 577}, {682, 719}, {710, 833}, {694, 885}, {688, 892}, {618, 920}, {583, 928},
};

static const lv_point_t pts_plate[] = {
    {580, 926}, {561, 920}, {554, 914}, {538, 864}, {521, 848}, {499, 840}, {485, 840}, {463, 848},
    {444, 867}, {438, 895}, {430, 914}, {408, 926}, {384, 924}, {317, 898}, {312, 894}, {314, 888},
    {290, 796}, {318, 726}, {326, 629}, {316, 604}, {316, 587}, {318, 459}, {306, 348}, {310, 270},
    {322, 217}, {332, 203}, {403, 144}, {426, 132}, {558, 132}, {568, 136}, {641, 194}, {658, 211},
    {674, 266}, {676, 355}, {668, 443}, {668, 591}, {668, 604}, {656, 632}, {660, 638}, {666, 725},
    {694, 791}, {694, 799}, {670, 894}, {603, 924}, {580, 926},
};

static const lv_point_t pts_top_circle[] = {
    {507, 398}, {476, 398}, {444, 392}, {422, 367}, {400, 327}, {396, 304},
    {408, 272}, {432, 238}, {451, 224}, {496, 218}, {537, 226}, {564, 256},
    {586, 302}, {578, 338}, {556, 373}, {540, 390}, {507, 398},
};

// The bottom fan is a simple circle (see draw_circle in draw_nozzle_stealthburner):
// a polygon with fan blade details triangulates with artifacts

static const lv_point_t pts_logo_1[] = {
    {457, 498}, {472, 472}, {474, 470}, {485, 470}, {469, 499},
};

static const lv_point_t pts_logo_2[] = {
    {468, 530}, {502, 471}, {515, 470}, {481, 529}, {479, 531},
};

static const lv_point_t pts_logo_3[] = {
    {497, 530},
    {513, 502},
    {525, 502},
    {509, 531},
};

// Facet polygons for 3D shading effect
static const lv_point_t pts_facet_1[] = {
    {663, 898}, {640, 869}, {658, 787}, {648, 758}, {640, 628}, {592, 445}, {600, 423}, {610, 418},
    {606, 409}, {612, 406}, {624, 372}, {630, 369}, {628, 362}, {642, 334}, {640, 299}, {588, 206},
    {580, 200}, {584, 202}, {584, 195}, {578, 198}, {578, 189}, {574, 188}, {601, 180}, {605, 184},
    {606, 178}, {611, 186}, {614, 174}, {612, 188}, {605, 186}, {604, 194}, {601, 192}, {595, 200},
    {588, 196}, {586, 201}, {598, 204}, {601, 196}, {608, 192}, {612, 194}, {616, 188}, {618, 192},
    {609, 202}, {627, 200}, {627, 192}, {632, 198}, {636, 192}, {643, 196}, {660, 215}, {674, 269},
    {676, 355}, {666, 445}, {666, 605}, {656, 632}, {666, 725}, {694, 799}, {672, 888}, {663, 898},
};

static const lv_point_t pts_facet_2[] = {
    {587, 892}, {584, 884}, {582, 890}, {570, 888}, {574, 879}, {562, 877}, {562, 865}, {552, 854},
    {558, 849}, {544, 839}, {550, 836}, {555, 842}, {556, 831}, {541, 838}, {512, 822}, {516, 832},
    {512, 836}, {508, 822}, {520, 818}, {508, 804}, {520, 806}, {519, 796}, {525, 804}, {526, 795},
    {542, 788}, {550, 794}, {550, 787}, {596, 747}, {606, 722}, {617, 736}, {634, 732}, {636, 746},
    {636, 732}, {643, 730}, {648, 772}, {642, 779}, {650, 782}, {644, 785}, {648, 794}, {656, 795},
    {648, 825}, {638, 834}, {644, 837}, {638, 846}, {642, 848}, {638, 864}, {622, 876}, {614, 866},
    {616, 876}, {609, 882}, {592, 876}, {590, 884}, {602, 881}, {587, 892},
};

static const lv_point_t pts_facet_3[] = {
    {498, 790}, {481, 784}, {468, 770}, {464, 750}, {470, 738}, {442, 718}, {436, 718}, {442, 710},
    {432, 691}, {432, 674}, {440, 648}, {456, 630}, {484, 618}, {520, 624}, {526, 612}, {534, 616},
    {543, 610}, {546, 590}, {534, 574}, {546, 583}, {552, 596}, {546, 620}, {528, 633}, {544, 653},
    {550, 667}, {548, 705}, {540, 720}, {554, 732}, {582, 742}, {566, 750}, {550, 750}, {527, 730},
    {503, 740}, {472, 738}, {472, 765}, {486, 780}, {495, 780}, {498, 790},
};

static const lv_point_t pts_facet_4[] = {
    {343, 626}, {318, 554}, {320, 461}, {316, 445}, {320, 442}, {314, 438}, {312, 420}, {338, 348},
    {336, 344}, {340, 343}, {340, 334}, {364, 384}, {362, 389}, {368, 395}, {390, 443}, {343, 626},
};

static const lv_point_t pts_facet_5[] = {
    {391, 206}, {374, 204}, {373, 198}, {367, 202}, {344, 196}, {425, 134},
    {559, 134}, {576, 145}, {576, 150}, {550, 178}, {428, 176}, {391, 206},
};

static const lv_point_t pts_facet_6[] = {
    {431, 452}, {420, 449}, {424, 430}, {404, 410}, {392, 388}, {387, 384}, {384, 388}, {380, 385},
    {388, 379}, {378, 374}, {384, 371}, {376, 368}, {378, 362}, {364, 343}, {366, 340}, {361, 334},
    {358, 340}, {354, 337}, {362, 321}, {356, 316}, {376, 274}, {372, 266}, {376, 260}, {382, 262},
    {386, 256}, {378, 248}, {384, 244}, {392, 248}, {406, 224}, {406, 211}, {411, 208}, {413, 216},
    {418, 210}, {424, 213}, {416, 233}, {416, 246}, {408, 249}, {408, 255}, {402, 263}, {404, 273},
    {398, 279}, {400, 289}, {396, 296}, {394, 320}, {410, 354}, {436, 390}, {436, 408}, {432, 418},
    {426, 420}, {434, 427}, {426, 428}, {426, 434}, {434, 439}, {428, 443}, {434, 449}, {431, 452},
};

static const lv_point_t pts_facet_7[] = {
    {406, 272}, {404, 265}, {410, 263}, {407, 258}, {402, 261}, {408, 255}, {408, 249}, {416, 246},
    {418, 228}, {426, 211}, {422, 207}, {428, 202}, {514, 196}, {547, 200}, {556, 207}, {552, 212},
    {552, 234}, {535, 224}, {510, 218}, {445, 224}, {420, 249}, {406, 272},
};

static const lv_point_t pts_facet_8[] = {
    {490, 436}, {484, 430}, {482, 436}, {479, 430}, {475, 434}, {464, 428}, {453, 436},
    {450, 434}, {451, 428}, {436, 430}, {430, 423}, {433, 416}, {436, 418}, {434, 403},
    {440, 396}, {436, 389}, {454, 396}, {486, 400}, {539, 396}, {545, 426}, {536, 424},
    {537, 432}, {527, 428}, {521, 436}, {518, 426}, {513, 432}, {506, 428}, {490, 436},
};

static const lv_point_t pts_facet_9[] = {
    {596, 306}, {588, 304}, {576, 269}, {558, 246}, {560, 238}, {554, 234}, {557, 190}, {572, 195},
    {560, 201}, {570, 204}, {562, 209}, {576, 208}, {584, 217}, {576, 227}, {580, 232}, {586, 225},
    {586, 243}, {594, 238}, {586, 237}, {595, 224}, {600, 232}, {594, 237}, {602, 236}, {602, 247},
    {614, 255}, {592, 245}, {588, 253}, {599, 264}, {601, 254}, {606, 263}, {596, 268}, {596, 277},
    {609, 276}, {611, 270}, {603, 274}, {602, 270}, {618, 262}, {624, 285}, {630, 284}, {628, 290},
    {636, 294}, {624, 292}, {624, 302}, {616, 296}, {610, 276}, {602, 282}, {604, 304}, {596, 306},
};

static const lv_point_t pts_facet_10[] = {
    {323, 898}, {314, 889}, {318, 885}, {312, 884}, {312, 870}, {302, 841}, {308, 838}, {300, 837},
    {290, 796}, {291, 792}, {293, 796}, {307, 788}, {324, 791}, {344, 867}, {323, 898},
};

// Maximum polygon size (pts_housing has 71 points)
// ============================================================================
// Helper Functions
// ============================================================================

/// @brief Draw a filled circle using triangle fan (clean, simple rendering)
/// @param layer LVGL draw layer
/// @param cx Center X in screen coordinates
/// @param cy Center Y in screen coordinates
/// @param radius Circle radius in pixels
/// @param color Fill color
/// @param segments Number of segments (more = smoother, 24 is good)
static void draw_circle(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t radius, lv_color_t color,
                        int segments = 24) {
    lv_draw_triangle_dsc_t tri_dsc;
    lv_draw_triangle_dsc_init(&tri_dsc);
    tri_dsc.color = color;
    tri_dsc.opa = LV_OPA_COVER;

    for (int i = 0; i < segments; i++) {
        float angle1 = (float)i * 2.0f * 3.14159265f / (float)segments;
        float angle2 = (float)(i + 1) * 2.0f * 3.14159265f / (float)segments;

        tri_dsc.p[0].x = cx;
        tri_dsc.p[0].y = cy;
        tri_dsc.p[1].x = cx + (int32_t)(radius * cosf(angle1));
        tri_dsc.p[1].y = cy + (int32_t)(radius * sinf(angle1));
        tri_dsc.p[2].x = cx + (int32_t)(radius * cosf(angle2));
        tri_dsc.p[2].y = cy + (int32_t)(radius * sinf(angle2));
        lv_draw_triangle(layer, &tri_dsc);
    }
}

// Visual center of the Stealthburner polygon data (not exactly 500,500)
// Measured from bounding box: X=[274,710] midpoint=492, Y=[78,928] midpoint=503
static constexpr int kDesignCenterX = 492;
static constexpr int kDesignCenterY = 500;

namespace {
enum : uint8_t {
    HOUSING,
    PRIMARY,
    HIGHLIGHT,
    MID_SHADOW,
    SHADOW,
    DEEP_SHADOW,
    SOFT_HIGHLIGHT,
    RECESS,
    LOGO,
};
} // namespace

// ============================================================================
// Main Drawing Function
// ============================================================================

void draw_nozzle_stealthburner(lv_layer_t* layer, int32_t cx, int32_t cy,
                               std::optional<lv_color_t> filament, int32_t scale_unit,
                               lv_opa_t opa) {
    // The design space is 1000x1000, but the actual toolhead spans about
    // 440 units wide (280-720) and 850 units tall (78-928)
    // Stealthburner is larger than Bambu toolhead, so render at 2x
    int32_t render_size = scale_unit * 10;
    float scale = (float)render_size / 1000.0f;

    // Body color is ALWAYS Voron red - the toolhead housing doesn't change
    lv_color_t primary = helix::nr_dim(lv_color_hex(0xD11D1D), opa);

    // Facet shading colors using nr_lighten/nr_darken for consistent 3D effect
    // Original SVG colors: highlights=#f55c5b, shadows=#c31615/#bd0f10, deep=#4b1514
    const lv_color_t palette[] = {
        helix::nr_dim(lv_color_hex(0x121212), opa), // HOUSING
        primary,
        nr_lighten(primary, 60),                    // HIGHLIGHT: bright highlight facets
        nr_darken(primary, 30),                     // MID_SHADOW: slight shadow
        nr_darken(primary, 50),                     // SHADOW: medium shadow
        nr_darken(primary, 80),                     // DEEP_SHADOW
        nr_lighten(primary, 20),                    // SOFT_HIGHLIGHT
        helix::nr_dim(lv_color_hex(0x100C0B), opa), // RECESS
        helix::nr_dim(lv_color_black(), opa),       // LOGO
    };

    static const helix::NrPolygon body_polys[] = {
        helix::nr_poly(pts_housing, HOUSING),        // Dark frame outline
        helix::nr_poly(pts_plate, PRIMARY),          // Main plate
        helix::nr_poly(pts_facet_1, HIGHLIGHT),      // Right side, lit by top-left light
        helix::nr_poly(pts_facet_2, MID_SHADOW),     // Bottom area
        helix::nr_poly(pts_facet_3, DEEP_SHADOW),    // Fan area shadow
        helix::nr_poly(pts_facet_4, HIGHLIGHT),      // Left side
        helix::nr_poly(pts_facet_5, SOFT_HIGHLIGHT), // Top area
        helix::nr_poly(pts_facet_6, SHADOW),         // Left bevel
        helix::nr_poly(pts_facet_7, DEEP_SHADOW),    // Motor recess area
        helix::nr_poly(pts_facet_8, DEEP_SHADOW),    // Top center recess
        helix::nr_poly(pts_facet_9, SHADOW),         // Right bevel
        helix::nr_poly(pts_facet_10, HIGHLIGHT),     // Bottom left
        helix::nr_poly(pts_top_circle, RECESS),      // Extruder motor recess
    };
    helix::nr_draw_polygons(layer, body_polys, std::size(body_polys), palette, cx, cy, scale,
                            {kDesignCenterX, kDesignCenterY});

    // Bottom circle (fan) - simple filled circle instead of complex polygon
    // Fan center is at (490, 690) in design space with radius ~115
    int32_t fan_cx = cx + (int32_t)((490 - kDesignCenterX) * scale);
    int32_t fan_cy = cy + (int32_t)((690 - kDesignCenterY) * scale);
    int32_t fan_radius = (int32_t)(115 * scale);
    draw_circle(layer, fan_cx, fan_cy, fan_radius, palette[RECESS], 32);

    // Logo stripes (Voron logo)
    static const helix::NrPolygon logo_polys[] = {
        helix::nr_poly(pts_logo_1, LOGO),
        helix::nr_poly(pts_logo_2, LOGO),
        helix::nr_poly(pts_logo_3, LOGO),
    };
    helix::nr_draw_polygons(layer, logo_polys, std::size(logo_polys), palette, cx, cy, scale,
                            {kDesignCenterX, kDesignCenterY});

    // Nozzle tip indicator at bottom (shows filament color when loaded)
    // Position below the Stealthburner body (body bottom is ~Y=898)
    // Y=920 places the tip just below the housing where the nozzle emerges
    // Offset +2px right, -4px up to align with the Stealthburner body's nozzle opening
    int32_t tip_cx = cx;
    int32_t nozzle_top_y = cy + (int32_t)((920 - kDesignCenterY) * scale) - 6;
    int32_t nozzle_height = (int32_t)(40 * scale); // Small tip
    int32_t nozzle_top_width = (int32_t)(60 * scale);
    int32_t nozzle_bottom_width = (int32_t)(20 * scale);

    helix::nr_draw_filament_tip(layer, tip_cx, nozzle_top_y, nozzle_top_width, nozzle_bottom_width,
                                nozzle_height, filament, opa);
}
