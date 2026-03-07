// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/// @file nozzle_renderer_a4t.cpp
/// @brief Armored Turtle (A4T) toolhead renderer implementation
///
/// Traced from A4T SVG — polygon-based rendering using LVGL triangle
/// primitives with ear-clipping triangulation, matching the approach
/// of nozzle_renderer_faceted.cpp (Stealthburner).

#include "nozzle_renderer_a4t.h"

#include "nozzle_renderer_common.h"

#include <cmath>

// ============================================================================
// Polygon Data (design space: width=1000, height=1334)
// Traced from Armored Turtle (A4T) SVG (viewBox 0 0 766 1021)
// Visual center measured from bounding box: X=500, Y=667
// ============================================================================

static constexpr int DESIGN_CENTER_X = 500;
static constexpr int DESIGN_CENTER_Y = 630;

// --- Silhouette (overall outline) ---
static const lv_point_t pts_silhouette[] = {
    {135, 673}, {125, 1169}, {172, 1174}, {255, 1284}, {370, 1289}, {386, 1263}, {386, 1211},
    {433, 1169}, {563, 1169}, {616, 1216}, {610, 1253}, {621, 1258}, {626, 1289}, {684, 1279},
    {720, 1289}, {757, 1274}, {819, 1180}, {887, 1169}, {877, 668}, {819, 657}, {819, 626},
    {793, 600}, {798, 522}, {819, 501}, {866, 485}, {866, 349}, {819, 308}, {819, 292}, {845, 271},
    {840, 250}, {746, 161}, {757, 104}, {704, 93}, {668, 114}, {668, 83}, {642, 78}, {642, 0},
    {548, 0}, {553, 78}, {522, 83}, {516, 140}, {469, 135}, {454, 140}, {459, 156}, {396, 151},
    {365, 167}, {328, 271}, {334, 323}, {323, 328}, {318, 360}, {328, 370}, {328, 469}, {214, 501},
    {214, 610}, {198, 616}, {198, 652},
};
static constexpr int pts_silhouette_cnt = sizeof(pts_silhouette) / sizeof(pts_silhouette[0]);

// Nozzle / copper area silhouette (separate piece below body)
static const lv_point_t pts_silhouette_nozzle[] = {
    {412, 1211}, {433, 1221}, {433, 1274}, {443, 1279}, {443, 1289}, {475, 1295}, {496, 1310},
    {506, 1331}, {516, 1305}, {527, 1305}, {532, 1295}, {563, 1295}, {563, 1284}, {574, 1279},
    {574, 1237}, {590, 1211}, {558, 1190}, {433, 1190},
};
static constexpr int pts_silhouette_nozzle_cnt =
    sizeof(pts_silhouette_nozzle) / sizeof(pts_silhouette_nozzle[0]);

// --- Body (main gray surface) ---
static const lv_point_t pts_body_0[] = {
    {168, 1006}, {171, 1140}, {211, 1138}, {315, 1282}, {351, 1285}, {387, 1211}, {421, 1176},
    {484, 1168}, {585, 1180}, {644, 1284}, {679, 1283}, {784, 1138}, {831, 1149}, {832, 1028},
    {845, 1024}, {866, 1077}, {883, 1063}, {862, 882}, {846, 1007}, {813, 987}, {805, 846},
    {808, 671}, {874, 668}, {820, 656}, {815, 620}, {793, 603}, {766, 613}, {803, 658}, {787, 677},
    {745, 630}, {708, 616}, {648, 618}, {599, 659}, {395, 656}, {347, 617}, {291, 616}, {257, 626},
    {209, 676}, {196, 659}, {210, 645}, {212, 638}, {192, 656}, {142, 665}, {138, 674}, {188, 668},
    {193, 680}, {195, 995}, {185, 1032}, {186, 1006},
};
static constexpr int pts_body_0_cnt = sizeof(pts_body_0) / sizeof(pts_body_0[0]);

