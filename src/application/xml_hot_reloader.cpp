// SPDX-License-Identifier: GPL-3.0-or-later

#include "xml_hot_reloader.h"

#include "ui_update_queue.h"

#include "layout_manager.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <lvgl.h>
#include <sstream>

extern "C" {
#include "helix-xml/src/libs/expat/expat.h"
#include "helix-xml/src/xml/lv_xml_component_private.h"
}

namespace fs = std::filesystem;

namespace {

/// Components whose registered scope is extended from C++ after startup, so
/// re-registering them from XML alone cannot reproduce the live scope.
///
/// - globals: not a UI component at all. It owns every XML subject in the app
///   (~1000 of them, most backed by C++-owned storage) plus the runtime theme
///   constants ThemeManager injects. Those constants are pushed in from C++
///   after registration, so a fresh registration from globals.xml resolves every
///   theme token to nothing across the entire UI. (Scope teardown itself is now
///   safe — `lv_xml_component_unregister` skips borrowed subjects instead of
///   free()ing C++ storage — but the constants are not recoverable.)
/// - color_picker / color_swatch_grid: register_xml_components() pushes
///   breakpoint-computed constants into their scopes after registration. A
///   fresh registration would resolve those tokens to nothing.
constexpr const char* NON_RELOADABLE_COMPONENTS[] = {"globals", "color_picker",
                                                     "color_swatch_grid"};

bool is_non_reloadable(const std::string& component) {
    for (const auto* name : NON_RELOADABLE_COMPONENTS) {
        if (component == name)
            return true;
    }
    return false;
}

/// Read a file into `out`. Returns false on open/read failure (e.g. file is
/// mid-rename, briefly empty, or permission-denied). Sets `err_out` for logging.
bool read_file_contents(const std::string& abs_path, std::string& out, std::string& err_out) {
    std::ifstream f(abs_path, std::ios::binary);
    if (!f) {
        err_out = "open failed";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f && !f.eof()) {
        err_out = "read error mid-file";
        return false;
    }
    out = ss.str();
    if (out.empty()) {
        // Empty file = editor is mid-write (truncate-then-fill) or user really
        // emptied it. Either way, don't try to parse — defer to next poll.
        err_out = "empty (mid-write?)";
        return false;
    }
    return true;
}

/// Check XML well-formedness with expat. No LVGL state touched, safe to call
/// from the polling thread. Returns true if `xml_def` parses cleanly. Sets
/// `err_out` with expat's error string on failure.
bool xml_is_well_formed(const std::string& xml_def, std::string& err_out) {
    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser) {
        err_out = "expat allocation failed";
        return false;
    }
    enum XML_Status status =
        XML_Parse(parser, xml_def.data(), static_cast<int>(xml_def.size()), XML_TRUE);
    bool ok = (status == XML_STATUS_OK);
    if (!ok) {
        err_out = XML_ErrorString(XML_GetErrorCode(parser));
    }
    XML_ParserFree(parser);
    return ok;
}

/// Count a scope's subjects split by provenance: `owned` ones were allocated by
/// the XML parser for a <subject>/<subject_expr> element and die with the scope;
/// `borrowed` ones point at C++-owned storage and outlive it.
void count_scope_subjects(const char* component_name, size_t& owned_out, size_t& borrowed_out) {
    owned_out = 0;
    borrowed_out = 0;
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(component_name);
    if (!scope)
        return;
    // LV_LL_READ is a C macro that relies on implicit void* conversion — expand it manually
    // for C++ so we can cast explicitly.
    for (void* node = lv_ll_get_head(&scope->subjects_ll); node != nullptr;
         node = lv_ll_get_next(&scope->subjects_ll, node)) {
        if (static_cast<lv_xml_subject_t*>(node)->owned)
            ++owned_out;
        else
            ++borrowed_out;
    }
}

} // anonymous namespace

