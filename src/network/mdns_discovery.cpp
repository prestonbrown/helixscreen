// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file mdns_discovery.cpp
 * @brief mDNS discovery implementation for finding Moonraker printers
 *
 * @pattern PIMPL with background thread for network I/O
 * @threading Discovery runs on background thread; callbacks dispatched via
 * helix::ui::queue_update()
 * @gotchas Socket may fail on systems without network; handle gracefully
 *
 * @see wifi_manager.cpp for similar threading patterns
 */

#define MDNS_IMPLEMENTATION
#include "mdns_discovery.h"

#include "core/update_queue.h"
#include "helix_thread.h"
#include "mdns/mdns.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <sys/time.h>
#include <thread>

using namespace helix;

namespace {

// Query interval for mDNS discovery (re-query every 3 seconds)
constexpr auto kQueryInterval = std::chrono::milliseconds(3000);

// Buffer size for mDNS operations (must be 32-bit aligned)
constexpr size_t kMdnsBufferSize = 2048;

// Timeout for socket receive operations (milliseconds)
constexpr int kSocketTimeoutMs = 500;

/**
 * @brief Extract display name from a full hostname
 *
 * Removes ".local" suffix if present to get a human-readable name.
 * Example: "voron.local" -> "voron", "printer" -> "printer"
 */
std::string extract_display_name(const std::string& hostname) {
    const std::string suffix = ".local";
    if (hostname.size() > suffix.size() &&
        hostname.compare(hostname.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return hostname.substr(0, hostname.size() - suffix.size());
    }
    return hostname;
}

/**
 * @brief Convert IPv4 sockaddr to string
 */
std::string sockaddr_to_string(const struct sockaddr_in* addr) {
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
        return std::string(buf);
    }
    return "";
}

} // namespace

/**
 * @brief Internal data structure for tracking partial service records
 *
 * mDNS responses may come in multiple packets with different record types.
 * We need to collect PTR -> SRV -> A records to build a complete printer entry.
 */
struct ServiceRecord {
    std::string instance_name; ///< Full instance name from PTR record
    std::string hostname;      ///< Target host from SRV record
    uint16_t port = 0;         ///< Port from SRV record
    std::string ip_address;    ///< IPv4 address from A record
    MdnsTxtFields txt;         ///< From the TXT record

    bool is_complete() const {
        return !hostname.empty() && port > 0 && !ip_address.empty();
    }

    /// Best available display name: TXT "ty" > TXT "product" > hostname
    std::string best_display_name() const {
        if (!txt.model_name.empty())
            return txt.model_name;
        if (!txt.product.empty())
            return txt.product;
        return extract_display_name(hostname);
    }
};

/**
 * @brief PIMPL implementation class
 */
class MdnsDiscovery::Impl {
  public:
    explicit Impl(std::string service_type) : service_type_(std::move(service_type)) {}
    ~Impl() {
        stop();
    }

    void start(DiscoveryCallback callback) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback_ = std::move(callback);