static const lv_point_t pts_body_1[] = {
    {331, 269}, {328, 371}, {388, 429}, {373, 468}, {377, 477}, {313, 481}, {216, 503}, {217, 546},
    {586, 541}, {794, 548}, {793, 521}, {805, 506}, {803, 500}, {823, 488}, {868, 445}, {871, 435},
    {867, 402}, {861, 397}, {855, 425}, {838, 457}, {787, 499}, {686, 480}, {654, 479}, {613, 444},
    {599, 442}, {613, 464}, {610, 498}, {593, 513}, {563, 510}, {550, 493}, {550, 461}, {577, 443},
    {578, 437}, {551, 433}, {518, 398}, {517, 316}, {537, 297}, {603, 261}, {638, 172}, {633, 179},
    {584, 163}, {528, 217}, {507, 218}, {394, 324}, {357, 270}, {344, 226},
};
static constexpr int pts_body_1_cnt = sizeof(pts_body_1) / sizeof(pts_body_1[0]);

static const lv_point_t pts_body_2[] = {
    {805, 270}, {740, 164}, {729, 194}, {775, 267}, {781, 284}, {781, 316}, {777, 333}, {752, 363},
    {753, 366}, {763, 364}, {775, 366}, {780, 362}, {784, 364}, {764, 400}, {772, 398}, {823, 364},
    {818, 361}, {815, 342}, {811, 340}, {808, 333}, {811, 329}, {809, 272},
};
static constexpr int pts_body_2_cnt = sizeof(pts_body_2) / sizeof(pts_body_2[0]);

// --- Surface detail (mid-gray) ---
static const lv_point_t pts_detail_0[] = {
    {459, 150}, {462, 158}, {453, 160}, {422, 150}, {409, 150}, {375, 158}, {366, 167}, {344, 227},
    {355, 270}, {392, 327}, {400, 324}, {508, 220}, {563, 217}, {585, 166}, {636, 180}, {644, 159},
    {654, 160}, {650, 150}, {651, 145}, {655, 140}, {663, 140}, {666, 131}, {647, 124}, {641, 113},
    {632, 107}, {597, 100}, {583, 107}, {573, 120}, {528, 137}, {540, 140}, {572, 129}, {561, 160},
    {530, 156}, {534, 152}, {548, 154}, {553, 147}, {483, 146},
};
static constexpr int pts_detail_0_cnt = sizeof(pts_detail_0) / sizeof(pts_detail_0[0]);

static const lv_point_t pts_detail_1[] = {
    {196, 655}, {195, 661}, {200, 674}, {209, 678}, {215, 675}, {229, 657}, {264, 623}, {290, 618},
    {344, 618}, {359, 624}, {406, 670}, {417, 675}, {442, 678}, {573, 678}, {592, 669}, {634, 627},
    {652, 618}, {701, 618}, {715, 618}, {737, 625}, {785, 679}, {800, 677}, {805, 661}, {797, 646},
    {768, 615}, {790, 608}, {733, 598}, {681, 597}, {650, 597}, {632, 602}, {581, 651}, {569, 656},
    {547, 658}, {424, 655}, {409, 645}, {373, 607}, {357, 601}, {286, 598}, {222, 606}, {218, 604},
    {209, 611}, {231, 612}, {232, 614},
};
static constexpr int pts_detail_1_cnt = sizeof(pts_detail_1) / sizeof(pts_detail_1[0]);

static const lv_point_t pts_detail_2[] = {
    {329, 322}, {324, 331}, {330, 337}, {321, 344}, {326, 353}, {320, 364}, {327, 371}, {320, 380},
    {326, 388}, {322, 396}, {325, 401}, {334, 403}, {327, 414}, {339, 418}, {335, 429}, {346, 433},
    {343, 445}, {357, 445}, {353, 457}, {357, 460}, {368, 457}, {366, 473}, {323, 474}, {220, 497},
    {215, 502}, {215, 549}, {218, 547}, {219, 504}, {320, 482}, {426, 480}, {426, 476}, {422, 474},
    {378, 473}, {376, 468}, {390, 429}, {332, 370}, {333, 323},
};
static constexpr int pts_detail_2_cnt = sizeof(pts_detail_2) / sizeof(pts_detail_2[0]);

