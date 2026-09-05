// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_preview_colors.cpp
 * @brief The streaming G-code preview's per-tool colour path.
 *
 * A tool changer (Snapmaker U1) always streams — 961MB RAM puts it under
 * MemoryInfo::should_force_streaming()'s 2GB threshold — and streaming holds no
 * ParsedGCodeFile. Everything the print-status preview knew about which tool
 * printed what came from ParsedGCodeFile, so the whole model rendered in T0's
 * colour. These cover the streaming-side replacements:
 *
 *   - the layer index tracks the RUNNING tool, not just the file's first one,
 *     and accumulates the used-tool set in the same pass;
 *   - each layer chunk is parsed seeded with the tool active at ITS byte
 *     offset, so a tool change in a previous chunk is not lost;
 *   - a shorter AMS override vector does not discard slicer palette entries for
 *     higher tool indices.
 */

#include "ui_filament_mapping_card.h"

#include "ams_types.h"
#include "filament_mapper.h"
#include "gcode_color_palette.h"
#include "gcode_layer_index.h"
#include "gcode_parser.h"
#include "gcode_streaming_controller.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

namespace {

/// Writes @p content to a uniquely-named temp file and removes it on scope exit.
class TempGCodeFile {
  public:
    explicit TempGCodeFile(const std::string& content) {
        char temp_path[] = "/tmp/helix_tc_preview_XXXXXX";
        int fd = mkstemp(temp_path);
        REQUIRE(fd != -1);
        close(fd);
        path_ = temp_path;
        std::ofstream out(path_);
        out << content;
    }

    ~TempGCodeFile() {
        std::remove(path_.c_str());
    }

    const std::string& path() const {
        return path_;
    }

  private:
    std::string path_;
};

/// Two tool changes, each landing in the byte range of the layer BEFORE the one
/// that has to be printed with it — the shape that makes a per-layer parse lose
/// the tool. Layer 0 prints with T0, layers 1 and 2 with T1 (layer 2's chunk
/// contains no T command at all), layer 3 with T2.
const std::string TOOLCHANGER_GCODE = R"(; toolchanger sample
M82
G28
T0
G1 Z0.3 F1000
G1 X10 Y10 E1
G1 X20 Y10 E2
T1
G1 Z0.6 F1000
G1 X30 Y10 E3
G1 X40 Y10 E4
G1 Z0.9 F1000
G1 X50 Y10 E5
G1 X60 Y10 E6
T2
G1 Z1.2 F1000
G1 X70 Y10 E7
G1 X80 Y10 E8
)";

/// Collect the distinct tool indices tagged on a streamed layer's segments.
std::set<int> tools_in_layer(GCodeStreamingController& ctrl, size_t layer) {
    std::set<int> tools;
    auto segments = ctrl.get_layer_segments(layer);
    REQUIRE(segments != nullptr);
    REQUIRE_FALSE(segments->empty());
    for (const auto& seg : *segments) {
        tools.insert(static_cast<int>(seg.tool_index));
    }
    return tools;
}

} // namespace

// ============================================================================
// A. The streaming layer index is tool-aware
// ============================================================================

TEST_CASE("Layer index: a standalone T survives leading whitespace",
          "[gcode][toolchanger][layer_index]") {
    // The index used to hand-roll its T parse as `line[0] == 'T'`, which is
    // looser than the canonical tool_index_for_line() in two directions: it
    // misses an indented tool change and accepts "T0 X1" (not a tool change at
    // all). Both now go through the one parser the full-file path uses.
    TempGCodeFile file(R"(G28
T0 X1
  T2 ; indented tool change
G1 Z0.3 F1000
G1 X10 Y10 E1
)");
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));
    // "T0 X1" is not a standalone tool change; the indented T2 is.
    REQUIRE(index.get_stats().initial_tool_index == 2);
}

