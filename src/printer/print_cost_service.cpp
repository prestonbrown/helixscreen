// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_cost_service.h"

#include "ams_state.h"
#include "app_globals.h"
#include "core/run_after.h"
#include "core/subject.h"
#include "i_moonraker_api.h"
#include "i_moonraker_sub_apis.h"
#include "moonraker_types.h"
#include "print_start_inputs.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "spoolman_types.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>

#include "hv/json.hpp"

namespace helix::print_cost {

namespace {

/// The spool and material feeding @p tool by the printer's own routing: the
/// tool's lane, else the active (external) spool, which also prices a file that
/// states no per-tool split.
std::optional<LanePick> routed_lane(int tool) {
    auto& ams = AmsState::instance();
    if (tool != kWholeFile) {
        if (const auto lane = ams.lane_feeding_tool(tool)) {
            return LanePick{lane->info.spoolman_id, lane->info.material};
        }
    }
    if (const auto ext = ams.get_external_spool_info(); ext.has_value() && ext->spoolman_id > 0) {
        return LanePick{ext->spoolman_id, ext->material};
    }
    if (tool == kWholeFile) {
        if (const auto lane = ams.lane_feeding_tool(0)) {
            return LanePick{lane->info.spoolman_id, lane->info.material};
        }
    }
    return std::nullopt;
}

struct Pending {
    std::string filename;
    Estimate estimate;
    std::vector<ToolInput> tools;
    std::vector<int> spool_ids; ///< Parallel to tools; 0 = none
    std::string spoolman_currency;
    int outstanding = 0;
    std::function<void(const Estimate&)> on_done;
};

void finish(const std::shared_ptr<Pending>& p) {
    Estimate& e = p->estimate;
    e.own = compute(p->tools, e.estimated_seconds, e.settings);
    e.choice = choose_estimate(e.record, e.file_modified, e.own.complete);
    e.own_currency = !p->spoolman_currency.empty()
                         ? p->spoolman_currency
                         : SettingsManager::instance().get_cost_currency();
    spdlog::debug("[PrintCost] {}: own {:.2f} (complete {}), record {}, showing {}", p->filename,
                  e.own.total, e.own.complete, e.record.has_value(), static_cast<int>(e.choice));
    p->on_done(e);
}

void settle(const std::shared_ptr<Pending>& p) {
    if (--p->outstanding == 0) {
        finish(p);
    }
}

} // namespace

std::string format_cost(double amount, const std::string& currency) {
    return helix::format_price_with_currency(amount, currency, true);
}

CostSettings current_settings() {
    return SettingsManager::instance().get_cost_settings();
}

std::string Estimate::text_at(double filament_fraction, double elapsed_seconds) const {
    switch (choice) {
    case Choice::Record: {
        const double f = std::clamp(filament_fraction, 0.0, 1.0);
        return format_cost(record->estimate->total * f, record->currency);
    }
    case Choice::Own:
        return format_cost(scaled(own, filament_fraction, elapsed_seconds, settings).total,
                           own_currency);
    case Choice::AwaitRecord:
    case Choice::None:
        break;
    }
    return {};
}

void read_record(const std::string& filename, const LifetimeToken& token,
                 std::function<void(const std::optional<Record>&)> on_done) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr || filename.empty()) {
        on_done(std::nullopt);
        return;
    }
    auto shared_done =
        std::make_shared<std::function<void(const std::optional<Record>&)>>(std::move(on_done));
    api->database_get_item(
        kRecordNamespace, record_key(filename),
        [token, shared_done](const nlohmann::json& value) {
            auto record = parse_record(value);
            token.defer("PrintCost::record", [shared_done, record]() { (*shared_done)(record); });
        },
        [token, shared_done](const MoonrakerError&) {
            // A missing key is the common answer: no other program prices prints here.
            token.defer("PrintCost::record_absent",
                        [shared_done]() { (*shared_done)(std::nullopt); });
        });
}

