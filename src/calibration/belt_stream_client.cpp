// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "belt_stream_client.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "hv/json.hpp"

namespace helix::calibration {

namespace {

using json = nlohmann::json;

/// Milliseconds on the same clock the stall timer compares against.
uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

/// First whitespace-separated token, or "" if there is none.
std::string first_token(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = s.find_first_of(" \t", begin);
    return s.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

/// Last whitespace-separated token, or "" if there is none.
std::string last_token(const std::string& s) {
    const size_t end = s.find_last_not_of(" \t");
    if (end == std::string::npos) {
        return {};
    }
    const size_t begin = s.find_last_of(" \t", end);
    return begin == std::string::npos ? s.substr(0, end + 1) : s.substr(begin + 1, end - begin);
}

/// Fill a sockaddr_un, rejecting paths that would be silently truncated.
bool fill_sockaddr(const std::string& path, sockaddr_un& addr) {
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        return false;
    }
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    return true;
}

/// socket() + connect() to a UDS path. Returns -1 on failure, errno preserved.
int connect_uds(const std::string& path) {
    sockaddr_un addr{};
    if (!fill_sockaddr(path, addr)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

} // namespace

BeltStreamClient::BeltStreamClient() = default;

BeltStreamClient::~BeltStreamClient() {
    // stop() joins the libhv loop thread. It MUST run before member destruction
    // begins: members are destroyed in reverse declaration order, so
    // loop_thread_ (declared first) goes last - after read_buf_, which libhv is
    // reading into, after decoder_, and after the callbacks.
    //
    // Deleting this line produces no compile error and no test failure, so the
    // ordering is *also* pinned structurally by BeltStreamClient::joiner_, the
    // last-declared member; see its comment in the header.
    stop();
}

std::string BeltStreamClient::endpoint_for_chip(const std::string& accel_chip) {
    const std::string chip = first_token(accel_chip);
    if (chip.empty()) {
        return {};
    }
    return chip + "/dump_" + chip;
}

std::string BeltStreamClient::sensor_key_for_chip(const std::string& accel_chip) {
    return last_token(accel_chip);
}

bool BeltStreamClient::socket_reachable(const std::string& socket_path) {
    const int fd = connect_uds(socket_path);
    if (fd < 0) {
        spdlog::debug("[BeltStream] Socket '{}' unreachable: {}", socket_path,
                      std::strerror(errno));
        return false;
    }
    ::close(fd);
    return true;
}

bool BeltStreamClient::start(const std::string& socket_path, const std::string& accel_chip,
                             BatchCallback on_batch, ErrorCallback on_error) {
    if (running_.load()) {
        spdlog::warn("[BeltStream] start() ignored - already running");
        return false;
    }

    // A previous start()/stop() cycle may have left a joinable thread behind
    // (stop() called from a callback cannot join itself). Retire it first.
    if (loop_thread_) {
        loop_thread_->stop();
        loop_thread_->join();
        loop_thread_.reset();
    }

    endpoint_ = endpoint_for_chip(accel_chip);
    sensor_key_ = sensor_key_for_chip(accel_chip);
    if (endpoint_.empty() || sensor_key_.empty()) {
        spdlog::error("[BeltStream] Empty accelerometer chip name");
        return false;
    }

    // Connect on the calling thread: a UDS connect is effectively instant, and
    // doing it here lets start() return a real false instead of an async error.
    fd_ = connect_uds(socket_path);
    if (fd_ < 0) {
        spdlog::warn("[BeltStream] Cannot connect to klippy socket '{}': {}", socket_path,
                     std::strerror(errno));
        return false;
    }

    on_batch_ = std::move(on_batch);
    on_error_ = std::move(on_error);
    decoder_.reset();
    header_seen_ = false;
    col_time_ = 0;
    col_x_ = 1;
    col_y_ = 2;
    col_z_ = 3;
    col_max_ = 3;
    time_base_set_ = false;
    first_sample_time_ = 0.0;
    last_sample_time_ = 0.0;
    total_samples_ = 0;
    sample_rate_hz_.store(0.0f);
    stall_reported_ = false;
    stopping_.store(false);
    running_.store(true);

    spdlog::info("[BeltStream] Subscribing to {} sensor='{}' on {}", endpoint_, sensor_key_,
                 socket_path);

    loop_thread_ = std::make_unique<hv::EventLoopThread>();
    loop_thread_->start(true, [this]() -> int { return attach() ? 0 : -1; });
    return true;
}

bool BeltStreamClient::attach() {
    read_buf_.resize(READ_BUFFER_BYTES);

    io_ = hio_get(loop_thread_->loop()->loop(), fd_);
    if (io_ == nullptr) {
        report_error("failed to register klippy socket with the event loop");
        ::close(fd_);
        fd_ = -1;
        running_.store(false);
        return false;
    }

    // Required order, documented at libhv hloop.h:388. hio_get -> hio_ready()
    // has already pointed the io at the loop's shared 8 KB buffer, which is
    // smaller than one 13-15 KB batch frame, so the override is not optional.
    hio_set_context(io_, this);
    hio_set_readbuf(io_, read_buf_.data(), read_buf_.size());
    hio_setcb_read(io_, &BeltStreamClient::on_readable);
    hio_setcb_close(io_, &BeltStreamClient::on_closed);
    hio_read_start(io_);
    io_open_.store(true);

    json req = {{"id", 1},
                {"method", endpoint_},
                {"params",
                 {{"sensor", sensor_key_},
                  {"response_template", {{"method", std::string(BATCH_METHOD)}}}}}};
    std::string framed = req.dump();
    framed.push_back(KlippyFrameDecoder::FRAME_TERMINATOR);

    if (hio_write(io_, framed.data(), framed.size()) < 0) {
        report_error("failed to send the accelerometer subscribe request");
        running_.store(false);
        // Close here, on the loop thread. Returning false stops the loop, and a
        // later stop() could not then get a lambda scheduled onto it.
        close_on_loop();
        return false;
    }

    last_batch_ms_.store(now_ms());
    arm_stall_timer();
    return true;
}

void BeltStreamClient::arm_stall_timer() {
    // Half the timeout, so a stall is noticed within STALL_TIMEOUT_MS..1.5x.
    stall_timer_ = loop_thread_->loop()->setInterval(
        static_cast<int>(STALL_TIMEOUT_MS / 2), [this](hv::TimerID) {
            if (stall_reported_ || stopping_.load() || !running_.load()) {
                return;
            }
            if (now_ms() - last_batch_ms_.load() <= STALL_TIMEOUT_MS) {
                return;
            }
            // Latch rather than killing the timer from inside its own callback:
            // one wakeup a second costs nothing and avoids htimer_del reentrancy.
            stall_reported_ = true;
            report_error("accelerometer stream stalled");
        });
}

void BeltStreamClient::on_readable(hio_t* io, void* data, int readbytes) {
    auto* self = static_cast<BeltStreamClient*>(hio_context(io));
    if (self == nullptr) {
        spdlog::error("[BeltStream] Read callback with a null context");
        return;
    }
    if (readbytes <= 0) {
        // libhv classifies our fd as HIO_TYPE_TCP, so it handles EOF itself and
        // the close callback fires. Nothing to feed the decoder either way.
        return;
    }
    self->handle_bytes(static_cast<const char*>(data), static_cast<size_t>(readbytes));
}

void BeltStreamClient::on_closed(hio_t* io) {
    auto* self = static_cast<BeltStreamClient*>(hio_context(io));
    if (self == nullptr) {
        return;
    }
    self->io_open_.store(false);
    if (self->stopping_.load()) {
        return; // Our own teardown.
    }
    self->running_.store(false);
    self->report_error("klippy closed the accelerometer stream");
}

void BeltStreamClient::handle_bytes(const char* data, size_t len) {
    decoder_.feed(data, len, [this](std::string_view frame) { handle_frame(frame); });
    if (decoder_.overflowed()) {
        report_error("klippy sent an unterminated frame larger than the decoder ceiling");
        running_.store(false);
        close_on_loop();
    }
}

void BeltStreamClient::handle_frame(std::string_view frame) {
    // Never throws: a malformed frame is dropped rather than killing the loop.
    const json doc = json::parse(frame.begin(), frame.end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        spdlog::warn("[BeltStream] Dropping unparseable frame ({} bytes)", frame.size());
        return;
    }

    if (doc.contains("error")) {
        const auto& err = doc["error"];
        report_error("klippy rejected the subscribe: " +
                     (err.is_object() && err.contains("message") && err["message"].is_string()
                          ? err["message"].get<std::string>()
                          : err.dump()));
        running_.store(false);
        return;
    }

    if (doc.contains("id") && doc.contains("result")) {
        const auto& result = doc["result"];
        if (result.is_object() && result.contains("header") && result["header"].is_array()) {
            int index = 0;
            bool mapped = false;
            for (const auto& column : result["header"]) {
                if (column.is_string()) {
                    const std::string name = column.get<std::string>();
                    if (name == "time") {
                        col_time_ = index;
                        mapped = true;
                    } else if (name == "x_acceleration") {
                        col_x_ = index;
                        mapped = true;
                    } else if (name == "y_acceleration") {
                        col_y_ = index;
                        mapped = true;
                    } else if (name == "z_acceleration") {
                        col_z_ = index;
                        mapped = true;
                    }
                }
                ++index;
            }
            if (mapped) {
                col_max_ = std::max({col_time_, col_x_, col_y_, col_z_});
                header_seen_ = true;
                spdlog::debug("[BeltStream] Column map time={} x={} y={} z={}", col_time_, col_x_,
                              col_y_, col_z_);
            }
        }
        return;
    }

    if (!doc.contains("method") || !doc["method"].is_string() ||
        doc["method"].get<std::string>() != BATCH_METHOD) {
        return; // Not ours.
    }

    const auto params_it = doc.find("params");
    if (params_it == doc.end() || !params_it->is_object()) {
        return;
    }
    const json& params = *params_it;

    if (!header_seen_) {
        // Klipper always sends the ack first, so this means the column names
        // changed shape. The defaults still match every shipping accelerometer.
        spdlog::warn("[BeltStream] Batch before a usable result.header - assuming [t,x,y,z]");
        header_seen_ = true;
    }

    AccelBatch batch;
    if (params.contains("errors") && params["errors"].is_number()) {
        batch.errors = params["errors"].get<int>();
    }
    if (params.contains("overflows") && params["overflows"].is_number()) {
        batch.overflows = params["overflows"].get<int>();
    }

    const auto data_it = params.find("data");
    if (data_it != params.end() && data_it->is_array()) {
        batch.samples.reserve(data_it->size());
        for (const auto& row : *data_it) {
            if (!row.is_array() || static_cast<int>(row.size()) <= col_max_) {
                continue; // Short row - a Klipper that added columns we do not know.
            }
            const double t = row[col_time_].get<double>();
            if (!time_base_set_) {
                time_base_ = t;
                first_sample_time_ = t;
                time_base_set_ = true;
            }
            last_sample_time_ = t;
            ++total_samples_;

            AccelSample sample{};
            sample.time = static_cast<float>(t - time_base_);
            sample.x = row[col_x_].get<float>();
            sample.y = row[col_y_].get<float>();
            sample.z = row[col_z_].get<float>();
            batch.samples.push_back(sample);
        }
    }

    const double span = last_sample_time_ - first_sample_time_;
    if (total_samples_ > 1 && span > 0.0) {
        sample_rate_hz_.store(static_cast<float>(static_cast<double>(total_samples_ - 1) / span));
    }

    last_batch_ms_.store(now_ms());
    stall_reported_ = false;

    if (on_batch_) {
        on_batch_(batch);
    }
}

void BeltStreamClient::report_error(const std::string& message) {
    spdlog::warn("[BeltStream] {}", message);
    if (on_error_) {
        on_error_(message);
    }
}

void BeltStreamClient::close_on_loop() {
    if (stall_timer_ != INVALID_TIMER_ID) {
        loop_thread_->loop()->killTimer(stall_timer_);
        stall_timer_ = INVALID_TIMER_ID;
    }
    if (io_ != nullptr) {
        hio_read_stop(io_);
        // libhv classified the fd as HIO_TYPE_TCP (SO_TYPE says SOCK_STREAM),
        // so hio_close() calls closesocket(fd) for us - closing fd_ again here
        // would risk killing an unrelated descriptor another thread just opened.
        hio_close(io_);
        io_ = nullptr;
        fd_ = -1;
    } else if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    io_open_.store(false);
}

void BeltStreamClient::stop() {
    stopping_.store(true);

    if (!loop_thread_) {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        running_.store(false);
        return;
    }

    const auto& loop = loop_thread_->loop();
    if (loop && (io_open_.load() || fd_ >= 0)) {
        // hio_read_stop and hio_close must run on the loop thread. The promise
        // lives in a shared_ptr captured by value so that a timed-out wait
        // cannot leave the queued lambda writing to a destroyed promise - the
        // same use-after-free closed in wifi_backend_wpa_supplicant.cpp:566-593.
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> fut = done->get_future();
        loop->runInLoop([this, done]() {
            close_on_loop();
            done->set_value();
        });
        if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
            spdlog::warn("[BeltStream] Socket teardown timed out after 2 seconds");
        }
    }

    loop_thread_->stop();
    if (loop && loop->isInLoopThread()) {
        // stop() was called from on_batch/on_error. Joining the loop thread
        // from itself would throw; the socket is already closed and the loop is
        // stopping, so leave the join to the destructor (or the next start()).
        spdlog::debug("[BeltStream] stop() called from the loop thread - join deferred");
    } else {
        loop_thread_->join();
        loop_thread_.reset();
    }

    // stopping_ deliberately stays set: it must remain true for as long as the
    // loop could still fire on_closed for our own teardown. start() clears it.
    running_.store(false);
}

} // namespace helix::calibration
