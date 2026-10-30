// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_health_widget.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_settings_system.h"
#include "ui_subject_registry.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "helix_psram_attr.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "performance_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_io.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "value_status.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

using helix::ui::copy_string_if_changed;
using helix::ui::measure_text_px;
using helix::ui::set_int_if_changed;

namespace {

constexpr const char* kTickSubject = "perf_history_tick";
constexpr const char* kDotSep = " \xc2\xb7 ";

// The verdict and readings the widget publishes, read by panel_widget_host_health.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
HELIX_PSRAM_BSS lv_subject_t s_rung_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_mcu_row_subject{};
HELIX_PSRAM_BSS lv_subject_t s_mcu_rows_subject{};
HELIX_PSRAM_BSS lv_subject_t s_status_subject{};
HELIX_PSRAM_BSS lv_subject_t s_cause_subject{};
HELIX_PSRAM_BSS lv_subject_t s_cpu_status_subject{};
HELIX_PSRAM_BSS lv_subject_t s_temp_status_subject{};
HELIX_PSRAM_BSS lv_subject_t s_throttle_status_subject{};
HELIX_PSRAM_BSS lv_subject_t s_disk_status_subject{};
HELIX_PSRAM_BSS lv_subject_t s_busiest_load_subject{};
HELIX_PSRAM_BSS lv_subject_t s_glance_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_cpu_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_temp_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_mem_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_detail_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_busiest_name_subject{};
HELIX_PSRAM_BSS lv_subject_t s_busiest_text_subject{};
HELIX_PSRAM_BSS char s_glance_text_buf[16];
HELIX_PSRAM_BSS char s_cpu_text_buf[16];
HELIX_PSRAM_BSS char s_temp_text_buf[24];
HELIX_PSRAM_BSS char s_mem_text_buf[16];
HELIX_PSRAM_BSS char s_detail_text_buf[48];
HELIX_PSRAM_BSS char s_busiest_name_buf[48];
HELIX_PSRAM_BSS char s_busiest_text_buf[64];
helix::WidgetSubjects s_subjects{"HostHealthWidgetSubjects"};
// The instance bound to the live tile. Only it publishes: a transient instance (a
// rebuild's outgoing widget) must not overwrite the tile's verdict.
helix::LiveTileOwner<helix::HostHealthWidget> s_tile;

void host_health_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_rung_subject, static_cast<int>(helix::SizeRung::Wide),
                               "host_health_rung", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_mcu_row_subject, 0, "host_health_show_mcu_row", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_mcu_rows_subject, 0, "host_health_mcu_rows", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_status_subject, 0, "host_health_status", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_cause_subject, 0, "host_health_cause", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_cpu_status_subject, 0, "host_health_cpu_status", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_temp_status_subject, 0, "host_health_temp_status", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_throttle_status_subject, 0, "host_health_throttle_status",
                               s_subjects);
        UI_MANAGED_SUBJECT_INT(s_disk_status_subject, 0, "host_health_disk_status", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_busiest_load_subject, 0, "host_health_busiest_load", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_glance_text_subject, s_glance_text_buf, "",
                                  "host_health_glance_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_cpu_text_subject, s_cpu_text_buf, "", "host_health_cpu_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_temp_text_subject, s_temp_text_buf, "", "host_health_temp_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_mem_text_subject, s_mem_text_buf, "", "host_health_mem_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_detail_text_subject, s_detail_text_buf, "",
                                  "host_health_detail_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_busiest_name_subject, s_busiest_name_buf, "",
                                  "host_health_busiest_name", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_busiest_text_subject, s_busiest_text_buf, "",
                                  "host_health_busiest_text", s_subjects);
    });
}

/// A perf subject's value, or `fallback` when the subject does not exist (the performance
/// state is not up, or an MCU has not been seen).
int perf_int(const std::string& name, int fallback) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name.c_str());
    return subject ? lv_subject_get_int(subject) : fallback;
}

