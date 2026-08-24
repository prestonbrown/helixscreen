// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 14 — SoftAP + esp_http_server captive portal (companion header:
// provisioning_esp.h). Deliberately NOT the IDF wifi_provisioning component —
// this is the minimal single-page form the design calls for.
//
// Credential hygiene (R3, absolute): the portal never writes NVS itself. The
// "/save" handler validates the form, then hands the SSID/password to the
// shared WiFiManager (Task 13) exactly the way Settings > Network does —
// WiFiManager::connect() -> WifiBackendEsp::connect_network(), the ONE NVS
// writer. The password is read off the wire, passed to that call, and never
// logged at any level (only the SSID and a bool are logged below).
//
// APSTA choice (documented per the brief): WifiBackendEsp::start() (called by
// app_net_thread_main's wifi->retry_async() BEFORE this runs) already brings
// esp_wifi up in WIFI_MODE_STA. Provisioning adds soft-AP on top via
// esp_wifi_set_mode(WIFI_MODE_APSTA) rather than stopping/restarting the
// driver — the same "STA already running, layer AP on top" shape the IDF
// wifi_provisioning softAP scheme itself uses, so a join attempt during
// provisioning goes through the exact STA runtime Settings > Network uses,
// and the AP survives a failed join so the portal can report the error and
// let the user retry (R2) without losing the phone's connection to the
// device.
//
// Threading: the DNS catch-all + AP/portal lifecycle run synchronously on the
// caller's thread (app_net_thread_main's dedicated pthread — see app_boot.cpp)
// via a bounded recv-timeout poll loop; esp_http_server owns its own request
// task internally (httpd_start), which is the standard ESP-IDF shape, not a
// raw std::thread spawn. httpd handler bodies are therefore a BG-thread
// surface: the only UI-touching work (showing/hiding the instructions modal)
// is marshalled with helix::ui::queue_update(), never touched directly.

#include "provisioning_esp.h"

#include "sdkconfig.h"

#if CONFIG_HELIX_MOCK_PRINTER

// Mock builds have no radio / net path (see app_boot.cpp's synthetic driver) —
// provisioning must never activate. Inert stubs so the call site in
// app_boot.cpp needs no #if of its own.
namespace helix {
bool provisioning_needed() {
    return false;
}
bool provisioning_run_portal() {
    return false;
}
} // namespace helix

#else // !CONFIG_HELIX_MOCK_PRINTER

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_redact.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs.h"
#include "utils/network_validation.h"
#include "wifi_backend_esp.h"
#include "wifi_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char* TAG = "provisioning";

constexpr char WIFI_NVS_NAMESPACE[] = "wifi"; // Task 13's namespace — read-only here
constexpr char WIFI_NVS_KEY_SSID[] = "ssid";

constexpr int AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int DNS_PORT = 53;
constexpr int DNS_RECV_TIMEOUT_MS = 250; // bounds how often the poll loop re-checks exit flags
// The /save handler's join poll must outlast the backend's own real assoc
// envelope (wifi_backend_esp.cpp): ASSOC_TIMEOUT_US=15s, then a bounded-backoff
// retry starting at RETRY_BACKOFF_FLOOR_US=2s. 15s (first attempt) + 2s
// (backoff floor) + 15s (retry attempt) = 32s worst case for the common
// slow-but-successful case; 40s leaves comfortable margin so in-window
// resolution is the normal case, not the exception (review MEDIUM-1).
constexpr int SAVE_JOIN_TIMEOUT_MS = 40000;
constexpr int SAVE_POLL_MS = 200;

// Ceiling on how long the portal keeps the net thread parked. Nothing else
// ends an unattended session: the dismiss flag needs a touch and s_join_succeeded
// needs someone to submit the form, so without this the thread waits forever and
// the bounded startup wait in app_boot.cpp never gets to run. Sized for a human
// doing the whole flow by hand — find the SSID, switch networks, type a
// password — not for the happy path.
constexpr uint64_t PORTAL_MAX_US = 10ULL * 60 * 1'000'000; // 10 min

// ---------------------------------------------------------------------------
// Cross-thread state. Process-lifetime, deliberately never torn down — same
// "leaked on purpose" shape as app_boot.cpp's s_net_lifetime (net_hil.cpp
// precedent): this is a one-shot boot-time flow with no owning object.
// ---------------------------------------------------------------------------