void estimate_file(const std::string& filename, const FileMetadata& metadata,
                   const LaneLookup& lanes, const LifetimeToken& token,
                   std::function<void(const Estimate&)> on_done) {
    auto p = std::make_shared<Pending>();
    p->filename = filename;
    p->on_done = std::move(on_done);
    p->estimate.file_modified = metadata.modified;
    p->estimate.estimated_seconds = metadata.estimated_time;
    p->estimate.settings = current_settings();

    const FilePalette palette = file_palette_from_metadata(metadata);
    const auto file_material = [&palette, &metadata](int tool) -> std::string {
        const size_t idx = tool == kWholeFile ? 0 : static_cast<size_t>(tool);
        if (idx < palette.materials.size() && !palette.materials[idx].empty()) {
            return palette.materials[idx];
        }
        return tool == kWholeFile ? metadata.filament_type : std::string();
    };

    const float diameter = get_printer_state().get_discovery().filament_diameter_mm();
    for (const ToolGrams& g : file_tool_grams(metadata, file_material(kWholeFile), diameter)) {
        std::optional<LanePick> lane = lanes ? lanes(g.tool) : std::nullopt;
        if (!lane.has_value()) {
            lane = routed_lane(g.tool);
        }
        ToolInput in;
        in.tool = g.tool;
        in.grams = g.grams;
        in.material = file_material(g.tool);
        if (in.material.empty() && lane.has_value()) {
            in.material = lane->material;
        }
        p->tools.push_back(in);
        p->spool_ids.push_back(lane.has_value() ? lane->spoolman_id : 0);
    }

    IMoonrakerAPI* api = get_moonraker_api();
    const bool spoolman = api != nullptr && get_printer_state().is_spoolman_available();

    // One count per request below, plus one for this function, so no reply can
    // finish the estimate before every request is out.
    p->outstanding = 1;

    ++p->outstanding;
    read_record(filename, token, [p](const std::optional<Record>& record) {
        p->estimate.record = record;
        settle(p);
    });

    if (spoolman) {
        ++p->outstanding;
        api->spoolman().get_spoolman_currency(
            [token, p](const std::string& currency) {
                token.defer("PrintCost::currency", [p, currency]() {
                    p->spoolman_currency = currency;
                    settle(p);
                });
            },
            [token, p](const MoonrakerError&) {
                token.defer("PrintCost::currency_error", [p]() { settle(p); });
            });

        for (size_t i = 0; i < p->tools.size(); ++i) {
            const int spool_id = p->spool_ids[i];
            if (spool_id <= 0) {
                continue;
            }
            ++p->outstanding;
            api->spoolman().get_spoolman_spool(
                spool_id,
                [token, p, i](const std::optional<SpoolInfo>& spool) {
                    std::optional<SpoolPrice> price;
                    if (spool.has_value()) {
                        price = SpoolPrice{spool->effective_price(), spool->initial_weight_g};
                    }
                    token.defer("PrintCost::spool", [p, i, price]() {
                        p->tools[i].spool = price;
                        settle(p);
                    });
                },
                [token, p](const MoonrakerError&) {
                    token.defer("PrintCost::spool_error", [p]() { settle(p); });
                },
                true);
        }
    }

    settle(p);
}

// ---------------------------------------------------------------------------
// ActivePrintCost
// ---------------------------------------------------------------------------

namespace {

helix::WidgetSubjects s_active_subjects{"ActivePrintCost"};
char s_cost_text_buf[48];
helix::Subject s_cost_text;
helix::Subject s_cost_visible;

/// Reads of a shared record still waiting for its writer's actual, and the gap
/// between them; the writer adds it when its own end-of-print handling runs.
constexpr int kAwaitAttempts = 4;
constexpr uint32_t kAwaitIntervalMs = 3000;

} // namespace

ActivePrintCost& ActivePrintCost::instance() {
    static ActivePrintCost s_instance;
    return s_instance;
}