TEST_CASE("Streaming: a layer chunk is parsed with the tool active at its offset",
          "[gcode][toolchanger][streaming]") {
    // The bug: every chunk was seeded with the FILE-GLOBAL first tool, so on a
    // tool changer every segment reported T0 and the preview coloured the whole
    // model with lane 0's filament.
    TempGCodeFile file(TOOLCHANGER_GCODE);
    GCodeStreamingController ctrl;
    REQUIRE(ctrl.open_file(file.path()));
    REQUIRE(ctrl.get_layer_count() == 4);

    SECTION("layer printed before any tool change keeps T0") {
        REQUIRE(tools_in_layer(ctrl, 0) == std::set<int>{0});
    }
    SECTION("tool change in the previous chunk is carried into this one") {
        REQUIRE(tools_in_layer(ctrl, 1) == std::set<int>{1});
    }
    SECTION("a chunk containing no T at all inherits the running tool") {
        REQUIRE(tools_in_layer(ctrl, 2) == std::set<int>{1});
    }
    SECTION("a later tool change is picked up") {
        REQUIRE(tools_in_layer(ctrl, 3) == std::set<int>{2});
    }
}

TEST_CASE("Layer index: the running tool is recorded per layer, not just the file's first",
          "[gcode][toolchanger][layer_index]") {
    TempGCodeFile file(TOOLCHANGER_GCODE);
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));
    REQUIRE(index.get_layer_count() == 4);

    SECTION("every distinct tool the file changes to is collected in one pass") {
        REQUIRE(index.get_stats().tools_used == std::set<int>{0, 1, 2});
    }

    SECTION("each entry carries the tool active at its own byte offset") {
        REQUIRE(index.get_entry(0).start_tool == 0);
        REQUIRE(index.get_entry(1).start_tool == 1);
        // Layer 2's byte range contains no T at all — it inherits T1.
        REQUIRE(index.get_entry(2).start_tool == 1);
        REQUIRE(index.get_entry(3).start_tool == 2);
    }

    SECTION("initial_tool_index still reports the file's FIRST tool") {
        // The streaming controller falls back to it for entries with no tool of
        // their own, so widening the tracking must not have moved it.
        REQUIRE(index.get_stats().initial_tool_index == 0);
    }
}

TEST_CASE("Layer index: layers before the file's first tool change have no tool",
          "[gcode][toolchanger][layer_index]") {
    // -1 is what makes the streaming controller fall back to initial_tool_index,
    // reproducing the pre-fix seeding for the layers that genuinely predate any
    // tool change. A single-extruder file must not suddenly claim T0 here.
    TempGCodeFile file(R"(M82
G28
G1 Z0.3 F1000
G1 X10 Y10 E1
G1 Z0.6 F1000
G1 X20 Y10 E2
)");
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));
    REQUIRE(index.get_layer_count() == 2);
    REQUIRE(index.get_entry(0).start_tool == -1);
    REQUIRE(index.get_entry(1).start_tool == -1);
    REQUIRE(index.get_stats().tools_used.empty());
    REQUIRE(index.get_stats().initial_tool_index == -1);
}

TEST_CASE("Layer index: a per-layer entry stays 40 bytes", "[gcode][toolchanger][layer_index]") {
    // One entry per layer is held for the whole session; a 5M-line print indexes
    // ~10k of them. start_tool was placed in start_feature_type's padding.
    REQUIRE(sizeof(StreamingLayerEntry) == 40);
}

// ============================================================================
// B. The used-tool set and per-tool info are available in streaming mode
// ============================================================================

TEST_CASE("Streaming: the index answers which tools the file prints with",
          "[gcode][toolchanger][streaming]") {
    // This is the only used-tool source a streamed file has — there is no
    // ParsedGCodeFile to read tools_used_indices from.
    TempGCodeFile file(TOOLCHANGER_GCODE);
    GCodeStreamingController ctrl;
    REQUIRE(ctrl.open_file(file.path()));
    REQUIRE(ctrl.get_index_stats().tools_used == std::set<int>{0, 1, 2});
}

