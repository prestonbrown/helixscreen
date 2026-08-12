// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"
#include "hv/EventLoopThread.h"
#include "hv/hloop.h"
#include "klippy_frame_decoder.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace helix::calibration {

/// One decoded accelerometer batch as klippy delivered it.
struct AccelBatch {
    /// Sample times are seconds since the first sample of this stream, not
    /// klippy's absolute print time. AccelSample::time is a float, and klippy's
    /// print time is routinely six digits (317413.98 s of uptime), which leaves
    /// a float about 1/32 s of resolution - coarser than the 1/3200 s sample
    /// interval. Rebasing keeps microsecond resolution for any session shorter
    /// than a few hours.
    std::vector<AccelSample> samples;

    /// Samples dropped **during this batch alone**. Nonzero means this window
    /// has a gap in it and is not a clean ring-down; the next clean batch
    /// reports zero again.
    ///
    /// The transport does the conversion because klippy sends running totals,
    /// not deltas (`adxl345.py:_process_batch` returns `self.last_error_count`
    /// and `ffreader.get_last_overflows()`, which only reset when measurements
    /// start). Left raw, one overflow would latch contiguous() false for the
    /// rest of the session.
    int errors = 0;
    int overflows = 0;

    [[nodiscard]] bool contiguous() const {
        return errors == 0 && overflows == 0;
    }
};

/**
 * @brief Streams accelerometer samples straight from Klipper's UDS socket
 *
 * Moonraker is not in the data path. Klipper's webhooks ServerSocket accepts
 * many clients, so this attaches alongside Moonraker's own connection without
 * disturbing it.
 *
 * @par Threading
 * Owns a private hv::EventLoopThread - libhv spawns and joins the thread, so
 * there is no bare std::thread here (which would be EAGAIN -> std::terminate on
 * AD5M and CC1). start() and stop() are main-thread calls. **on_batch and
 * on_error fire on the loop thread and must not touch LVGL** - the consumer
 * marshals with ui_queue_update() or an AsyncLifetimeGuard.
 *
 * @par Why the fd dance
 * libhv has no UDS client helper: hloop.h exposes only host:port constructors
 * and hio_type_e has no HIO_TYPE_UNIX. So the socket is created and connected
 * by hand, then handed to the loop with hio_get / hio_set_readbuf /
 * hio_setcb_read / hio_read_start, in that order (hloop.h:388). libhv's
 * hio_ready() calls fill_io_type(), which asks the kernel for SO_TYPE and
 * classifies our AF_UNIX SOCK_STREAM fd as HIO_TYPE_TCP - so recv(2)/send(2)
 * are used, EOF and read errors close the io, and hio_close() closes the fd for
 * us. This mirrors WifiBackendWpaSupplicant, which does the same with
 * wpa_supplicant's control socket.
 */