void ActivePrintCost::init_subjects() {
    s_active_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_STRING(s_cost_text, s_cost_text_buf, "", "print_cost_text",
                                  s_active_subjects);
        UI_MANAGED_SUBJECT_INT(s_cost_visible, 0, "print_cost_visible", s_active_subjects);
    });
}

void ActivePrintCost::set_file(const std::string& filename, const FileMetadata& metadata) {
    if (filename == filename_ && estimate_.has_value() &&
        estimate_->file_modified == metadata.modified) {
        return; // a retry of the same file's metadata
    }
    filename_ = filename;
    filament_total_mm_ = metadata.filament_total;
    estimate_.reset();
    publish();
    estimate_file(filename, metadata, {}, lifetime_.token(), [this, filename](const Estimate& e) {
        if (filename != filename_) {
            return; // superseded by the next print
        }
        estimate_ = e;
        publish();
    });
}

void ActivePrintCost::update(double filament_used_mm, double progress, double elapsed_seconds) {
    filament_used_mm_ = filament_used_mm;
    progress_ = progress;
    elapsed_seconds_ = elapsed_seconds;
    publish();
}

double ActivePrintCost::filament_fraction(double filament_used_mm, double progress) const {
    // Filament actually used where the printer reports it; progress otherwise.
    if (filament_total_mm_ > 0.0 && filament_used_mm > 0.0) {
        return filament_used_mm / filament_total_mm_;
    }
    return progress;
}

void ActivePrintCost::publish() {
    if (!s_cost_text.is_live()) {
        return;
    }
    std::string text;
    if (estimate_.has_value()) {
        text =
            estimate_->text_at(filament_fraction(filament_used_mm_, progress_), elapsed_seconds_);
    }
    std::snprintf(s_cost_text_buf, sizeof(s_cost_text_buf), "%s", text.c_str());
    lv_subject_set_string(&s_cost_text, s_cost_text_buf);
    lv_subject_set_int(&s_cost_visible, text.empty() ? 0 : 1);
}

void ActivePrintCost::actual(double duration_seconds, double filament_used_mm, double progress,
                             std::function<void(const std::string&)> on_text) {
    if (!estimate_.has_value()) {
        on_text({});
        return;
    }
    const double fraction = filament_fraction(filament_used_mm, progress);
    const double started_at = static_cast<double>(std::time(nullptr)) - duration_seconds;
    await_actual(0, duration_seconds, fraction, started_at, std::move(on_text));
}

void ActivePrintCost::await_actual(int attempt, double duration_seconds, double fraction,
                                   double print_started_at,
                                   std::function<void(const std::string&)> on_text) {
    const std::string filename = filename_;
    read_record(
        filename, lifetime_.token(),
        [this, attempt, duration_seconds, fraction, print_started_at, filename,
         on_text](const std::optional<Record>& record) {
            if (filename != filename_ || !estimate_.has_value()) {
                on_text({});
                return;
            }
            const Estimate& e = *estimate_;
            Choice choice =
                choose_actual(record, e.file_modified, print_started_at, e.own.complete);
            if (choice == Choice::AwaitRecord && attempt + 1 < kAwaitAttempts) {
                const LifetimeToken tok = lifetime_.token();
                helix::run_after_ms(kAwaitIntervalMs, [this, tok, attempt, duration_seconds,
                                                       fraction, print_started_at, on_text]() {
                    if (tok.expired()) {
                        return;
                    }
                    await_actual(attempt + 1, duration_seconds, fraction, print_started_at,
                                 on_text);
                });
                return;
            }
            if (choice == Choice::AwaitRecord) {
                // The writer never added it; our own figure beats none.
                choice = e.own.complete ? Choice::Own : Choice::None;
            }
            switch (choice) {
            case Choice::Record:
                on_text(format_cost(record->actual->total, record->currency));
                return;
            case Choice::Own:
                on_text(format_cost(scaled(e.own, fraction, duration_seconds, e.settings).total,
                                    e.own_currency));
                return;
            case Choice::AwaitRecord:
            case Choice::None:
                break;
            }
            on_text({});
        });
}

} // namespace helix::print_cost
