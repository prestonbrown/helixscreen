// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"

#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>

namespace helix::ui {

/// An exact-size cached copy of the printer picture, so no redraw scales the
/// full-resolution decode. One instance per image surface (the home widget,
/// the print status printer tile). Cache entries are keyed (source, w, h) by
/// prerendered_images and pruned there.
///
/// Main thread only. A cache miss generates the copy on HttpExecutor::fast();
/// the result returns to the main thread through this object's lifetime
/// guard, so a destroyed owner or a cancel() drops it.
class ExactSizeImage {
  public:
    /// The image widget as the owner's tree has it now; nullptr when it has none.
    /// Looked up again when a generation lands, since a rebuild can replace it.
    using ImageLookup = std::function<lv_obj_t*()>;
    /// Points the image at `lvgl_path`. The home widget sets the src directly;
    /// the print status tile publishes it through its bound subject.
    using SetSource = std::function<void(lv_obj_t* img, const std::string& lvgl_path)>;

    ExactSizeImage(const char* log_tag, ImageLookup image, SetSource set_src);

    /// Shows the cached copy of `source` for the image's declared size, when one
    /// is on disk. False when the image has no size yet or nothing is cached.
    bool show_cached(const std::string& source);
    /// Shows `source` itself, CONTAIN-scaled into the image.
    void show_scaled(const std::string& source);
    /// Starts generating the copy for the declared size when none is cached;
    /// the scaled source stays on screen until it lands, so nothing blanks.
    void generate_if_missing(const std::string& source);
    /// show_cached(), else show_scaled() and generate_if_missing(). Repeated
    /// for the same source and size, it touches neither the disk nor the widget.
    /// True when the exact copy is on screen.
    bool apply(const std::string& source);

    /// The image was rewritten under the same path or the tree was rebuilt:
    /// the next call shows and checks again.
    void forget_displayed();
    /// Drops the in-flight generation's result (owner teardown, tree destroyed).
    /// The worker still finishes and writes its entry for the next attach, and
    /// a request refused while it ran is generated too, with no result.
    void cancel();

  private:
    void show_exact(lv_obj_t* img, const std::string& lvgl_path);

    const char* log_tag_;
    ImageLookup image_;
    SetSource set_src_;
    /// The source the owner asked for last; a generation for another is stale.
    std::string source_;
    /// What the image was last pointed at, so a repeat does not invalidate it.
    std::string displayed_path_;
    /// The (source, size) apply() last resolved, and whether that was a hit.
    std::string applied_source_;
    int32_t applied_w_ = 0;
    int32_t applied_h_ = 0;
    bool applied_exact_ = false;
    /// One generation at a time: every relayout or navigation back asks again.
    bool inflight_ = false;
    /// The latest request refused while a generation ran. The running one's
    /// result asks again, or cancel() hands it to the worker.
    struct Deferred {
        std::string source;
        std::string cache_path;
        int w = 0;
        int h = 0;
    };
    std::optional<Deferred> deferred_;
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix::ui
