// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_guided_load_modal.cpp
 * @brief When the guided load opens, and which view each stage shows.
 *
 * Run with: ./build/bin/helix-tests "[guided_load]"
 */

#include "ui_guided_load_modal.h"

#include "../catch_amalgamated.hpp"

using helix::GuidedLoadStage;
using helix::ui::GuidedLoadModal;

TEST_CASE("GuidedLoadModal: never opens while a print job holds the machine", "[guided_load]") {
    CHECK(GuidedLoadModal::opens_during(PrintState::Idle));
    CHECK(GuidedLoadModal::opens_during(PrintState::Complete));
    CHECK(GuidedLoadModal::opens_during(PrintState::Cancelled));
    CHECK(GuidedLoadModal::opens_during(PrintState::Error));
    // Paused is the runout-recovery case: its buttons are motion-gated there,
    // and every ending would cool the nozzle under the paused job.
    CHECK_FALSE(GuidedLoadModal::opens_during(PrintState::Paused));
    CHECK_FALSE(GuidedLoadModal::opens_during(PrintState::Printing));
    CHECK_FALSE(GuidedLoadModal::opens_during(PrintState::Preparing));
}

TEST_CASE("GuidedLoadModal: each stage maps to the view that offers its buttons", "[guided_load]") {
    using View = GuidedLoadModal::View;
    CHECK(GuidedLoadModal::view_for(GuidedLoadStage::AwaitPush) == View::Push);
    CHECK(GuidedLoadModal::view_for(GuidedLoadStage::ConfirmOutput) == View::Confirm);
    CHECK(GuidedLoadModal::view_for(GuidedLoadStage::Feeding) == View::Working);
    CHECK(GuidedLoadModal::view_for(GuidedLoadStage::Finishing) == View::Working);
    CHECK(GuidedLoadModal::view_for(GuidedLoadStage::Heating) == View::Heating);
}
