// SPDX-License-Identifier: GPL-3.0-or-later
// Ground truth, measured on a K2 Plus (.30.196) for #1378:
//   /usr/bin/detection <in.jpg> <out.jpg> 0   (mode 0 = the spaghetti model)
// runs the stock yolov5n on a file in ~4s. Positive output, one line per
// detection, each printed twice:
//   label: 1 prob: 0.903177 x:151.378 y:302.575 w:744.139 h:569.242
// Zero detections: no stdout, no output file, exit 0.
#include "k2_stock_detection_source.h"

#include "http_executor.h"
#include "hv/requests.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include "hv/json.hpp"

extern "C" {
char** environ; // the libc environment, handed to posix_spawn
}

namespace helix::detection {

namespace {

// Stock paths on the K2 (Tina rootfs). Everything Creality-specific about this
// source lives in this file.
constexpr const char* DETECTION_BIN = "/usr/bin/detection";
constexpr const char* USER_PRINT_REFER =
    "/mnt/UDISK/creality/userdata/config/user_print_refer.json";
constexpr const char* SNAPSHOT_IN = "/tmp/helix_spaghetti_in.jpg";
constexpr const char* SNAPSHOT_OUT = "/tmp/helix_spaghetti_out.jpg";

bool fetch_snapshot_default(const std::string& url, const std::string& dest_path) {
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = url;
    req->timeout = 10;
    auto resp = requests::request(req);
    if (!resp || resp->status_code < 200 || resp->status_code >= 300 || resp->body.empty())
        return false;
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    out.write(resp->body.data(), static_cast<std::streamsize>(resp->body.size()));
    return out.good();
}

int run_detection_default(const std::vector<std::string>& argv, std::string& stdout_out) {
    return run_detection_with_deadline(argv, stdout_out);
}

void poll_timer_trampoline(lv_timer_t* t) {
    static_cast<K2StockDetectionSource*>(lv_timer_get_user_data(t))->poll_tick();
}

} // namespace

int run_detection_with_deadline(const std::vector<std::string>& argv, std::string& stdout_out,
                                int deadline_s) {
    if (argv.empty())
        return -1;

    int out_pipe[2];
    if (pipe(out_pipe) != 0) {
        spdlog::warn("[K2StockSource] pipe() failed: {}", strerror(errno));
        return -1;
    }

    // Fixed argv straight to exec, no shell: every path in it is ours.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv)
        cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, argv[0].c_str(), &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    if (rc != 0) {
        close(out_pipe[0]);
        spdlog::warn("[K2StockSource] spawning {} failed: {}", argv[0], strerror(rc));
        return -1;
    }

    // The pipe is nonblocking and the wait is polled: a child that hangs with
    // stdout open would block a plain read() forever, outliving the deadline.
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(deadline_s);
    char buf[512];
    for (;;) {
        struct pollfd pfd {
            out_pipe[0], POLLIN, 0
        };
        if (poll(&pfd, 1, 100) > 0) {
            for (;;) {
                const ssize_t n = read(out_pipe[0], buf, sizeof(buf));
                if (n <= 0)
                    break; // EAGAIN (drained) or EOF
                stdout_out.append(buf, static_cast<size_t>(n));
            }
        }
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            close(out_pipe[0]);
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0); // reap the killed child
            close(out_pipe[0]);
            spdlog::warn("[K2StockSource] detection still running after {} s, killed", deadline_s);
            return -1;
        }
    }
}

std::optional<float> max_spaghetti_probability(const std::string& detection_stdout) {
    std::optional<float> best;
    std::istringstream lines(detection_stdout);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("label:") == std::string::npos)
            continue;
        std::istringstream tokens(line);
        std::string tok;
        while (tokens >> tok) {
            if (tok != "prob:")
                continue;
            double p = 0.0;
            if (tokens >> p) {
                if (!best || static_cast<float>(p) > *best)
                    best = static_cast<float>(p);
            }
            break;
        }
    }
    return best;
}

K2StockDetectionSource::K2StockDetectionSource(helix::PrinterState* state) : state_(state) {
    config_path_ = USER_PRINT_REFER;
    if (!fetcher_)
        fetcher_ = fetch_snapshot_default;
    if (!runner_)
        runner_ = run_detection_default;
    if (!submitter_)
        submitter_ = [](std::function<void()> work) {
            helix::http::HttpExecutor::fast().submit(std::move(work));
        };
}

void K2StockDetectionSource::refresh_capability() {
    // HELIX_MOCK_DETECTION_CAPABLE=1 forces the capability probe true so the
    // detection UI can be driven on a dev machine (--test mocks are never a
    // K2 and the detection binary does not exist there).
    if (const char* force = std::getenv("HELIX_MOCK_DETECTION_CAPABLE")) {
        capable_ = std::atoi(force) != 0;
    } else {
        capable_ = PrinterDetector::is_creality_k2() && access(DETECTION_BIN, X_OK) == 0;
    }
    if (capable_) {
        spdlog::info("[K2StockSource] capable: poll every {} s at prob >= {:.3}", period_s_,
                     threshold_);
    } else {
        spdlog::debug("[K2StockSource] not capable on this machine; poll ticks stay no-ops");
    }
}