// --- Dark features (fan opening, screw holes, background cutouts) ---
static const lv_point_t pts_fan_opening[] = {
    {370, 763}, {352, 797}, {348, 821}, {357, 825}, {362, 856}, {380, 907}, {388, 922}, {405, 942},
    {442, 969}, {479, 980}, {537, 977}, {572, 959}, {603, 932}, {617, 912}, {631, 867}, {634, 863},
    {642, 827}, {645, 797}, {628, 764}, {616, 746}, {583, 717}, {572, 714}, {572, 719}, {588, 742},
    {587, 744}, {545, 721}, {502, 713}, {476, 715}, {449, 723}, {410, 747}, {421, 728}, {440, 708},
    {439, 705}, {415, 717}, {398, 731},
};
static constexpr int pts_fan_opening_cnt = sizeof(pts_fan_opening) / sizeof(pts_fan_opening[0]);

static const lv_point_t pts_dark_mechanism[] = {
    {621, 445}, {653, 475}, {790, 498}, {835, 464}, {857, 426}, {862, 397}, {831, 360}, {774, 394},
    {774, 383}, {788, 363}, {755, 362}, {778, 333}, {779, 285}, {773, 275}, {757, 273}, {756, 298},
    {743, 325}, {731, 329}, {731, 291}, {746, 285}, {752, 258}, {748, 228}, {739, 214}, {739, 250},
    {731, 266}, {724, 270}, {710, 247}, {698, 280}, {703, 328}, {677, 382}, {624, 406}, {625, 414},
    {629, 407}, {646, 403}, {650, 411}, {622, 425},
};
static constexpr int pts_dark_mechanism_cnt =
    sizeof(pts_dark_mechanism) / sizeof(pts_dark_mechanism[0]);

// Screw holes
static const lv_point_t pts_screw_left[] = {
    {303, 1174}, {318, 1177}, {334, 1173}, {347, 1162}, {354, 1146}, {354, 1131}, {349, 1118},
    {339, 1108}, {313, 1103}, {297, 1109}, {287, 1120}, {282, 1137}, {283, 1147}, {291, 1165},
};
static constexpr int pts_screw_left_cnt = sizeof(pts_screw_left) / sizeof(pts_screw_left[0]);

static const lv_point_t pts_screw_right[] = {
    {649, 1164}, {662, 1173}, {676, 1177}, {696, 1172}, {707, 1165}, {714, 1146}, {715, 1131},
    {712, 1124}, {699, 1110}, {685, 1102}, {658, 1108}, {646, 1122}, {642, 1133}, {642, 1143},
};
static constexpr int pts_screw_right_cnt = sizeof(pts_screw_right) / sizeof(pts_screw_right[0]);

// --- Highlights ---
static const lv_point_t pts_highlight_top_left[] = {
    {517, 134}, {458, 139}, {455, 143}, {456, 157}, {407, 148}, {384, 157}, {421, 152}, {454, 162},
    {471, 157}, {463, 155}, {464, 151}, {483, 148}, {553, 148}, {553, 142}, {549, 139}, {520, 135},
    {525, 110}, {548, 106}, {551, 78}, {550, 1}, {548, 76}, {521, 77}, {520, 88}, {513, 90},
};
static constexpr int pts_highlight_top_left_cnt =
    sizeof(pts_highlight_top_left) / sizeof(pts_highlight_top_left[0]);

static const lv_point_t pts_highlight_top_right[] = {
    {642, 76}, {640, 0}, {638, 78}, {643, 106}, {663, 109}, {669, 121}, {683, 107}, {712, 97},
    {721, 98}, {756, 110}, {751, 104}, {715, 91}, {682, 105}, {676, 105}, {676, 93}, {669, 87},
    {668, 77},
};
static constexpr int pts_highlight_top_right_cnt =
    sizeof(pts_highlight_top_right) / sizeof(pts_highlight_top_right[0]);

