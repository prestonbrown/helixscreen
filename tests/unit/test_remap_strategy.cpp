// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin each backend's RemapStrategy advertisement.
//
// RemapStrategy tells the preflight filament validator how a backend routes
// tool->material assignments through to the printer:
//   None         — no mapping (base / default); also used for ACE which uses
//                  ACE_CHANGE_TOOL TOOL=n rather than the Tn/SM_PRINT_* families
//                  that GcodeToolRemapper handles (remap unimplemented for ACE)
//   Native       — backend owns the T0..Tn slot mapping internally (HH, AFC,
//                  CFS, AD5X IFS, ToolChanger); helix does NOT rewrite gcode
//   GcodeRewrite — helix must rewrite T-commands in the gcode file because the
//                  backend has no internal tool-routing. Declared by ToolChanger
//                  on a changer with no ASSIGN_TOOL; ACE declares None until the
//                  ACE_CHANGE_TOOL family is handled
//   PrePrintSend: firmware pre-print send, no gcode rewrite (Snapmaker U1)
//
// The per-backend probes come from tests/test_helpers/ams_backend_probes.h.

#include "../test_helpers/ad5x_ifs_test_access.h"
#include "../test_helpers/ams_backend_probes.h"
#include "ams_backend.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_remap.h"
#include "test_helpers/backend_harness.h"

#include "../catch_amalgamated.hpp"

using helix::AmsBackend;
using helix::AmsBackendMock;
using helix::AmsError;
using helix::AmsErrorHelper;
using helix::GcodeRewriteBlock;

using helix::Ad5xIfsTestAccess;

namespace {

// Minimal concrete subclass of the base to test the default.
class BaseProbe : public AmsBackend {
  public:
    // Stubs for pure-virtual methods — only strategy is under test.
    AmsError start() override {
        return AmsErrorHelper::success();
    }
    void stop() override {}
    [[nodiscard]] bool is_running() const override {
        return false;
    }
    void set_event_callback(EventCallback) override {}
    [[nodiscard]] helix::AmsSystemInfo get_system_info() const override {
        return {};
    }
    [[nodiscard]] helix::AmsType get_type() const override {
        return helix::AmsType::NONE;
    }
    [[nodiscard]] helix::SlotInfo get_slot_info(int) const override {
        return {};
    }
    [[nodiscard]] helix::AmsAction get_current_action() const override {
        return helix::AmsAction::IDLE;
    }
    [[nodiscard]] int get_current_tool() const override {
        return -1;
    }
    [[nodiscard]] int get_current_slot() const override {
        return -1;
    }
    [[nodiscard]] bool is_filament_loaded() const override {
        return false;
    }
    [[nodiscard]] helix::PathTopology get_topology() const override {
        return helix::PathTopology::LINEAR;
    }
    [[nodiscard]] helix::PathSegment get_filament_segment() const override {
        return helix::PathSegment::NONE;
    }
    [[nodiscard]] helix::PathSegment get_slot_filament_segment(int) const override {
        return helix::PathSegment::NONE;
    }
    [[nodiscard]] helix::PathSegment infer_error_segment() const override {
        return helix::PathSegment::NONE;
    }
    AmsError load_filament(int) override {
        return AmsErrorHelper::success();
    }
    AmsError unload_filament(int) override {
        return AmsErrorHelper::success();
    }
    AmsError select_slot(int) override {
        return AmsErrorHelper::success();
    }
    AmsError change_tool(int) override {
        return AmsErrorHelper::success();
    }
    AmsError recover() override {
        return AmsErrorHelper::success();
    }
    AmsError reset() override {
        return AmsErrorHelper::success();
    }
    AmsError cancel() override {
        return AmsErrorHelper::success();
    }
    AmsError apply_user_edit(int, const helix::SlotInfo&, const helix::ams::Observation&) override {
        return AmsErrorHelper::success();
    }
    AmsError sync_external_identity(int, const helix::SlotInfo&) override {
        return AmsErrorHelper::success();
    }
    AmsError can_set_tool_mapping(int, int) const override {
        return AmsErrorHelper::success();
    }
    AmsError set_tool_mapping_impl(int, int) override {
        return AmsErrorHelper::success();
    }
    AmsError enable_bypass() override {
        return AmsErrorHelper::success();
    }
    AmsError disable_bypass() override {
        return AmsErrorHelper::success();
    }
    [[nodiscard]] bool is_bypass_active() const override {
        return false;
    }
};

/// Declares a caller-chosen strategy and readiness, so can_remap() can be
/// covered over combinations no shipped backend produces — GcodeRewrite, and
/// None-but-not-ready. Without it those rows would be unreachable and the
/// function would only ever be tested on the diagonal.
class StubBackend : public BaseProbe {
  public:
    StubBackend(AmsBackend::RemapStrategy strategy, bool ready)
        : strategy_(strategy), ready_(ready) {}

    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return strategy_;
    }
    [[nodiscard]] bool remap_ready() const override {
        return ready_;
    }