std::string perf_string(const std::string& name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name.c_str());
    const char* text = subject ? lv_subject_get_string(subject) : nullptr;
    return text ? text : "";
}

/// The reading, or -1 when the host reports none (`present` is the perf subject's flag).
int perf_reading(const char* value_subject, const char* present_subject) {
    return perf_int(present_subject, 0) != 0 ? perf_int(value_subject, -1) : -1;
}

struct Fonts {
    const lv_font_t* xs;
    const lv_font_t* body;
    const lv_font_t* icon;
};

Fonts load_fonts() {
    return {theme_manager_get_font("font_xs"), theme_manager_get_font("font_body"),
            theme_manager_get_font("icon_font_xs")};
}

using helix::line_px;

/// The height of the sparklines in a row, and the least width a row gives one. Mirrors
/// panel_widget_host_health.xml and host_health_mcu_row.xml; the Cell rung's graph is a
/// button tall.
int row_spark_px() {
    return theme_manager_get_spacing("space_lg");
}
/// The least width a row gives its sparkline.
int spark_min_px() {
    return theme_manager_get_spacing("space_xl");
}
int cell_spark_px() {
    return theme_manager_get_spacing("button_height_sm");
}

/// The widest a retransmit-carrying MCU reading gets in practice: "100% . 9999 retx".
const char* const kWidestMcuText = "100% \xc2\xb7 9999 retx";

/// The minimum content box each rung draws in, for the state being decided. Mirrors
/// panel_widget_host_health.xml: spacing is the XML's pad_row (space_xxs) and pad_column
/// (space_xs).
helix::RungNeeds measure_needs(bool printing, int* mcu_row_pitch) {
    using helix::rung_box;
    const Fonts f = load_fonts();
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    // Measured and rendered widths disagree by a few pixels; a text rung is never
    // budgeted to the last one.
    const int comfort = theme_manager_get_spacing("space_xs");
    // The small rungs hold one short number under a glyph; they get the least margin.
    const int tight = std::max<int>(2, theme_manager_get_spacing("space_xxs") / 2);

    const int icon_w = measure_text_px(helix::ui::icon::lookup_codepoint("gauge"), f.icon);
    const int icon_h = line_px(f.icon);
    const int xs_h = line_px(f.xs);
    const int body_h = line_px(f.body);
    const int spark_h = row_spark_px();
    const int spark_min = spark_min_px();

    const int pct_xs = measure_text_px("100%", f.xs);
    const int temp_xs = measure_text_px("100\xc2\xb0"
                                        "C",
                                        f.xs);
    const std::string ram_caption = lv_tr("RAM");
    const std::string mem_label = lv_tr("Memory");
    const std::string cpu_label = lv_tr("CPU");

    helix::RungNeeds needs;

    {
        const int text_w =
            std::max({measure_text_px("100%", f.xs), measure_text_px("112\xc2\xb0", f.xs),
                      measure_text_px("999M", f.xs)});
        needs.glance = rung_box(std::max(icon_w, text_w) + tight, icon_h + xs_h + 1);
    }
    needs.strip =
        rung_box(icon_w + col_gap + pct_xs + col_gap + temp_xs + tight, std::max(icon_h, xs_h) + 1);
    {
        const int w =
            std::max({icon_w, pct_xs, temp_xs, measure_text_px(ram_caption.c_str(), f.xs)});
        needs.column = rung_box(w + tight, icon_h + 4 * xs_h + 4 * row_gap + 1);
    }
    {
        const int detail_w = measure_text_px("100\xc2\xb0"
                                             "C \xc2\xb7 100%",
                                             f.xs);
        const int first_w = measure_text_px("100%", f.body) + col_gap + spark_min;
        needs.cell = rung_box(std::max(detail_w, first_w) + comfort,
                              std::max(body_h, cell_spark_px()) + row_gap + xs_h + 1);
    }
    const int row_h = std::max(xs_h, spark_h);
    const int label_w =
        std::max({measure_text_px(cpu_label.c_str(), f.xs),
                  measure_text_px(mem_label.c_str(), f.xs), measure_text_px("mcu sb", f.xs)});
    {
        const int value_w =
            printing ? measure_text_px(kWidestMcuText, f.xs) : measure_text_px("100%", f.xs);
        // The label column is a quarter of the row, so the row is at least four labels wide.
        needs.wide = rung_box(
            std::max(label_w + col_gap + spark_min + col_gap + value_w + comfort, 4 * label_w),
            2 * row_h + row_gap + 1);
    }
    {
        const int value_w =
            std::max(measure_text_px(kWidestMcuText, f.xs), measure_text_px("9999 MB free", f.xs));
        const int row_w = label_w + col_gap + spark_min + col_gap + value_w;
        const int detail_w = measure_text_px("100.0\xc2\xb0"
                                             "C",
                                             f.xs) +
                             col_gap + measure_text_px("18.4 GB free", f.xs);
        // Throttle line (shown, for the worst case), CPU, Memory, detail, then the first
        // MCU row.
        const int h = 2 * xs_h + 3 * row_h + 4 * row_gap + 1;
        needs.full = rung_box(std::max({row_w + comfort, detail_w + comfort, 4 * label_w}), h);
    }
    if (mcu_row_pitch)
        *mcu_row_pitch = row_h + row_gap;
    return needs;
}

} // namespace