TEST_CASE("Per-tool info for a streamed multi-tool file keeps real tool numbers",
          "[gcode][toolchanger][tool_info]") {
    // What PrintStatusPanel::build_print_tool_info() now does, minus the widget:
    // slicer palette from metadata + used-tool set from the streaming index.
    TempGCodeFile file(TOOLCHANGER_GCODE);
    GCodeStreamingController ctrl;
    REQUIRE(ctrl.open_file(file.path()));

    const std::vector<std::string> palette = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    const std::vector<std::string> materials = {"PLA", "PETG", "ABS", "ASA"};

    const auto tools = helix::ui::FilamentMappingCard::build_used_tool_info(
        palette, materials, ctrl.get_index_stats().tools_used);

    REQUIRE(tools.size() == 3);
    REQUIRE(tools[0].tool_index == 0);
    REQUIRE(tools[0].color_rgb == 0xFF0000);
    REQUIRE(tools[1].tool_index == 1);
    REQUIRE(tools[1].color_rgb == 0x00FF00);
    REQUIRE(tools[2].tool_index == 2);
    REQUIRE(tools[2].color_rgb == 0x0000FF);
    REQUIRE(tools[2].material == "ABS");
    // T3 is in the palette but the file never selects it.
    for (const auto& t : tools) {
        REQUIRE(t.tool_index != 3);
    }
}

TEST_CASE("build_used_tool_info: a sparse used-set does not collapse tool numbers",
          "[gcode][toolchanger][tool_info]") {
    // The trap this guards: returning entries positionally would put T2's color
    // at index 1, and effective_tool_colors() scatters by .tool_index into a
    // vector the viewer reads by tool number.
    const auto tools = helix::ui::FilamentMappingCard::build_used_tool_info(
        {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}, {"PLA", "PETG", "ABS", "ASA"},
        std::set<int>{0, 2});

    REQUIRE(tools.size() == 2);
    REQUIRE(tools[0].tool_index == 0);
    REQUIRE(tools[1].tool_index == 2);
    REQUIRE(tools[1].color_rgb == 0x0000FF);
    REQUIRE(tools[1].material == "ABS");
}

TEST_CASE("build_used_tool_info: tools outside the palette are dropped, empty set yields nothing",
          "[gcode][toolchanger][tool_info]") {
    SECTION("a used tool with no palette entry is dropped") {
        const auto tools = helix::ui::FilamentMappingCard::build_used_tool_info(
            {"#FF0000"}, {"PLA"}, std::set<int>{0, 7});
        REQUIRE(tools.size() == 1);
        REQUIRE(tools[0].tool_index == 0);
    }
    SECTION("no used tools means no per-tool info to push") {
        REQUIRE(
            helix::ui::FilamentMappingCard::build_used_tool_info({"#FF0000"}, {"PLA"}, {}).empty());
    }
}

// ============================================================================
// D. A shorter override vector must not discard slicer palette entries
// ============================================================================

TEST_CASE("Palette: a shorter override vector does not truncate the slicer palette",
          "[gcode][toolchanger][palette]") {
    // set_tool_color_overrides() used to resize() the palette down to the
    // override count. Tools at or above it then resolved to the renderer's
    // single fallback — which streaming init had set to T0's filament color,
    // painting every dropped tool T0.
    GCodeColorPalette pal;
    pal.set_from_hex_palette({"#FF0000", "#00FF00", "#0000FF", "#FFFF00"});
    REQUIRE(pal.tool_colors.size() == 4);

    const lv_color_t sentinel = lv_color_hex(0x123456);
    pal.apply_overrides({0xAAAAAA, 0xBBBBBB});

    REQUIRE(pal.tool_colors.size() == 4);
    // Overridden tools take the AMS lane color...
    REQUIRE(lv_color_to_u32(pal.resolve(0, sentinel)) == lv_color_to_u32(lv_color_hex(0xAAAAAA)));
    REQUIRE(lv_color_to_u32(pal.resolve(1, sentinel)) == lv_color_to_u32(lv_color_hex(0xBBBBBB)));
    // ...and the rest keep what the slicer said, NOT the fallback.
    REQUIRE(lv_color_to_u32(pal.resolve(2, sentinel)) == lv_color_to_u32(lv_color_hex(0x0000FF)));
    REQUIRE(lv_color_to_u32(pal.resolve(3, sentinel)) == lv_color_to_u32(lv_color_hex(0xFFFF00)));
    REQUIRE(lv_color_to_u32(pal.resolve(2, sentinel)) != lv_color_to_u32(sentinel));
}