namespace helix {

std::vector<BorrowedSubject> snapshot_borrowed_subjects(const char* component_name) {
    std::vector<BorrowedSubject> borrowed;
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(component_name);
    if (!scope)
        return borrowed;
    for (void* node = lv_ll_get_head(&scope->subjects_ll); node != nullptr;
         node = lv_ll_get_next(&scope->subjects_ll, node)) {
        auto* rec = static_cast<lv_xml_subject_t*>(node);
        if (rec->owned || rec->name == nullptr || rec->subject == nullptr)
            continue;
        borrowed.emplace_back(rec->name, rec->subject);
    }
    return borrowed;
}

size_t restore_borrowed_subjects(const char* component_name,
                                 const std::vector<BorrowedSubject>& borrowed) {
    if (borrowed.empty())
        return 0;
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(component_name);
    if (!scope) {
        spdlog::warn("[HotReload] '{}' has no scope — {} C++-registered subject(s) lost",
                     component_name, borrowed.size());
        return 0;
    }
    size_t restored = 0;
    for (const auto& [name, subject] : borrowed) {
        // Re-registers as borrowed (the public entry point never claims
        // ownership), so the next reload can snapshot it again.
        if (lv_xml_register_subject(scope, name.c_str(), subject) == LV_RESULT_OK)
            ++restored;
        else
            spdlog::warn("[HotReload] '{}': failed to re-register subject '{}'", component_name,
                         name);
    }
    return restored;
}

XmlHotReloader::~XmlHotReloader() {
    stop();
}

void XmlHotReloader::start(const std::vector<std::string>& xml_dirs, int poll_interval_ms) {
    if (running_.load()) {
        return;
    }

    poll_interval_ms_ = poll_interval_ms;
    initial_scan(xml_dirs);

    spdlog::info("[HotReload] Watching {} XML files across {} directories (poll every {}ms)",
                 file_mtimes_.size(), xml_dirs.size(), poll_interval_ms_);

    running_.store(true);
    poll_thread_ = std::thread(&XmlHotReloader::poll_loop, this);
}

void XmlHotReloader::stop() {
    if (!running_.load()) {
        return;
    }
    {
        // Under the mutex so the poll thread cannot miss the wakeup between
        // testing the flag and parking on the condvar.
        std::lock_guard<std::mutex> lock(stop_mutex_);
        running_.store(false);
    }
    stop_cv_.notify_all();
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
    spdlog::debug("[HotReload] Stopped");
}

void XmlHotReloader::initial_scan(const std::vector<std::string>& xml_dirs) {
    file_mtimes_.clear();
    file_to_lvgl_path_.clear();
    file_to_logical_path_.clear();
    file_to_variant_.clear();

    static constexpr const char* SKIP_DIRS[] = {"translations", ".claude-recall"};
    auto is_skipped = [](const fs::path& p) {
        auto name = p.filename().string();
        for (const auto* skip : SKIP_DIRS) {
            if (name == skip)
                return true;
        }
        return false;
    };

    std::unordered_map<std::string, size_t> per_dir_counts;

    for (const auto& dir : xml_dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            spdlog::warn("[HotReload] Directory not found: {}", dir);
            continue;
        }

        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (it != end) {
            const auto& entry = *it;
            std::error_code dir_ec;
            if (entry.is_directory(dir_ec) && is_skipped(entry.path())) {
                it.disable_recursion_pending();
                std::error_code inc_ec;
                it.increment(inc_ec);
                if (inc_ec) {
                    spdlog::warn("[HotReload] Iterator error in {}: {}", dir, inc_ec.message());
                    break;
                }
                continue;
            }
            std::error_code file_ec;
            if (entry.is_regular_file(file_ec) && entry.path().extension() == ".xml") {
                std::error_code mtime_ec;
                auto mtime = fs::last_write_time(entry.path(), mtime_ec);
                if (mtime_ec) {
                    spdlog::warn("[HotReload] Could not stat {}: {}", entry.path().string(),
                                 mtime_ec.message());
                    std::error_code inc_ec;
                    it.increment(inc_ec);
                    if (inc_ec) {
                        spdlog::warn("[HotReload] Iterator error in {}: {}", dir, inc_ec.message());
                        break;
                    }
                    continue;
                }

                auto abs_path = fs::absolute(entry.path()).string();
                file_mtimes_[abs_path] = mtime;

                auto rel_path = entry.path().string();
                file_to_lvgl_path_[abs_path] = "A:" + rel_path;

                // Split the path-under-the-watch-root into the layout variant
                // it belongs to and the filename LayoutManager resolves with,
                // so scan_and_reload() can tell whether this copy is the one
                // the running layout is actually using.
                std::string variant;
                fs::path logical = fs::relative(entry.path(), dir, ec);
                if (ec) {
                    logical = entry.path().filename();
                    ec.clear();
                }
                auto first = logical.begin();
                if (first != logical.end() &&
                    helix::LayoutManager::is_variant_dir(first->string())) {
                    variant = first->string();
                    fs::path stripped;
                    for (auto it = ++logical.begin(); it != logical.end(); ++it) {
                        stripped /= *it;
                    }
                    logical = stripped;
                }
                file_to_logical_path_[abs_path] = logical.generic_string();
                file_to_variant_[abs_path] = variant;

                auto parent = entry.path().parent_path().filename().string();
                per_dir_counts[parent.empty() ? dir : parent]++;

                spdlog::trace("[HotReload] Tracking: {} ({})", rel_path,
                              component_name_from_path(entry.path()));
            }
            std::error_code inc_ec;
            it.increment(inc_ec);
            if (inc_ec) {
                spdlog::warn("[HotReload] Iterator error in {}: {}", dir, inc_ec.message());
                break;
            }
        }
    }