std::atomic<bool> s_dismiss_requested{false};
std::atomic<bool> s_join_succeeded{false};

enum class JoinState { PENDING, CONNECTED, FAILED };
std::atomic<JoinState> s_connect_state{JoinState::PENDING};
// Written once by the WiFiManager::connect() completion callback BEFORE
// s_connect_state flips off PENDING, read only by the /save handler AFTER it
// observes that flip — the atomic provides the happens-before edge, so this
// plain std::string has no concurrent-access race despite crossing threads.
std::string s_connect_error;

std::string s_ap_ssid;
esp_netif_t* s_ap_netif = nullptr;
httpd_handle_t s_httpd = nullptr;

// Written once by provisioning_run_portal() BEFORE start_httpd() is called,
// read-only for the rest of the portal session — see scan_networks_bounded().
// No mutex needed: the write happens-before httpd (and therefore any reader)
// exists.
std::vector<std::string> s_scan_results;

// Touched only on the LVGL/main thread (set when the modal is shown, cleared
// when it is hidden or deleted) — every access is inside a
// helix::ui::queue_update() lambda.
lv_obj_t* s_alert_dialog = nullptr;

// ---------------------------------------------------------------------------
// NVS read-only trigger check (R1)
// ---------------------------------------------------------------------------

bool read_stored_ssid_present() {
    nvs_handle_t h;
    esp_err_t rc = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return false; // namespace never created — never seeded, never user-set
    }
    if (rc != ESP_OK) {
        spdlog::warn("[provisioning] nvs_open('wifi', READONLY) failed: {} — treating as "
                     "no stored network",
                     esp_err_to_name(rc));
        return false;
    }
    size_t len = 0;
    esp_err_t ssid_rc = nvs_get_str(h, WIFI_NVS_KEY_SSID, nullptr, &len);
    nvs_close(h);
    return ssid_rc == ESP_OK;
}

std::string make_ap_ssid() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "HelixScreen-%02X%02X", mac[4], mac[5]);
    return buf;
}

// ---------------------------------------------------------------------------
// UI (R1) — a plain info modal via the existing Modal system. No new XML,
// subjects, or panel: the instructions are two lines of interpolated text
// (SSID + IP), which modal_show_alert()'s title/message args already cover
// (same shape as show_low_ram_resonance_warning's fmt::format(lv_tr(...))
// pattern) — translated static copy, interpolated dynamic values.
// ---------------------------------------------------------------------------

// Fires on ANY dismissal path (OK button, backdrop click, ESC) — the modal
// system routes backdrop-click/ESC straight to Modal::hide() without calling
// a static modal's on-click callback, so LV_EVENT_DELETE is the one hook that
// reliably covers every path. Idempotent: harmless if the OK-button callback
// already set the flag.
void on_modal_deleted(lv_event_t*) {
    s_dismiss_requested.store(true);
    s_alert_dialog = nullptr;
}

void on_dismiss_clicked(lv_event_t*) {
    s_dismiss_requested.store(true);
    if (s_alert_dialog) {
        Modal::hide(s_alert_dialog);
    }
}

void show_instructions_modal(std::string ssid, std::string ip) {
    helix::ui::queue_update("provisioning::show_modal", [ssid, ip]() {
        std::string title = lv_tr("Set Up WiFi");
        std::string message =
            fmt::format(lv_tr("On your phone or computer, join the WiFi network \"{}\", then "
                              "open http://{} in a browser to finish setup."),
                        ssid, ip);
        lv_obj_t* dialog =
            helix::ui::modal_show_alert(title.c_str(), message.c_str(), ModalSeverity::Info,
                                        lv_tr("Use Settings Instead"), on_dismiss_clicked, nullptr);
        if (dialog) {
            s_alert_dialog = dialog;
            lv_obj_add_event_cb(dialog, on_modal_deleted, LV_EVENT_DELETE, nullptr);
        }
    });
}

void hide_instructions_modal() {
    helix::ui::queue_update("provisioning::hide_modal", []() {
        if (s_alert_dialog) {
            Modal::hide(s_alert_dialog);
            s_alert_dialog = nullptr;
        }
    });
}

