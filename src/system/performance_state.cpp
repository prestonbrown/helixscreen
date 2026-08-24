// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "performance_state.h"

#include "static_subject_registry.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace helix {
namespace perf {

PerformanceState& PerformanceState::instance() {
    static PerformanceState s;
    return s;
}

void PerformanceState::init_subjects() {
    if (initialized_) {
        return;
    }

    // Fresh lifetime for this generation of subjects. Observers created against
    // these subjects (e.g. HelixSparkline) hold a weak ref to this token and
    // release safely once deinit_subjects() flips/resets it. (#816 pattern.)
    subjects_lifetime_ = std::make_shared<bool>(true);

    UI_MANAGED_SUBJECT_INT(s_host_cpu_pct_, 0, "perf_host_cpu_pct", subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_cpu_pct_present_, 0, "perf_host_cpu_pct_present", subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_cpu_temp_c10_, 0, "perf_host_cpu_temp_c10", subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_cpu_temp_present_, 0, "perf_host_cpu_temp_present", subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_mem_free_mb_, 0, "perf_host_mem_free_mb", subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_mem_pct_used_, 0, "perf_host_mem_pct_used", subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_mem_present_, 0, "perf_host_mem_present", subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_throttle_state_, 0, "perf_host_throttle_state", subjects_);
    UI_MANAGED_SUBJECT_STRING(s_host_throttle_text_, buf_throttle_text_, "",
                              "perf_host_throttle_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(s_mcu_names_, buf_mcu_names_, "", "perf_mcu_names", subjects_);
    UI_MANAGED_SUBJECT_STRING(s_about_summary_, buf_about_summary_, "\xe2\x80\x94",
                              "perf_about_summary", subjects_);
    UI_MANAGED_SUBJECT_INT(s_available_, 0, "perf_available", subjects_);
    UI_MANAGED_SUBJECT_INT(s_history_tick_, 0, "perf_history_tick", subjects_);
    UI_MANAGED_SUBJECT_STRING(s_host_cpu_pct_text_, buf_cpu_text_, "\xe2\x80\x94",
                              "perf_host_cpu_pct_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(s_host_mem_free_text_, buf_mem_text_, "\xe2\x80\x94",
                              "perf_host_mem_free_text", subjects_);

    StaticSubjectRegistry::instance().register_deinit(
        "PerformanceState", []() { PerformanceState::instance().deinit_subjects(); });

    initialized_ = true;
}

void PerformanceState::deinit_subjects() {
    if (!initialized_) {
        return;
    }
    if (source_) {
        source_->stop();
        source_.reset();
    }
    lifetime_.invalidate();
    {
        std::lock_guard<std::mutex> lk(history_mu_);
        history_.clear();
    }
    // LATENT: the per-MCU subjects in mcu_subjects_ were registered into the
    // GLOBAL XML scope by update_mcu_subjects() (apply_sample runs with no active
    // scope override, so register_subject_in_current_scope() targets "globals").
    // clear() frees them but there is no lv_xml_unregister_subject() primitive in
    // this helix-xml build, so the global scope keeps dangling perf_mcu_* nodes.
    // Benign TODAY: deinit_subjects() only runs at lv_deinit shutdown (no
    // mid-session re-init path exists), lv_subject_deinit detaches observers from
    // both sides, and the next lazy recreate re-points the stale node in place.
    // It becomes a real UAF the moment a runtime re-init path (the "printer
    // switch" the header anticipates) is wired: a name lookup between this clear()
    // and the next sample would deref freed memory. Before adding such a path,
    // add lv_xml_unregister_subject() to lib/helix-xml and unregister each
    // perf_mcu_<safe>_{load_pct,retrans,present,text} name HERE, before clear().
    mcu_subjects_.clear();
    // Signal subject death to observers (e.g. HelixSparkline) BEFORE freeing the
    // subjects. Flipping the bool false makes every ObserverGuard holding this
    // token detect the dead subject even if a shared_ptr copy keeps it alive,
    // so reset() releases instead of calling lv_observer_remove() on freed
    // observer nodes (UAF at lv_observer.c:584). Must precede deinit_all().
    if (subjects_lifetime_) {
        *subjects_lifetime_ = false;
        subjects_lifetime_.reset();
    }
    subjects_.deinit_all();
    initialized_ = false;
}

void PerformanceState::set_source(std::unique_ptr<IPerformanceSource> source) {
    if (source_) {
        source_->stop();
        source_.reset();
    }
    source_ = std::move(source);
    if (!source_)
        return;

    auto tok = lifetime_.token();
    source_->set_callback([this, tok](const PerfSample& s) {
        tok.defer("PerformanceState::apply_sample", [this, s]() { apply_sample(s); });
    });
    source_->start();
}

std::vector<float> PerformanceState::read_history(const std::string& name) const {
    std::lock_guard<std::mutex> lk(history_mu_);
    auto it = history_.find(name);
    if (it == history_.end())
        return {};
    const auto& ring = it->second;
    std::vector<float> out;
    out.reserve(ring.fill);
    std::size_t start = (ring.fill == HISTORY_SAMPLES) ? ring.head : 0;
    for (std::size_t i = 0; i < ring.fill; ++i) {
        out.push_back(ring.data[(start + i) % HISTORY_SAMPLES]);
    }
    return out;
}

void PerformanceState::apply_sample(const PerfSample& s) {
    auto set_present = [](lv_subject_t& subj, bool present) {
        lv_subject_set_int(&subj, present ? 1 : 0);
    };

    if (s.host_cpu_pct) {
        lv_subject_set_int(&s_host_cpu_pct_, static_cast<int>(*s.host_cpu_pct + 0.5f));
        push_history("host_cpu_pct", *s.host_cpu_pct);
    }
    set_present(s_host_cpu_pct_present_, s.host_cpu_pct.has_value());

    if (s.host_cpu_temp_c) {
        lv_subject_set_int(&s_host_cpu_temp_c10_,
                           static_cast<int>(*s.host_cpu_temp_c * 10.0f + 0.5f));
    }
    set_present(s_host_cpu_temp_present_, s.host_cpu_temp_c.has_value());

    if (s.host_mem_free_mb) {
        lv_subject_set_int(&s_host_mem_free_mb_, static_cast<int>(*s.host_mem_free_mb));
    }
    if (s.host_mem_pct_used) {
        lv_subject_set_int(&s_host_mem_pct_used_, static_cast<int>(*s.host_mem_pct_used + 0.5f));
        push_history("host_mem_pct_used", *s.host_mem_pct_used);
    }
    set_present(s_host_mem_present_,
                s.host_mem_free_mb.has_value() && s.host_mem_pct_used.has_value());

    lv_subject_set_int(&s_host_throttle_state_, static_cast<int>(s.host_throttle_bits));
    if (!s.host_throttle_text.empty()) {
        lv_subject_copy_string(&s_host_throttle_text_, s.host_throttle_text.c_str());
    }

    char tmp[48];
    if (s.host_cpu_pct && s.host_cpu_temp_c) {
        snprintf(tmp, sizeof(tmp),
                 "%d%% \xc2\xb7 %.1f\xc2\xb0"
                 "C",
                 static_cast<int>(*s.host_cpu_pct + 0.5f), *s.host_cpu_temp_c);
    } else if (s.host_cpu_pct) {
        snprintf(tmp, sizeof(tmp), "%d%%", static_cast<int>(*s.host_cpu_pct + 0.5f));
    } else {
        snprintf(tmp, sizeof(tmp), "\xe2\x80\x94");
    }
    lv_subject_copy_string(&s_host_cpu_pct_text_, tmp);

    if (s.host_mem_free_mb) {
        snprintf(tmp, sizeof(tmp), "%u MB free", *s.host_mem_free_mb);
    } else {
        snprintf(tmp, sizeof(tmp), "\xe2\x80\x94");
    }
    lv_subject_copy_string(&s_host_mem_free_text_, tmp);

    update_mcu_subjects(s.mcus);

    lv_subject_set_int(&s_available_, 1);

    int tick = lv_subject_get_int(&s_history_tick_) + 1;
    lv_subject_set_int(&s_history_tick_, tick);

    update_about_summary(s);
}

void PerformanceState::update_about_summary(const PerfSample& s) {
    const int cpu = lv_subject_get_int(&s_host_cpu_pct_);
    const int cpu_present = lv_subject_get_int(&s_host_cpu_pct_present_);

    int mcu_load = -1;
    // Prefer the MCU literally named "mcu"; fall back to the first entry.
    auto it = std::find_if(s.mcus.begin(), s.mcus.end(),
                           [](const McuStat& m) { return m.name == "mcu"; });
    if (it == s.mcus.end() && !s.mcus.empty()) {
        it = s.mcus.begin();
    }
    if (it != s.mcus.end() && it->load) {
        mcu_load = static_cast<int>(*it->load * 100.0f + 0.5f);
    }

    char tmp[64];
    if (cpu_present && mcu_load >= 0) {
        snprintf(tmp, sizeof(tmp), "%d%% CPU \xc2\xb7 %d%% MCU", cpu, mcu_load);
    } else if (cpu_present) {
        snprintf(tmp, sizeof(tmp), "%d%% CPU", cpu);
    } else {
        snprintf(tmp, sizeof(tmp), "\xe2\x80\x94");
    }
    lv_subject_copy_string(&s_about_summary_, tmp);
}
void PerformanceState::update_mcu_subjects(const std::vector<McuStat>& mcus) {
    // Sort by name for deterministic order
    std::vector<McuStat> sorted = mcus;
    std::sort(sorted.begin(), sorted.end(),
              [](const McuStat& a, const McuStat& b) { return a.name < b.name; });

    // Add subjects for any new MCUs
    for (const auto& m : sorted) {
        const std::string safe = mcu_safe_name(m.name);
        if (mcu_subjects_.find(safe) == mcu_subjects_.end()) {
            auto subs = std::make_unique<McuSubjects>();
            const std::string load_name = "perf_mcu_" + safe + "_load_pct";
            const std::string retr_name = "perf_mcu_" + safe + "_retrans";
            const std::string pres_name = "perf_mcu_" + safe + "_present";
            const std::string text_name = "perf_mcu_" + safe + "_text";
            lv_subject_init_int(&subs->load_pct, 0);
            lv_subject_init_int(&subs->retrans, 0);
            lv_subject_init_int(&subs->present, 0);
            lv_subject_init_string(&subs->text, subs->buf_text, nullptr, sizeof(subs->buf_text),
                                   "\xe2\x80\x94");
            helix::xml::register_subject_in_current_scope(load_name.c_str(), &subs->load_pct);
            helix::xml::register_subject_in_current_scope(retr_name.c_str(), &subs->retrans);
            helix::xml::register_subject_in_current_scope(pres_name.c_str(), &subs->present);
            helix::xml::register_subject_in_current_scope(text_name.c_str(), &subs->text);
            mcu_subjects_.emplace(safe, std::move(subs));
        }
    }

    // Update values + present flags
    std::unordered_map<std::string, bool> seen;
    for (const auto& m : sorted) {
        const std::string safe = mcu_safe_name(m.name);
        auto& subs = mcu_subjects_[safe];
        if (m.load) {
            lv_subject_set_int(&subs->load_pct, static_cast<int>(*m.load * 100.0f + 0.5f));
            push_history("mcu_" + safe + "_load_pct", *m.load * 100.0f);
        }
        if (m.retransmits) {
            lv_subject_set_int(&subs->retrans, static_cast<int>(*m.retransmits));
        }
        lv_subject_set_int(&subs->present, m.load.has_value() ? 1 : 0);

        // Format per-MCU text: "22% · 14 retx" or "14%"
        const int load_pct_val = lv_subject_get_int(&subs->load_pct);
        const int retrans_val = lv_subject_get_int(&subs->retrans);
        if (retrans_val > 0) {
            snprintf(subs->buf_text, sizeof(subs->buf_text), "%d%% \xc2\xb7 %d retx", load_pct_val,
                     retrans_val);
        } else {
            snprintf(subs->buf_text, sizeof(subs->buf_text), "%d%%", load_pct_val);
        }
        lv_subject_copy_string(&subs->text, subs->buf_text);

        seen[safe] = true;
    }
    for (auto& [safe, subs] : mcu_subjects_) {
        if (!seen.count(safe)) {
            lv_subject_set_int(&subs->present, 0);
        }
    }

    // Build comma-joined names string
    std::string joined;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i)
            joined += ",";
        joined += sorted[i].name;
    }
    lv_subject_copy_string(&s_mcu_names_, joined.c_str());
}

void PerformanceState::push_history(const std::string& key, float value) {
    std::lock_guard<std::mutex> lk(history_mu_);
    auto& ring = history_[key];
    ring.data[ring.head] = value;
    ring.head = (ring.head + 1) % HISTORY_SAMPLES;
    if (ring.fill < HISTORY_SAMPLES)
        ++ring.fill;
}

std::string PerformanceState::mcu_safe_name(const std::string& raw) {
    std::string out = raw;
    for (auto& c : out) {
        if (c == ' ' || c == '/' || c == '.')
            c = '_';
    }
    return out;
}

} // namespace perf
} // namespace helix
