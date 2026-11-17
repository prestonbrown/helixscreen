// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote_screen_http_server.h"

#include "helix_thread.h"
#include "http_listen_socket.h"
#include "hv/HttpServer.h"
#include "json_utils.h"
#include "remote_screen_server_sink.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "hv/json.hpp"

namespace helix {

namespace {
/// Open MJPEG streams served at once; each costs a socket and a write per frame.
constexpr size_t kMaxStreams = 4;
constexpr const char* kStreamBoundary = "helixframe";

/// The host a proxy in front of the nginx route forwarded, for the Origin check.
std::string forwarded_host(bool lan_mode, HttpRequest* req) {
    return trusted_forwarded_host(!lan_mode, req->GetHeader("X-Forwarded-Host"),
                                  req->GetHeader("Forwarded"));
}
} // namespace

struct ScreenHttpServer::Impl {
    hv::HttpService router;
    hv::HttpServer server;

    /// Every open stream.mjpg, fed by one thread so no request owns a thread of its own.
    struct Stream {
        HttpResponseWriterPtr writer;
        uint64_t sent_generation = 0;
        bool sent_any = false;
    };
    std::mutex streams_mutex;
    std::condition_variable streams_cv;
    std::vector<Stream> streams;
    bool stopping = false;
    std::thread stream_thread;
};

ScreenHttpServer::ScreenHttpServer(std::shared_ptr<ScreenFrameStore> store, ScreenServerHooks hooks)
    : store_(std::move(store)), hooks_(std::move(hooks)) {}

ScreenHttpServer::~ScreenHttpServer() {
    stop();
}

bool ScreenHttpServer::start(const ScreenServerConfig& cfg) {
    if (impl_) {
        return true;
    }
    cfg_ = cfg;
    control_allowed_.store(cfg.control_allowed);
    const SnapshotRateCap cap = snapshot_rate_cap(remote_screen_host_is_constrained());

    int port = 0;
    const int fd = bind_tcp_listener(cfg.bind_host.c_str(), cfg.port, &port);
    if (fd < 0) {
        spdlog::warn("[RemoteScreen] Cannot listen on {}:{} - screen server not started",
                     cfg.bind_host, cfg.port);
        return false;
    }

    auto impl = std::make_unique<Impl>();

    impl->router.GET("/snapshot.jpg", [this, cap](HttpRequest* req, HttpResponse* resp) {
        if (req->GetParam(kViewerPageParam) == "1") {
            store_->note_page_view();
        }
        const uint64_t full = store_->full_generation();
        if (store_->note_request() == ScreenFrameStore::Need::FullRedraw) {
            if (hooks_.request_full_redraw) {
                hooks_.request_full_redraw();
            }
            store_->wait_for_full_refresh_after(full, std::chrono::milliseconds(kFirstFrameWaitMs));
        }
        const bool printing = hooks_.is_printing && hooks_.is_printing();
        const auto snap = store_->snapshot(printing, cap);
        resp->headers["Cache-Control"] = "no-store";
        if (!snap) {
            resp->headers["Retry-After"] = "1";
            return static_cast<int>(HTTP_STATUS_SERVICE_UNAVAILABLE);
        }
        resp->content_type = IMAGE_JPEG;
        resp->body.assign(reinterpret_cast<const char*>(snap->jpeg->data()), snap->jpeg->size());
        return static_cast<int>(HTTP_STATUS_OK);
    });

    impl->router.GET("/stream.mjpg", [this, raw = impl.get()](const HttpContextPtr& ctx) {
        std::lock_guard<std::mutex> lock(raw->streams_mutex);
        if (raw->stopping || raw->streams.size() >= kMaxStreams) {
            ctx->writer->Begin();
            ctx->writer->WriteStatus(HTTP_STATUS_SERVICE_UNAVAILABLE);
            ctx->writer->WriteHeader("Retry-After", "1");
            ctx->writer->End();
            return static_cast<int>(HTTP_STATUS_UNFINISHED);
        }
        ctx->writer->Begin();
        ctx->writer->WriteStatus(HTTP_STATUS_OK);
        ctx->writer->WriteHeader(
            "Content-Type", std::string("multipart/x-mixed-replace; boundary=") + kStreamBoundary);
        ctx->writer->WriteHeader("Cache-Control", "no-store");
        ctx->writer->EndHeaders();
        raw->streams.push_back({ctx->writer});
        raw->streams_cv.notify_all();
        return static_cast<int>(HTTP_STATUS_UNFINISHED);
    });

    impl->router.GET("/info", [this](HttpRequest* req, HttpResponse* resp) {
        const auto [w, h] = store_->frame_size();
        nlohmann::json j = {{"w", w},
                            {"h", h},
                            {"control", control_allowed_.load()},
                            {"paired", request_paired(req)}};
        resp->headers["Cache-Control"] = "no-store";
        resp->content_type = APPLICATION_JSON;
        resp->body = json_util::safe_dump(j);
        return static_cast<int>(HTTP_STATUS_OK);
    });

    impl->router.POST("/touch", [this](HttpRequest* req, HttpResponse* resp) {
        const TouchGate gate = check_touch_request(
            control_allowed_.load(), cfg_.lan_mode, req->GetHeader("Content-Type"),
            req->GetHeader("X-Helix-Touch"), req->GetHeader("Origin"), req->GetHeader("Host"),
            forwarded_host(cfg_.lan_mode, req), request_paired(req));
        if (gate != TouchGate::Allowed) {
            spdlog::debug("[RemoteScreen] touch refused ({})", static_cast<int>(gate));
            return static_cast<int>(gate == TouchGate::NotPaired ? HTTP_STATUS_UNAUTHORIZED
                                                                 : HTTP_STATUS_FORBIDDEN);
        }
        const auto [w, h] = store_->frame_size();
        const auto event = parse_touch_body(req->body, w, h);
        if (!event) {
            return static_cast<int>(HTTP_STATUS_BAD_REQUEST);
        }
        store_->note_request();
        if (!hooks_.apply_touch || !hooks_.apply_touch(*event)) {
            resp->headers["Retry-After"] = "1";
            return static_cast<int>(HTTP_STATUS_SERVICE_UNAVAILABLE);
        }
        return static_cast<int>(HTTP_STATUS_NO_CONTENT);
    });

    impl->router.POST("/pair", [this](HttpRequest* req, HttpResponse* resp) {
        // The same cross-site rules as touch; pairing itself is what this grants.
        const TouchGate gate =
            check_touch_request(control_allowed_.load(), false, req->GetHeader("Content-Type"),
                                req->GetHeader("X-Helix-Touch"), req->GetHeader("Origin"),
                                req->GetHeader("Host"), forwarded_host(cfg_.lan_mode, req), false);
        if (gate != TouchGate::Allowed || !hooks_.pairing) {
            return static_cast<int>(HTTP_STATUS_FORBIDDEN);
        }
        const auto j = nlohmann::json::parse(req->body, nullptr, false);
        const auto code = j.is_object() ? j.find("code") : j.end();
        if (!j.is_object() || code == j.end() || !code->is_string()) {
            return static_cast<int>(HTTP_STATUS_BAD_REQUEST);
        }
        std::string token;
        switch (hooks_.pairing->try_pair(code->get<std::string>(), token)) {
        case PairingStore::PairResult::Paired:
            // The page keeps the token and sends it as kPairHeaderName: a cookie set
            // here never reaches a frame embedded by Mainsail on another host name.
            resp->headers["Cache-Control"] = "no-store";
            resp->content_type = APPLICATION_JSON;
            resp->body = json_util::safe_dump(nlohmann::json{{"paired", true}, {"token", token}});
            return static_cast<int>(HTTP_STATUS_OK);
        case PairingStore::PairResult::RateLimited:
            resp->headers["Retry-After"] = std::to_string(hooks_.pairing->retry_after_seconds());
            return static_cast<int>(HTTP_STATUS_TOO_MANY_REQUESTS);
        case PairingStore::PairResult::WrongCode:
            break;
        }
        return static_cast<int>(HTTP_STATUS_FORBIDDEN);
    });

    impl->router.GET("/", [this](HttpRequest*, HttpResponse* resp) {
        std::ifstream in(cfg_.page_path, std::ios::binary);
        if (!in) {
            resp->content_type = TEXT_PLAIN;
            resp->body = "screen page not installed\n";
            return static_cast<int>(HTTP_STATUS_NOT_FOUND);
        }
        resp->content_type = TEXT_HTML;
        resp->body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return static_cast<int>(HTTP_STATUS_OK);
    });

    impl->server.registerHttpService(&impl->router);
    impl->server.setListenFD(fd);
    impl->server.setPort(0); // leave libhv's own Listen() path disabled
    impl->server.setThreadNum(1);
    if (impl->server.start() != 0) {
        spdlog::warn("[RemoteScreen] Screen server failed to start on port {}", port);
        ::close(fd);
        return false;
    }

    impl->stream_thread =
        helix::make_thread([this, raw = impl.get(), cap] { run_streams(raw, cap); });
    impl_ = std::move(impl);
    port_ = port;
    spdlog::info("[RemoteScreen] Screen server listening on {}:{}", cfg.bind_host, port);
    return true;
}

void ScreenHttpServer::stop() {
    if (!impl_) {
        return;
    }
    // A handler blocked in its first-frame wait would hold stop() for the whole wait.
    store_->abort_waits();
    {
        std::lock_guard<std::mutex> lock(impl_->streams_mutex);
        impl_->stopping = true;
    }
    impl_->streams_cv.notify_all();
    if (impl_->stream_thread.joinable()) {
        impl_->stream_thread.join();
    }
    for (auto& s : impl_->streams) {
        s.writer->close();
    }
    impl_->streams.clear();
    impl_->server.stop();
    impl_.reset();
    port_ = 0;
    spdlog::info("[RemoteScreen] Screen server stopped");
}

void ScreenHttpServer::run_streams(Impl* impl, SnapshotRateCap cap) {
    std::unique_lock<std::mutex> lock(impl->streams_mutex);
    while (!impl->stopping) {
        if (impl->streams.empty()) {
            impl->streams_cv.wait(lock,
                                  [impl] { return impl->stopping || !impl->streams.empty(); });
            continue;
        }
        lock.unlock();

        // An open stream is a viewer, whether or not anyone polls snapshot.jpg.
        if (store_->note_request() == ScreenFrameStore::Need::FullRedraw &&
            hooks_.request_full_redraw) {
            hooks_.request_full_redraw();
        }
        const bool printing = hooks_.is_printing && hooks_.is_printing();
        const auto snap = store_->snapshot(printing, cap);

        lock.lock();
        auto& streams = impl->streams;
        streams.erase(
            std::remove_if(streams.begin(), streams.end(),
                           [](const Impl::Stream& s) { return !s.writer->isConnected(); }),
            streams.end());
        if (snap) {
            for (auto& s : streams) {
                if (s.sent_any && s.sent_generation == snap->generation) {
                    continue;
                }
                // A client that stopped reading (a phone that locked with Mainsail open)
                // would otherwise have frames queued for it until libhv's 16 MB cap.
                // Skip it until its last frame has gone out; it gets the newest one then.
                if (s.writer->writeBufsize() > 0) {
                    continue;
                }
                std::string head = std::string("--") + kStreamBoundary +
                                   "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                                   std::to_string(snap->jpeg->size()) + "\r\n\r\n";
                s.writer->WriteBody(head);
                s.writer->WriteBody(reinterpret_cast<const char*>(snap->jpeg->data()),
                                    static_cast<int>(snap->jpeg->size()));
                s.writer->WriteBody("\r\n", 2);
                s.sent_generation = snap->generation;
                s.sent_any = true;
            }
        }
        impl->streams_cv.wait_for(lock, min_encode_interval(cap, printing),
                                  [impl] { return impl->stopping; });
    }
}

bool ScreenHttpServer::request_paired(HttpRequest* req) const {
    if (!cfg_.lan_mode) {
        return true;
    }
    return hooks_.pairing && hooks_.pairing->is_paired(req->GetHeader(kPairHeaderName));
}

int ScreenHttpServer::port() const {
    return port_;
}

void ScreenHttpServer::set_control_allowed(bool allowed) {
    control_allowed_.store(allowed);
}

} // namespace helix