// ---------------------------------------------------------------------------
// Scan (R2) — one bounded, best-effort scan kicked ONCE before the portal
// opens (populates s_scan_results, read by every subsequent request — see
// its declaration above). WiFiManager::scan_once() only returns whatever's
// already cached (it triggers a scan and returns immediately, per
// src/api/wifi_manager.cpp) so a fresh SSID list needs the callback form with
// a short bounded wait — the same shape as app_net_thread_main's own boot
// wait, not a new pattern. Doing this once per portal session (not once per
// HTTP request) matters in practice: phones fire several captive-portal
// probes back to back (Android /generate_204, Apple /hotspot-detect.html,
// the user's own browser GET), and a fresh multi-second scan on each would
// make the form feel like it hangs. Best-effort: an empty result just means
// the portal's SSID field falls back to typed entry (still works — see
// render_form_page()'s <datalist>).
// ---------------------------------------------------------------------------

std::vector<std::string> scan_networks_bounded(std::shared_ptr<helix::WiFiManager> wifi) {
    constexpr int SCAN_WAIT_MS = 3000;
    constexpr int SCAN_POLL_MS = 200;
    constexpr int STOP_SCAN_WAIT_MS = 3000;

    auto results = std::make_shared<std::vector<std::string>>();
    auto done = std::make_shared<std::atomic<bool>>(false);

    // start_scan()/stop_scan() create and delete an lv_timer and are therefore
    // LVGL-thread-only; we are on app_net_thread_main. Hop both.
    helix::ui::queue_update("provisioning::start_scan", [wifi, results, done]() {
        wifi->start_scan([results, done](const std::vector<WiFiNetwork>& networks) {
            results->clear();
            for (const auto& n : networks) {
                results->push_back(n.ssid);
            }
            done->store(true);
        });
    });

    for (int waited = 0; waited < SCAN_WAIT_MS && !done->load(); waited += SCAN_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(SCAN_POLL_MS));
    }

    // The timeout path can expire while a scan-complete dispatch is still queued,
    // so `done` alone does not make *results safe to read. stop_scan() clears
    // scan_callback_ under callback_mutex_, and WiFiManager's queued
    // scan-complete lambda re-reads that callback before touching anything
    // (wifi_manager.cpp handle_scan_complete) — both on the LVGL thread, so once
    // stop_scan() has returned there, no later dispatch can write *results.
    // Waiting for that specific point is what makes the read below safe.
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    helix::ui::queue_update("provisioning::stop_scan", [wifi, stopped]() {
        wifi->stop_scan();
        stopped->store(true);
    });

    for (int waited = 0; waited < STOP_SCAN_WAIT_MS && !stopped->load(); waited += SCAN_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(SCAN_POLL_MS));
    }
    if (!stopped->load()) {
        // The UI thread never drained the stop. *results may still be written,
        // so return nothing rather than read it — the shared_ptr keeps the
        // vector alive for whatever is still holding it. The portal's SSID
        // field falls back to typed entry.
        ESP_LOGW(TAG, "scan: stop_scan did not run within %d ms — dropping results",
                 STOP_SCAN_WAIT_MS);
        return {};
    }
    return *results;
}

