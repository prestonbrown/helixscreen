// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_dryer_airflow.h"

#include "../test_fixtures.h"
#include "display_settings_manager.h"
#include "dryer_airflow_geometry.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace af = helix::ui::dryer_airflow;

TEST_CASE("dryer_airflow: chevrons stay evenly spaced and fade at both ends", "[dryer_airflow]") {
    for (float phase : {0.0f, 0.2f, 0.5f, 0.95f}) {
        CAPTURE(phase);
        const auto c = af::chevrons(phase);
        for (int i = 0; i < af::kChevronCount; ++i) {
            const float next = c[static_cast<size_t>((i + 1) % af::kChevronCount)].rise;
            float gap = next - c[static_cast<size_t>(i)].rise;
            gap -= std::floor(gap);
            CHECK(gap == Catch::Approx(1.0f / af::kChevronCount).margin(1e-4));
        }
    }
    // At the bottom and the top a chevron is invisible; mid-box it is opaque.
    CHECK(af::chevrons(0.0f)[0].opa == 0);
    CHECK(af::chevrons(0.5f)[0].opa == 255);
    CHECK(af::chevrons(0.999f)[0].opa < 5);
}

TEST_CASE("dryer_airflow: the stream rises only while animations are on", "[dryer_airflow]") {
    CHECK(af::advance(0.25f, false) == 0.25f);
    const float moved = af::advance(0.25f, true);
    CHECK(moved > 0.25f);
    CHECK(af::chevrons(moved)[0].rise > af::chevrons(0.25f)[0].rise);
    // It wraps rather than running off the top.
    CHECK(af::advance(1.0f - 0.5f / af::kTicksPerRise, true) < 0.1f);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "dryer_airflow: colour follows the phase, the tick follows the setting",
                 "[dryer_airflow][widget]") {
    helix::ui::register_dryer_airflow_widget();
    SettingsManager::instance().init_subjects();
    const ThemePalette& p = ThemeManager::instance().current_palette();
    const auto rgb = [](lv_color_t c) { return lv_color_to_u32(c) & 0xFFFFFF; };
    REQUIRE(rgb(p.info) != rgb(p.warning));
    CHECK(rgb(helix::ui::dryer_airflow_color(DryingPhase::Holding)) == rgb(p.info));
    CHECK(rgb(helix::ui::dryer_airflow_color(DryingPhase::Heating)) == rgb(p.warning));
    CHECK(rgb(helix::ui::dryer_airflow_color(DryingPhase::Unknown)) == rgb(p.warning));

    auto& dsm = DisplaySettingsManager::instance();
    const bool was = dsm.get_animations_enabled();
    const char* attrs[] = {"width", "56", "height", "56", nullptr};
    auto* flow = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "dryer_airflow", attrs));
    REQUIRE(flow != nullptr);

    // The test display never runs periodic timers, so step the shared one by hand.
    dsm.set_animations_enabled(false);
    const float still = helix::ui::dryer_airflow_position();
    helix::ui::dryer_airflow_tick();
    CHECK(helix::ui::dryer_airflow_position() == still);

    dsm.set_animations_enabled(true);
    helix::ui::dryer_airflow_tick();
    CHECK(helix::ui::dryer_airflow_position() != still);

    dsm.set_animations_enabled(was);
    lv_obj_delete(flow);
}
