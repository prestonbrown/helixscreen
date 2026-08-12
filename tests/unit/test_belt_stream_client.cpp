// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_stream_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Minimal stand-in for klippy's webhooks socket: accepts one client, replies
/// to the subscribe with an ack, then emits whatever batches the test asks for.
///
/// The writer thread is a bare std::thread on purpose - this is test
/// scaffolding, never shipped, and the no-bare-threads rule exists for the
/// EAGAIN-on-AD5M failure of production code.
class FakeKlippySocket {
  public:
    explicit FakeKlippySocket(std::string path) : path_(std::move(path)) {
        ::unlink(path_.c_str());
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(fd_ >= 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path_.c_str());
        REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(fd_, 1) == 0);
    }

    ~FakeKlippySocket() {
        stop();
        if (fd_ >= 0)
            ::close(fd_);
        ::unlink(path_.c_str());
    }

    /// Column names for the ack. Defaults to Klipper's real order.
    void set_header(std::vector<std::string> header) {
        header_ = std::move(header);
    }

    /// The three acceleration values emitted after the timestamp, in whatever
    /// order set_header() declared.
    void set_row_values(double a, double b, double c) {
        a_ = a;
        b_ = b;
        c_ = c;
    }

    void set_counters(int errors, int overflows) {
        errors_ = errors;
        overflows_ = overflows;
    }

    /// Close the connection once every batch has been written.
    void set_close_when_done(bool close_when_done) {
        close_when_done_ = close_when_done;
    }

    /// Serve one client: read the subscribe, send the ack, then send `batches`
    /// frames of `rows` rows each.
    void serve(int batches, int rows) {
        thread_ = std::thread([this, batches, rows]() {
            client_ = ::accept(fd_, nullptr, nullptr);
            if (client_ < 0)
                return;

            char buf[4096];
            ssize_t n = ::read(client_, buf, sizeof(buf));
            if (n > 0) {
                std::lock_guard<std::mutex> lk(m_);
                subscribe_.assign(buf, static_cast<size_t>(n));
                got_subscribe_ = true;
                cv_.notify_all();
            }

            std::string ack = R"({"id":1,"result":{"header":[)";
            for (size_t i = 0; i < header_.size(); ++i) {
                if (i)
                    ack += ',';
                ack += '"' + header_[i] + '"';
            }
            ack += "]}}";
            send_frame(ack);

            double t = 1000.0;
            for (int b = 0; b < batches && !stopping_; ++b) {
                std::string f = R"({"method":"helix_belt_batch","params":{"data":[)";
                for (int r = 0; r < rows; ++r) {
                    if (r)
                        f += ',';
                    f += '[' + std::to_string(t) + ',' + std::to_string(a_) + ',' +
                         std::to_string(b_) + ',' + std::to_string(c_) + ']';
                    t += 1.0 / 3200.0;
                }
                f += R"(],"errors":)" + std::to_string(errors_) + R"(,"overflows":)" +
                     std::to_string(overflows_) + "}}";
                send_frame(f);
            }

            if (close_when_done_ && client_ >= 0) {
                ::shutdown(client_, SHUT_RDWR);
                ::close(client_);
                client_ = -1;
            }
        });
    }

    void stop() {
        stopping_ = true;
        if (thread_.joinable())
            thread_.join();
        if (client_ >= 0) {
            ::close(client_);
            client_ = -1;
        }
    }

    std::string wait_for_subscribe(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, timeout, [this] { return got_subscribe_; });
        return subscribe_;
    }

  private:
    void send_frame(const std::string& payload) {
        std::string framed = payload;
        framed.push_back('\x03');
        ssize_t off = 0;
        while (off < static_cast<ssize_t>(framed.size()) && !stopping_) {
            // MSG_NOSIGNAL: the client under test closes first in several
            // cases, and a plain write(2) would raise SIGPIPE and kill the
            // whole test binary.
            ssize_t w = ::send(client_, framed.data() + off,
                               framed.size() - static_cast<size_t>(off), MSG_NOSIGNAL);
            if (w <= 0)
                return;
            off += w;
        }
    }

    std::string path_;
    int fd_ = -1;
    int client_ = -1;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex m_;
    std::condition_variable cv_;
    bool got_subscribe_ = false;
    std::string subscribe_;
    std::vector<std::string> header_{"time", "x_acceleration", "y_acceleration", "z_acceleration"};
    double a_ = 1000.0, b_ = 2000.0, c_ = 3000.0;
    int errors_ = 0;
    int overflows_ = 0;
    std::atomic<bool> close_when_done_{false};
};