// ---------------------------------------------------------------------------
// Portal page rendering — self-contained HTML, inline CSS, no external
// assets, placeholder text only (R3: no real values baked into the page
// source; host/port pre-fill below is the RUNTIME current config value, not
// a literal in the HTML template).
// ---------------------------------------------------------------------------

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string render_form_page(const std::string& ap_ssid, const std::vector<std::string>& scanned,
                             const std::string& error_msg, const std::string& cur_host,
                             int cur_port) {
    std::string datalist;
    for (const auto& s : scanned) {
        datalist += "<option value=\"" + html_escape(s) + "\">";
    }
    std::string error_html;
    if (!error_msg.empty()) {
        error_html = "<p class=\"err\">" + html_escape(error_msg) + "</p>";
    }

    std::ostringstream html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>HelixScreen Setup</title><style>"
            "body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px;"
            "color:#222}h1{font-size:1.3em}label{display:block;margin-top:14px;"
            "font-weight:bold}input{width:100%;box-sizing:border-box;padding:8px;"
            "font-size:1em;margin-top:4px}button{margin-top:20px;width:100%;padding:12px;"
            "font-size:1.05em;background:#2b7a3f;color:#fff;border:0;border-radius:4px}"
            ".err{color:#b00020;font-weight:bold}.hint{color:#666;font-size:0.85em}"
            "</style></head><body>"
         << "<h1>Set up " << html_escape(ap_ssid) << "</h1>" << error_html
         << "<form method=\"POST\" action=\"/save\">"
            "<label for=\"ssid\">WiFi network</label>"
            "<input list=\"ssids\" id=\"ssid\" name=\"ssid\" required autocomplete=\"off\">"
            "<datalist id=\"ssids\">"
         << datalist
         << "</datalist>"
            "<label for=\"password\">WiFi password</label>"
            "<input type=\"password\" id=\"password\" name=\"password\" autocomplete=\"off\">"
            "<label for=\"host\">Moonraker host (optional)</label>"
            "<input id=\"host\" name=\"host\" value=\""
         << html_escape(cur_host)
         << "\" placeholder=\"e.g. 192.168.1.50\">"
            "<label for=\"port\">Moonraker port</label>"
            "<input id=\"port\" name=\"port\" value=\""
         << cur_port
         << "\">"
            "<p class=\"hint\">Leave host blank to keep the current setting.</p>"
            "<button type=\"submit\">Connect</button>"
            "</form></body></html>";
    return html.str();
}

constexpr const char SUCCESS_PAGE[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>Connected</title></head>"
    "<body style=\"font-family:sans-serif;text-align:center;margin-top:80px\">"
    "<h1>Connected!</h1><p>HelixScreen is joining your network. You can close this page.</p>"
    "</body></html>";

std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += (in[i] == '+') ? ' ' : in[i];
    }
    return out;
}