namespace helix {

struct HostHealthWidget::Sample {
    int cpu_pct = -1;
    int cpu_temp_c10 = -1;
    int mem_pct = -1;
    int disk_free_mb = -1;
    uint32_t throttle_bits = 0;
    int mcu_max_load = -1;
    bool retransmits_rose = false;
    /// The most loaded MCU, or -1 when none reports a load.
    int busiest = -1;
    struct Mcu {
        std::string name;
        std::string safe;
        int load = -1;
    };
    std::vector<Mcu> mcus;
};

HostHealthWidget::HostHealthWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

HostHealthWidget::~HostHealthWidget() {
    detach();
}

bool HostHealthWidget::is_printing() const {
    return lv_subject_get_int(printer_state_.print_state().get_print_active_subject()) != 0;
}

void HostHealthWidget::refresh_mcu_names() {
    // The MCU set changes rarely; the string is re-published on every sample.
    const std::string names = perf_string("perf_mcu_names");
    if (names == names_key_)
        return;
    names_key_ = names;
    mcu_names_.clear();
    for (std::string_view sv : text_io::lines(names, ',')) {
        if (!sv.empty())
            mcu_names_.emplace_back(sv);
    }
    prev_retransmits_.clear();
}

HostHealthWidget::Sample HostHealthWidget::read_sample() {
    Sample s;
    s.cpu_pct = perf_reading("perf_host_cpu_pct", "perf_host_cpu_pct_present");
    s.cpu_temp_c10 = perf_reading("perf_host_cpu_temp_c10", "perf_host_cpu_temp_present");
    s.mem_pct = perf_reading("perf_host_mem_pct_used", "perf_host_mem_present");
    s.disk_free_mb = perf_reading("perf_host_disk_free_mb", "perf_host_disk_present");
    s.throttle_bits = static_cast<uint32_t>(std::max(0, perf_int("perf_host_throttle_state", 0)));

    refresh_mcu_names();

    const int tick = perf_int(kTickSubject, -1);
    const bool new_tick = tick != baseline_tick_;
    bool rose = false;
    for (const auto& name : mcu_names_) {
        Sample::Mcu mcu;
        mcu.name = name;
        mcu.safe = perf::PerformanceState::mcu_safe_name(name);
        const std::string base = "perf_mcu_" + mcu.safe;
        mcu.load = perf_reading((base + "_load_pct").c_str(), (base + "_present").c_str());

        if (new_tick) {
            const int retrans = perf_int(base + "_retrans", 0);
            auto prev = prev_retransmits_.find(name);
            if (prev != prev_retransmits_.end() && retrans > prev->second)
                rose = true;
            prev_retransmits_[name] = retrans;
        }

        if (mcu.load > s.mcu_max_load) {
            s.mcu_max_load = mcu.load;
            s.busiest = static_cast<int>(s.mcus.size());
        }
        s.mcus.push_back(std::move(mcu));
    }
    if (new_tick) {
        baseline_tick_ = tick;
        retransmits_rose_ = rose;
    }
    s.retransmits_rose = retransmits_rose_;
    return s;
}

HostHealthVerdict HostHealthWidget::decide(int width_px, int height_px, bool printing,
                                           int mcu_count) const {
    // The tree's own padding where it exists; before it does (the load path asks
    // fits_at then) the root's padding on each side.
    const ContentChrome chrome = content_chrome_or_estimate(root(), "space_xxs", "space_xxs");
    HostHealthLayoutInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    in.printing = printing;
    in.mcu_count = mcu_count;
    in.needs = measure_needs(printing, &in.mcu_row_pitch);
    in.full_min_h = widget_size::h_tall() - chrome.h;
    const auto& n = in.needs;
    spdlog::trace("[HostHealthWidget] box {}x{} chrome {}x{} needs g{}x{} s{}x{} c{}x{} cell{}x{} "
                  "w{}x{} f{}x{} pitch={} printing={} mcus={}",
                  width_px, height_px, chrome.w, chrome.h, n.glance.w, n.glance.h, n.strip.w,
                  n.strip.h, n.column.w, n.column.h, n.cell.w, n.cell.h, n.wide.w, n.wide.h,
                  n.full.w, n.full.h, in.mcu_row_pitch, printing, mcu_count);
    return decide_host_health_layout(in);
}

void HostHealthWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    widget_obj_ = widget_obj;
    s_tile.claim(this);
    install_delete_hook(widget_obj);
    if (!disk_consumer_) {
        disk_consumer_ = true;
        perf::PerformanceState::instance().add_disk_consumer();
    }

