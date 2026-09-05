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
//                  backend has no internal tool-routing. No backend declares it
//                  today: it was written for ACE, which declares None until the
//                  ACE_CHANGE_TOOL family is handled
//   SnapmakerNative — firmware pre-print send, no gcode rewrite (Snapmaker U1)
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

#include "../catch_amalgamated.hpp"

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
    [[nodiscard]] AmsSystemInfo get_system_info() const override {
        return {};
    }
    [[nodiscard]] AmsType get_type() const override {
        return AmsType::NONE;
    }
    [[nodiscard]] SlotInfo get_slot_info(int) const override {
        return {};
    }
    [[nodiscard]] AmsAction get_current_action() const override {
        return AmsAction::IDLE;
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
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::LINEAR;
    }
    [[nodiscard]] PathSegment get_filament_segment() const override {
        return PathSegment::NONE;
    }
    [[nodiscard]] PathSegment get_slot_filament_segment(int) const override {
        return PathSegment::NONE;
    }
    [[nodiscard]] PathSegment infer_error_segment() const override {
        return PathSegment::NONE;
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
    AmsError set_slot_info(int, const SlotInfo&, bool) override {
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
    SECTION("AFC") {
        AfcProbe afc;
        REQUIRE(afc.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
    SECTION("Happy Hare") {
        HappyHareProbe hh;
        REQUIRE(hh.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
    SECTION("CFS") {
        CfsProbe cfs;
        REQUIRE(cfs.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
    SECTION("AD5X IFS") {
        Ad5xIfsProbe ad5x;
        REQUIRE(ad5x.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
    SECTION("ToolChanger") {
        ToolChangerProbe tc;
        REQUIRE(tc.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
    SECTION("QIDI Box") {
        QidiProbe qidi;
        REQUIRE(qidi.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
}

TEST_CASE("Snapmaker returns RemapStrategy::SnapmakerNative", "[ams][strategy]") {
    SECTION("Snapmaker") {
        SnapmakerProbe sm;
        REQUIRE(sm.get_remap_strategy() == AmsBackend::RemapStrategy::SnapmakerNative);
    }
}

TEST_CASE("Base AmsBackend default returns RemapStrategy::None", "[ams][strategy]") {
    BaseProbe base;
    REQUIRE(base.get_remap_strategy() == AmsBackend::RemapStrategy::None);
}

TEST_CASE("ACE returns RemapStrategy::None (GcodeRewrite unimplemented)", "[ams][strategy]") {
    // ACE uses ACE_CHANGE_TOOL TOOL=n, not the Tn/SM_PRINT_* families that
    // GcodeToolRemapper handles, so remap is disabled until that command family
    // is implemented and validated on a real ACE file.
    AceProbe ace;
    REQUIRE(ace.get_remap_strategy() == AmsBackend::RemapStrategy::None);
}

// ---------------------------------------------------------------------------
// remap_ready(): the axis the retired ToolMappingCapabilities hid.
//
// A backend can be BUILT to remap and not be able to yet, because the firmware
// object it writes through has not been discovered. Only AD5X IFS has such a
// gate, and it is exactly where the old model contradicted itself:
// get_remap_strategy() answered Native unconditionally while
// get_tool_mapping_capabilities() answered {false,false} until _IFS_VARS was
// found. Two spellings, opposite answers, no compile error and no test.
// ---------------------------------------------------------------------------

TEST_CASE("Backends declare remap readiness, defaulting to ready", "[ams][strategy]") {
    SECTION("A backend with no discovery gate is ready on construction") {
        AfcProbe afc;
        REQUIRE(afc.remap_ready());
        SnapmakerProbe sm;
        REQUIRE(sm.remap_ready());
        // Ready, and still cannot remap: readiness says the declared route is
        // usable, not that one was declared.
        AceProbe ace;
        REQUIRE(ace.remap_ready());
        REQUIRE_FALSE(helix::printer::can_remap(ace));
    }

    SECTION("AD5X IFS is not ready until the wire tool_map is discovered") {
        Ad5xIfsProbe ad5x;
        REQUIRE(ad5x.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
        REQUIRE_FALSE(ad5x.remap_ready());
        REQUIRE_FALSE(helix::printer::can_remap(ad5x));

        // A printer.ifs frame carrying tool_map arms the gate.
        Ad5xIfsTestAccess::deliver_identity_tool_map(ad5x);

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
        {"SnapmakerNative and ready", RS::SnapmakerNative, true, true},
        {"SnapmakerNative, not ready", RS::SnapmakerNative, false, false},
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
        // The two rows that make this a different question from can_remap():
        // the U1 can remap and writes no table, and a Native backend that has
        // not discovered its firmware object yet writes nothing that lands.
        {"SnapmakerNative and ready", RS::SnapmakerNative, true, false},
        {"Native, not ready", RS::Native, false, false},
        {"GcodeRewrite and ready", RS::GcodeRewrite, true, true},
        {"GcodeRewrite, not ready", RS::GcodeRewrite, false, false},
        {"None but ready", RS::None, true, false},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        StubBackend stub(c.strategy, c.ready);
        CHECK(helix::printer::can_write_mapping_table(stub) == c.expected);
    }
}

TEST_CASE("remap_is_persistent separates the table-writing routes from the pre-send",
          "[ams][strategy]") {
    using RS = AmsBackend::RemapStrategy;
    // Native writes the machine's own table; GcodeRewrite writes the job file.
    CHECK(helix::printer::remap_is_persistent(RS::Native));
    CHECK(helix::printer::remap_is_persistent(RS::GcodeRewrite));
    // SnapmakerNative tells the firmware once, before PRINT_START. Nothing keeps it.
    CHECK_FALSE(helix::printer::remap_is_persistent(RS::SnapmakerNative));
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
        CHECK(afc.owns_tool_mapping_table());
        HappyHareProbe hh;
        CHECK(hh.owns_tool_mapping_table());
        CfsProbe cfs;
        CHECK(cfs.owns_tool_mapping_table());
        QidiProbe qidi;
        CHECK(qidi.owns_tool_mapping_table());
        ToolChangerProbe tc;
        CHECK(tc.owns_tool_mapping_table());
    }

    SECTION("ACE and the base default own none") {
        AceProbe ace;
        CHECK_FALSE(ace.owns_tool_mapping_table());
        BaseProbe base;
        CHECK_FALSE(base.owns_tool_mapping_table());
    }

    SECTION("AD5X IFS owns one only once the wire tool_map is discovered") {
        Ad5xIfsProbe ad5x;
        CHECK_FALSE(ad5x.owns_tool_mapping_table());
        Ad5xIfsTestAccess::deliver_identity_tool_map(ad5x);
        CHECK(ad5x.owns_tool_mapping_table());
    }
}

// ---------------------------------------------------------------------------
// requires_preprint_send(): a backend capability that gates whether
// PrintStartController must emit build_preprint_gcode() BEFORE PRINT_START.
// Previously the controller proxied this as
// `get_remap_strategy() == SnapmakerNative` (a backend-type check disguised as
// a strategy comparison). Only Snapmaker U1 needs the pre-send; everyone else
// takes the unchanged synchronous start path. Pin each backend so a regression
// (e.g. accidentally enabling the pre-send for a Native backend) fails here.
// ---------------------------------------------------------------------------

TEST_CASE("Only Snapmaker requires a pre-print send", "[ams][strategy][preprint]") {
    SECTION("Snapmaker requires the pre-print send") {
        SnapmakerProbe sm;
        REQUIRE(sm.requires_preprint_send());
    }
    SECTION("AFC does not") {
        AfcProbe afc;
        REQUIRE_FALSE(afc.requires_preprint_send());
    }
    SECTION("Happy Hare does not") {
        HappyHareProbe hh;
        REQUIRE_FALSE(hh.requires_preprint_send());
    }
    SECTION("CFS does not") {
        CfsProbe cfs;
        REQUIRE_FALSE(cfs.requires_preprint_send());
    }
    SECTION("AD5X IFS does not") {
        Ad5xIfsProbe ad5x;
        REQUIRE_FALSE(ad5x.requires_preprint_send());
    }
    SECTION("ToolChanger does not") {
        ToolChangerProbe tc;
        REQUIRE_FALSE(tc.requires_preprint_send());
    }
    SECTION("QIDI Box does not") {
        QidiProbe qidi;
        REQUIRE_FALSE(qidi.requires_preprint_send());
    }
    SECTION("ACE does not") {
        AceProbe ace;
        REQUIRE_FALSE(ace.requires_preprint_send());
    }
    SECTION("Base default does not") {
        BaseProbe base;
        REQUIRE_FALSE(base.requires_preprint_send());
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
    SECTION("Native backend: no send AND emits nothing") {
        AfcProbe afc;
        REQUIRE_FALSE(afc.requires_preprint_send());
        REQUIRE(afc.build_preprint_gcode({0, 1, 2}, {{0, 1}}).empty());
    }
}

// ---------------------------------------------------------------------------
// The MOCK has to satisfy the same invariant, and it is the one backend that
// did not.
//
// AmsBackendMock::requires_preprint_send() returns true in Snapmaker mode, but
// the mock never overrode build_preprint_gcode(), so the base returned "". That
// makes the U1's whole remap chain a no-op under --test: the controller logs
// "U1 pre-print config empty - starting print directly" and the user's pick
// never reaches the wire. Driven on the mock, picking a head for T0 through the
// picker looked like it worked (the mapping is stored and logged) while nothing
// was sent. A regression anywhere in get_tools_used() ->
// get_effective_remap() -> build_preprint_gcode() would be invisible in every
// mock run, which is where this feature is developed.
//
// The case above pins the invariant for the real backends only; that is exactly
// why the mock could violate it unnoticed.
// ---------------------------------------------------------------------------

TEST_CASE("Mock in Snapmaker mode satisfies requires_preprint_send/build agreement",
          "[ams][strategy][preprint][mock]") {
    AmsBackendMock mock(4);
    mock.set_snapmaker_mode(true);

    REQUIRE(mock.requires_preprint_send());
    // The gate promises there is work to do; an empty string breaks that promise
    // and silently drops the user's remap.
    REQUIRE_FALSE(mock.build_preprint_gcode({0}, {}).empty());
}

TEST_CASE("Mock without Snapmaker mode neither requires a pre-send nor emits gcode",
          "[ams][strategy][preprint][mock]") {
    // The other half of the invariant: a backend that does not gate on the
    // pre-send must have nothing to send, or the controller would skip real work.
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
    // A mock that emits SOMETHING is not enough - it has to emit what the
    // printer will actually be sent, or the mock teaches the wrong shape and a
    // format change lands untested on hardware. Compared against the real
    // builder rather than a literal, so this cannot drift when the format moves.
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
