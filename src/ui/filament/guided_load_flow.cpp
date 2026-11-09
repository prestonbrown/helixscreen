// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "guided_load_flow.h"

#include <utility>

namespace helix {

namespace {

/// @p a then @p b on separate lines, skipping an empty part.
std::string join_script(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) {
        return a.empty() ? b : a;
    }
    return a + "\n" + b;
}

} // namespace

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
        return join_script(steps_.release, steps_.cleanup);
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
    // The flush ends the printer's load state itself, so no release follows it.
    std::string script =
        waits_for_push_ ? join_script(steps_.flush, steps_.cleanup) : steps_.cleanup;
    // Nothing to send means nothing to wait for.
    stage_ = script.empty() ? GuidedLoadStage::Finished : GuidedLoadStage::Finishing;
    return script;
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
