// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "consumption_sink.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "filament_database.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix {

namespace {
constexpr uint32_t PERSIST_INTERVAL_MS = 60'000;
constexpr float DELTA_WRITE_THRESHOLD_G = 0.05f;
constexpr float REBASELINE_THRESHOLD_G = 0.5f;
// TODO(filament-diameter): SlotInfo/external-spool don't carry filament
// diameter yet. 1.75 mm is correct for ~99% of hobbyist setups; wire from
// a per-spool field once Spoolman exposes it.
constexpr float DEFAULT_DIAMETER_MM = 1.75f;
} // namespace

std::string_view ExternalSpoolSink::name() const {
    return "external";
}

bool ExternalSpoolSink::is_trackable() const {
    return active_;
}

uint32_t ExternalSpoolSink::persist_interval_ms() const {
    return persist_interval_override_ms_ ? persist_interval_override_ms_ : PERSIST_INTERVAL_MS;
}

void ExternalSpoolSink::snapshot(float filament_used_mm) {
    active_ = false;
    auto info_opt = AmsState::instance().get_external_spool_info();
    if (!info_opt.has_value()) {
        spdlog::debug("[ConsumptionSink:external] No external spool; skipping");
        return;
    }
    const auto& info = *info_opt;
    if (info.remaining_weight_g < 0.0f) {
        spdlog::debug("[ConsumptionSink:external] Unknown remaining weight; skipping");
        return;
    }
    auto material = filament::find_material(info.material);
    if (!material.has_value() || material->density_g_cm3 <= 0.0f) {
        spdlog::warn("[ConsumptionSink:external] Cannot resolve density for material '{}'; "
                     "consumption tracking disabled for this print",
                     info.material);
        return;
    }
    density_g_cm3_ = material->density_g_cm3;
    diameter_mm_ = DEFAULT_DIAMETER_MM;
    snapshot_mm_ = filament_used_mm;
    snapshot_weight_g_ = info.remaining_weight_g;
    last_written_weight_g_ = info.remaining_weight_g;
    last_persist_tick_ms_ = lv_tick_get();
    active_ = true;
    spdlog::info("[ConsumptionSink:external] Snapshot: material={}, density={} g/cm3, "
                 "weight={} g, filament_used_mm={}",
                 info.material, density_g_cm3_, snapshot_weight_g_, snapshot_mm_);
}

void ExternalSpoolSink::apply_delta(float filament_used_mm) {
    if (!active_) {
        return;
    }

    auto info_opt = AmsState::instance().get_external_spool_info();
    if (!info_opt.has_value()) {
        return;
    }
    SlotInfo info = *info_opt;

    // External-write detection: someone other than us updated remaining_weight_g.
    // Treat as authoritative and rebase our snapshot from it.
    if (std::abs(info.remaining_weight_g - last_written_weight_g_) > REBASELINE_THRESHOLD_G) {
        spdlog::info("[ConsumptionSink:external] External write detected (was {} g, now "
                     "{} g); rebaselining",
                     last_written_weight_g_, info.remaining_weight_g);
        rebaseline(filament_used_mm);
        return;
    }

    float consumed_mm = filament_used_mm - snapshot_mm_;
    if (consumed_mm < 0.0f) {
        // filament_used was reset under us (e.g. new print). Rebase.
        snapshot_mm_ = filament_used_mm;
        snapshot_weight_g_ = info.remaining_weight_g;
        last_written_weight_g_ = info.remaining_weight_g;
        return;
    }

    float consumed_g = filament::length_to_weight_g(consumed_mm, density_g_cm3_, diameter_mm_);
    float new_remaining_g = snapshot_weight_g_ - consumed_g;
    if (new_remaining_g < 0.0f) {
        new_remaining_g = 0.0f;
    }

    // Avoid noise writes for sub-gram changes the UI can't show anyway.
    if (std::abs(new_remaining_g - info.remaining_weight_g) < DELTA_WRITE_THRESHOLD_G) {
        return;
    }

    info.remaining_weight_g = new_remaining_g;
    AmsState::instance().set_external_spool_info_in_memory(info);

    // Throttled disk persist (crash-safety).
    if (lv_tick_elaps(last_persist_tick_ms_) >= persist_interval_ms()) {
        AmsState::instance().set_external_spool_info(info);
        last_persist_tick_ms_ = lv_tick_get();
    }
    last_written_weight_g_ = new_remaining_g;
}