bool extract_field(const std::string& body, const char* key, std::string& out) {
    char buf[192];
    if (httpd_query_key_value(body.c_str(), key, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    out = url_decode(buf);
    return true;
}

std::string current_moonraker_host() {
    helix::Config* config = helix::Config::get_instance();
    return config->get<std::string>(config->df() + "moonraker_host", "");
}

int current_moonraker_port() {
    helix::Config* config = helix::Config::get_instance();
    return config->get<int>(config->df() + "moonraker_port", 7125);
}

// ---------------------------------------------------------------------------
// httpd handlers (BG thread — esp_http_server's own worker task)
// ---------------------------------------------------------------------------

esp_err_t root_get_handler(httpd_req_t* req) {
    // Also the catch-all target for OS captive-portal probes
    // (/generate_204, /hotspot-detect.html, /ncsi.txt, ...) registered below
    // via the "/*" wildcard — every unmatched GET opens straight to the form.
    std::string page = render_form_page(s_ap_ssid, s_scan_results, "", current_moonraker_host(),
                                        current_moonraker_port());
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page.c_str(), static_cast<ssize_t>(page.size()));
    return ESP_OK;
}

esp_err_t save_post_handler(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_OK;
    }
    std::vector<char> buf(req->content_len + 1, 0);
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf.data() + received, req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
            return ESP_OK;
        }
        received += static_cast<size_t>(r);
    }
    std::string body(buf.data(), received);

    std::string ssid, password, host, port_str;
    extract_field(body, "ssid", ssid);
    extract_field(body, "password", password); // NEVER logged (R3)
    extract_field(body, "host", host);
    extract_field(body, "port", port_str);

    auto respond_with_error = [&](const std::string& err, const std::string& host_prefill) {
        std::string page = render_form_page(s_ap_ssid, s_scan_results, err, host_prefill,
                                            current_moonraker_port());
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, page.c_str(), static_cast<ssize_t>(page.size()));
    };

    if (ssid.empty()) {
        respond_with_error("Network name is required.", current_moonraker_host());
        return ESP_OK;
    }
    if (!host.empty() && !is_valid_ip_or_hostname(host)) {
        respond_with_error("Moonraker host is not a valid IP address or hostname.", host);
        return ESP_OK;
    }
    if (!port_str.empty() && !is_valid_port(port_str)) {
        respond_with_error("Moonraker port must be a number from 1-65535.", host);
        return ESP_OK;
    }

    if (!host.empty()) {
        helix::Config* config = helix::Config::get_instance();
        config->set(config->df() + "moonraker_host", host);
        if (!port_str.empty()) {
            config->set(config->df() + "moonraker_port", std::stoi(port_str));
        }
        config->save();
    }

    spdlog::info("[provisioning] portal: attempting join to '{}' (password {})",
                 helix::redact::ssid(ssid), password.empty() ? "none" : "provided");

    s_connect_state.store(JoinState::PENDING);
    auto wifi = helix::get_wifi_manager();
    // We are on an esp_http_server task; connect() deletes the auth-fail grace
    // lv_timer (wifi_manager.cpp cancel_auth_fail_grace), so it has to run on
    // the LVGL thread. The poll below is unchanged — s_connect_state is already
    // PENDING, so a not-yet-drained hop just reads as "still connecting".
    helix::ui::queue_update("provisioning::connect", [wifi, ssid, password]() {
        wifi->connect(ssid, password, [](bool success, const std::string& error) {
            s_connect_error = error;
            s_connect_state.store(success ? JoinState::CONNECTED : JoinState::FAILED);
        });
    });

    int waited = 0;
    while (waited < SAVE_JOIN_TIMEOUT_MS && s_connect_state.load() == JoinState::PENDING) {
        vTaskDelay(pdMS_TO_TICKS(SAVE_POLL_MS));
        waited += SAVE_POLL_MS;
    }

    JoinState result = s_connect_state.load();
    if (result == JoinState::CONNECTED) {
        s_join_succeeded.store(true); // unblocks provisioning_run_portal()'s poll loop -> teardown
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, SUCCESS_PAGE, HTTPD_RESP_USE_STRLEN);
    } else {
        std::string reason;
        if (result == JoinState::FAILED) {
            reason = s_connect_error;
            // Self-healing rollback (review item 2): connect_network() already
            // persisted these creds to NVS before the assoc result was known
            // (Task 13's design — the NVS write is unconditional, the join
            // attempt follows). A DEFINITIVE failure (wrong password, etc.)
            // here means bad creds are now sitting in NVS; left alone, the
            // next reboot sees a stored SSID and never re-triggers the portal,
            // silently retrying a known-bad password forever in the
            // background. The "wifi" namespace was empty before this portal
            // session wrote it, so erasing it is rolling back this session's
            // own unverified write, not destroying prior user data. A bare
            // timeout (PENDING, not FAILED) is NOT rolled back — the backend
            // may still resolve it on a later background retry, which the
            // outer teardown loop's wifi->is_connected() check already
            // catches correctly.
            helix::wifi_backend_esp_clear_stored_credentials();
        } else {
            reason = "Timed out waiting for a connection.";
        }
        respond_with_error("Could not join \"" + ssid + "\": " + reason,
                           host.empty() ? current_moonraker_host() : host);
    }
    return ESP_OK;
}

bool start_httpd() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    // Default (4096) is sized for C-string handlers; render_form_page() below
    // builds several std::string/ostringstream temporaries per request, so
    // give the httpd task's own stack more headroom (transient — this task
    // exists only while the portal is up).
    config.stack_size = 8192;
    esp_err_t rc = httpd_start(&s_httpd, &config);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(rc));
        return false;
    }
    httpd_uri_t save_uri{};
    save_uri.uri = "/save";
    save_uri.method = HTTP_POST;
    save_uri.handler = save_post_handler;
    httpd_uri_t root_uri{};
    root_uri.uri = "/*";
    root_uri.method = HTTP_GET;
    root_uri.handler = root_get_handler;
    httpd_register_uri_handler(s_httpd, &save_uri);
    httpd_register_uri_handler(s_httpd, &root_uri);
    return true;
}

void stop_httpd() {
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }
}

// ---------------------------------------------------------------------------
// DNS catch-all (R2) — answers every A-record query with the AP's own IP so
// any domain the phone probes lands on the portal. Trimmed single-rule
// version of ESP-IDF's captive_portal example (examples/protocols/http_server/
// captive_portal/components/dns_server, Unlicense/CC0): no rule table, no
// separate task — runs as a bounded recv-timeout poll on the caller's own
// thread so one loop (below) can watch both DNS traffic and the dismiss/join
// flags without a second thread.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct DnsHeader {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
};
struct DnsAnswer {
    uint16_t name_ptr;
    uint16_t type;
    uint16_t klass;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
};
#pragma pack(pop)