// Voron cube logo highlights
static const lv_point_t pts_highlight_cube_l[] = {
    {459, 1106}, {467, 1113}, {492, 1126}, {494, 1086}, {463, 1067}, {458, 1068}, {457, 1095},
};
static constexpr int pts_highlight_cube_l_cnt =
    sizeof(pts_highlight_cube_l) / sizeof(pts_highlight_cube_l[0]);

static const lv_point_t pts_highlight_cube_r[] = {
    {536, 1066}, {504, 1085}, {503, 1126}, {506, 1127}, {539, 1107}, {540, 1070},
};
static constexpr int pts_highlight_cube_r_cnt =
    sizeof(pts_highlight_cube_r) / sizeof(pts_highlight_cube_r[0]);

static const lv_point_t pts_highlight_cube_top[] = {
    {498, 1079}, {532, 1058}, {530, 1054}, {501, 1038}, {493, 1039}, {464, 1055}, {474, 1065},
};
static constexpr int pts_highlight_cube_top_cnt =
    sizeof(pts_highlight_cube_top) / sizeof(pts_highlight_cube_top[0]);

// --- Green shadow (darkest green tone) ---
static const lv_point_t pts_green_shadow_0[] = {
    {627, 382}, {624, 426}, {649, 416}, {656, 409}, {651, 408}, {649, 400}, {677, 384}, {685, 371},
    {705, 328}, {705, 309}, {699, 280}, {685, 309},
};
static constexpr int pts_green_shadow_0_cnt =
    sizeof(pts_green_shadow_0) / sizeof(pts_green_shadow_0[0]);

static const lv_point_t pts_green_shadow_1[] = {
    {439, 705}, {395, 759}, {392, 769}, {421, 742}, {450, 725}, {476, 717}, {502, 715}, {536, 720},
    {552, 726}, {584, 746}, {606, 767}, {573, 713}, {558, 704}, {529, 696}, {485, 693}, {450, 700},
};
static constexpr int pts_green_shadow_1_cnt =
    sizeof(pts_green_shadow_1) / sizeof(pts_green_shadow_1[0]);

static const lv_point_t pts_green_shadow_band[] = {
    {789, 543}, {350, 539}, {337, 542}, {217, 544}, {216, 547}, {218, 550}, {226, 551}, {362, 546},
    {631, 546}, {792, 551}, {795, 548},
};
static constexpr int pts_green_shadow_band_cnt =
    sizeof(pts_green_shadow_band) / sizeof(pts_green_shadow_band[0]);

// --- Green dark ---
static const lv_point_t pts_green_dark_0[] = {
    {607, 439}, {623, 444}, {628, 382}, {690, 305}, {754, 133}, {751, 130}, {708, 226}, {690, 273},
    {690, 282}, {634, 354}, {621, 366}, {589, 411}, {581, 417}, {564, 421}, {585, 423}, {586, 428},
    {581, 437},
};
static constexpr int pts_green_dark_0_cnt = sizeof(pts_green_dark_0) / sizeof(pts_green_dark_0[0]);

static const lv_point_t pts_green_dark_1[] = {
    {763, 177}, {753, 174}, {808, 272}, {809, 330}, {814, 335}, {828, 334}, {837, 340}, {848, 359},
    {845, 368}, {862, 388}, {863, 354}, {857, 337}, {815, 304}, {818, 290}, {838, 269}, {839, 256},
};
static constexpr int pts_green_dark_1_cnt = sizeof(pts_green_dark_1) / sizeof(pts_green_dark_1[0]);

static const lv_point_t pts_green_dark_band[] = {
    {403, 607}, {370, 605}, {370, 608}, {409, 647}, {424, 657}, {444, 659}, {547, 660}, {577, 656},
    {587, 649}, {625, 611}, {614, 607}, {608, 613}, {594, 613}, {564, 647}, {551, 654}, {449, 654},
    {435, 646},
};
static constexpr int pts_green_dark_band_cnt =
    sizeof(pts_green_dark_band) / sizeof(pts_green_dark_band[0]);