void ExternalSpoolSink::flush() {
    auto info_opt = AmsState::instance().get_external_spool_info();
    if (!info_opt.has_value()) {
        return;
    }
    // Full write updates settings.json via SettingsManager and re-fires the subject.
    AmsState::instance().set_external_spool_info(*info_opt);
    last_written_weight_g_ = info_opt->remaining_weight_g;
    last_persist_tick_ms_ = lv_tick_get();
}

void ExternalSpoolSink::rebaseline(float filament_used_mm) {
    auto info_opt = AmsState::instance().get_external_spool_info();
    if (!info_opt.has_value()) {
        active_ = false;
        return;
    }
    snapshot_mm_ = filament_used_mm;
    snapshot_weight_g_ = info_opt->remaining_weight_g;
    last_written_weight_g_ = info_opt->remaining_weight_g;
}

// ---------------------------------------------------------------------------
// AmsSlotSink
// ---------------------------------------------------------------------------

AmsSlotSink::AmsSlotSink(int backend_index, int slot_index)
    : backend_index_(backend_index), slot_index_(slot_index),
      name_("ams:" + std::to_string(backend_index) + ":" + std::to_string(slot_index)) {}

std::string_view AmsSlotSink::name() const {
    return name_;
}

bool AmsSlotSink::is_trackable() const {
    return active_;
}

uint32_t AmsSlotSink::persist_interval_ms() const {
    return persist_interval_override_ms_ ? persist_interval_override_ms_ : PERSIST_INTERVAL_MS;
}

std::optional<SlotInfo> AmsSlotSink::current_info() const {
    AmsBackend* backend = AmsState::instance().get_backend(backend_index_);
    if (!backend) {
        return std::nullopt;
    }
    return backend->get_slot_info(slot_index_);
}

void AmsSlotSink::snapshot(float filament_used_mm) {
    active_ = false;
    AmsBackend* backend = AmsState::instance().get_backend(backend_index_);
    if (!backend) {
        spdlog::debug("[ConsumptionSink:{}] Backend not registered; skipping", name_);
        return;
    }
    if (backend->tracks_consumption_natively()) {
        spdlog::debug("[ConsumptionSink:{}] Backend tracks consumption natively; skipping", name_);
        return;
    }
    auto info_opt = current_info();
    if (!info_opt.has_value()) {
        spdlog::debug("[ConsumptionSink:{}] Slot info unavailable; skipping", name_);
        return;
    }
    const auto& info = *info_opt;
    if (info.remaining_weight_g < 0.0f) {
        spdlog::debug("[ConsumptionSink:{}] Unknown remaining weight; skipping", name_);
        return;
    }
    if (info.spoolman_id != 0) {
        spdlog::debug("[ConsumptionSink:{}] Spoolman-linked (id={}); skipping", name_,
                      info.spoolman_id);
        return;
    }
    auto material = filament::find_material(info.material);
    if (!material.has_value() || material->density_g_cm3 <= 0.0f) {
        spdlog::warn("[ConsumptionSink:{}] Cannot resolve density for material '{}'; "
                     "consumption tracking disabled for this print",
                     name_, info.material);
        return;
    }
    density_g_cm3_ = material->density_g_cm3;
    diameter_mm_ = DEFAULT_DIAMETER_MM;
    snapshot_mm_ = filament_used_mm;
    snapshot_weight_g_ = info.remaining_weight_g;
    last_written_weight_g_ = info.remaining_weight_g;
    last_persist_tick_ms_ = lv_tick_get();
    active_ = true;
    spdlog::info("[ConsumptionSink:{}] Snapshot: material={}, density={} "
                 "g/cm3, weight={} g, filament_used_mm={}",
                 name_, info.material, density_g_cm3_, snapshot_weight_g_, snapshot_mm_);
}