TEST_CASE("Palette: an empty interior slot keeps its index instead of compacting",
          "[gcode][toolchanger][palette]") {
    // set_from_hex_palette() used to SKIP entries it could not parse, so a
    // slot-aligned palette collapsed and every tool past the gap inherited its
    // neighbour's colour: {"#A", "", "#B"} became size 2, tool 1 rendered #B,
    // and tool 2 fell off the end to the fallback. The 3D builder indexes the
    // original strings and painted the same file correctly - one file, two
    // renderers, two answers. gcode_color_metadata.h guarantees the palette is
    // slot-aligned ("#A;;#B" -> {"#A", "", "#B"}), so this must preserve it.
    GCodeColorPalette pal;
    pal.set_from_hex_palette({"#FF0000", "", "#0000FF"});

    const lv_color_t sentinel = lv_color_hex(0x123456);
    REQUIRE(pal.tool_colors.size() == 3);
    REQUIRE(lv_color_to_u32(pal.resolve(0, sentinel)) == lv_color_to_u32(lv_color_hex(0xFF0000)));
    // Tool 1 has no colour of its own: it must fall through to the caller's
    // fallback, and specifically must NOT have inherited tool 2's blue.
    REQUIRE(lv_color_to_u32(pal.resolve(1, sentinel)) == lv_color_to_u32(sentinel));
    REQUIRE(lv_color_to_u32(pal.resolve(1, sentinel)) != lv_color_to_u32(lv_color_hex(0x0000FF)));
    // Tool 2 still answers from its own slot, not from off the end.
    REQUIRE(lv_color_to_u32(pal.resolve(2, sentinel)) == lv_color_to_u32(lv_color_hex(0x0000FF)));
}

TEST_CASE("Palette: an unparseable entry occupies its slot without answering",
          "[gcode][toolchanger][palette]") {
    // A bare '#' and a non-hex blob are both reachable: the palette splitter
    // emits an empty string for a token it rejects, and callers can hand this
    // whatever the slicer wrote. Each must hold its slot and answer nothing.
    GCodeColorPalette pal;
    pal.set_from_hex_palette({"#FF0000", "#", "nonsense", "#0000FF"});

    const lv_color_t sentinel = lv_color_hex(0x123456);
    REQUIRE(pal.tool_colors.size() == 4);
    REQUIRE(lv_color_to_u32(pal.resolve(1, sentinel)) == lv_color_to_u32(sentinel));
    REQUIRE(lv_color_to_u32(pal.resolve(2, sentinel)) == lv_color_to_u32(sentinel));
    REQUIRE(lv_color_to_u32(pal.resolve(3, sentinel)) == lv_color_to_u32(lv_color_hex(0x0000FF)));
}

TEST_CASE("Palette: an 8-digit RGBA entry resolves to its RGB", "[gcode][toolchanger][palette]") {
    // Slicers emit #RRGGBBAA and the metadata parser accepts it, so it reaches
    // the palette. The old strtol spelling shifted every channel one byte left.
    GCodeColorPalette pal;
    pal.set_from_hex_palette({"#800080FF", "#00C1AE"});

    const lv_color_t sentinel = lv_color_hex(0x123456);
    REQUIRE(lv_color_to_u32(pal.resolve(0, sentinel)) == lv_color_to_u32(lv_color_hex(0x800080)));
    REQUIRE(lv_color_to_u32(pal.resolve(0, sentinel)) != lv_color_to_u32(lv_color_hex(0x0080FF)));
    REQUIRE(lv_color_to_u32(pal.resolve(1, sentinel)) == lv_color_to_u32(lv_color_hex(0x00C1AE)));
}

