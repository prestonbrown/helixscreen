// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_timer_guard.h"

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "detection_source.h"
#include "printer_state.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class PrinterState;
} // namespace helix

namespace helix::detection {

/// Highest spaghetti probability in /usr/bin/detection stdout, or nullopt when
/// the output holds no parsable detection. Measured format (one line per
/// detection, printed twice):
///   label: 1 prob: 0.903177 x:151.378 y:302.575 w:744.139 h:569.242
/// With zero detections the tool prints nothing and exits 0.
std::optional<float> max_spaghetti_probability(const std::string& detection_stdout);

/// Hard cap on one /usr/bin/detection run. The model files live on /mnt/UDISK;
/// a hung child must not hold one of the few shared HttpExecutor workers
/// forever, which would latch busy_ and silently stop detection for the rest
/// of the print.
inline constexpr int kK2DetectionDeadlineS = 20;

/// Run @p argv[0] with a fixed argv (posix_spawn, no shell), capture stdout
/// into @p stdout_out, and SIGKILL the child if it runs past @p deadline_s.
/// Returns the child's exit code, or -1 on spawn failure or timeout (logged).
int run_detection_with_deadline(const std::vector<std::string>& argv, std::string& stdout_out,
                                int deadline_s = kK2DetectionDeadlineS);

/**
 * @brief Detection source backed by the Creality K2 stock yolov5n model.
 *
 * The stock loop (master-server -> cam_app -> ai_engine) needs exclusive
 * /dev/video0 and cannot coexist with our ustreamer. Instead, while a print is
 * active this source fetches a JPEG from the local ustreamer snapshot endpoint
 * and runs /usr/bin/detection on it, off the main thread, every pastaTime
 * seconds. A probability at or above pastaTruth/100 (user_print_refer.json
 * ai_control block) is a spaghetti detection, edge-triggered so a persistent
 * failure fires once. The source only reports: whether a detection pauses
 * the print is the settings' decision, made above the source.
 */
class K2StockDetectionSource : public DetectionSource {
  public:
    /// Fetch a snapshot JPEG from @p url into @p dest_path. Blocking.
    using SnapshotFetcher =
        std::function<bool(const std::string& url, const std::string& dest_path)>;
    /// Run @p argv, write its stdout into @p stdout_out, return its exit code. Blocking,
    /// but bounded: the default runner kills the child at kK2DetectionDeadlineS.
    using DetectionRunner =
        std::function<int(const std::vector<std::string>& argv, std::string& stdout_out)>;
    /// Schedule blocking work off the main thread.
    using WorkSubmitter = std::function<void(std::function<void()>)>;

    K2StockDetectionSource(helix::PrinterState* state);

    static constexpr const char* kSourceId = "k2_stock";

    std::string id() const override {
        return kSourceId;
    }
    bool available() const override {
        return capable_;
    }
    void set_callback(Callback cb) override {
        cb_ = std::move(cb);
    }
    /// The ai_control block this printer boots with: switch = detection on,
    /// pausePrint = pause on a hit. nullopt when no block was readable.
    std::optional<DetectionPreference> printer_preference() const override {
        if (!has_preference_)
            return std::nullopt;
        return DetectionPreference{ai_enabled_, ai_pause_};
    }

    /// Probe capability (Creality K2 + /usr/bin/detection present), read the
    /// ai_control thresholds and start the poll timer. Called once.
    void start();

    /// Re-run the capability probe. The printer type comes from the wizard's
    /// saved config, so a first install that picks K2 only reaches a true
    /// is_creality_k2() after that save; DetectionManager calls this on every
    /// connect so no restart is needed.
    void refresh_capability() override;

    /// Test seams (the defaults do real HTTP / popen / HttpExecutor).
    void set_fetcher(SnapshotFetcher f) {
        fetcher_ = std::move(f);
    }
    void set_runner(DetectionRunner r) {
        runner_ = std::move(r);
    }
    void set_submitter(WorkSubmitter s) {
        submitter_ = std::move(s);
    }
    /// Test seam: force the capability start() would have probed.
    void set_capable_for_test(bool v) {
        capable_ = v;
    }
    /// Test seam: read ai_control from @p path instead of the stock file. Set
    /// before start(), which is the only reader.
    void set_config_path_for_test(const std::string& path) {
        config_path_ = path;
    }

    /// One poll decision: gates, then one background fetch+infer round.
    /// Public because the test pump only runs one-shot timers, so tests drive
    /// ticks directly instead of waiting on the periodic timer.
    void poll_tick();

  private:
    struct PollResult {
        bool ran = false;      ///< a full fetch+infer round completed
        bool positive = false; ///< detection at or above threshold
        float prob = 0.0f;
    };

    void on_print_state(PrintJobState state);
    void handle_result(const PollResult& r);
    void fire(const PollResult& r);

    helix::PrinterState* state_ = nullptr;
    Callback cb_;

    bool capable_ = false;
    bool busy_ = false;          ///< a poll round is in flight (main thread only)
    bool last_positive_ = false; ///< edge-trigger: fire only on negative->positive
    // RAW_PRINT_STATE_OK: tracks the wire's pause/resume pair; PrintState
    // collapses paused and printing, which is exactly the edge this needs.
    PrintJobState last_job_state_ = PrintJobState::STANDBY; ///< pause->resume edges
    float threshold_ = 0.775f;                              ///< pastaTruth / 100
    int period_s_ = 25;                                     ///< pastaTime
    bool ai_enabled_ = true;                                ///< ai_control.switch (1/absent = on)
    bool ai_pause_ = true;        ///< ai_control.pausePrint (1/absent = pause)
    bool has_preference_ = false; ///< an ai_control block was parsed in start()
    std::string config_path_;     ///< user_print_refer.json, overridable for tests
    std::string snapshot_url_ = "http://127.0.0.1:8080/snapshot";

    SnapshotFetcher fetcher_;
    DetectionRunner runner_;
    WorkSubmitter submitter_;

    helix::ui::LvglTimerGuard poll_timer_;
    ObserverGuard state_observer_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::detection
