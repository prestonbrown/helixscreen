// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_mock.h"
#include "ams_step_model.h"

#include <optional>
#include <string>
#include <vector>

namespace helix {

inline std::vector<std::string> step_labels(const OperationStepModel& model) {
    std::vector<std::string> out;
    for (const auto& s : model.steps) {
        out.push_back(s.label);
    }
    return out;
}

/// A mock backend whose action and running operation a test sets directly.
class RunningOpBackend : public AmsBackendMock, public IStepOperation {
  public:
    RunningOpBackend() : AmsBackendMock(4) {}
    AmsAction action = AmsAction::SELECTING;
    std::optional<StepOperationType> running;

    void* capability(TypeTag tag) override {
        return tag == type_tag<IStepOperation>() ? static_cast<IStepOperation*>(this)
                                                 : AmsBackendMock::capability(tag);
    }
    AmsAction get_current_action() const override {
        return action;
    }
    OperationStepModel get_operation_step_model(StepOperationType) const override {
        return {};
    }
    lv_subject_t* get_operation_step_index_subject(StepOperationType) override {
        return nullptr;
    }
    std::optional<StepOperationType> running_step_operation() const override {
        return running;
    }
};

} // namespace helix