// --- Bright green (A4T signature color) ---
static const lv_point_t pts_green_bright_band[] = {
    {646, 545}, {219, 549}, {215, 556}, {213, 569}, {215, 606}, {286, 600}, {338, 601}, {383, 606},
    {403, 609}, {434, 647}, {446, 655}, {543, 656}, {552, 656}, {566, 649}, {595, 614}, {609, 615},
    {616, 609}, {624, 611}, {634, 603}, {649, 599}, {732, 599}, {754, 603}, {794, 604}, {794, 553},
    {792, 550},
};
static constexpr int pts_green_bright_band_cnt =
    sizeof(pts_green_bright_band) / sizeof(pts_green_bright_band[0]);

static const lv_point_t pts_green_bright_top[] = {
    {524, 326}, {524, 393}, {555, 424}, {572, 422}, {586, 416}, {692, 282}, {692, 273}, {709, 226},
    {723, 200}, {750, 134}, {754, 133}, {756, 127}, {756, 110}, {715, 95}, {682, 106}, {674, 112},
    {653, 159}, {614, 264}, {537, 308}, {528, 317},
};
static constexpr int pts_green_bright_top_cnt =
    sizeof(pts_green_bright_top) / sizeof(pts_green_bright_top[0]);

static const lv_point_t pts_green_bright_ptfe[] = {
    {549, 77}, {540, 133}, {574, 122}, {585, 108}, {597, 102}, {635, 111}, {646, 125}, {640, 78},
    {639, 1}, {550, 0},
};
static constexpr int pts_green_bright_ptfe_cnt =
    sizeof(pts_green_bright_ptfe) / sizeof(pts_green_bright_ptfe[0]);

// --- Copper nozzle ---
static const lv_point_t pts_copper[] = {
    {465, 1194}, {436, 1191}, {428, 1196}, {428, 1214}, {433, 1228}, {435, 1271}, {446, 1290},
    {453, 1294}, {496, 1298}, {541, 1295}, {546, 1301}, {550, 1295}, {560, 1291}, {574, 1271},
    {574, 1232}, {581, 1217}, {586, 1214}, {574, 1199}, {564, 1193}, {555, 1193}, {533, 1197},
    {472, 1198},
};
static constexpr int pts_copper_cnt = sizeof(pts_copper) / sizeof(pts_copper[0]);

// Maximum polygon size across all A4T polygons
static constexpr int MAX_POLYGON_POINTS = 80;

// ============================================================================
// Ear-Clipping Triangulation (same algorithm as faceted renderer)
// ============================================================================