constexpr uint16_t DNS_OPCODE_MASK = 0x7800;
constexpr uint16_t DNS_QR_FLAG = 1 << 7;
constexpr uint16_t DNS_TYPE_A = 0x0001;
constexpr uint32_t DNS_ANSWER_TTL_SEC = 300;

int open_dns_socket() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns: socket() failed: errno %d", errno);
        return -1;
    }
    struct timeval tv {};
    tv.tv_sec = 0;
    tv.tv_usec = DNS_RECV_TIMEOUT_MS * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DNS_PORT);
    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns: bind() failed: errno %d", errno);
        close(sock);
        return -1;
    }
    return sock;
}

// One recv-with-timeout cycle: replies if a query arrived, returns immediately
// (timeout) otherwise so the caller's loop can re-check exit flags.
void dns_pump_once(int sock, uint32_t ap_ip_be) {
    char rx[256];
    // IPv4 only (the socket is AF_INET/SOCK_DGRAM) — this build's lwIP config
    // doesn't compile in IPv6 support, so sockaddr_in6 is an incomplete type
    // here; sockaddr_in is the correct (and sufficient) peer-address type.
    struct sockaddr_in from {};
    socklen_t fromlen = sizeof(from);
    int len =
        recvfrom(sock, rx, sizeof(rx) - 1, 0, reinterpret_cast<struct sockaddr*>(&from), &fromlen);
    if (len <= static_cast<int>(sizeof(DnsHeader))) {
        return; // timeout, error, or too short to be a real query
    }

    auto* header = reinterpret_cast<DnsHeader*>(rx);
    if ((header->flags & htons(DNS_OPCODE_MASK)) != 0) {
        return; // not a standard query — ignore
    }

    uint16_t qd_count = ntohs(header->qd_count);
    if (qd_count == 0) {
        return;
    }

    // Skip past the (possibly multi-label) question name to find its end —
    // we don't need the decoded name since every query gets the same answer.
    char* q = rx + sizeof(DnsHeader);
    char* end = rx + len;
    while (q < end && *q != 0) {
        q += static_cast<unsigned char>(*q) + 1;
    }
    if (q >= end) {
        return; // malformed
    }
    q += 1 + 4; // skip the terminating zero-length label + QTYPE/QCLASS

    char reply[256];
    if (len > static_cast<int>(sizeof(reply)) ||
        static_cast<size_t>(len) + sizeof(DnsAnswer) > sizeof(reply)) {
        return; // question section too large for our fixed reply buffer — drop
    }
    std::memcpy(reply, rx, len);
    header = reinterpret_cast<DnsHeader*>(reply);
    header->flags |= htons(DNS_QR_FLAG);
    header->an_count = htons(1);
    header->ns_count = 0;
    header->ar_count = 0;

    auto* answer = reinterpret_cast<DnsAnswer*>(reply + len);
    answer->name_ptr = htons(static_cast<uint16_t>(0xC000 | sizeof(DnsHeader)));
    answer->type = htons(DNS_TYPE_A);
    answer->klass = htons(1); // IN
    answer->ttl = htonl(DNS_ANSWER_TTL_SEC);
    answer->addr_len = htons(static_cast<uint16_t>(sizeof(uint32_t)));
    answer->ip_addr = ap_ip_be;

    int reply_len = len + static_cast<int>(sizeof(DnsAnswer));
    sendto(sock, reply, reply_len, 0, reinterpret_cast<struct sockaddr*>(&from), fromlen);
}

} // namespace