TEST_CASE("Palette: an override fills a gap without inventing colours elsewhere",
          "[gcode][toolchanger][palette]") {
    // apply_overrides() grows with resize(), which introduces slots holding no
    // colour. A grown-but-not-overridden slot must keep falling through rather
    // than reporting the value-initialised black a plain lv_color_t would give.
    GCodeColorPalette pal;
    pal.set_from_hex_palette({"#FF0000", "", "#0000FF"});
    pal.apply_overrides({0xAAAAAA, 0xBBBBBB});

    const lv_color_t sentinel = lv_color_hex(0x123456);
    REQUIRE(pal.tool_colors.size() == 3);
    REQUIRE(lv_color_to_u32(pal.resolve(1, sentinel)) == lv_color_to_u32(lv_color_hex(0xBBBBBB)));
    REQUIRE(lv_color_to_u32(pal.resolve(2, sentinel)) == lv_color_to_u32(lv_color_hex(0x0000FF)));
}

TEST_CASE("Palette: growth past the slicer palette leaves the new slots unanswered",
          "[gcode][toolchanger][palette]") {
    GCodeColorPalette pal;
    pal.set_from_hex_palette({"#FF0000"});
    pal.tool_colors.resize(3); // what apply_overrides() does before it assigns

    const lv_color_t sentinel = lv_color_hex(0x123456);
    REQUIRE(lv_color_to_u32(pal.resolve(0, sentinel)) == lv_color_to_u32(lv_color_hex(0xFF0000)));
    REQUIRE(lv_color_to_u32(pal.resolve(2, sentinel)) == lv_color_to_u32(sentinel));
    REQUIRE(lv_color_to_u32(pal.resolve(2, sentinel)) != lv_color_to_u32(lv_color_hex(0x000000)));
}

TEST_CASE("Palette: a longer override vector still grows the palette",
          "[gcode][toolchanger][palette]") {
    GCodeColorPalette pal;
    pal.set_from_hex_palette({"#FF0000"});
    pal.apply_overrides({0xAAAAAA, 0xBBBBBB, 0xCCCCCC});

    const lv_color_t sentinel = lv_color_hex(0x123456);
    REQUIRE(pal.tool_colors.size() == 3);
    REQUIRE(lv_color_to_u32(pal.resolve(2, sentinel)) == lv_color_to_u32(lv_color_hex(0xCCCCCC)));
}

// ============================================================================
// F. ONE colour rule: colour(tool N) = colour of the lane that actually prints N
// ============================================================================

namespace {

/// Four heads holding, in physical order: black, off-white, red, gold. These are
/// the real lane colours from the U1 the maintainer is testing on.
std::vector<helix::AvailableSlot> u1_lanes() {
    std::vector<helix::AvailableSlot> slots;
    const uint32_t colors[] = {0x080A0D, 0xE2DEDB, 0xE72F1D, 0xF4C032};
    for (int i = 0; i < 4; ++i) {
        helix::AvailableSlot s{};
        s.slot_index = i;
        s.backend_index = 0;
        s.color_rgb = colors[i];
        s.material = "PLA";
        s.is_empty = false;
        s.current_tool_mapping = -1;
        slots.push_back(s);
    }
    return slots;
}

} // namespace

TEST_CASE("Routing: both tools resolve to their real head, not the identity answer",
          "[gcode][toolchanger][routing]") {
    // The discriminating case. The file is sliced T0=red body, T3=black bits, but
    // physically red sits in head 2 and black in head 0 — inverted from the file.
    // Firmware routing for this print: T0->head2, T3->head0. NEITHER tool is
    // identity, so an identity read is wrong for both and the test can tell.
    std::vector<int> routing(4, -1);
    routing[0] = 2; // body  -> head 2 (red)
    routing[3] = 0; // bits  -> head 0 (black)

    const auto slots = u1_lanes();
    const auto colors = helix::FilamentMapper::routed_tool_colors(routing, slots, helix::printer::ToolMappingOrigin::Unvouched);

    REQUIRE(colors.size() == 4);
    // Correct render: body red, bits black.
    REQUIRE(colors[0] == 0xE72F1D);
    REQUIRE(colors[3] == 0x080A0D);
    // The broken render this replaces: identity would paint body black, bits gold.
    REQUIRE(colors[0] != 0x080A0D);
    REQUIRE(colors[3] != 0xF4C032);
}

