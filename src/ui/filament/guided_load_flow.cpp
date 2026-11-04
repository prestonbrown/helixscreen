// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "guided_load_flow.h"

#include <utility>

namespace helix {

GuidedLoadFlow::GuidedLoadFlow(filament_macros::GuidedLoad steps, bool waits_for_push)
    : steps_(std::move(steps)), waits_for_push_(waits_for_push),
      stage_(waits_for_push ? GuidedLoadStage::AwaitPush : GuidedLoadStage::ConfirmOutput) {}

std::string GuidedLoadFlow::feed() {
    stage_ = GuidedLoadStage::Feeding;
    return steps_.feed;
}

std::string GuidedLoadFlow::cleanup_script() const {
    // A printer that waited is still in its load state until released.
    if (waits_for_push_) {
        return steps_.release + "\n" + steps_.cleanup;
    }
    return steps_.cleanup;
}

std::string GuidedLoadFlow::filament_detected() {
    if (stage_ != GuidedLoadStage::AwaitPush) {
        return {};
    }
    return feed();
}

std::string GuidedLoadFlow::extrude() {
    switch (stage_) {
    case GuidedLoadStage::AwaitPush:
        return feed();
    case GuidedLoadStage::ConfirmOutput:
        stage_ = GuidedLoadStage::Feeding;
        return steps_.retry;
    default:
        return {};
    }
}

std::string GuidedLoadFlow::done() {
    if (stage_ != GuidedLoadStage::ConfirmOutput) {
        return {};
    }
    stage_ = GuidedLoadStage::Finishing;
    // The flush ends the printer's load state itself, so no release follows it.
    if (waits_for_push_) {
        return steps_.flush + "\n" + steps_.cleanup;
    }
    return steps_.cleanup;
}

std::string GuidedLoadFlow::cancel() {
    if (ended()) {
        return {};
    }
    stage_ = GuidedLoadStage::Cancelled;
    return cleanup_script();
}

std::string GuidedLoadFlow::command_finished(bool ok) {
    if (!busy()) {
        return {};
    }
    if (!ok) {
        stage_ = GuidedLoadStage::Cancelled;
        return cleanup_script();
    }
    stage_ = (stage_ == GuidedLoadStage::Finishing) ? GuidedLoadStage::Finished
                                                    : GuidedLoadStage::ConfirmOutput;
    return {};
}

} // namespace helix
