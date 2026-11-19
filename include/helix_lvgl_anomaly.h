// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HELIX_LVGL_ANOMALY_H
#define HELIX_LVGL_ANOMALY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Report an LVGL-layer anomaly (double-schedule, use-after-free near-miss,
 * event chain corruption) to telemetry. Thread-safe, rate-limited at 1 per
 * category per 5 minutes by the underlying TelemetryManager.
 *
 * Called from patched LVGL code (lib/lvgl) — exposed as C so the submodule's
 * C translation units can link against it.
 */
void helix_lvgl_anomaly(const char* code, const char* context);

/** Called with every anomaly before it reaches telemetry, on the reporting thread. */
typedef void (*helix_lvgl_anomaly_observer_fn)(const char* code, const char* context);

/**
 * Install `observer` (null removes it). The unit test binary installs one to fail the
 * case that reports an anomaly; the app installs none.
 */
void helix_lvgl_anomaly_set_observer(helix_lvgl_anomaly_observer_fn observer);

#ifdef __cplusplus
}
#endif

#endif