void AmsSlotSink::apply_delta(float filament_used_mm) {
    if (!active_) {
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend(backend_index_);
    if (!backend) {
        active_ = false;
        return;
    }
    auto info_opt = current_info();
    if (!info_opt.has_value()) {
        active_ = false;
        return;
    }
    SlotInfo info = *info_opt;

    // Re-evaluate gating each tick: user may link to Spoolman mid-print, or
    // the backend may flip native-tracking (unlikely but cheap). Stop
    // decrementing without flushing — last_written_weight_g_ is already the
    // latest we applied.
    if (info.spoolman_id != 0 || backend->tracks_consumption_natively()) {
        spdlog::info("[ConsumptionSink:{}] Slot became untrackable mid-stream "
                     "(spoolman_id={}, native={}); pausing",
                     name_, info.spoolman_id, backend->tracks_consumption_natively());
        active_ = false;
        return;
    }

    // External-write detection: someone other than us changed
    // remaining_weight_g between ticks (user edit, Spoolman poll that didn't
    // also set spoolman_id, etc.). Rebase from their value.
    if (std::abs(info.remaining_weight_g - last_written_weight_g_) > REBASELINE_THRESHOLD_G) {
        spdlog::info("[ConsumptionSink:{}] External write detected (was {} g, now "
                     "{} g); rebaselining",
                     name_, last_written_weight_g_, info.remaining_weight_g);
        rebaseline(filament_used_mm);
        return;
    }

    float consumed_mm = filament_used_mm - snapshot_mm_;
    if (consumed_mm < 0.0f) {
        // Filament-used reset (new print). Rebase.
        snapshot_mm_ = filament_used_mm;
        snapshot_weight_g_ = info.remaining_weight_g;
        last_written_weight_g_ = info.remaining_weight_g;
        return;
    }

    float consumed_g = filament::length_to_weight_g(consumed_mm, density_g_cm3_, diameter_mm_);
    float new_remaining_g = std::max(0.0f, snapshot_weight_g_ - consumed_g);

    if (std::abs(new_remaining_g - info.remaining_weight_g) < DELTA_WRITE_THRESHOLD_G) {
        return;
    }

    info.remaining_weight_g = new_remaining_g;
    const bool persist_now = lv_tick_elaps(last_persist_tick_ms_) >= persist_interval_ms();
    // Weight-only update: must NOT re-emit material/color or re-lock identity.
    // set_slot_info() here re-wrote the firmware store every persist and reverted
    // externally-set materials (#981).
    backend->update_slot_weight(slot_index_, info.remaining_weight_g, info.total_weight_g,
                                /*persist=*/persist_now);
    if (persist_now) {
        last_persist_tick_ms_ = lv_tick_get();
    }
    last_written_weight_g_ = new_remaining_g;
}

void AmsSlotSink::flush() {
    // Only persist sinks that actually tracked consumption this print. The
    // tracker registers ONE sink per slot and flush_all_sinks() flushes every
    // one at PAUSE/COMPLETE (filament_consumption_tracker.cpp) — without this
    // guard a flush fires a per-slot weight persist for slots that were never
    // tracked (unknown weight / Spoolman-linked / no consumption), churning the
    // override store and, before the weight-only split, rewriting Adventurer5M
    // .json for every slot at pause (the #981 revert trigger). active_ is set in
    // snapshot() and cleared when a slot goes untrackable mid-print.
    if (!active_) {
        return;
    }
    AmsBackend* backend = AmsState::instance().get_backend(backend_index_);
    if (!backend) {
        return;
    }
    auto info_opt = current_info();
    if (!info_opt.has_value()) {
        return;
    }
    // Weight-only flush (see apply_delta) — identity must not be re-asserted.
    backend->update_slot_weight(slot_index_, info_opt->remaining_weight_g, info_opt->total_weight_g,
                                /*persist=*/true);
    last_written_weight_g_ = info_opt->remaining_weight_g;
    last_persist_tick_ms_ = lv_tick_get();
}

void AmsSlotSink::rebaseline(float filament_used_mm) {
    auto info_opt = current_info();
    if (!info_opt.has_value()) {
        active_ = false;
        return;
    }
    snapshot_mm_ = filament_used_mm;
    snapshot_weight_g_ = info_opt->remaining_weight_g;
    last_written_weight_g_ = info_opt->remaining_weight_g;
}

} // namespace helix
