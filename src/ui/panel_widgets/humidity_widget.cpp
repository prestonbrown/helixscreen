// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/observer_factory.h"
#include "format_utils.h"
#include "humidity_sensor_manager.h"
#include "panel_widget_registry.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

// File-static subject: formatted chamber humidity text for XML binding
static lv_subject_t s_chamber_humidity_text;
static char s_chamber_humidity_text_buf[8]; // "45%" or "--"
static helix::WidgetSubjects s_subjects{"HumidityWidgetSubjects"};
static ObserverGuard s_humidity_observer;

static void humidity_widget_init_subjects() {
    s_subjects.ensure_init(
        [] {
            lv_subject_init_string(&s_chamber_humidity_text, s_chamber_humidity_text_buf, nullptr,
                                   sizeof(s_chamber_humidity_text_buf), "--");
            s_subjects.publish("chamber_humidity_text", &s_chamber_humidity_text);
            HELIX_SUBJECT_DEBUG_REGISTER(&s_chamber_humidity_text, "chamber_humidity_text",
                                         LV_SUBJECT_TYPE_STRING);

            // Observe raw humidity int and format for display
            auto* raw_subj =
                helix::sensors::HumiditySensorManager::instance().get_chamber_humidity_subject();
            if (raw_subj) {
                s_humidity_observer = helix::ui::observe<int>(
                    raw_subj, &s_chamber_humidity_text,
                    [](lv_subject_t* target, int humidity_x10) {
                        char buf[8];
                        if (humidity_x10 >= 0) {
                            helix::format::format_humidity(humidity_x10, buf, sizeof(buf));
                        } else {
                            snprintf(buf, sizeof(buf), "--");
                        }
                        if (strcmp(lv_subject_get_string(target), buf) != 0) {
                            lv_subject_copy_string(target, buf);
                        }
                    },
                    helix::sensors::HumiditySensorManager::instance().subjects_lifetime());
            }

            spdlog::debug("[HumidityWidget] Subjects initialized (chamber_humidity_text)");
        },
        [] {
            // Release observer — raw subject from HumiditySensorManager may already
            // be destroyed during reverse-order deinit [L073]
            s_humidity_observer.release();
        });
}

namespace helix {
void register_humidity_widget() {
    register_widget_subjects("humidity", humidity_widget_init_subjects);
}
} // namespace helix
