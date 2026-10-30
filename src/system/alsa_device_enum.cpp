// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "alsa_device_enum.h"

#include "lvgl.h"

#include <algorithm>
#include <cstdlib>

#ifdef HELIX_HAS_ALSA
#include "audio_settings_manager.h"

#include <spdlog/spdlog.h>

#include <alsa/asoundlib.h>
#endif

namespace helix::audio {

std::string make_pcm_name(const std::string& card_id) {
    return "plughw:CARD=" + card_id + ",DEV=0";
}

std::string make_label(const std::string& card_id, const std::string& card_name) {
    if (card_name.empty()) {
        return card_id;
    }
    return card_name + " (" + card_id + ")";
}

std::vector<AudioOutputDevice> assemble(const std::vector<RawCard>& cards) {
    std::vector<AudioOutputDevice> out;
    out.push_back({"", lv_tr("System default"), "default"});
    for (const auto& c : cards) {
        if (!c.has_playback || c.id.empty()) {
            continue;
        }
        std::string pcm = make_pcm_name(c.id);
        bool dup = std::any_of(out.begin(), out.end(),
                               [&](const AudioOutputDevice& d) { return d.pcm == pcm; });
        if (dup) {
            continue;
        }
        out.push_back({c.id, make_label(c.id, c.name), pcm});
    }
    return out;
}

bool is_safe_pcm_name(const std::string& pcm) {
    if (pcm.find('|') != std::string::npos || pcm.find("file") != std::string::npos ||
        pcm.find("tee") != std::string::npos) {
        return false;
    }
    if (pcm == "default" || pcm == "sysdefault") {
        return true;
    }
    for (const char* prefix : {"sysdefault:", "hw:", "plughw:", "dmix:"}) {
        if (pcm.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string resolve_alsa_device(const std::string& settings_device, const char* env_device) {
    if (env_device && env_device[0] != '\0') {
        if (is_safe_pcm_name(env_device)) {
            return env_device; // 1. env override (highest)
        }
#ifdef HELIX_HAS_ALSA
        spdlog::warn("[Audio] Ignoring HELIX_ALSA_DEVICE '{}': not a hardware PCM name",
                     env_device);
#endif
    }
    if (!settings_device.empty()) {
        if (is_safe_pcm_name(settings_device)) {
            return settings_device; // 2. user preference
        }
#ifdef HELIX_HAS_ALSA
        spdlog::warn("[Audio] Ignoring saved output device '{}': not a hardware PCM name",
                     settings_device);
#endif
    }
    return "default"; // 3. ALSA default (honors /etc/asound.conf)
}

#ifdef HELIX_HAS_ALSA

std::vector<AudioOutputDevice> list() {
    std::vector<RawCard> cards;
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hw[32];
        std::snprintf(hw, sizeof(hw), "hw:%d", card);

        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, hw, 0) < 0) {
            continue;
        }

        snd_ctl_card_info_t* info = nullptr;
        snd_ctl_card_info_alloca(&info);
        std::string id, name;
        if (snd_ctl_card_info(ctl, info) >= 0) {
            const char* cid = snd_ctl_card_info_get_id(info);
            const char* cname = snd_ctl_card_info_get_name(info);
            id = cid ? cid : "";
            name = cname ? cname : "";
        }

        // Does this card expose at least one playback PCM?
        bool has_playback = false;
        int dev = -1;
        while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
            snd_pcm_info_t* pinfo = nullptr;
            snd_pcm_info_alloca(&pinfo);
            snd_pcm_info_set_device(pinfo, static_cast<unsigned>(dev));
            snd_pcm_info_set_subdevice(pinfo, 0);
            snd_pcm_info_set_stream(pinfo, SND_PCM_STREAM_PLAYBACK);
            if (snd_ctl_pcm_info(ctl, pinfo) >= 0) {
                has_playback = true;
                break;
            }
        }

        snd_ctl_close(ctl);
        cards.push_back({card, id, name, has_playback});
    }
    return assemble(cards);
}

std::string resolve_alsa_device() {
    std::string settings = helix::AudioSettingsManager::instance().get_output_device();
    return resolve_alsa_device(settings, std::getenv("HELIX_ALSA_DEVICE"));
}

#else // !HELIX_HAS_ALSA

std::vector<AudioOutputDevice> list() {
    return assemble({});
}

std::string resolve_alsa_device() {
    const char* env = std::getenv("HELIX_ALSA_DEVICE");
    return resolve_alsa_device(std::string{}, env);
}

#endif

} // namespace helix::audio
