// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.h"
#include "json_fwd.h"
#include "thermal_rate_model.h"
#include "wizard_config_paths.h"

#include <map>
#include <string>

/**
 * @brief Scopes the Config a pre-print estimate reads, and puts it back after
 *
 * The printer type the database defaults key on, the saved phase history and
 * the saved heating rates all live in the one process-wide Config, so a test
 * that sets or saves them hands them to every test after it. The scope starts
 * with the given printer type and no history or rates.
 */
class PreprintConfigScope {
  public:
    explicit PreprintConfigScope(const std::string& printer_type = "")
        : cfg_(helix::Config::get_instance()),
          type_path_(cfg_->df() + helix::wizard::PRINTER_TYPE) {
        saved_type_ = cfg_->get<std::string>(type_path_, "");
        saved_history_ = cfg_->get<nlohmann::json>(kHistoryPath, nlohmann::json::array());
        for (const char* heater : ThermalRateManager::kPersistedHeaters) {
            saved_rates_[heater] = cfg_->get<float>(rate_path(heater), 0.0f);
            cfg_->set<float>(rate_path(heater), 0.0f);
        }
        cfg_->set<std::string>(type_path_, printer_type);
        cfg_->set<nlohmann::json>(kHistoryPath, nlohmann::json::array());
    }

    ~PreprintConfigScope() {
        cfg_->set<std::string>(type_path_, saved_type_);
        cfg_->set<nlohmann::json>(kHistoryPath, saved_history_);
        for (const auto& [heater, rate] : saved_rates_) {
            cfg_->set<float>(rate_path(heater), rate);
        }
    }

    PreprintConfigScope(const PreprintConfigScope&) = delete;
    PreprintConfigScope& operator=(const PreprintConfigScope&) = delete;

  private:
    static constexpr const char* kHistoryPath = "/print_start_history/entries";

    static std::string rate_path(const std::string& heater) {
        return "/thermal/rates/" + heater + "/heat_rate";
    }

    helix::Config* cfg_;
    std::string type_path_;
    std::string saved_type_;
    nlohmann::json saved_history_;
    std::map<std::string, float> saved_rates_;
};
