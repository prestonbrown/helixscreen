// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_stats_widget.h"

#include "ui_event_safety.h"
#include "ui_filename_utils.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <string>

// Subjects owned by PrintStatsWidget module
static lv_subject_t s_size_mode;
static lv_subject_t s_view_mode; // 0=lifetime, 1=weekly
static lv_subject_t s_title;
static lv_subject_t s_total_prints;
static lv_subject_t s_total_time;
static lv_subject_t s_total_time_short;
static lv_subject_t s_success_rate;
static lv_subject_t s_weekly;
static lv_subject_t s_last_print;

static char s_title_buf[32] = "Lifetime Print Stats";
static char s_total_prints_buf[16] = "--";
static char s_total_time_buf[24] = "--";
static char s_total_time_short_buf[16] = "--";
static char s_success_rate_buf[8] = "--";
static char s_weekly_buf[16] = "--";
static char s_last_print_buf[64] = "";

static bool s_subjects_initialized = false;

static void print_stats_init_subjects() {
    if (s_subjects_initialized)
        return;

    lv_subject_init_int(&s_size_mode, 2);
    lv_subject_init_int(&s_view_mode, 0);
    lv_subject_init_string(&s_title, s_title_buf, nullptr, sizeof(s_title_buf),
                           lv_tr("Lifetime Print Stats"));
    lv_subject_init_string(&s_total_prints, s_total_prints_buf, nullptr, sizeof(s_total_prints_buf),
                           "--");
    lv_subject_init_string(&s_total_time, s_total_time_buf, nullptr, sizeof(s_total_time_buf),
                           "--");
    lv_subject_init_string(&s_total_time_short, s_total_time_short_buf, nullptr,
                           sizeof(s_total_time_short_buf), "--");
    lv_subject_init_string(&s_success_rate, s_success_rate_buf, nullptr, sizeof(s_success_rate_buf),
                           "--");
    lv_subject_init_string(&s_weekly, s_weekly_buf, nullptr, sizeof(s_weekly_buf), "--");
    lv_subject_init_string(&s_last_print, s_last_print_buf, nullptr, sizeof(s_last_print_buf), "");

    lv_xml_register_subject(nullptr, "print_stats_size_mode", &s_size_mode);
    lv_xml_register_subject(nullptr, "print_stats_view_mode", &s_view_mode);
    lv_xml_register_subject(nullptr, "print_stats_title", &s_title);
    lv_xml_register_subject(nullptr, "print_stats_total_prints", &s_total_prints);
    lv_xml_register_subject(nullptr, "print_stats_total_time", &s_total_time);
    lv_xml_register_subject(nullptr, "print_stats_total_time_short", &s_total_time_short);
    lv_xml_register_subject(nullptr, "print_stats_success_rate", &s_success_rate);
    lv_xml_register_subject(nullptr, "print_stats_weekly", &s_weekly);
    lv_xml_register_subject(nullptr, "print_stats_last_print", &s_last_print);

    SubjectDebugRegistry::instance().register_subject(&s_size_mode, "print_stats_size_mode",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&s_view_mode, "print_stats_view_mode",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&s_title, "print_stats_title",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&s_total_prints, "print_stats_total_prints",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&s_total_time, "print_stats_total_time",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&s_total_time_short,
                                                      "print_stats_total_time_short",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&s_success_rate, "print_stats_success_rate",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&s_weekly, "print_stats_weekly",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&s_last_print, "print_stats_last_print",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    s_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("PrintStatsWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_last_print);
            lv_subject_deinit(&s_weekly);
            lv_subject_deinit(&s_success_rate);
            lv_subject_deinit(&s_total_time_short);
            lv_subject_deinit(&s_total_time);
            lv_subject_deinit(&s_total_prints);
            lv_subject_deinit(&s_title);
            lv_subject_deinit(&s_view_mode);
            lv_subject_deinit(&s_size_mode);
            s_subjects_initialized = false;
        }
    });

    spdlog::debug("[PrintStatsWidget] Subjects initialized");
}

namespace helix {

void register_print_stats_widget() {
    register_widget_factory(
        "print_stats", [](const std::string&) { return std::make_unique<PrintStatsWidget>(); });
    register_widget_subjects("print_stats", print_stats_init_subjects);
    lv_xml_register_event_cb(nullptr, "print_stats_clicked_cb",
                             PrintStatsWidget::print_stats_clicked_cb);
}

PrintStatsWidget::PrintStatsWidget() = default;

PrintStatsWidget::~PrintStatsWidget() {
    detach();
}

void PrintStatsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    lv_obj_set_user_data(widget_obj_, this);

    // Pressed feedback: dim on touch
    lv_obj_set_style_opa(widget_obj_, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);