void K2StockDetectionSource::start() {
    if (!state_)
        return;
    refresh_capability();

    // Tuning thresholds and the printer's stored on/off + pause choice: the
    // ai_control block the stock stack reads at startup. Parsed whether or not
    // this machine is capable (the file is absent everywhere but a Creality
    // install, so the read is a no-op elsewhere). Missing or malformed file =
    // no stored preference (settings keep their defaults) and factory tuning
    // (25 s / 77.5 %) rather than refusing to detect.
    std::ifstream f(config_path_);
    if (f) {
        try {
            const json j = json::parse(f);
            const auto& ai = j.at("ai_control");
            period_s_ = std::clamp(ai.value("pastaTime", period_s_), 5, 600);
            const double truth = ai.value("pastaTruth", 100.0 * threshold_);
            threshold_ = static_cast<float>(std::clamp(truth / 100.0, 0.05, 1.0));
            ai_enabled_ = ai.value("switch", 1) != 0;
            ai_pause_ = ai.value("pausePrint", 1) != 0;
            has_preference_ = true;
        } catch (const std::exception& e) {
            spdlog::warn("[K2StockSource] unreadable ai_control ({}), using defaults", e.what());
        }
    }

    // Between jobs the detection context is new: a second print that starts
    // already failing must be able to fire even though the previous job ended
    // with last_positive_ latched.
    // RAW_PRINT_STATE_OK: the re-arm wants the job boundary itself; lifecycle's
    // Preparing/Idle distinction would leave the edge armed across pre-print.
    state_observer_ = helix::ui::observe_print_state<K2StockDetectionSource>(
        state_->get_print_state_enum_subject(), this,
        [](K2StockDetectionSource* self, PrintJobState value) { self->on_print_state(value); },
        state_->get_subjects_lifetime());

    poll_timer_.reset(lv_timer_create(poll_timer_trampoline, period_s_ * 1000, this));
}

void K2StockDetectionSource::on_print_state(PrintJobState state) {
    const PrintJobState prev = last_job_state_;
    last_job_state_ = state;
    if (!printer_has_job(state)) {
        last_positive_ = false;
        return;
    }
    // RAW_PRINT_STATE_OK: the resume edge needs the wire's PAUSED -> PRINTING
    // pair; PrintState collapses paused and printing into one value, so the
    // user's choice to continue is invisible without the raw pair.
    if (prev == PrintJobState::PAUSED && state == PrintJobState::PRINTING)
        last_positive_ = false;
}

void K2StockDetectionSource::poll_tick() {
    if (!capable_ || busy_)
        return;
    if (!SettingsManager::instance().get_detection_enabled())
        return;
    // RAW_PRINT_STATE_OK: the stock daemon polls while the printer itself holds
    // a job; lifecycle would also poll through Preparing, before any plastic
    // has moved.
    if (!printer_has_job(state_->get_print_job_state()))
        return;

    busy_ = true;
    // The worker lambda holds by-value copies of everything it executes; the
    // raw `this` it captures is only ever stored into the deferred callback,
    // never dereferenced on the worker. The fetch/round can outlive this
    // object (HttpExecutor stop detaches stuck workers); the token re-checks
    // liveness on the main thread before handle_result runs.
    const auto fetch = fetcher_;
    const auto run = runner_;
    const auto url = snapshot_url_;
    const std::vector<std::string> argv{DETECTION_BIN, SNAPSHOT_IN, SNAPSHOT_OUT, "0"};
    const auto tok = lifetime_.token();
    const float threshold = threshold_;
    submitter_([this, fetch, run, url, argv, tok, threshold] {
        PollResult r;
        if (fetch(url, SNAPSHOT_IN)) {
            std::string out;
            if (run(argv, out) == 0) {
                if (const auto prob = max_spaghetti_probability(out)) {
                    r.ran = true;
                    r.prob = *prob;
                    r.positive = *prob >= threshold;
                }
            } else {
                spdlog::debug("[K2StockSource] detection exited nonzero or timed out");
            }
        } else {
            spdlog::debug("[K2StockSource] snapshot fetch failed");
        }
        spdlog::debug("[K2StockSource] poll done: ran={} positive={} prob={:.3}", r.ran, r.positive,
                      r.prob);
        // tok.defer marshals to the main thread and re-checks the generation
        // before the body runs, so `this` is valid inside handle_result.
        tok.defer("K2StockSource::handle_result", [this, r] { handle_result(r); });
    });
}

void K2StockDetectionSource::handle_result(const PollResult& r) {
    busy_ = false;
    if (r.positive && !last_positive_) {
        // The round started while printing; a job that ended while the model
        // ran leaves nothing to pause and no print for the modal to speak of.
        // RAW_PRINT_STATE_OK: same wire read poll_tick makes - the round's
        // result is only meaningful against the job it was launched under.
        if (printer_has_job(state_->get_print_job_state()))
            fire(r);
    }
    last_positive_ = r.positive;
}

void K2StockDetectionSource::fire(const PollResult& r) {
    const int pct = static_cast<int>(r.prob * 100.0f + 0.5f);
    spdlog::warn("[K2StockSource] spaghetti detected ({}%)", pct);

    DetectionEvent e;
    e.source_id = id();
    e.kind = DetectionKind::Spaghetti;
    e.attributable = true;
    e.confidence = r.prob;
    // A job the user (or anything else) already paused needs no second pause
    // from the response: the presenter reads this flag to skip its own.
    // RAW_PRINT_STATE_OK: already_paused asks PAUSED specifically; the
    // lifecycle collapses paused and printing into one PrintState.
    e.already_paused = state_->get_print_job_state() == PrintJobState::PAUSED;
    e.message = "Spaghetti detected (" + std::to_string(pct) + "%)";
    if (cb_)
        cb_(e);
}

} // namespace helix::detection