            // If already running, just dispatch current results
            if (running_.load()) {
                dispatch_update();
                return;
            }
        }

        // Reap any prior thread that exited on its own (e.g. socket-open
        // failure path below sets running_=false and returns early). Without
        // this, the next move-assignment to a joinable thread_ calls
        // std::terminate WITHOUT an active exception (UBZQ94EE).
        if (thread_.joinable()) {
            thread_.join();
        }

        // Start discovery thread. Wrap — EAGAIN under thread exhaustion
        // throws std::system_error ([L083]).
        running_.store(true);
        initial_update_sent_.store(false); // Reset so first query dispatches even if empty
        try {
            thread_ = helix::make_thread(&Impl::discovery_loop, this);
            spdlog::info("[MdnsDiscovery] Started discovery for {}", service_type_);
        } catch (const std::system_error& e) {
            spdlog::error("[MdnsDiscovery] Failed to spawn discovery thread: {}", e.what());
            running_.store(false);
        }
    }

    void stop() {
        // Signal thread to stop. Always proceed to the join below — the loop
        // can clear running_ on its own (socket-open failure path), in which
        // case an early return here would leave thread_ joinable. The next
        // ~Impl or the next start() would then std::terminate.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.store(false);
            callback_ = nullptr;
        }

        // Wake up thread if it's sleeping
        stop_cv_.notify_all();

        // Wait for thread to exit. Only log "Stopped" if there was actually
        // a thread to reap — covers the case where ~Impl() runs without
        // start() ever being called, or after the loop self-terminated.
        if (thread_.joinable()) {
            thread_.join();
            spdlog::info("[MdnsDiscovery] Stopped discovery");
        }
    }

    bool is_running() const {
        return running_.load();
    }

    std::vector<DiscoveredPrinter> get_printers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return printers_;
    }

  private:
    /**
     * @brief Main discovery loop running on background thread
     */
    void discovery_loop() {
        spdlog::debug("[MdnsDiscovery] Discovery thread started");

        // Open mDNS socket
        int sock = mdns_socket_open_ipv4(nullptr);
        if (sock < 0) {
            spdlog::warn("[MdnsDiscovery] Failed to open mDNS socket - network may be unavailable");
            running_.store(false);
            // Dispatch empty result so UI knows discovery is complete (with no results)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                dispatch_update();
            }
            return;
        }

        // Set socket timeout for non-blocking receives
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = kSocketTimeoutMs * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Aligned buffer for mDNS operations
        alignas(4) uint8_t buffer[kMdnsBufferSize];

        while (running_.load()) {
            // Send PTR query for Moonraker service
            int query_id = mdns_query_send(sock, MDNS_RECORDTYPE_PTR, service_type_.c_str(),
                                           service_type_.size(), buffer, sizeof(buffer), 0);

            if (query_id < 0) {
                spdlog::debug("[MdnsDiscovery] Failed to send mDNS query");
            } else {
                spdlog::trace("[MdnsDiscovery] Sent PTR query for {}", service_type_);

                // Receive responses for a short window
                auto recv_deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

                while (std::chrono::steady_clock::now() < recv_deadline && running_.load()) {
                    size_t records = mdns_query_recv(sock, buffer, sizeof(buffer), record_callback,
                                                     this, query_id);

                    if (records == 0) {
                        // No more responses, brief pause before next receive attempt
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
            }

            // Process collected records and update printer list
            process_pending_records();

            // Wait for next query interval (or until stop requested)
            std::unique_lock<std::mutex> lock(stop_mutex_);
            stop_cv_.wait_for(lock, kQueryInterval, [this]() { return !running_.load(); });
        }

        mdns_socket_close(sock);
        spdlog::debug("[MdnsDiscovery] Discovery thread exiting");
    }

    /**
     * @brief Static callback for mDNS record parsing
     */
    static int record_callback(int sock, const struct sockaddr* from, size_t addrlen,
                               mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                               uint16_t rclass, uint32_t ttl, const void* data, size_t size,
                               size_t name_offset, size_t name_length, size_t record_offset,
                               size_t record_length, void* user_data) {
        (void)sock;
        (void)from;
        (void)addrlen;
        (void)entry;
        (void)query_id;
        (void)rclass;
        (void)ttl;
        (void)name_length;

        auto* self = static_cast<Impl*>(user_data);
        if (!self || !self->running_.load()) {
            return 1; // Stop processing
        }

        char namebuf[256];
        char entrybuf[256];

        // Extract the name this record is for
        mdns_string_t name_str =
            mdns_string_extract(data, size, &name_offset, namebuf, sizeof(namebuf));

        switch (rtype) {
        case MDNS_RECORDTYPE_PTR: {
            // PTR record gives us the service instance name
            mdns_string_t ptr_str = mdns_record_parse_ptr(data, size, record_offset, record_length,
                                                          entrybuf, sizeof(entrybuf));

            if (ptr_str.length > 0) {
                std::string instance_name(ptr_str.str, ptr_str.length);
                spdlog::trace("[MdnsDiscovery] PTR: {} -> {}",
                              std::string(name_str.str, name_str.length), instance_name);

                std::lock_guard<std::mutex> lock(self->records_mutex_);
                // Create or get existing record for this instance
                self->pending_records_[instance_name].instance_name = instance_name;
            }
            break;
        }

        case MDNS_RECORDTYPE_SRV: {
            // SRV record gives us host and port
            mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length,
                                                          entrybuf, sizeof(entrybuf));

            if (srv.name.length > 0 && srv.port > 0) {
                std::string record_name(name_str.str, name_str.length);
                std::string hostname(srv.name.str, srv.name.length);

                spdlog::trace("[MdnsDiscovery] SRV: {} -> {}:{}", record_name, hostname, srv.port);

                std::lock_guard<std::mutex> lock(self->records_mutex_);
                auto& record = self->pending_records_[record_name];
                record.instance_name = record_name;
                record.hostname = hostname;
                record.port = srv.port;
            }
            break;
        }

        case MDNS_RECORDTYPE_A: {
            // A record gives us IPv4 address
            struct sockaddr_in addr;
            mdns_record_parse_a(data, size, record_offset, record_length, &addr);

            std::string record_name(name_str.str, name_str.length);
            std::string ip = sockaddr_to_string(&addr);

            if (!ip.empty()) {
                spdlog::trace("[MdnsDiscovery] A: {} -> {}", record_name, ip);

                std::lock_guard<std::mutex> lock(self->records_mutex_);
                // A records are keyed by hostname, need to find matching SRV records
                self->address_cache_[record_name] = ip;
            }
            break;
        }

        case MDNS_RECORDTYPE_TXT: {
            // TXT records contain key=value pairs (ty, product, etc.)
            mdns_record_txt_t txt_records[16];
            size_t txt_count =
                mdns_record_parse_txt(data, size, record_offset, record_length, txt_records, 16);

            if (txt_count > 0) {
                std::string record_name(name_str.str, name_str.length);
                std::lock_guard<std::mutex> lock(self->records_mutex_);
                auto& record = self->pending_records_[record_name];
                record.instance_name = record_name;

                for (size_t i = 0; i < txt_count; i++) {
                    std::string key(txt_records[i].key.str, txt_records[i].key.length);
                    std::string value;
                    if (txt_records[i].value.str && txt_records[i].value.length > 0) {
                        value.assign(txt_records[i].value.str, txt_records[i].value.length);
                    }

                    spdlog::trace("[MdnsDiscovery] TXT {}: {} -> {}", key, record_name, value);
                    apply_mdns_txt_entry(record.txt, key, value);
                }
            }
            break;
        }

        case MDNS_RECORDTYPE_AAAA:
            // IPv6 - we prefer IPv4, so skip these
            break;

        default:
            break;
        }

        return 0; // Continue processing
    }

    /**
     * @brief Process collected records into complete printer entries
     */
    void process_pending_records() {
        std::vector<DiscoveredPrinter> new_printers;

        {
            std::lock_guard<std::mutex> lock(records_mutex_);

            for (auto& [name, record] : pending_records_) {
                // Try to resolve IP from address cache if not already set
                if (record.ip_address.empty() && !record.hostname.empty()) {
                    auto it = address_cache_.find(record.hostname);
                    if (it != address_cache_.end()) {
                        record.ip_address = it->second;
                    }
                }

                // Only add complete records
                if (record.is_complete()) {
                    auto printer =
                        make_discovered_printer(record.best_display_name(), record.hostname,
                                                record.ip_address, record.port, record.txt);

                    // Deduplicate by IP:port
                    auto it = std::find(new_printers.begin(), new_printers.end(), printer);
                    if (it == new_printers.end()) {
                        new_printers.push_back(printer);
                    }
                }
            }

            // Prune stale incomplete records that failed to resolve across
            // multiple query cycles.  Complete records and the address cache
            // are kept so that services still on the network are recognised
            // immediately on the next cycle without waiting for all DNS
            // record types to arrive again.
            for (auto it = pending_records_.begin(); it != pending_records_.end();) {
                if (!it->second.is_complete()) {
                    it = pending_records_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Check if list changed (or if this is the first update)
        bool changed = false;
        bool first_update = !initial_update_sent_.load();
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (new_printers.size() != printers_.size()) {
                changed = true;
            } else {
                for (const auto& printer : new_printers) {
                    if (std::find(printers_.begin(), printers_.end(), printer) == printers_.end()) {
                        changed = true;
                        break;
                    }
                }
            }

            // Dispatch if list changed OR if this is the first update (even if empty)
            if (changed || first_update) {
                printers_ = std::move(new_printers);
                initial_update_sent_.store(true);
                spdlog::info("[MdnsDiscovery] Found {} services for {}", printers_.size(),
                             service_type_);

                for (const auto& p : printers_) {
                    spdlog::trace("[MdnsDiscovery]   {} ({}) at {}:{}", p.name, p.hostname,
                                  p.ip_address, p.port);
                }

                dispatch_update();
            }
        }
    }

    /**
     * @brief Dispatch update to main thread via ui_queue_update()
     *
     * Must be called with mutex_ held.
     */
    void dispatch_update() {
        if (!callback_) {
            return;
        }

        // Copy data for async dispatch
        auto printers_copy = std::make_shared<std::vector<DiscoveredPrinter>>(printers_);
        auto callback_copy = callback_;

        helix::ui::queue_update("MdnsDiscovery::dispatch_update", [printers_copy, callback_copy]() {
            if (callback_copy) {
                callback_copy(*printers_copy);
            }
        });
    }

    // Service type to query
    std::string service_type_;

    // Thread management
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initial_update_sent_{false};

    // Synchronization for stop
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;

    // Protected by mutex_
    mutable std::mutex mutex_;
    DiscoveryCallback callback_;
    std::vector<DiscoveredPrinter> printers_;

    // Protected by records_mutex_ (separate lock for record collection)
    std::mutex records_mutex_;
    std::map<std::string, ServiceRecord> pending_records_;
    std::map<std::string, std::string> address_cache_; // hostname -> IP
};

// ============================================================================
// MdnsDiscovery public interface
// ============================================================================

MdnsDiscovery::MdnsDiscovery(std::string service_type)
    : impl_(std::make_unique<Impl>(std::move(service_type))) {}

MdnsDiscovery::~MdnsDiscovery() = default;

void MdnsDiscovery::start_discovery(DiscoveryCallback on_update) {
    impl_->start(std::move(on_update));
}

void MdnsDiscovery::stop_discovery() {
    impl_->stop();
}

bool MdnsDiscovery::is_discovering() const {
    return impl_->is_running();
}

std::vector<DiscoveredPrinter> MdnsDiscovery::get_discovered_printers() const {
    return impl_->get_printers();
}