    const auto token = lifetime_.token();
    print_active_observer_ = ui::observe<int>(
        printer_state_.print_state().get_print_active_subject(), this,
        [token](HostHealthWidget* self, int) {
            if (token.expired())
                return;
            // The content being measured changed: Wide swaps memory for the MCU.
            if (self->is_printing() != self->decided_printing_ && self->has_granted_size())
                self->relayout_for_granted_size();
            else
                self->on_sample();
        },
        printer_state_.get_subjects_lifetime());

    // perf_history_tick moves once per sample, after every other perf subject has been
    // updated. The subject is torn down and recreated with the performance state, so the
    // observer carries its lifetime.
    if (lv_subject_t* tick = lv_xml_get_subject(nullptr, kTickSubject)) {
        sample_observer_ = ui::observe<int>(
            tick, this,
            [token](HostHealthWidget* self, int) {
                if (token.expired())
                    return;
                self->on_sample();
            },
            perf::PerformanceState::instance().subjects_lifetime());
    }

    // Instances are recycled across rebuilds: the verdict is republished here too.
    relayout_for_granted_size();
    on_sample();
}

void HostHealthWidget::detach() {
    lifetime_.invalidate();
    if (disk_consumer_) {
        disk_consumer_ = false;
        perf::PerformanceState::instance().remove_disk_consumer();
    }
    sample_observer_.reset();
    print_active_observer_.reset();
    if (HostHealthWidget* displaced = s_tile.release(this)) {
        displaced->relayout_for_granted_size();
        displaced->on_sample();
    }
    widget_obj_ = nullptr;
    built_rows_key_.clear();
}

void HostHealthWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                       int height_px) {
    const bool printing = is_printing();
    refresh_mcu_names();
    verdict_ = decide(width_px, height_px, printing, static_cast<int>(mcu_names_.size()));
    decided_printing_ = printing;
    decided_mcu_count_ = static_cast<int>(mcu_names_.size());
    if (!s_tile.is_owner(this))
        return;

    set_int_if_changed(&s_rung_subject, static_cast<int>(verdict_.rung));
    set_int_if_changed(&s_show_mcu_row_subject, verdict_.show_mcu_row ? 1 : 0);
    set_int_if_changed(&s_mcu_rows_subject, verdict_.mcu_rows);
    sync_mcu_rows();
    on_sample();

    spdlog::debug("[HostHealthWidget] on_size_changed {}x{} rung={} mcu_row={} mcu_rows={} "
                  "fits={} printing={}",
                  width_px, height_px, static_cast<int>(verdict_.rung), verdict_.show_mcu_row,
                  verdict_.mcu_rows, verdict_.fits, printing);
}

bool HostHealthWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Glance content in either state.
    return decide(width_px, height_px, false, 0).fits && decide(width_px, height_px, true, 0).fits;
}

void HostHealthWidget::sync_mcu_rows() {
    lv_obj_t* list = root() ? lv_obj_find_by_name(root(), "host_health_mcu_list") : nullptr;
    if (!list)
        return;
    // Rows exist only while the Full rung draws them: a row carries a sparkline that
    // repaints on every sample.
    std::string key;
    const size_t wanted =
        verdict_.rung == SizeRung::Full ? static_cast<size_t>(verdict_.mcu_rows) : 0;
    for (size_t i = 0; i < wanted && i < mcu_names_.size(); ++i)
        key += mcu_names_[i] + ",";
    if (key == built_rows_key_)
        return;
    built_rows_key_ = key;

    ui::safe_clean_children(list);
    for (size_t i = 0; i < wanted && i < mcu_names_.size(); ++i) {
        const std::string& name = mcu_names_[i];
        const std::string safe = perf::PerformanceState::mcu_safe_name(name);
        const std::string text_subject = "perf_mcu_" + safe + "_text";
        const std::string spark_source = "mcu_" + safe + "_load_pct";
        const char* attrs[] = {
            "label",        name.c_str(),         "text_subject", text_subject.c_str(),
            "spark_source", spark_source.c_str(), nullptr,        nullptr};
        if (!lv_xml_create(list, "host_health_mcu_row", attrs))
            spdlog::warn("[HostHealthWidget] Failed to create a row for MCU '{}'", name);
    }
    // Content-sized rows carry an unresolved size until the next layout pass, and a
    // render that blits one in that window overflows 32-bit fill arithmetic.
    lv_obj_update_layout(list);
}

void HostHealthWidget::publish_status(const Sample& sample) {
    status::HostHealthInput in;
    in.cpu_pct = sample.cpu_pct;
    in.cpu_temp_c10 = sample.cpu_temp_c10;
    in.throttle_bits = sample.throttle_bits;
    in.mcu_max_load_pct = sample.mcu_max_load;
    in.mcu_retransmits_rose = sample.retransmits_rose;
    in.disk_free_mb = sample.disk_free_mb;
    const auto verdict = status::host_health_status(in);
    set_int_if_changed(&s_status_subject, static_cast<int>(verdict.status));
    set_int_if_changed(&s_cause_subject, static_cast<int>(verdict.cause));

    // Each reading's own status, judged alone, colours that reading wherever it is drawn.
    const auto alone = [](auto set) {
        status::HostHealthInput one;
        set(one);
        return static_cast<int>(status::host_health_status(one).status);
    };
    set_int_if_changed(&s_cpu_status_subject,
                       alone([&](status::HostHealthInput& i) { i.cpu_pct = sample.cpu_pct; }));
    set_int_if_changed(&s_temp_status_subject, alone([&](status::HostHealthInput& i) {
        i.cpu_temp_c10 = sample.cpu_temp_c10;
    }));
    set_int_if_changed(&s_throttle_status_subject, alone([&](status::HostHealthInput& i) {
        i.throttle_bits = sample.throttle_bits;
    }));
    set_int_if_changed(&s_disk_status_subject, alone([&](status::HostHealthInput& i) {
        i.disk_free_mb = sample.disk_free_mb;
    }));
}