  private:
    RemapStrategy strategy_;
    bool ready_;
};

} // namespace

TEST_CASE("Native-strategy backends return RemapStrategy::Native", "[ams][strategy]") {
    AfcProbe afc;
    HappyHareProbe hh;
    CfsProbe cfs;
    Ad5xIfsProbe ad5x;
    ToolChangerProbe tc;
    QidiProbe qidi;
    const AmsBackend* const native[] = {&afc, &hh, &cfs, &ad5x, &tc, &qidi};
    for (const AmsBackend* backend : native) {
        CHECK(backend->get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
}

TEST_CASE("The non-Native backends answer their own remap strategy", "[ams][strategy]") {
    SnapmakerProbe sm;
    CHECK(sm.get_remap_strategy() == AmsBackend::RemapStrategy::PrePrintSend);
    BaseProbe base;
    CHECK(base.get_remap_strategy() == AmsBackend::RemapStrategy::None);
    // ACE uses ACE_CHANGE_TOOL TOOL=n, not the Tn/SM_PRINT_* families that
    // GcodeToolRemapper handles, so remap is disabled until that command family
    // is implemented and validated on a real ACE file.
    AceProbe ace;
    CHECK(ace.get_remap_strategy() == AmsBackend::RemapStrategy::None);
}

// ---------------------------------------------------------------------------
// remap_ready(): a backend can be BUILT to remap and not be able to yet, because the
// firmware object it writes through has not been discovered. Only AD5X IFS has such a gate.
// ---------------------------------------------------------------------------

TEST_CASE("Backends declare remap readiness, defaulting to ready", "[ams][strategy]") {
    SECTION("A backend with no discovery gate is ready on construction") {
        AfcProbe afc;
        REQUIRE(afc.remap_ready());
        SnapmakerProbe sm;
        REQUIRE(sm.remap_ready());
        // Ready, and still cannot remap: readiness says a declared route is usable.
        AceProbe ace;
        REQUIRE(ace.remap_ready());
        REQUIRE_FALSE(helix::printer::can_remap(ace));
    }

    SECTION("AD5X IFS is not ready until the wire tool_map is discovered") {
        helix::test::StartedBackend<Ad5xIfsProbe> ad5x;
        REQUIRE(ad5x.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
        REQUIRE_FALSE(ad5x.remap_ready());
        REQUIRE_FALSE(helix::printer::can_remap(ad5x));

        // A printer.ifs frame carrying tool_map arms the gate.
        ad5x.fire_status(helix::test::ad5x_identity_tool_map_frame());

        REQUIRE(ad5x.remap_ready());
        REQUIRE(helix::printer::can_remap(ad5x));
        // The strategy is a static declaration and must NOT move with the gate.
        REQUIRE(ad5x.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
}

TEST_CASE("can_remap needs both a declared route and readiness", "[ams][strategy]") {
    using RS = AmsBackend::RemapStrategy;
    struct Case {
        const char* name;
        RS strategy;
        bool ready;
        bool expected;
    };
    const Case cases[] = {
        {"Native and ready", RS::Native, true, true},
        {"Native, not ready", RS::Native, false, false},
        {"PrePrintSend and ready", RS::PrePrintSend, true, true},
        {"PrePrintSend, not ready", RS::PrePrintSend, false, false},
        {"GcodeRewrite and ready", RS::GcodeRewrite, true, true},
        {"None but ready", RS::None, true, false},
        {"None and not ready", RS::None, false, false},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        StubBackend stub(c.strategy, c.ready);
        CHECK(helix::printer::can_remap(stub) == c.expected);
    }
}

TEST_CASE("can_write_mapping_table needs a table-writing route AND readiness", "[ams][strategy]") {
    using RS = AmsBackend::RemapStrategy;
    struct Case {
        const char* name;
        RS strategy;
        bool ready;
        bool expected;
    };
    const Case cases[] = {
        {"Native and ready", RS::Native, true, true},
        // The rows that make this a different question from can_remap(): the
        // U1 can remap and writes no table, a GcodeRewrite backend honors the
        // pick by rewriting the job file and has no table either, and a Native
        // backend that has not discovered its firmware object yet writes
        // nothing that lands.
        {"PrePrintSend and ready", RS::PrePrintSend, true, false},
        {"Native, not ready", RS::Native, false, false},
        {"GcodeRewrite and ready", RS::GcodeRewrite, true, false},
        {"GcodeRewrite, not ready", RS::GcodeRewrite, false, false},
        {"None but ready", RS::None, true, false},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        StubBackend stub(c.strategy, c.ready);
        CHECK(helix::printer::can_write_mapping_table(stub) == c.expected);
    }
}

TEST_CASE("remap_is_persistent separates the routes that outlive the send", "[ams][strategy]") {
    using RS = AmsBackend::RemapStrategy;
    // Where this parts company with can_write_mapping_table(): GcodeRewrite's
    // answer outlives the send because it is in the job file, but there is no
    // table for set_tool_mapping() to write.
    // Native writes the machine's own table; GcodeRewrite writes the job file.
    CHECK(helix::printer::remap_is_persistent(RS::Native));
    CHECK(helix::printer::remap_is_persistent(RS::GcodeRewrite));
    // PrePrintSend tells the firmware once, before PRINT_START. Nothing keeps it.
    CHECK_FALSE(helix::printer::remap_is_persistent(RS::PrePrintSend));
    CHECK_FALSE(helix::printer::remap_is_persistent(RS::None));
}

// ---------------------------------------------------------------------------
// owns_tool_mapping_table(): a DIFFERENT question, and the U1 is where the two
// part company. It decides whether AmsState hands ToolState an AMS tool->slot
// topology or leaves it enumerating extruders. Routing it through remap
// capability would give the U1 — four independent extruders, remapped through a
// pre-print send, owning no table — a topology it has never had.
// ---------------------------------------------------------------------------

TEST_CASE("owns_tool_mapping_table is answered independently of remap capability",
          "[ams][strategy]") {
    SECTION("Snapmaker can remap and owns no table") {
        SnapmakerProbe sm;
        CHECK(helix::printer::can_remap(sm));
        CHECK_FALSE(sm.owns_tool_mapping_table());
    }

    SECTION("Lane-multiplexing backends own one") {
        AfcProbe afc;
        HappyHareProbe hh;
        CfsProbe cfs;
        QidiProbe qidi;
        ToolChangerProbe tc;
        const AmsBackend* const owners[] = {&afc, &hh, &cfs, &qidi, &tc};
        for (const AmsBackend* backend : owners) {
            CHECK(backend->owns_tool_mapping_table());
        }
    }

    SECTION("ACE and the base default own none") {
        AceProbe ace;
        CHECK_FALSE(ace.owns_tool_mapping_table());
        BaseProbe base;
        CHECK_FALSE(base.owns_tool_mapping_table());
    }

    SECTION("AD5X IFS owns one only once the wire tool_map is discovered") {
        helix::test::StartedBackend<Ad5xIfsProbe> ad5x;
        CHECK_FALSE(ad5x.owns_tool_mapping_table());
        ad5x.fire_status(helix::test::ad5x_identity_tool_map_frame());
        CHECK(ad5x.owns_tool_mapping_table());
    }
}

// ---------------------------------------------------------------------------
// requires_preprint_send(): a backend capability that gates whether
// PrintStartController must emit build_preprint_gcode() BEFORE PRINT_START. Only
// Snapmaker U1 needs the pre-send; everyone else takes the synchronous start path.
// ---------------------------------------------------------------------------

TEST_CASE("Only Snapmaker requires a pre-print send", "[ams][strategy][preprint]") {
    SnapmakerProbe sm;
    AfcProbe afc;
    HappyHareProbe hh;
    CfsProbe cfs;
    Ad5xIfsProbe ad5x;
    ToolChangerProbe tc;
    QidiProbe qidi;
    AceProbe ace;
    BaseProbe base;
    CHECK(sm.requires_preprint_send());
    const AmsBackend* const others[] = {&afc, &hh, &cfs, &ad5x, &tc, &qidi, &ace, &base};
    for (const AmsBackend* backend : others) {
        CHECK_FALSE(backend->requires_preprint_send());
    }
}

// The pre-send capability must agree with build_preprint_gcode() being a no-op:
// a backend that returns "" for all inputs has nothing to send, and one that
// requires the send must actually produce gcode for a non-empty tool set. This
// pins the invariant the controller relies on (gate == has-work-to-do).
TEST_CASE("requires_preprint_send agrees with build_preprint_gcode output",
          "[ams][strategy][preprint]") {
    SECTION("Snapmaker: requires send AND emits gcode for a used tool") {
        SnapmakerProbe sm;
        REQUIRE(sm.requires_preprint_send());
        REQUIRE_FALSE(sm.build_preprint_gcode({0}, {}).empty());
    }
    SECTION("Native backend: no send AND no step to build") {
        AfcProbe afc;
        REQUIRE_FALSE(afc.requires_preprint_send());
        REQUIRE(afc.as<helix::IPrintRouting>() == nullptr);
    }
}

// ---------------------------------------------------------------------------
// The MOCK has to satisfy the same invariant. An empty build in Snapmaker mode makes
// the controller start directly, so a head picked under --test never reaches the wire
// while the picker logs it as stored, and the U1's remap chain goes unexercised in
// every mock run, which is where this feature is developed.
// ---------------------------------------------------------------------------

TEST_CASE("Mock in Snapmaker mode satisfies requires_preprint_send/build agreement",
          "[ams][strategy][preprint][mock]") {
    AmsBackendMock mock(4);
    mock.set_snapmaker_mode(true);

    REQUIRE(mock.requires_preprint_send());
    // An empty string breaks the gate's promise and silently drops the user's remap.
    REQUIRE_FALSE(mock.build_preprint_gcode({0}, {}).empty());
}

TEST_CASE("Mock without Snapmaker mode neither requires a pre-send nor emits gcode",
          "[ams][strategy][preprint][mock]") {
    // A backend that does not gate on the pre-send must have nothing to send.
    AmsBackendMock mock(4);
    REQUIRE_FALSE(mock.requires_preprint_send());
    REQUIRE(mock.build_preprint_gcode({0, 1, 2}, {{0, 1}}).empty());

    // Tool-changer mode is the mock's other non-Snapmaker shape.
    mock.set_tool_changer_mode(true);
    REQUIRE_FALSE(mock.requires_preprint_send());
    REQUIRE(mock.build_preprint_gcode({0, 1, 2}, {{0, 1}}).empty());
}

TEST_CASE("Mock Snapmaker pre-print gcode is byte-identical to the real backend",
          "[ams][strategy][preprint][mock]") {
    // The mock has to emit what the printer will actually be sent, or a format
    // change lands untested on hardware. Compared against the real builder rather
    // than a literal, so this cannot drift when the format moves.
    AmsBackendMock mock(4);
    mock.set_snapmaker_mode(true);
    SnapmakerProbe real;

    struct Case {
        std::set<int> tools;
        std::map<int, int> remap;
    };
    const Case cases[] = {
        {{0}, {}},          // single tool, no remap
        {{0, 1, 2, 3}, {}}, // every head, identity
        {{0, 2}, {{0, 3}}}, // the "print T0 from head 4" ask in #962
        {{0, 1}, {{1, 0}}}, // colliding heads (dedup path)
        {{5}, {}},          // extended tool -> firmware head 0
    };

    for (const auto& c : cases) {
        const std::string from_mock = mock.build_preprint_gcode(c.tools, c.remap);
        const std::string from_real = real.build_preprint_gcode(c.tools, c.remap);
        CAPTURE(from_mock, from_real);
        REQUIRE(from_mock == from_real);
        REQUIRE_FALSE(from_mock.empty());
    }

    // Empty tool set is the one input that legitimately yields nothing, on both.
    REQUIRE(mock.build_preprint_gcode({}, {}).empty());
    REQUIRE(real.build_preprint_gcode({}, {}).empty());
}

TEST_CASE("remap_block answers the backend's half before anything else", "[ams][strategy][block]") {
    using helix::printer::remap_block;
    using helix::printer::RemapBlock;
    AmsBackendMock backend;

    SECTION("a backend with no route is blocked whatever the job or plugin say") {
        backend.set_remap_strategy(AmsBackend::RemapStrategy::None);
        CHECK(remap_block(backend, GcodeRewriteBlock::None, 4) == RemapBlock::NoStrategy);
        CHECK(remap_block(backend, GcodeRewriteBlock::Probing, 0) == RemapBlock::NoStrategy);
    }

    SECTION("a route that is not up yet outranks the job and plugin terms") {
        backend.set_remap_strategy(AmsBackend::RemapStrategy::Native);
        backend.set_remap_ready(false);
        CHECK(remap_block(backend, GcodeRewriteBlock::None, 4) == RemapBlock::NotReady);
    }
}

TEST_CASE("remap_block names the job before it names the plugin", "[ams][strategy][block]") {
    using helix::printer::remap_block;
    using helix::printer::RemapBlock;
    AmsBackendMock backend;
    backend.set_remap_strategy(AmsBackend::RemapStrategy::GcodeRewrite);

    // Installing the plugin would not make a toolless job mappable, so the
    // plugin must not be the reason offered for one.
    SECTION("no tools reads as NothingToRemap even with the plugin absent") {
        CHECK(remap_block(backend, GcodeRewriteBlock::NeedsPlugin, 0) ==
              RemapBlock::NothingToRemap);
    }

    SECTION("no tools reads as NothingToRemap even while the probe is in flight") {
        CHECK(remap_block(backend, GcodeRewriteBlock::Probing, 0) == RemapBlock::NothingToRemap);
    }

    SECTION("a negative count is treated as none, not as a mappable job") {
        CHECK(remap_block(backend, GcodeRewriteBlock::None, -1) == RemapBlock::NothingToRemap);
    }
}

TEST_CASE("Only GcodeRewrite consults the plugin", "[ams][strategy][block]") {
    using helix::printer::remap_block;
    using helix::printer::RemapBlock;
    AmsBackendMock backend;

    SECTION("GcodeRewrite without the plugin is blocked") {
        backend.set_remap_strategy(AmsBackend::RemapStrategy::GcodeRewrite);
        CHECK(remap_block(backend, GcodeRewriteBlock::NeedsPlugin, 4) == RemapBlock::NeedsPlugin);
    }

    SECTION("GcodeRewrite with the plugin is available") {
        backend.set_remap_strategy(AmsBackend::RemapStrategy::GcodeRewrite);
        CHECK(remap_block(backend, GcodeRewriteBlock::None, 4) == RemapBlock::None);
    }

    SECTION("an unprobed plugin is Probing, never NeedsPlugin") {
        // The probe lands after first paint. Reading -1 as absent is what makes
        // a card show its refusal on every boot and then withdraw it.
        backend.set_remap_strategy(AmsBackend::RemapStrategy::GcodeRewrite);
        CHECK(remap_block(backend, GcodeRewriteBlock::Probing, 4) == RemapBlock::Probing);
    }

    SECTION("routes that write firmware state ignore the plugin entirely") {
        for (auto strategy :
             {AmsBackend::RemapStrategy::Native, AmsBackend::RemapStrategy::PrePrintSend}) {
            backend.set_remap_strategy(strategy);
            CHECK(remap_block(backend, GcodeRewriteBlock::NeedsPlugin, 4) == RemapBlock::None);
            CHECK(remap_block(backend, GcodeRewriteBlock::Probing, 4) == RemapBlock::None);
            CHECK(remap_block(backend, GcodeRewriteBlock::None, 4) == RemapBlock::None);
            CHECK(remap_block(backend, GcodeRewriteBlock::NoLocalCopies, 4) == RemapBlock::None);
        }
    }

    SECTION("GcodeRewrite on a transport without local copies is not offered on this device") {
        backend.set_remap_strategy(AmsBackend::RemapStrategy::GcodeRewrite);
        CHECK(remap_block(backend, GcodeRewriteBlock::NoLocalCopies, 4) ==
              RemapBlock::NotOnThisDevice);
        // A job with nothing to map still says so first.
        CHECK(remap_block(backend, GcodeRewriteBlock::NoLocalCopies, 0) ==
              RemapBlock::NothingToRemap);
    }
}

TEST_CASE("gcode_rewrite_block: the transport outranks the plugin", "[ams][strategy][block]") {
    using helix::gcode_rewrite_available;
    using helix::gcode_rewrite_block;
    CHECK(gcode_rewrite_block(1, true) == GcodeRewriteBlock::None);
    CHECK(gcode_rewrite_block(0, true) == GcodeRewriteBlock::NeedsPlugin);
    CHECK(gcode_rewrite_block(-1, true) == GcodeRewriteBlock::Probing);
    for (int plugin : {-1, 0, 1}) {
        CHECK(gcode_rewrite_block(plugin, false) == GcodeRewriteBlock::NoLocalCopies);
    }
    // A pending probe keeps a gated row visible; every refusal hides it.
    CHECK(gcode_rewrite_available(GcodeRewriteBlock::None) == 1);
    CHECK(gcode_rewrite_available(GcodeRewriteBlock::Probing) == -1);
    CHECK(gcode_rewrite_available(GcodeRewriteBlock::NeedsPlugin) == 0);
    CHECK(gcode_rewrite_available(GcodeRewriteBlock::NoLocalCopies) == 0);
}

TEST_CASE("remap_block agrees with can_remap wherever can_remap has an opinion",
          "[ams][strategy][block]") {
    using helix::printer::can_remap;
    using helix::printer::remap_block;
    using helix::printer::RemapBlock;
    AmsBackendMock backend;

    // can_remap() is the backend-only half, so it must never say yes where
    // remap_block() reports a backend-side block, nor no where it reports None.
    for (auto strategy :
         {AmsBackend::RemapStrategy::None, AmsBackend::RemapStrategy::Native,
          AmsBackend::RemapStrategy::GcodeRewrite, AmsBackend::RemapStrategy::PrePrintSend}) {
        for (bool ready : {false, true}) {
            backend.set_remap_strategy(strategy);
            backend.set_remap_ready(ready);
            const auto block = remap_block(backend, GcodeRewriteBlock::None, 4);
            const bool backend_side_block =
                block == RemapBlock::NoStrategy || block == RemapBlock::NotReady;
            CHECK(can_remap(backend) == !backend_side_block);
        }
    }
}

TEST_CASE("remap_block_name covers every rung", "[ams][strategy][block]") {
    using helix::printer::remap_block_name;
    using helix::printer::RemapBlock;
    // A name falling through to "unknown" means a rung was added without a
    // spelling, and every log line about it would then be indistinguishable.
    for (auto block :
         {RemapBlock::None, RemapBlock::Probing, RemapBlock::NoStrategy, RemapBlock::NotReady,
          RemapBlock::NeedsPlugin, RemapBlock::NothingToRemap, RemapBlock::NotOnThisDevice}) {
        CHECK(std::string(remap_block_name(block)) != "unknown");
    }
}

TEST_CASE("Only a file rewrite reports start macro params it leaves alone", "[ams][strategy]") {
    using helix::printer::start_params_remap_leaves;
    using S = AmsBackend::RemapStrategy;
    const std::string_view line = "PRINT_START INITIAL_TOOL=0 EXTRUDER1_TEMP=0 BED_TEMP=60";

    CHECK(start_params_remap_leaves(S::GcodeRewrite, line) ==
          std::vector<std::string>{"EXTRUDER1_TEMP"});
    // No start line scanned for this file: nothing to say.
    CHECK(start_params_remap_leaves(S::GcodeRewrite, "").empty());
    // The other routes never rewrite the file, so the rewrite's blind spots are not theirs.
    CHECK(start_params_remap_leaves(S::Native, line).empty());
    CHECK(start_params_remap_leaves(S::PrePrintSend, line).empty());
    CHECK(start_params_remap_leaves(S::None, line).empty());
}

namespace {
helix::ToolMapping picked(int tool, int slot) {
    helix::ToolMapping m;
    m.tool_index = tool;
    m.mapped_slot = slot;
    return m;
}
} // namespace

TEST_CASE("gcode_rewrite_remap keeps only the tools a pick actually sends", "[ams][strategy]") {
    using helix::printer::gcode_rewrite_remap;
    CHECK(gcode_rewrite_remap({picked(0, 1), picked(1, -1), picked(-1, 2), picked(2, 2)}) ==
          std::map<int, int>{{0, 1}, {2, 2}});
    CHECK(gcode_rewrite_remap({}).empty());
}

TEST_CASE("The start macro note follows the picks", "[ams][strategy]") {
    using helix::printer::start_macro_note_shown;
    // Identity: the rewrite changes no tool number, so nothing is left behind.
    CHECK_FALSE(start_macro_note_shown(true, {picked(0, 0), picked(1, 1)}));
    // One tool moved.
    CHECK(start_macro_note_shown(true, {picked(0, 0), picked(1, 0)}));
    // An unmapped or auto tool is not sent, so it moves nothing.
    CHECK_FALSE(start_macro_note_shown(true, {picked(0, -1), picked(1, 1)}));
    // A move with nothing the rewrite leaves: no note.
    CHECK_FALSE(start_macro_note_shown(false, {picked(0, 1)}));
}