    // Register history observer for live updates.
    // NOTE: Do NOT call fetch() or update_stats() here — attach() runs inside
    // a ScopedFreeze (rebuild_widget_grid), so queue_update() callbacks are
    // silently dropped. Data loading is deferred to on_activate().
    auto* hm = get_print_history_manager();
    if (hm) {
        auto token = lifetime_.token();
        history_observer_ = [this, token]() {
            if (token.expired())
                return;
            // A finished print changes the server totals too, so re-ask.
            fetch_lifetime_totals();
            update_stats();
        };
        hm->add_observer(&history_observer_);
    }

    spdlog::debug("[PrintStatsWidget] Attached");
}

void PrintStatsWidget::on_activate() {
    // Instances are recycled across grid rebuilds, so this runs again on every
    // activation — the totals request is the only thing that re-seeds a fresh
    // instance's lifetime numbers (attach() cannot: it runs inside a
    // ScopedFreeze, where queued callbacks are dropped).
    fetch_lifetime_totals();

    auto* hm = get_print_history_manager();
    if (!hm)
        return;

    if (hm->is_loaded()) {
        update_stats();
    } else {
        // Defer fetch to next tick so it runs outside any ScopedFreeze
        auto token = lifetime_.token();
        lv_async_call(
            [](void* ctx) {
                auto token_ptr = static_cast<helix::LifetimeToken*>(ctx);
                if (!token_ptr->expired()) {
                    auto* history = get_print_history_manager();
                    if (history && !history->is_loaded()) {
                        history->fetch();
                    }
                }
                delete token_ptr;
            },
            new helix::LifetimeToken(token));
    }
}

void PrintStatsWidget::detach() {
    lifetime_.invalidate();
    // invalidate() drops any totals callback still in flight, so the guard has
    // to be cleared by hand — this instance may be recycled into the next grid
    // rebuild, and a stale flag would block its refetch forever.
    lifetime_totals_in_flight_ = false;
    if (auto* hm = get_print_history_manager()) {
        hm->remove_observer(&history_observer_);
    }
    history_observer_ = nullptr;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[PrintStatsWidget] Detached");
}

void PrintStatsWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                       int height_px) {
    int mode;
    if (height_px < widget_size::H_TALL && width_px < widget_size::W_WIDE) {
        mode = 0; // narrow compact: time · success
    } else if (height_px < widget_size::H_TALL) {
        mode = 3; // wide compact: prints · time · success · weekly
    } else if (width_px < widget_size::W_WIDE) {
        mode = 1; // 2x2 grid
    } else {
        mode = 2; // 3x2 full
    }
    spdlog::debug("[PrintStatsWidget] on_size_changed {}x{}px -> mode {}", width_px, height_px,
                  mode);
    lv_subject_set_int(&s_size_mode, mode);
}

void PrintStatsWidget::fetch_lifetime_totals() {
    if (lifetime_totals_in_flight_)
        return;

    IMoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    lifetime_totals_in_flight_ = true;

    // Both callbacks fire on the HTTP/WebSocket thread and touch members, so
    // both go through bg_cb — it marshals the whole body to the main thread and
    // drops it if the widget was torn down while the request was in flight.
    api->history().get_history_totals(
        lifetime_.bg_cb("PrintStatsWidget::get_history_totals",
                        [this](const PrintHistoryTotals& totals) {
                            lifetime_totals_in_flight_ = false;
                            lifetime_totals_ = totals;
                            lifetime_totals_valid_ = true;
                            update_stats();
                        }),
        lifetime_.bg_cb("PrintStatsWidget::get_history_totals_error",
                        [this](const MoonrakerError& error) {
                            lifetime_totals_in_flight_ = false;
                            spdlog::warn("[PrintStatsWidget] Totals fetch failed, using cached "
                                         "jobs: {}",
                                         error.message);
                        }));
}