std::string temp_sock_path(const char* tag) {
    return std::string("/tmp/helix-belt-test-") + tag + "-" + std::to_string(::getpid()) + ".sock";
}

} // namespace

using helix::calibration::AccelBatch;
using helix::calibration::BeltStreamClient;

TEST_CASE("socket_reachable is false for a path that does not exist", "[belt][stream][slow]") {
    CHECK_FALSE(BeltStreamClient::socket_reachable("/tmp/helix-belt-nope-does-not-exist.sock"));
}

TEST_CASE("socket_reachable is true for a live listener", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("reach");
    FakeKlippySocket fake(path);
    CHECK(BeltStreamClient::socket_reachable(path));
}

TEST_CASE("socket_reachable rejects an oversized path rather than truncating",
          "[belt][stream][slow]") {
    // sun_path is 108 bytes. A silently truncated path would connect to the
    // wrong socket, which is worse than failing.
    CHECK_FALSE(BeltStreamClient::socket_reachable("/tmp/" + std::string(200, 'a')));
}

TEST_CASE("endpoint and sensor key are derived from the config section name",
          "[belt][stream][slow]") {
    // Klipper: self.name = config.get_name().split()[-1], and the endpoint is
    // registered by the chip module as "<chip>/dump_<chip>".
    CHECK(BeltStreamClient::endpoint_for_chip("adxl345") == "adxl345/dump_adxl345");
    CHECK(BeltStreamClient::sensor_key_for_chip("adxl345") == "adxl345");

    CHECK(BeltStreamClient::endpoint_for_chip("adxl345 hotend") == "adxl345/dump_adxl345");
    CHECK(BeltStreamClient::sensor_key_for_chip("adxl345 hotend") == "hotend");

    CHECK(BeltStreamClient::endpoint_for_chip("lis2dw bed") == "lis2dw/dump_lis2dw");
    CHECK(BeltStreamClient::sensor_key_for_chip("lis2dw bed") == "bed");

    CHECK(BeltStreamClient::endpoint_for_chip("mpu9250") == "mpu9250/dump_mpu9250");

    CHECK(BeltStreamClient::endpoint_for_chip("").empty());
    CHECK(BeltStreamClient::sensor_key_for_chip("   ").empty());
}

TEST_CASE("client sends a well-formed subscribe naming the sensor", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("sub");
    FakeKlippySocket fake(path);
    fake.serve(0, 0);

    BeltStreamClient client;
    REQUIRE(client.start(path, "adxl345 hotend", nullptr, nullptr));

    const std::string req = fake.wait_for_subscribe(std::chrono::seconds(3));
    client.stop();

    REQUIRE_FALSE(req.empty());
    CHECK(req.back() == '\x03');
    CHECK(req.find("adxl345/dump_adxl345") != std::string::npos);
    // The mux key is the LAST token of the section name. Klipper's
    // register_mux_endpoint stores config.get_name().split()[-1], so sending
    // the full "adxl345 hotend" gets "The value ... is not valid for sensor".
    CHECK(req.find(R"("sensor":"hotend")") != std::string::npos);
    CHECK(req.find(R"("sensor":"adxl345 hotend")") == std::string::npos);
    CHECK(req.find("response_template") != std::string::npos);
    CHECK(req.find("helix_belt_batch") != std::string::npos);
}