    std::string breakdown;
    for (const auto& [bucket, n] : per_dir_counts) {
        if (!breakdown.empty())
            breakdown += ", ";
        breakdown += bucket + ": " + std::to_string(n);
    }
    spdlog::info("[HotReload] Scan complete ({} files) [{}]", file_mtimes_.size(), breakdown);
}

void XmlHotReloader::poll_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(stop_mutex_);
            // Returns the predicate's value: true means stop() asked us to
            // quit, false means the interval simply elapsed and it is time to
            // scan. Scanning happens outside the lock -- it is the slow half.
            if (stop_cv_.wait_for(lock, std::chrono::milliseconds(poll_interval_ms_),
                                  [this] { return !running_.load(); })) {
                break;
            }
        }
        scan_and_reload();
    }
}

void XmlHotReloader::scan_and_reload() {
    for (auto& [abs_path, cached_mtime] : file_mtimes_) {
        std::error_code ec;
        auto current_mtime = fs::last_write_time(abs_path, ec);
        if (ec) {
            // File may have been deleted — skip silently
            continue;
        }

        if (current_mtime == cached_mtime) {
            continue;
        }

        auto comp_name = component_name_from_path(fs::path(abs_path));
        auto lvgl_path = file_to_lvgl_path_[abs_path];

        if (is_non_reloadable(comp_name)) {
            cached_mtime = current_mtime;
            spdlog::warn("[HotReload] '{}' cannot be hot-reloaded (its scope is extended from "
                         "C++ after registration) — restart to pick up the change",
                         comp_name);
            continue;
        }

        // A component that has a breakpoint override exists in several copies
        // under ui_xml/ (base, micro/, portrait/, …) that all register under
        // the same name. Registering the copy that just changed would replace
        // the live component with a layout the running display isn't using, so
        // only the copy the active layout resolves to is allowed to reload.
        // Editing a shadowed copy still takes effect on the next launch at that
        // breakpoint. Committing the mtime keeps it from re-checking each poll.
        const auto& variant = file_to_variant_[abs_path];
        if (helix::LayoutManager::instance().active_variant_dir(file_to_logical_path_[abs_path]) !=
            variant) {
            cached_mtime = current_mtime;
            spdlog::debug("[HotReload] '{}' changed in {} layout, but the active layout uses a "
                          "different copy — not reloading",
                          comp_name, variant.empty() ? "base" : variant);
            continue;
        }

        // PRE-FLIGHT (parse-then-swap): read the file and validate XML
        // well-formedness BEFORE touching the registered component. If the file
        // is mid-write (truncated, briefly empty during atomic rename) or
        // contains a syntax error (user saved while typing), the existing
        // component stays registered and live widgets keep working. We also
        // don't update cached_mtime, so the next poll tick retries
        // automatically — recovering from transient mid-write states without
        // user intervention.
        std::string xml_buf;
        std::string err_msg;
        if (!read_file_contents(abs_path, xml_buf, err_msg) ||
            !xml_is_well_formed(xml_buf, err_msg)) {
            spdlog::warn("[HotReload] '{}' not reloadable yet ({}); deferring to next poll",
                         comp_name, err_msg);
            continue;
        }

        // File is stable + valid XML. Commit the mtime cache + fire the reload.
        cached_mtime = current_mtime;
        spdlog::info("[HotReload] Detected change: {} ({})", comp_name, abs_path);

        if (reload_callback_) {
            // Test mode — invoke callback directly instead of LVGL operations
            reload_callback_(comp_name, lvgl_path);
            if (after_reload_callback_)
                after_reload_callback_(comp_name);
        } else {
            // Marshal the reload to the LVGL main thread. We pass the
            // pre-validated buffer (not the file path) so the main thread
            // doesn't re-read mid-write content, and so register_from_data
            // cannot fail on a parse error we already ruled out.
            auto reload_name = comp_name;
            auto reload_buf = std::move(xml_buf);
            auto after_cb = after_reload_callback_;
            helix::ui::queue_update([reload_name, reload_buf = std::move(reload_buf), after_cb]() {
                auto start = std::chrono::steady_clock::now();

                size_t owned_subjects = 0;
                size_t borrowed_subjects = 0;
                count_scope_subjects(reload_name.c_str(), owned_subjects, borrowed_subjects);
                if (owned_subjects > 0 || borrowed_subjects > 0) {
                    spdlog::debug("[HotReload] '{}' scope holds {} XML-declared subject(s) "
                                  "(freed and re-parsed; observers detached, so live widgets "
                                  "bound to them go inert until rebuilt below) and {} "
                                  "C++-registered subject(s) (kept alive by C++ and carried "
                                  "across into the new scope)",
                                  reload_name, owned_subjects, borrowed_subjects);
                }

                // Subjects the scope only borrows are registered from C++ once at
                // startup. Unregister destroys the scope and with it those
                // registrations, and re-parsing the XML cannot recreate them — so
                // snapshot them here and put them back below, or the component
                // comes back live but with every bind_* naming one resolving to
                // nothing.
                auto borrowed = snapshot_borrowed_subjects(reload_name.c_str());

                // Unregister old component definition
                auto result = lv_xml_component_unregister(reload_name.c_str());
                if (result != LV_RESULT_OK) {
                    spdlog::warn("[HotReload] Failed to unregister '{}' — registering fresh",
                                 reload_name);
                }

                // Re-register from the pre-validated buffer (NOT from file —
                // file could change again between pre-flight and now).
                result =
                    lv_xml_register_component_from_data(reload_name.c_str(), reload_buf.c_str());
                if (result != LV_RESULT_OK) {
                    // Shouldn't happen — pre-flight already parsed this exact
                    // buffer successfully. If we ever get here, the component
                    // is unregistered and live widgets are inert; log loudly.
                    spdlog::error("[HotReload] Post-pre-flight parse failure for '{}' — "
                                  "component is now unregistered; fix and save again",
                                  reload_name);
                    return;
                }

                size_t restored = restore_borrowed_subjects(reload_name.c_str(), borrowed);
                if (restored > 0) {
                    spdlog::debug("[HotReload] '{}': carried {} C++-registered subject(s) into "
                                  "the new scope",
                                  reload_name, restored);
                }

                auto elapsed = std::chrono::steady_clock::now() - start;
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
                spdlog::info("[HotReload] Reloaded: {} ({:.1f}ms)", reload_name, us / 1000.0);

                if (after_cb)
                    after_cb(reload_name);
            });
        }
    }
}

std::string XmlHotReloader::component_name_from_path(const fs::path& path) {
    // "home_panel.xml" -> "home_panel"
    return path.stem().string();
}

} // namespace helix