void PrintStatsWidget::update_stats() {
    auto* hm = get_print_history_manager();
    if (!hm)
        return;

    const auto& jobs = hm->get_jobs();
    bool weekly_mode = (lv_subject_get_int(&s_view_mode) == 1);

    // Update title
    const char* title = weekly_mode ? lv_tr("Weekly Print Stats") : lv_tr("Lifetime Print Stats");
    std::snprintf(s_title_buf, sizeof(s_title_buf), "%s", title);
    lv_subject_copy_string(&s_title, s_title_buf);

    // Determine which jobs to aggregate
    std::vector<PrintHistoryJob> filtered_jobs;
    if (weekly_mode) {
        auto now = std::chrono::system_clock::now();
        auto week_ago = std::chrono::duration_cast<std::chrono::seconds>(
                            (now - std::chrono::hours(24 * 7)).time_since_epoch())
                            .count();
        filtered_jobs = hm->get_jobs_since(static_cast<double>(week_ago));
    }

    const auto& source = weekly_mode ? filtered_jobs : jobs;

    // Compute totals from the cached jobs. For the lifetime view this is only
    // the fallback: the cache is capped at PrintHistoryManager::fetch()'s limit,
    // so on a printer with a longer history it undercounts both prints and time
    // (#1272). Server-computed totals replace it below as soon as they arrive.
    uint64_t total_jobs = source.size();
    uint64_t total_time_secs = 0;
    size_t completed_count = 0;

    for (const auto& job : source) {
        total_time_secs += static_cast<uint64_t>(job.total_duration);
        if (job.status == PrintJobStatus::COMPLETED) {
            completed_count++;
        }
    }

    // `server.history.totals` is lifetime-only — it cannot answer a 7-day
    // window, so weekly mode keeps aggregating the filtered cache.
    if (!weekly_mode && lifetime_totals_valid_) {
        total_jobs = lifetime_totals_.total_jobs;
        total_time_secs = lifetime_totals_.total_time;
    }

    // Total prints
    std::snprintf(s_total_prints_buf, sizeof(s_total_prints_buf), "%llu",
                  static_cast<unsigned long long>(total_jobs));
    lv_subject_copy_string(&s_total_prints, s_total_prints_buf);

    // Total time — full (hours+minutes) and short (hours only)
    auto hours = total_time_secs / 3600;
    auto mins = (total_time_secs % 3600) / 60;
    if (hours >= 100) {
        std::snprintf(s_total_time_buf, sizeof(s_total_time_buf), "%luh",
                      static_cast<unsigned long>(hours));
    } else {
        std::snprintf(s_total_time_buf, sizeof(s_total_time_buf), "%luh %lum",
                      static_cast<unsigned long>(hours), static_cast<unsigned long>(mins));
    }
    lv_subject_copy_string(&s_total_time, s_total_time_buf);

    std::snprintf(s_total_time_short_buf, sizeof(s_total_time_short_buf), "%luh",
                  static_cast<unsigned long>(hours));
    lv_subject_copy_string(&s_total_time_short, s_total_time_short_buf);

    // Success rate stays list-derived on BOTH sides of the ratio: Moonraker's
    // totals endpoint carries no status breakdown, so the cached jobs are the
    // best approximation available (same call HistoryDashboardPanel documents
    // for its ALL_TIME filter). Mixing a server numerator with a cached
    // denominator — or vice versa — would produce a meaningless percentage.
    if (!source.empty()) {
        int rate = static_cast<int>((completed_count * 100) / source.size());
        std::snprintf(s_success_rate_buf, sizeof(s_success_rate_buf), "%d%%", rate);
    } else {
        std::snprintf(s_success_rate_buf, sizeof(s_success_rate_buf), "--");
    }
    lv_subject_copy_string(&s_success_rate, s_success_rate_buf);

    // Weekly count (always computed for compact/activity row)
    auto now = std::chrono::system_clock::now();
    auto week_ago_ts = std::chrono::duration_cast<std::chrono::seconds>(
                           (now - std::chrono::hours(24 * 7)).time_since_epoch())
                           .count();
    auto recent =
        weekly_mode ? filtered_jobs : hm->get_jobs_since(static_cast<double>(week_ago_ts));
    std::snprintf(s_weekly_buf, sizeof(s_weekly_buf), "%zu/wk", recent.size());
    lv_subject_copy_string(&s_weekly, s_weekly_buf);

    // Last print info
    if (!jobs.empty()) {
        const auto& last = jobs.front();
        // Same display name the rest of the UI shows: basename with any of
        // .gcode/.gco/.g/.3mf stripped case-insensitively.
        std::string name = helix::gcode::get_display_filename(last.filename);
        if (name.length() > 20)
            name = name.substr(0, 18) + "..";

        const char* status_icon = "";
        switch (last.status) {
        case PrintJobStatus::COMPLETED:
            status_icon = "OK";
            break;
        case PrintJobStatus::CANCELLED:
            status_icon = "X";
            break;
        case PrintJobStatus::ERROR:
            status_icon = "!";
            break;
        default:
            break;
        }
        std::snprintf(s_last_print_buf, sizeof(s_last_print_buf), "%s: %s %s", lv_tr("Last"),
                      name.c_str(), status_icon);
    } else {
        std::snprintf(s_last_print_buf, sizeof(s_last_print_buf), "%s", lv_tr("No prints yet"));
    }
    lv_subject_copy_string(&s_last_print, s_last_print_buf);

    spdlog::debug("[PrintStatsWidget] Updated ({}): {} jobs, {}h (source: {}, {} cached)",
                  weekly_mode ? "weekly" : "lifetime", total_jobs, hours,
                  (!weekly_mode && lifetime_totals_valid_) ? "server totals" : "cached jobs",
                  source.size());
}

void PrintStatsWidget::handle_clicked() {
    // Toggle between lifetime and weekly view
    int current = lv_subject_get_int(&s_view_mode);
    lv_subject_set_int(&s_view_mode, current == 0 ? 1 : 0);
    update_stats();
}

void PrintStatsWidget::print_stats_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatsWidget] print_stats_clicked_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<PrintStatsWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
