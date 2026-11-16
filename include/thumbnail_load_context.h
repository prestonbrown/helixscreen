// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

/**
 * @file thumbnail_load_context.h
 * @brief Async safety context for thumbnail loading operations
 *
 * ThumbnailLoadContext encapsulates the common pattern used across panels
 * when loading thumbnails asynchronously:
 * 1. A lifetime check to detect if the caller was destroyed
 * 2. A generation counter to detect stale callbacks
 *
 * Supports both the legacy shared_ptr<atomic<bool>> alive pattern and the
 * newer AsyncLifetimeGuard token pattern. When both are set, both must
 * report alive/valid for is_valid() to return true.
 *
 * ## What is_valid() is, and is not
 *
 * It is a STALENESS check (has a newer request superseded this one?) that also
 * happens to consult the lifetime token. It is **not** a thread guard: it calls
 * `lifetime_token->expired()` internally, so `if (!ctx.is_valid()) return;`
 * followed by a `this`/`self` dereference is the same check-then-use race as a
 * bare `tok.expired()` — L081 Mechanism C.
 *
 * `scripts/check_l081_anti_pattern.py` flags that shape
 * (prestonbrown/helixscreen#960).
 *
 * ThumbnailCache marshals its callbacks to the main thread at the fetch
 * boundary, so within a fetch callback the race is closed by that — but do not
 * rely on it silently. Keep member access inside `tok.defer(...)`, or annotate
 * the site `// L081_OK: <why>` so the guarantee is written down where the next
 * reader will look.
 *
 * ## Usage Example (AsyncLifetimeGuard — preferred)
 * ```cpp
 * // In your panel class (OverlayBase provides lifetime_):
 * void load_thumbnail() {
 *     auto ctx = ThumbnailLoadContext::create(lifetime_);
 *     auto tok = lifetime_.token();
 *
 *     ThumbnailRequest req;
 *     req.key = path;
 *     req.target = helix::ThumbnailProcessor::get_target_for_display(
 *         helix::ThumbnailSize::Detail);
 *     req.api = api_;
 *
 *     get_thumbnail_cache().fetch(
 *         req, ctx,
 *         [this, tok](const std::string& lvgl_path, bool degraded) {
 *             // Marshal, then touch members — never check-then-deref.
 *             tok.defer("MyPanel::apply_thumbnail", [this, lvgl_path]() {
 *                 lv_image_set_src(thumb_, lvgl_path.c_str());
 *             });
 *         });
 * }
 * ```
 *
 * ## Usage Example (legacy alive flag)
 * ```cpp
 * auto ctx = ThumbnailLoadContext::create(m_alive, &thumbnail_gen_);
 * ```
 *
 * @see ThumbnailCache::fetch(const ThumbnailRequest&, ThumbnailLoadContext, ...)
 */
struct ThumbnailLoadContext {
    /// Shared flag indicating if the owner object is still alive (legacy pattern)
    std::shared_ptr<std::atomic<bool>> alive;

    /// Lifetime token from AsyncLifetimeGuard (preferred pattern)
    std::optional<helix::LifetimeToken> lifetime_token;

    /// Pointer to the owner's generation counter (may be nullptr if not used)
    std::atomic<uint32_t>* generation = nullptr;

    /// The generation value captured at creation time
    uint32_t captured_gen = 0;

    /**
     * @brief Check if this context is still valid
     *
     * A context is valid if:
     * 1. The lifetime check passes (alive flag true OR lifetime token not expired)
     * 2. The generation counter hasn't changed (no newer request superseded this one)
     *
     * @return true if the callback should proceed, false if it should abort
     */
    [[nodiscard]] bool is_valid() const {
        // Check lifetime: token takes priority if set, otherwise check alive flag
        if (lifetime_token.has_value()) {
            if (lifetime_token->expired()) {
                return false;
            }
        } else if (!alive || !alive->load()) {
            return false;
        }
        // If no generation tracking, always valid
        if (generation == nullptr) {
            return true;
        }
        // Check generation hasn't changed
        return captured_gen == generation->load();
    }

    /**
     * @brief Create a context from AsyncLifetimeGuard, optionally with generation tracking
     *
     * @param guard The AsyncLifetimeGuard from the calling object
     * @param gen Pointer to generation counter (nullptr if not used)
     * @return A context that can be passed to async callbacks
     */
    static ThumbnailLoadContext create(helix::AsyncLifetimeGuard& guard,
                                       std::atomic<uint32_t>* gen = nullptr) {
        ThumbnailLoadContext ctx;
        ctx.lifetime_token = guard.token();
        ctx.generation = gen;
        ctx.captured_gen = gen ? ++(*gen) : 0;
        return ctx;
    }

    /**
     * @brief Create a context from legacy alive flag, incrementing the generation counter
     *
     * @param alive_flag Shared alive flag from the calling object
     * @param gen Pointer to generation counter (nullptr if not used)
     * @return A context that can be passed to async callbacks
     */
    static ThumbnailLoadContext create(std::shared_ptr<std::atomic<bool>> alive_flag,
                                       std::atomic<uint32_t>* gen = nullptr) {
        ThumbnailLoadContext ctx;
        ctx.alive = std::move(alive_flag);
        ctx.generation = gen;
        ctx.captured_gen = gen ? ++(*gen) : 0;
        return ctx;
    }

    /**
     * @brief Capture a context from AsyncLifetimeGuard without incrementing generation
     *
     * @param guard The AsyncLifetimeGuard from the calling object
     * @param gen Pointer to generation counter (nullptr if not used)
     * @return A context that captures current generation without incrementing
     */
    static ThumbnailLoadContext capture(helix::AsyncLifetimeGuard& guard,
                                        std::atomic<uint32_t>* gen = nullptr) {
        ThumbnailLoadContext ctx;
        ctx.lifetime_token = guard.token();
        ctx.generation = gen;
        ctx.captured_gen = gen ? gen->load() : 0;
        return ctx;
    }

    /**
     * @brief Capture a context from legacy alive flag without incrementing generation
     *
     * @param alive_flag Shared alive flag from the calling object
     * @param gen Pointer to generation counter (nullptr if not used)
     * @return A context that captures current generation without incrementing
     */
    static ThumbnailLoadContext capture(std::shared_ptr<std::atomic<bool>> alive_flag,
                                        std::atomic<uint32_t>* gen = nullptr) {
        ThumbnailLoadContext ctx;
        ctx.alive = std::move(alive_flag);
        ctx.generation = gen;
        ctx.captured_gen = gen ? gen->load() : 0;
        return ctx;
    }
};