static int64_t cross_product_sign(const lv_point_t& a, const lv_point_t& b, const lv_point_t& c) {
    return (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(b.y - a.y) * (c.x - a.x);
}

static bool point_in_triangle(const lv_point_t& p, const lv_point_t& a, const lv_point_t& b,
                              const lv_point_t& c) {
    int64_t d1 = cross_product_sign(p, a, b);
    int64_t d2 = cross_product_sign(p, b, c);
    int64_t d3 = cross_product_sign(p, c, a);
    return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
}

static bool is_convex_vertex(const int* indices, int idx_cnt, int i, const lv_point_t* pts,
                             bool ccw) {
    int prev_i = (i - 1 + idx_cnt) % idx_cnt;
    int next_i = (i + 1) % idx_cnt;
    int64_t cross =
        cross_product_sign(pts[indices[prev_i]], pts[indices[i]], pts[indices[next_i]]);
    return ccw ? (cross > 0) : (cross < 0);
}

static bool is_ear(const int* indices, int idx_cnt, int i, const lv_point_t* pts, bool ccw) {
    if (!is_convex_vertex(indices, idx_cnt, i, pts, ccw))
        return false;
    int prev_i = (i - 1 + idx_cnt) % idx_cnt;
    int next_i = (i + 1) % idx_cnt;
    const lv_point_t& a = pts[indices[prev_i]];
    const lv_point_t& b = pts[indices[i]];
    const lv_point_t& c = pts[indices[next_i]];
    for (int j = 0; j < idx_cnt; j++) {
        if (j == prev_i || j == i || j == next_i)
            continue;
        if (point_in_triangle(pts[indices[j]], a, b, c))
            return false;
    }
    return true;
}

static void draw_polygon(lv_layer_t* layer, const lv_point_t* pts, int cnt, lv_color_t color) {
    if (cnt < 3)
        return;
    if (cnt > MAX_POLYGON_POINTS)
        cnt = MAX_POLYGON_POINTS;

    if (cnt == 3) {
        lv_draw_triangle_dsc_t tri_dsc;
        lv_draw_triangle_dsc_init(&tri_dsc);
        tri_dsc.color = color;
        tri_dsc.opa = LV_OPA_COVER;
        tri_dsc.p[0].x = pts[0].x;
        tri_dsc.p[0].y = pts[0].y;
        tri_dsc.p[1].x = pts[1].x;
        tri_dsc.p[1].y = pts[1].y;
        tri_dsc.p[2].x = pts[2].x;
        tri_dsc.p[2].y = pts[2].y;
        lv_draw_triangle(layer, &tri_dsc);
        return;
    }

    int64_t winding_sum = 0;
    for (int i = 0; i < cnt; i++) {
        int next = (i + 1) % cnt;
        winding_sum += (int64_t)(pts[next].x - pts[i].x) * (pts[next].y + pts[i].y);
    }
    bool ccw = (winding_sum < 0);

    int indices[MAX_POLYGON_POINTS];
    for (int i = 0; i < cnt; i++)
        indices[i] = i;
    int idx_cnt = cnt;

    lv_draw_triangle_dsc_t tri_dsc;
    lv_draw_triangle_dsc_init(&tri_dsc);
    tri_dsc.color = color;
    tri_dsc.opa = LV_OPA_COVER;

    int safety_counter = cnt * cnt;
    while (idx_cnt > 3 && safety_counter-- > 0) {
        bool ear_found = false;
        for (int i = 0; i < idx_cnt; i++) {
            if (is_ear(indices, idx_cnt, i, pts, ccw)) {
                int prev_i = (i - 1 + idx_cnt) % idx_cnt;
                int next_i = (i + 1) % idx_cnt;
                tri_dsc.p[0].x = pts[indices[prev_i]].x;
                tri_dsc.p[0].y = pts[indices[prev_i]].y;
                tri_dsc.p[1].x = pts[indices[i]].x;
                tri_dsc.p[1].y = pts[indices[i]].y;
                tri_dsc.p[2].x = pts[indices[next_i]].x;
                tri_dsc.p[2].y = pts[indices[next_i]].y;
                lv_draw_triangle(layer, &tri_dsc);
                for (int j = i; j < idx_cnt - 1; j++)
                    indices[j] = indices[j + 1];
                idx_cnt--;
                ear_found = true;
                break;
            }
        }
        if (!ear_found) {
            int64_t fcx = 0, fcy = 0;
            for (int j = 0; j < idx_cnt; j++) {
                fcx += pts[indices[j]].x;
                fcy += pts[indices[j]].y;
            }
            fcx /= idx_cnt;
            fcy /= idx_cnt;
            for (int j = 0; j < idx_cnt; j++) {
                int next_j = (j + 1) % idx_cnt;
                tri_dsc.p[0].x = (int32_t)fcx;
                tri_dsc.p[0].y = (int32_t)fcy;
                tri_dsc.p[1].x = pts[indices[j]].x;
                tri_dsc.p[1].y = pts[indices[j]].y;
                tri_dsc.p[2].x = pts[indices[next_j]].x;
                tri_dsc.p[2].y = pts[indices[next_j]].y;
                lv_draw_triangle(layer, &tri_dsc);
            }
            return;
        }
    }

    if (idx_cnt == 3) {
        tri_dsc.p[0].x = pts[indices[0]].x;
        tri_dsc.p[0].y = pts[indices[0]].y;
        tri_dsc.p[1].x = pts[indices[1]].x;
        tri_dsc.p[1].y = pts[indices[1]].y;
        tri_dsc.p[2].x = pts[indices[2]].x;
        tri_dsc.p[2].y = pts[indices[2]].y;
        lv_draw_triangle(layer, &tri_dsc);
    }
}

// ============================================================================
// Helpers
// ============================================================================

static void scale_polygon(const lv_point_t* pts_in, int cnt, lv_point_t* pts_out, int32_t cx,
                          int32_t cy, float scale) {
    for (int i = 0; i < cnt; i++) {
        pts_out[i].x = cx + (int32_t)((pts_in[i].x - DESIGN_CENTER_X) * scale);
        pts_out[i].y = cy + (int32_t)((pts_in[i].y - DESIGN_CENTER_Y) * scale);
    }
}

// ============================================================================
// Main Drawing Function
// ============================================================================

void draw_nozzle_a4t(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t filament_color,
                     int32_t scale_unit, lv_opa_t opa) {
    int32_t render_size = scale_unit * 10;
    // Design space is 1000x1334; scale down to match the visual footprint
    // of the other renderers (bambu/stealthburner use 1000-unit space)
    float scale = (float)render_size / 2400.0f;

    auto dim = [opa](lv_color_t c) -> lv_color_t {
        if (opa >= LV_OPA_COVER)
            return c;
        float f = (float)opa / 255.0f;
        return lv_color_make((uint8_t)(c.red * f), (uint8_t)(c.green * f), (uint8_t)(c.blue * f));
    };

    // Filament detection for nozzle tip coloring
    static constexpr uint32_t NOZZLE_UNLOADED = 0x3A3A3A;
    bool has_filament = !lv_color_eq(filament_color, lv_color_hex(NOZZLE_UNLOADED)) &&
                        !lv_color_eq(filament_color, lv_color_hex(0x808080)) &&
                        !lv_color_eq(filament_color, lv_color_black());

    // Pre-dim colors
    lv_color_t col_silhouette    = dim(lv_color_hex(0x1A1A1A));
    lv_color_t col_body          = dim(lv_color_hex(0x353435));
    lv_color_t col_detail        = dim(lv_color_hex(0x5F5E5F));
    lv_color_t col_dark          = dim(lv_color_hex(0x0A0A0A));
    lv_color_t col_highlight     = dim(lv_color_hex(0xC7C8C5));
    lv_color_t col_green_shadow  = dim(lv_color_hex(0x282615));
    lv_color_t col_green_dark    = dim(lv_color_hex(0x615B12));
    lv_color_t col_green_bright  = dim(lv_color_hex(0xBFBB4B));
    lv_color_t col_copper        = dim(lv_color_hex(0xA6614C));

    lv_point_t tmp[MAX_POLYGON_POINTS];

    // Layer 1: Silhouette (dark outline under everything)
    scale_polygon(pts_silhouette, pts_silhouette_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_silhouette_cnt, col_silhouette);

    scale_polygon(pts_silhouette_nozzle, pts_silhouette_nozzle_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_silhouette_nozzle_cnt, col_silhouette);

    // Layer 2: Body (main gray surface)
    scale_polygon(pts_body_0, pts_body_0_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_body_0_cnt, col_body);

    scale_polygon(pts_body_1, pts_body_1_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_body_1_cnt, col_body);

    scale_polygon(pts_body_2, pts_body_2_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_body_2_cnt, col_body);

    // Layer 3: Surface detail (mid-gray)
    scale_polygon(pts_detail_0, pts_detail_0_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_detail_0_cnt, col_detail);

    scale_polygon(pts_detail_1, pts_detail_1_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_detail_1_cnt, col_detail);

    scale_polygon(pts_detail_2, pts_detail_2_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_detail_2_cnt, col_detail);

    // Layer 4: Dark features (fan opening, mechanism, screw holes)
    scale_polygon(pts_fan_opening, pts_fan_opening_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_fan_opening_cnt, col_dark);

    scale_polygon(pts_dark_mechanism, pts_dark_mechanism_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_dark_mechanism_cnt, col_dark);

    scale_polygon(pts_screw_left, pts_screw_left_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_screw_left_cnt, col_dark);

    scale_polygon(pts_screw_right, pts_screw_right_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_screw_right_cnt, col_dark);

    // Layer 5: Highlights (PTFE tube, cube logo)
    scale_polygon(pts_highlight_top_left, pts_highlight_top_left_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_highlight_top_left_cnt, col_highlight);

    scale_polygon(pts_highlight_top_right, pts_highlight_top_right_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_highlight_top_right_cnt, col_highlight);

    scale_polygon(pts_highlight_cube_l, pts_highlight_cube_l_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_highlight_cube_l_cnt, col_highlight);

    scale_polygon(pts_highlight_cube_r, pts_highlight_cube_r_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_highlight_cube_r_cnt, col_highlight);

    scale_polygon(pts_highlight_cube_top, pts_highlight_cube_top_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_highlight_cube_top_cnt, col_highlight);

    // Layer 6: Green shadow
    scale_polygon(pts_green_shadow_0, pts_green_shadow_0_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_green_shadow_0_cnt, col_green_shadow);

    scale_polygon(pts_green_shadow_1, pts_green_shadow_1_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_green_shadow_1_cnt, col_green_shadow);

    scale_polygon(pts_green_shadow_band, pts_green_shadow_band_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_green_shadow_band_cnt, col_green_shadow);

    // Layer 7: Green dark
    scale_polygon(pts_green_dark_0, pts_green_dark_0_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_green_dark_0_cnt, col_green_dark);

    scale_polygon(pts_green_dark_1, pts_green_dark_1_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_green_dark_1_cnt, col_green_dark);

    scale_polygon(pts_green_dark_band, pts_green_dark_band_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_green_dark_band_cnt, col_green_dark);

    // Layer 8: Bright green (A4T signature)
    scale_polygon(pts_green_bright_band, pts_green_bright_band_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_green_bright_band_cnt, col_green_bright);

    scale_polygon(pts_green_bright_top, pts_green_bright_top_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_green_bright_top_cnt, col_green_bright);

    scale_polygon(pts_green_bright_ptfe, pts_green_bright_ptfe_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_green_bright_ptfe_cnt, col_green_bright);

    // Layer 9: Copper nozzle
    scale_polygon(pts_copper, pts_copper_cnt, tmp, cx, cy, scale);
    draw_polygon(layer, tmp, pts_copper_cnt, col_copper);

    // Layer 10: Nozzle tip (shows filament color when loaded)
    {
        lv_color_t tip_color = dim(filament_color);
        lv_color_t nozzle_metal = dim(lv_color_hex(NOZZLE_UNLOADED));

        int32_t nozzle_top_y =
            cy + (int32_t)((1300 - DESIGN_CENTER_Y) * scale);
        int32_t nozzle_height = LV_MAX((int32_t)(30 * scale), 2);
        int32_t nozzle_top_width = LV_MAX((int32_t)(80 * scale), 4);
        int32_t nozzle_bottom_width = LV_MAX((int32_t)(30 * scale), 2);

        lv_color_t tip_left =
            has_filament ? nr_lighten(tip_color, 30) : nr_lighten(nozzle_metal, 30);
        lv_color_t tip_right =
            has_filament ? nr_darken(tip_color, 20) : nr_darken(nozzle_metal, 10);

        nr_draw_nozzle_tip(layer, cx, nozzle_top_y, nozzle_top_width, nozzle_bottom_width,
                           nozzle_height, tip_left, tip_right);

        lv_draw_fill_dsc_t glint_dsc;
        lv_draw_fill_dsc_init(&glint_dsc);
        glint_dsc.color = dim(lv_color_hex(0xFFFFFF));
        glint_dsc.opa = LV_OPA_70;
        int32_t glint_y = nozzle_top_y + nozzle_height - 1;
        lv_area_t glint = {cx - 1, glint_y, cx + 1, glint_y + 1};
        lv_draw_fill(layer, &glint_dsc, &glint);
    }
}
