// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_manager.h"

#include <spdlog/spdlog.h>

UsbManager::UsbManager(bool force_mock) : force_mock_(force_mock) {
    spdlog::debug("[UsbManager] Created (force_mock={})", force_mock);
}

UsbManager::~UsbManager() {
    // Don't call stop() which locks mutex_ - during static destruction
    // the mutex may already be destroyed, causing "mutex lock failed" crash.
    // The backend is stopped explicitly because a scan may still hold a
    // reference to it: left running, its monitor thread would keep calling
    // on_backend_event() on this destroyed manager.
    if (backend_) {
        backend_->stop();
    }
    backend_.reset();
}

bool UsbManager::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (backend_ && backend_->is_running()) {
        spdlog::debug("[UsbManager] Already running");
        return true;
    }

    // Create backend (returns nullptr if USB not supported on this platform)
    backend_ = UsbBackend::create(force_mock_);
    if (!backend_) {
        spdlog::info("[UsbManager] USB support not available on this platform");
        return false;
    }

    // Set up event callback
    backend_->set_event_callback(
        [this](UsbEvent event, const UsbDrive& drive) { on_backend_event(event, drive); });

    // Start backend
    UsbError result = backend_->start();
    if (!result.success()) {
        spdlog::error("[UsbManager] Failed to start backend: {}", result.technical_msg);
        backend_.reset();
        return false;
    }

    spdlog::debug("[UsbManager] Started successfully");
    return true;
}

void UsbManager::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!backend_) {
        return;
    }

    backend_->stop();
    backend_.reset();
    // Guard against logging during static destruction (spdlog may be gone)
    if (spdlog::default_logger()) {
        spdlog::info("[UsbManager] Stopped");
    }
}

bool UsbManager::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_ && backend_->is_running();
}

void UsbManager::set_drive_callback(DriveCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    drive_callback_ = std::move(callback);
}

std::vector<UsbDrive> UsbManager::get_drives() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<UsbDrive> drives;
    if (!backend_ || !backend_->is_running()) {
        return drives;
    }

    UsbError result = backend_->get_connected_drives(drives);
    if (!result.success()) {
        spdlog::warn("[UsbManager] Failed to get drives: {}", result.technical_msg);
        drives.clear();
    }

    return drives;
}

std::vector<UsbGcodeFile> UsbManager::scan_for_gcode(const std::string& mount_path,
                                                     int max_depth) const {
    std::shared_ptr<UsbBackend> backend;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        backend = backend_;
    }

    std::vector<UsbGcodeFile> files;
    if (!backend || !backend->is_running()) {
        return files;
    }

    UsbError result = backend->scan_for_gcode(mount_path, files, max_depth);
    if (!result.success()) {
        spdlog::warn("[UsbManager] Failed to scan for G-code: {}", result.technical_msg);
        files.clear();
    }

    return files;
}

std::shared_ptr<UsbBackend> UsbManager::backend_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_;
}

UsbBackend* UsbManager::get_backend() {
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_.get();
}

void UsbManager::on_backend_event(UsbEvent event, const UsbDrive& drive) {
    DriveCallback callback_copy;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_copy = drive_callback_;
    }

    // Log the event
    const char* event_name = (event == UsbEvent::DRIVE_INSERTED) ? "INSERTED" : "REMOVED";
    spdlog::debug("[UsbManager] Drive {}: {} ({})", event_name, drive.label, drive.mount_path);

    // Fire callback outside lock
    if (callback_copy) {
        callback_copy(event, drive);
    }
}
