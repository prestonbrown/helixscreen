// SPDX-License-Identifier: GPL-3.0-or-later
#include "lock_manager.h"

#include "config.h"
#include "picosha2.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <lvgl.h>

// Module-level subject storage — kept out of the header so lock_manager.h
// compiles cleanly in test contexts where LVGL is not fully available.
static lv_subject_t s_pin_set_subject;
static bool s_subjects_initialized = false;

namespace helix {

LockManager& LockManager::instance() {
    static LockManager inst;
    return inst;
}

LockManager::LockManager() {
    load_from_config();
}

bool LockManager::has_pin() const {
    return !pin_hash_.empty();
}

bool LockManager::set_pin(const std::string& pin) {
    if (static_cast<int>(pin.length()) < MIN_PIN_LENGTH ||
        static_cast<int>(pin.length()) > MAX_PIN_LENGTH) {
        return false;
    }
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
        return false;
    }
    pin_hash_ = hash_pin(pin);
    save_to_config();
    spdlog::info("[LockManager] PIN set");
    if (s_subjects_initialized) {
        lv_subject_set_int(&s_pin_set_subject, 1);
    }
    return true;
}

void LockManager::remove_pin() {
    pin_hash_.clear();
    locked_ = false;
    save_to_config();
    spdlog::info("[LockManager] PIN removed, lock disabled");
    if (s_subjects_initialized) {
        lv_subject_set_int(&s_pin_set_subject, 0);
    }
}

bool LockManager::verify_pin(const std::string& pin) const {
    if (pin_hash_.empty() || pin.empty())
        return false;
    return hash_pin(pin) == pin_hash_;
}

bool LockManager::is_locked() const {
    return locked_;
}

void LockManager::lock() {
    if (!has_pin())
        return;
    locked_ = true;
    spdlog::info("[LockManager] Screen locked");
}

bool LockManager::try_unlock(const std::string& pin) {
    if (verify_pin(pin)) {
        locked_ = false;
        spdlog::info("[LockManager] Screen unlocked");
        return true;
    }
    spdlog::debug("[LockManager] Unlock failed — wrong PIN");
    return false;
}

bool LockManager::auto_lock_enabled() const {
    return auto_lock_;
}

void LockManager::set_auto_lock(bool enabled) {
    auto_lock_ = enabled;
    save_to_config();
}

std::string LockManager::hash_pin(const std::string& pin) const {
    return picosha2::hash256_hex_string(pin);
}

void LockManager::load_from_config() {
    auto* config = Config::get_instance();
    if (!config)
        return;
    pin_hash_ = config->get<std::string>("/security/pin_hash", "");
    auto_lock_ = config->get<bool>("/security/auto_lock", false);
}

void LockManager::save_to_config() {
    auto* config = Config::get_instance();
    if (!config)
        return;
    config->set<std::string>("/security/pin_hash", pin_hash_);
    config->set<bool>("/security/auto_lock", auto_lock_);
    config->save();
}

void LockManager::init_subjects() {
    if (s_subjects_initialized)
        return;

    lv_subject_init_int(&s_pin_set_subject, has_pin() ? 1 : 0);
    lv_xml_register_subject(nullptr, "lock_pin_set", &s_pin_set_subject);

    s_subjects_initialized = true;
    subjects_initialized_ = true;

    // Self-register cleanup — co-located with init to prevent forgotten registrations.
    // Runs before lv_deinit() in StaticSubjectRegistry::deinit_all().
    StaticSubjectRegistry::instance().register_deinit("LockManager", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_pin_set_subject);
            s_subjects_initialized = false;
            spdlog::trace("[LockManager] Subjects deinitialized");
        }
        LockManager::instance().subjects_initialized_ = false;
    });

    spdlog::debug("[LockManager] Subjects initialized (pin_set={})", has_pin() ? 1 : 0);
}

} // namespace helix
