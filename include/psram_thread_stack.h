// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "helix_fs.h"
#endif

namespace helix {

/// While alive, a thread the calling thread creates (std::thread or pthread_create) gets its
/// stack in PSRAM on the ESP32, where internal RAM is what the WebSocket task needs and a
/// long-lived stack there splits its largest block. Elsewhere it does nothing.
///
/// A PSRAM stack sits behind the cache that any flash operation disables, so the thread must
/// never touch flash, NVS or files: call psram_thread_entered() first in its body, which bars
/// it from storage so a stray call fails loudly. esp_pthread's cfg is per-thread and sticky,
/// so the caller's is restored on destruction.
class PsramThreadStackScope {
  public:
    PsramThreadStackScope(const char* thread_name, std::size_t stack_bytes) {
#ifdef ESP_PLATFORM
        had_cfg_ = esp_pthread_get_cfg(&saved_) == ESP_OK;
        esp_pthread_cfg_t cfg = had_cfg_ ? saved_ : esp_pthread_get_default_config();
        cfg.stack_size = stack_bytes;
        cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        cfg.inherit_cfg = false;
        cfg.thread_name = thread_name;
        esp_pthread_set_cfg(&cfg);
#else
        (void)thread_name;
        (void)stack_bytes;
#endif
    }

    ~PsramThreadStackScope() {
#ifdef ESP_PLATFORM
        if (had_cfg_) {
            esp_pthread_set_cfg(&saved_);
        } else {
            const esp_pthread_cfg_t def = esp_pthread_get_default_config();
            esp_pthread_set_cfg(&def);
        }
#endif
    }

    PsramThreadStackScope(const PsramThreadStackScope&) = delete;
    PsramThreadStackScope& operator=(const PsramThreadStackScope&) = delete;

  private:
#ifdef ESP_PLATFORM
    esp_pthread_cfg_t saved_{};
    bool had_cfg_ = false;
#endif
};

/// First call in the body of a thread started under PsramThreadStackScope.
inline void psram_thread_entered(const char* thread_name) {
#ifdef ESP_PLATFORM
    helix::fs::forbid_storage_on_this_thread(thread_name);
#else
    (void)thread_name;
#endif
}

} // namespace helix