TEST_CASE("a non-adxl chip gets its own endpoint", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("lis2dw");
    FakeKlippySocket fake(path);
    fake.serve(0, 0);

    BeltStreamClient client;
    REQUIRE(client.start(path, "lis2dw bed", nullptr, nullptr));
    const std::string req = fake.wait_for_subscribe(std::chrono::seconds(3));
    client.stop();

    REQUIRE_FALSE(req.empty());
    CHECK(req.find("lis2dw/dump_lis2dw") != std::string::npos);
    CHECK(req.find("adxl345") == std::string::npos);
}

TEST_CASE("client decodes batches into samples", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("batch");
    FakeKlippySocket fake(path);
    fake.serve(3, 100);

    std::mutex m;
    std::condition_variable cv;
    size_t total = 0;
    int batches = 0;
    AccelBatch last;

    BeltStreamClient client;
    REQUIRE(client.start(
        path, "adxl345",
        [&](const AccelBatch& b) {
            std::lock_guard<std::mutex> lk(m);
            total += b.samples.size();
            ++batches;
            last = b;
            cv.notify_all();
        },
        nullptr));

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return total >= 300; });
    }
    const float rate = client.sample_rate_hz();
    client.stop();

    CHECK(batches == 3);
    CHECK(total == 300);
    REQUIRE_FALSE(last.samples.empty());
    CHECK(last.samples[0].x == Catch::Approx(1000.0f));
    CHECK(last.samples[0].y == Catch::Approx(2000.0f));
    CHECK(last.samples[0].z == Catch::Approx(3000.0f));
    CHECK(last.contiguous());
    // Timestamps are rebased to the first sample, so the last batch starts
    // ~200 samples in rather than at klippy's absolute print time of 1000 s.
    CHECK(last.samples[0].time == Catch::Approx(200.0f / 3200.0f).margin(0.001));
    // Derived from the received timestamps, not from any configured value.
    CHECK(rate == Catch::Approx(3200.0f).epsilon(0.01));
}

TEST_CASE("axis columns follow result.header rather than position", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("hdr");
    FakeKlippySocket fake(path);
    // Reversed axis order: the row is [t, z, y, x].
    fake.set_header({"time", "z_acceleration", "y_acceleration", "x_acceleration"});
    fake.set_row_values(3000.0, 2000.0, 1000.0);
    fake.serve(1, 10);

    std::mutex m;
    std::condition_variable cv;
    AccelBatch got;
    bool have = false;

    BeltStreamClient client;
    REQUIRE(client.start(
        path, "adxl345",
        [&](const AccelBatch& b) {
            std::lock_guard<std::mutex> lk(m);
            got = b;
            have = true;
            cv.notify_all();
        },
        nullptr));
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return have; });
    }
    client.stop();

    REQUIRE(have);
    REQUIRE_FALSE(got.samples.empty());
    CHECK(got.samples[0].x == Catch::Approx(1000.0f));
    CHECK(got.samples[0].y == Catch::Approx(2000.0f));
    CHECK(got.samples[0].z == Catch::Approx(3000.0f));
}

TEST_CASE("nonzero klippy counters mark the window as non-contiguous", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("drop");
    FakeKlippySocket fake(path);
    fake.set_counters(2, 7);
    fake.serve(1, 10);

    std::mutex m;
    std::condition_variable cv;
    AccelBatch got;
    bool have = false;

    BeltStreamClient client;
    REQUIRE(client.start(
        path, "adxl345",
        [&](const AccelBatch& b) {
            std::lock_guard<std::mutex> lk(m);
            got = b;
            have = true;
            cv.notify_all();
        },
        nullptr));
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return have; });
    }
    client.stop();

    REQUIRE(have);
    CHECK(got.errors == 2);
    CHECK(got.overflows == 7);
    CHECK_FALSE(got.contiguous());
}