void HostHealthWidget::publish_texts(const Sample& sample) {
    switch (verdict_.rung) {
    case SizeRung::Glance: {
        const auto cause = static_cast<status::HealthCause>(lv_subject_get_int(&s_cause_subject));
        copy_string_if_changed(
            &s_glance_text_subject,
            std::string(format_glance_text(cause, {sample.cpu_pct, sample.cpu_temp_c10,
                                                   sample.mcu_max_load, sample.disk_free_mb}))
                .c_str());
        break;
    }
    case SizeRung::Strip:
        copy_string_if_changed(&s_cpu_text_subject,
                               std::string(format_percent(sample.cpu_pct)).c_str());
        copy_string_if_changed(&s_temp_text_subject,
                               std::string(format_cpu_temp(sample.cpu_temp_c10, 0)).c_str());
        break;
    case SizeRung::Column:
        copy_string_if_changed(&s_cpu_text_subject,
                               std::string(format_percent(sample.cpu_pct)).c_str());
        copy_string_if_changed(&s_temp_text_subject,
                               std::string(format_cpu_temp(sample.cpu_temp_c10, 0)).c_str());
        copy_string_if_changed(&s_mem_text_subject,
                               std::string(format_percent(sample.mem_pct)).c_str());
        break;
    case SizeRung::Cell:
        copy_string_if_changed(&s_cpu_text_subject,
                               std::string(format_percent(sample.cpu_pct)).c_str());
        copy_string_if_changed(&s_detail_text_subject,
                               std::string(format_cpu_temp(sample.cpu_temp_c10, 0) + kDotSep +
                                           format_percent(sample.mem_pct))
                                   .c_str());
        break;
    case SizeRung::Wide:
        copy_string_if_changed(&s_cpu_text_subject,
                               std::string(format_percent(sample.cpu_pct)).c_str());
        if (verdict_.show_mcu_row && sample.busiest >= 0) {
            const auto& mcu = sample.mcus[static_cast<size_t>(sample.busiest)];
            copy_string_if_changed(&s_busiest_name_subject, mcu.name.c_str());
            copy_string_if_changed(
                &s_busiest_text_subject,
                std::string(perf_string("perf_mcu_" + mcu.safe + "_text")).c_str());
            set_int_if_changed(&s_busiest_load_subject, std::max(0, mcu.load));
        } else {
            copy_string_if_changed(&s_mem_text_subject,
                                   std::string(format_percent(sample.mem_pct)).c_str());
        }
        break;
    case SizeRung::Full:
    case SizeRung::Large:
        copy_string_if_changed(&s_cpu_text_subject,
                               std::string(format_percent(sample.cpu_pct)).c_str());
        copy_string_if_changed(&s_temp_text_subject,
                               std::string(format_cpu_temp(sample.cpu_temp_c10, 1)).c_str());
        break;
    }
}

void HostHealthWidget::on_sample() {
    if (!s_tile.is_owner(this))
        return;
    const Sample sample = read_sample();
    // A change in how many MCUs report changes the rows the Full rung holds.
    if (static_cast<int>(mcu_names_.size()) != decided_mcu_count_) {
        decided_mcu_count_ = static_cast<int>(mcu_names_.size());
        if (has_granted_size()) {
            relayout_for_granted_size();
            return;
        }
    }
    publish_status(sample);
    publish_texts(sample);
}

void HostHealthWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HostHealthWidget] clicked_cb");
    if (auto* self = panel_widget_from_event<HostHealthWidget>(e)) {
        self->record_interaction();
        settings::get_system_settings_overlay().open_performance();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void register_host_health_widget() {
    register_widget_factory("host_health", [](const std::string&) {
        return std::make_unique<HostHealthWidget>(get_printer_state());
    });
    register_widget_subjects("host_health", host_health_init_subjects);

    lv_xml_register_event_cb(nullptr, "host_health_clicked_cb", HostHealthWidget::clicked_cb);
}

} // namespace helix