TEST_CASE("Routing: a single-tool file takes the SAME path, with no special case",
          "[gcode][toolchanger][routing]") {
    // The regression guard against the tool-count split coming back. One tool,
    // routed away from identity, resolved by the identical call the N-tool case
    // makes — no palette, no active-lane fallback, no branch on tool count.
    std::vector<int> routing(4, -1);
    routing[0] = 2; // the only tool the file uses -> head 2 (red)

    const auto colors = helix::FilamentMapper::routed_tool_colors(routing, u1_lanes(), helix::printer::ToolMappingOrigin::Unvouched);

    REQUIRE(colors.size() == 4);
    REQUIRE(colors[0] == 0xE72F1D); // red, from the head that prints it
    REQUIRE(colors[0] != 0x080A0D); // NOT head 0's black
}

TEST_CASE("Routing: a filament system resolves through its own map, unchanged",
          "[gcode][toolchanger][routing]") {
    // AFC / Happy Hare shape: one nozzle, the backend's map IS the routing, and
    // it reaches this function through the exact same call. Non-identity so the
    // test proves the map is consulted rather than coinciding with the index.
    const auto slots = u1_lanes();
    const std::vector<int> routing = {2, 0, 1}; // T0<-lane2, T1<-lane0, T2<-lane1

    const auto colors = helix::FilamentMapper::routed_tool_colors(routing, slots, helix::printer::ToolMappingOrigin::Unvouched);

    REQUIRE(colors.size() == 3);
    REQUIRE(colors[0] == slots[2].color_rgb);
    REQUIRE(colors[1] == slots[0].color_rgb);
    REQUIRE(colors[2] == slots[1].color_rgb);
}

TEST_CASE("Routing: says nothing when it knows nothing", "[gcode][toolchanger][routing]") {
    SECTION("no routing published — an empty vector is not identity") {
        // The idle trap: with no print task the firmware holds a default identity
        // map. The backend answers empty there, and empty must stay empty rather
        // than being read as 'T0->head0'.
        REQUIRE(helix::FilamentMapper::routed_tool_colors({}, u1_lanes(), helix::printer::ToolMappingOrigin::Unvouched).empty());
    }
    SECTION("a tool with no routing entry stays unknown, not head 0") {
        std::vector<int> routing = {2, -1};
        const auto colors = helix::FilamentMapper::routed_tool_colors(routing, u1_lanes(), helix::printer::ToolMappingOrigin::Unvouched);
        REQUIRE(colors.size() == 2);
        REQUIRE(colors[0] == 0xE72F1D);
        REQUIRE(colors[1] == 0x808080);
    }
    SECTION("all lanes at the default colour would only overwrite the slicer palette") {
        auto slots = u1_lanes();
        for (auto& s : slots) {
            s.color_rgb = 0x808080;
        }
        REQUIRE(helix::FilamentMapper::routed_tool_colors({0, 1, 2, 3}, slots, helix::printer::ToolMappingOrigin::Unvouched).empty());
    }
    SECTION("a routing entry pointing at a slot that does not exist keeps the others") {
        auto slots = u1_lanes();
        slots.resize(2);
        const auto colors = helix::FilamentMapper::routed_tool_colors({0, 1, 2, 3}, slots, helix::printer::ToolMappingOrigin::Unvouched);
        REQUIRE(colors.size() == 4);
        REQUIRE(colors[0] == slots[0].color_rgb);
        REQUIRE(colors[1] == slots[1].color_rgb);
        REQUIRE(colors[2] == 0x808080);
        REQUIRE(colors[3] == 0x808080);
    }
}