namespace helix {

bool provisioning_needed() {
    return !read_stored_ssid_present();
}

bool provisioning_run_portal() {
    s_dismiss_requested.store(false);
    s_join_succeeded.store(false);
    s_ap_ssid = make_ap_ssid();

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    esp_err_t mode_rc = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(mode_rc));
        return false;
    }

    wifi_config_t ap_config = {};
    size_t n = std::min(s_ap_ssid.size(), sizeof(ap_config.ap.ssid));
    std::memcpy(ap_config.ap.ssid, s_ap_ssid.data(), n);
    ap_config.ap.ssid_len = static_cast<uint8_t>(n);
    ap_config.ap.channel = AP_CHANNEL;
    ap_config.ap.max_connection = AP_MAX_CONNECTIONS;
    ap_config.ap.authmode = WIFI_AUTH_OPEN; // R3: terminal design doc is silent on AP security;
                                            // brief's default (OPEN) applies
    esp_err_t cfg_rc = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (cfg_rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(cfg_rc));
        esp_wifi_set_mode(WIFI_MODE_STA); // don't leave an unconfigured AP up with no portal
        return false;
    }
    // Best-effort: wifi is very likely already started (STA, from the caller's
    // retry_async() immediately before this). If it genuinely isn't, this
    // brings it up now that the AP config above is in place; if it's already
    // running, esp_wifi_start() just re-applies config and returns ESP_OK.
    esp_err_t start_rc = esp_wifi_start();
    if (start_rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_start (AP add) returned %s — continuing",
                 esp_err_to_name(start_rc));
    }

    esp_netif_ip_info_t ip_info{};
    esp_netif_get_ip_info(s_ap_netif, &ip_info);
    char ip_str[16] = {};
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, static_cast<int>(sizeof(ip_str)));

    ESP_LOGI(TAG, "portal: SoftAP '%s' up, portal at http://%s", s_ap_ssid.c_str(), ip_str);
    show_instructions_modal(s_ap_ssid, ip_str);

    auto wifi = helix::get_wifi_manager();
    s_scan_results = scan_networks_bounded(wifi);

    bool httpd_up = start_httpd();
    int dns_sock = open_dns_socket();
    if (!httpd_up || dns_sock < 0) {
        ESP_LOGE(TAG, "portal: httpd or DNS failed to start — tearing down");
        if (httpd_up) {
            stop_httpd();
        }
        if (dns_sock >= 0) {
            close(dns_sock);
        }
        hide_instructions_modal();
        esp_wifi_set_mode(WIFI_MODE_STA);
        return false;
    }

    // Authoritative on ACTUAL connection state, not just the /save handler's
    // own in-window flag (review MEDIUM-1): the handler's join poll can give
    // up and render a "timed out" page while the backend keeps associating in
    // the background (its own bounded-backoff retry, wifi_backend_esp.cpp) —
    // a slow-but-real join then lands after the handler already returned,
    // with nothing to unblock this loop. Checking wifi->is_connected()
    // directly closes that window regardless of handler timing, and also
    // covers a concurrent Settings > Network join finishing first.
    //
    // The deadline is real elapsed time rather than a per-iteration accumulator
    // because dns_pump_once() returns as soon as a query arrives, well short of
    // its DNS_RECV_TIMEOUT_MS ceiling. Counting iterations would therefore run the
    // clock fast precisely while a client is talking to the portal — the case
    // that most needs the full window.
    const uint64_t portal_deadline_us = static_cast<uint64_t>(esp_timer_get_time()) + PORTAL_MAX_US;
    bool portal_expired = false;
    while (!s_dismiss_requested.load() && !s_join_succeeded.load() && !wifi->is_connected()) {
        if (static_cast<uint64_t>(esp_timer_get_time()) >= portal_deadline_us) {
            portal_expired = true;
            break;
        }
        dns_pump_once(dns_sock, ip_info.ip.addr);
    }
    if (portal_expired) {
        ESP_LOGI(TAG, "portal: no one provisioned within %llu s — closing",
                 (unsigned long long)(PORTAL_MAX_US / 1'000'000));
    }

    close(dns_sock);
    stop_httpd();
    // Drop back to STA-only. STA's own config/connect state (owned by
    // WifiBackendEsp) is untouched — mode is the only thing provisioning
    // changes on top of it.
    esp_wifi_set_mode(WIFI_MODE_STA);

    bool joined = s_join_succeeded.load() || wifi->is_connected();
    if (joined || portal_expired) {
        // Dismiss path already hid it via on_modal_deleted; the expiry path has
        // nobody to hide it, and leaving it up would outlive the AP it points at.
        hide_instructions_modal();
    }
    ESP_LOGI(TAG, "portal: closed (%s)", joined ? "joined" : "dismissed");
    return joined;
}

} // namespace helix

#endif // !CONFIG_HELIX_MOCK_PRINTER