TEST_CASE("a 14 KB batch split across many reads still decodes", "[belt][stream][slow]") {
    // 340 rows is the measured batch size on the reference printer: ~13.3 KB,
    // well past libhv's 8 KB default read buffer and past a single UDS read.
    const std::string path = temp_sock_path("big");
    FakeKlippySocket fake(path);
    fake.serve(2, 340);

    std::mutex m;
    std::condition_variable cv;
    size_t total = 0;

    BeltStreamClient client;
    REQUIRE(client.start(
        path, "adxl345",
        [&](const AccelBatch& b) {
            std::lock_guard<std::mutex> lk(m);
            total += b.samples.size();
            cv.notify_all();
        },
        nullptr));
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return total >= 680; });
    }
    client.stop();

    CHECK(total == 680);
}

TEST_CASE("client reports an error for an absent socket", "[belt][stream][slow]") {
    std::mutex m;
    std::condition_variable cv;
    std::string err;

    BeltStreamClient client;
    const bool ok =
        client.start("/tmp/helix-belt-absent.sock", "adxl345", nullptr, [&](const std::string& e) {
            std::lock_guard<std::mutex> lk(m);
            err = e;
            cv.notify_all();
        });

    if (ok) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(3), [&] { return !err.empty(); });
    }
    client.stop();

    // Either a synchronous false or an async error is acceptable, but silence
    // is not - a panel that gets neither would spin forever on "connecting".
    CHECK((!ok || !err.empty()));
    CHECK_FALSE(client.running());
}

TEST_CASE("klippy hanging up is reported as an error", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("eof");
    FakeKlippySocket fake(path);
    fake.set_close_when_done(true);
    fake.serve(1, 10);

    std::mutex m;
    std::condition_variable cv;
    std::string err;

    BeltStreamClient client;
    REQUIRE(client.start(path, "adxl345", nullptr, [&](const std::string& e) {
        std::lock_guard<std::mutex> lk(m);
        if (err.empty())
            err = e;
        cv.notify_all();
    }));
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return !err.empty(); });
    }
    client.stop();

    CHECK_FALSE(err.empty());
    CHECK_FALSE(client.running());
}

TEST_CASE("a silent socket trips the stall timeout", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("stall");
    FakeKlippySocket fake(path);
    fake.serve(0, 0); // Ack only, then silence, connection left open.

    std::mutex m;
    std::condition_variable cv;
    std::string err;

    BeltStreamClient client;
    REQUIRE(client.start(path, "adxl345", nullptr, [&](const std::string& e) {
        std::lock_guard<std::mutex> lk(m);
        if (err.empty())
            err = e;
        cv.notify_all();
    }));
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(8), [&] { return !err.empty(); });
    }
    client.stop();

    REQUIRE_FALSE(err.empty());
    CHECK(err.find("stall") != std::string::npos);
}

TEST_CASE("stop is idempotent and a second start reconnects", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("restart");

    {
        FakeKlippySocket fake(path);
        fake.serve(1, 10);
        BeltStreamClient client;
        REQUIRE(client.start(path, "adxl345", nullptr, nullptr));
        CHECK(client.running());
        client.stop();
        client.stop(); // Idempotent.
        CHECK_FALSE(client.running());

        // The same object must be usable again - a retry after a dropped
        // stream is the obvious thing a panel does.
        fake.stop();
    }

    FakeKlippySocket fake2(path);
    fake2.serve(1, 10);

    std::mutex m;
    std::condition_variable cv;
    size_t total = 0;

    BeltStreamClient client;
    REQUIRE(client.start(
        path, "adxl345",
        [&](const AccelBatch& b) {
            std::lock_guard<std::mutex> lk(m);
            total += b.samples.size();
            cv.notify_all();
        },
        nullptr));
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return total >= 10; });
    }
    client.stop();
    CHECK(total == 10);
}

TEST_CASE("destroying a running client does not hang or crash", "[belt][stream][slow]") {
    const std::string path = temp_sock_path("dtor");
    FakeKlippySocket fake(path);
    fake.serve(1000, 50);
    {
        BeltStreamClient client;
        REQUIRE(client.start(path, "adxl345", [](const AccelBatch&) {}, nullptr));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // No explicit stop() - the destructor must handle a live stream.
    }
    SUCCEED("destructor completed");
}