class BeltStreamClient {
  public:
    using BatchCallback = std::function<void(const AccelBatch&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    /// No batch within this long means the stream died. Klipper emits roughly
    /// ten batches a second, so two seconds is twenty missed batches - long
    /// enough that scheduling jitter on a loaded printer board cannot trip it.
    static constexpr uint32_t STALL_TIMEOUT_MS = 2000;

    /// Read buffer handed to libhv. Its 8 KB default is smaller than one
    /// 13-15 KB batch frame, so every frame would span at least two reads.
    static constexpr size_t READ_BUFFER_BYTES = 32 * 1024;

    /// The `method` we ask klippy to stamp on every batch. Chosen to be
    /// distinctive so a batch cannot be confused with any other traffic.
    static constexpr const char* BATCH_METHOD = "helix_belt_batch";

    BeltStreamClient();
    ~BeltStreamClient();
    BeltStreamClient(const BeltStreamClient&) = delete;
    BeltStreamClient& operator=(const BeltStreamClient&) = delete;

    /**
     * @brief Connect, subscribe, and begin delivering batches
     * @param socket_path klippy's UDS path, from Moonraker's
     *        /server/config -> config.server.klippy_uds_address
     * @param accel_chip The Klipper config section name, e.g. "adxl345" or
     *        "adxl345 hotend". Take it from resonance_tester.accel_chip; do not
     *        hardcode it. See endpoint_for_chip() for how it is split.
     * @param on_batch Fires on the loop thread for each batch. May be null.
     * @param on_error Fires on the loop thread. May be null.
     * @return false if the socket could not be opened. An error discovered
     *         after this returns arrives through @p on_error instead.
     */
    bool start(const std::string& socket_path, const std::string& accel_chip,
               BatchCallback on_batch, ErrorCallback on_error);

    /// Idempotent. Safe to call from the main thread while batches are
    /// arriving. Calling it from on_batch/on_error closes the socket but cannot
    /// join the loop thread from itself, so the join is left to the destructor.
    void stop();

    [[nodiscard]] bool running() const {
        return running_.load();
    }

    /// Rate derived from the timestamps of received samples, or 0 before enough
    /// have arrived. Do not assume the configured rate - the reference machine
    /// is configured for 3200 Hz and delivers 3053 Hz.
    [[nodiscard]] float sample_rate_hz() const {
        return sample_rate_hz_.load();
    }

    /// Can we open this socket at all? The co-location probe, and cheap enough
    /// to call on panel entry. Connects and immediately closes.
    static bool socket_reachable(const std::string& socket_path);

    /**
     * @brief Webhooks endpoint for a Klipper accelerometer section name
     *
     * Every bulk accelerometer registers `<chip>/dump_<chip>` from its own
     * module (`adxl345.py:307`, `lis2dw.py:107`, `mpu9250.py:91`,
     * `icm20948.py:92`), keyed on the *chip type*, which is the first
     * whitespace token of the config section name.
     *
     * "adxl345" -> "adxl345/dump_adxl345";
     * "adxl345 hotend" -> "adxl345/dump_adxl345";
     * "lis2dw bed" -> "lis2dw/dump_lis2dw".
     *
     * Returns an empty string for an empty or whitespace-only input.
     */
    [[nodiscard]] static std::string endpoint_for_chip(const std::string& accel_chip);

    /**
     * @brief Value for the endpoint's `sensor` mux key
     *
     * Klipper stores `self.name = config.get_name().split()[-1]`, so the mux
     * key is the **last** token of the section name, not the whole thing.
     * "adxl345" -> "adxl345"; "adxl345 hotend" -> "hotend". Sending the full
     * section name gets "The value 'adxl345 hotend' is not valid for sensor".
     */
    [[nodiscard]] static std::string sensor_key_for_chip(const std::string& accel_chip);

  private:
    static void on_readable(hio_t* io, void* data, int readbytes);
    static void on_closed(hio_t* io);

    /// Runs on the loop thread: registers the fd and sends the subscribe.
    bool attach();
    void handle_bytes(const char* data, size_t len);
    void handle_frame(std::string_view frame);
    void close_on_loop();
    void arm_stall_timer();
    void report_error(const std::string& message);

    /// Declared FIRST so it is destroyed LAST - after read_buf_, the decoder and
    /// the callbacks, all of which the loop thread touches. That ordering only
    /// helps because ~BeltStreamClient() calls stop() (which joins the thread)
    /// before member destruction begins; see the destructor's comment.
    std::unique_ptr<hv::EventLoopThread> loop_thread_;

    hio_t* io_ = nullptr;
    int fd_ = -1;

    /// libhv never frees or reallocates a buffer supplied through
    /// hio_set_readbuf (it clears alloced_readbuf, hevent.c:733), so this must
    /// outlive the hio_t.
    std::vector<char> read_buf_;

    KlippyFrameDecoder decoder_;
    BatchCallback on_batch_;
    ErrorCallback on_error_;
    std::string endpoint_;
    std::string sensor_key_;

    /// Column indices resolved from the ack's result.header, so a future
    /// Klipper that reorders or adds columns does not silently shift the axes.
    int col_time_ = 0, col_x_ = 1, col_y_ = 2, col_z_ = 3;
    int col_max_ = 3;
    bool header_seen_ = false;

    std::atomic<bool> running_{false};
    /// Set for the whole of stop() so the close callback does not report the
    /// deliberate teardown as a stream failure.
    std::atomic<bool> stopping_{false};
    /// True between a successful attach() and close_on_loop(). Lets stop() skip
    /// a pointless two-second wait when there is nothing left to close.
    std::atomic<bool> io_open_{false};

    /// Previous batch's cumulative counters, so AccelBatch can report a
    /// per-batch delta. Reset in start(), matching klippy's own reset.
    int prev_errors_ = 0;
    int prev_overflows_ = 0;

    std::atomic<float> sample_rate_hz_{0.0f};
    double time_base_ = 0.0;
    bool time_base_set_ = false;
    double first_sample_time_ = 0.0;
    double last_sample_time_ = 0.0;
    size_t total_samples_ = 0;

    hv::TimerID stall_timer_ = INVALID_TIMER_ID;
    std::atomic<uint64_t> last_batch_ms_{0};
    bool stall_reported_ = false;

    /**
     * @brief Joins the loop thread before any other member is destroyed
     *
     * ~BeltStreamClient() calls stop() explicitly, which is what a reader
     * should see. This guard exists because deleting that line is *silent* -
     * it produces no compile error and no test failure (verified: the whole
     * [stream] suite still passes with the destructor emptied), yet it leaves
     * the loop thread reading into a freed read_buf_ and calling destroyed
     * std::functions. Declared LAST, so it is destroyed FIRST, before every
     * member above it. stop() is idempotent, so the belt-and-braces costs
     * nothing.
     */
    struct LoopJoiner {
        BeltStreamClient* owner;
        ~LoopJoiner() {
            owner->stop();
        }
    };
    LoopJoiner joiner_{this};
};

} // namespace helix::calibration