TEST_CASE("Routing: a tool routed to an EMPTY lane resolves unknown, not to its stale colour",
          "[gcode][toolchanger][routing]") {
    // An emptied lane keeps reporting whatever used to be in it. Painting a tool
    // with a filament that is not loaded is the same class of confident-wrong
    // answer as the identity map, so it must resolve unknown instead.
    auto slots = u1_lanes();
    slots[2].is_empty = true; // red spool pulled; colour field still says E72F1D

    const auto colors = helix::FilamentMapper::routed_tool_colors({2, 0}, slots, helix::printer::ToolMappingOrigin::Unvouched);

    REQUIRE(colors.size() == 2);
    REQUIRE(colors[0] == 0x808080); // routed to the empty head -> unknown
    REQUIRE(colors[0] != 0xE72F1D); // NOT the stale colour it still reports
    REQUIRE(colors[1] == 0x080A0D); // the loaded head still answers normally
}

// ============================================================================
// effective_routing — which map may answer when the backend publishes nothing
// ============================================================================
//
// This is the seam the identity answer came back through. The pure colour
// resolver already refused an empty routing, but its CALLER substituted the
// attachment map first, so the refusal never ran: on a U1 with no print task
// configured the preview resolved every tool to its own head index and rendered
// a red-body/black-tail file as black body, red tail. Hardware-observed.

TEST_CASE("effective_routing: a tool changer's attachment map is not routing",
          "[gcode][toolchanger][routing]") {
    // Identity attachment: head N owns slot N. True of the hardware, and
    // useless as routing — it says nothing about which head prints which tool.
    const std::vector<int> attachment = {0, 1, 2, 3};

    SECTION("no published routing stays EMPTY, it does not become identity") {
        const auto routing = helix::FilamentMapper::effective_routing(
            {}, attachment, /*attachment_is_routing=*/false);
        REQUIRE(routing.empty());
    }

    SECTION("and therefore no colours are pushed over the slicer palette") {
        const auto routing = helix::FilamentMapper::effective_routing(
            {}, attachment, /*attachment_is_routing=*/false);
        REQUIRE(helix::FilamentMapper::routed_tool_colors(routing, u1_lanes(), helix::printer::ToolMappingOrigin::Unvouched).empty());
    }

    SECTION("the inversion this prevents, stated explicitly") {
        // Had the attachment map stood in, T0 would resolve to head 0 (black)
        // and T2 to head 2 (red) — exactly backwards for a file whose T0 is red
        // and T2 is black, which is what was seen on the printer.
        const auto wrong = helix::FilamentMapper::routed_tool_colors(attachment, u1_lanes(), helix::printer::ToolMappingOrigin::Unvouched);
        REQUIRE(wrong.size() == 4);
        REQUIRE(wrong[0] == 0x080A0D); // black — the wrong answer for a red T0
        REQUIRE(wrong[2] == 0xE72F1D); // red   — the wrong answer for a black T2

        const auto routing = helix::FilamentMapper::effective_routing(
            {}, attachment, /*attachment_is_routing=*/false);
        REQUIRE(helix::FilamentMapper::routed_tool_colors(routing, u1_lanes(), helix::printer::ToolMappingOrigin::Unvouched).empty());
    }
}

TEST_CASE("effective_routing: a filament system still answers from its own map",
          "[gcode][toolchanger][routing]") {
    // AFC / Happy Hare / ACE: one nozzle fed by many lanes, so the physical map
    // IS the print routing. Excluding tool changers must not regress these.
    // Non-identity so a pass cannot be a coincidence.
    const std::vector<int> attachment = {2, 0, 1};

    const auto routing =
        helix::FilamentMapper::effective_routing({}, attachment, /*attachment_is_routing=*/true);

    REQUIRE(routing == attachment);
    const auto colors = helix::FilamentMapper::routed_tool_colors(routing, u1_lanes(), helix::printer::ToolMappingOrigin::Unvouched);
    REQUIRE(colors.size() == 3);
    REQUIRE(colors[0] == 0xE72F1D); // lane 2
}

TEST_CASE("effective_routing: published routing always wins", "[gcode][toolchanger][routing]") {
    const std::vector<int> published = {2, 1, 0, 3}; // the U1 crossover
    const std::vector<int> attachment = {0, 1, 2, 3};

    for (bool attachment_is_routing : {false, true}) {
        const auto routing =
            helix::FilamentMapper::effective_routing(published, attachment, attachment_is_routing);
        REQUIRE(routing == published);
    }
}
