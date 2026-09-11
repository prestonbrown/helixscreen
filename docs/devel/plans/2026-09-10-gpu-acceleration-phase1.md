# GPU Acceleration Phase 1: Honest Config and Correct Rotation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the DRM backend's rotation and GPU claims true, so that turning EGL on in Phase 2 is a behaviour change rather than a bug reveal.

**Architecture:** `HELIX_ENABLE_OPENGLES` is a *request* that `lv_conf_internal.h` silently overrides, and `display_backend_drm.cpp` reads the request instead of the resolved result. Fixing that read un-masks a `DrmRotationStrategy::HARDWARE` path that double-applies rotation. So the double-apply is fixed first, while HARDWARE is still unreachable on all owned hardware, and only then does the code start telling the truth about EGL. The truth for Phase 1 is "EGL is off": the request is withdrawn from the six DRM targets and a `#error` makes any future disagreement between request and result a build failure.

**Tech Stack:** C++17, LVGL 9.5 (vendored fork), Catch2, pure Makefile.

**Spec:** `docs/devel/plans/2026-09-10-gpu-acceleration-design.md`

## Global Constraints

- Copyright header on every new source file: `// Copyright (C) 2025-2026 356C LLC` then `// SPDX-License-Identifier: GPL-3.0-or-later`.
- `spdlog` only. No `printf`, `cout`, or `LV_LOG_*` in our code.
- No comment archaeology: no commit SHAs, no "used to", no bug or review narration, in source or tests. A bare issue reference such as `(prestonbrown/helixscreen#1275)` is fine.
- Doc citations name a place (`src/api/display_backend_drm.cpp#set_display_rotation`), never a line number.
- This is the **shared main tree**. Never `git add` here except for a genuinely new file, and always commit with an explicit pathspec: `git commit -- <paths>`.
- Before compiling, check for a peer build: `pgrep -x -d' ' 'make|cc1plus'`, and use `make -j"$(scripts/helix-claim jobs)"`.
- Behaviour on every owned device must be unchanged by this plan. Every measured plane mask (`0x0` on Pi DSI and CB1, `0x21` on the U1) yields SOFTWARE or NONE, so no owned board reaches the HARDWARE branch either before or after.

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `include/drm_rotation_strategy.h` | Pure rotation decisions, no LVGL or DRM dependencies | Add two decision functions |
| `src/api/drm_rotation_strategy.cpp` | Their implementation | Add two functions |
| `tests/unit/test_drm_rotation_fallback.cpp` | Tests for those pure decisions | Add cases |
| `src/api/display_backend_drm.cpp` | DRM backend; applies the decisions | Consume them; read the resolved macro; add the `#error` gate |
| `src/application/display_manager.cpp` | Owns LVGL rotation and cached dimensions | Read dimensions after the backend settles |
| `mk/cross.mk` | Per-target build flags | Withdraw `ENABLE_OPENGLES` from six DRM targets |
| `docs/user/CONFIGURATION.md`, `docs/user/TROUBLESHOOTING.md` | User-facing rotation guidance | Correct per #1581 |

---

### Task 1: Pure rotation decisions

The rotation bug is a sequencing error between two files, which is untestable as written. Extract the part that is a rule, so it can be tested without a GPU.

**Files:**
- Modify: `include/drm_rotation_strategy.h`
- Modify: `src/api/drm_rotation_strategy.cpp`
- Test: `tests/unit/test_drm_rotation_fallback.cpp`

**Interfaces:**
- Consumes: the existing `DrmRotationStrategy` enum and `choose_drm_rotation_strategy()`.
- Produces: `LvglRotationAction lvgl_rotation_action_for(DrmRotationStrategy)` and `bool drm_rotation_needs_full_render(DrmRotationStrategy)`, both used by Task 2.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_drm_rotation_fallback.cpp`:

```cpp
TEST_CASE("HARDWARE rotation clears LVGL rotation", "[display][drm][rotation]") {
    // The plane rotates the scanout. LVGL rotating as well applies the
    // transform twice, and two 180s cancel (prestonbrown/helixscreen#1275).
    REQUIRE(lvgl_rotation_action_for(DrmRotationStrategy::HARDWARE) ==
            LvglRotationAction::CLEAR_TO_ZERO);
}

TEST_CASE("SOFTWARE rotation applies the requested angle to LVGL",
          "[display][drm][rotation]") {
    // The dumb-buffer flush callback reads lv_display_get_rotation() to decide
    // whether to reverse the pixel array, so LVGL must carry the angle.
    REQUIRE(lvgl_rotation_action_for(DrmRotationStrategy::SOFTWARE) ==
            LvglRotationAction::APPLY_REQUESTED);
}

TEST_CASE("NONE clears LVGL rotation", "[display][drm][rotation]") {
    REQUIRE(lvgl_rotation_action_for(DrmRotationStrategy::NONE) ==
            LvglRotationAction::CLEAR_TO_ZERO);
}

TEST_CASE("Only SOFTWARE needs FULL render mode", "[display][drm][rotation]") {
    // A partial-render buffer cannot be reversed in place.
    REQUIRE(drm_rotation_needs_full_render(DrmRotationStrategy::SOFTWARE));
    REQUIRE_FALSE(drm_rotation_needs_full_render(DrmRotationStrategy::HARDWARE));
    REQUIRE_FALSE(drm_rotation_needs_full_render(DrmRotationStrategy::NONE));
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
make -j"$(scripts/helix-claim jobs)" test && ./build/bin/helix-tests "[rotation]"
```

Expected: compile error, `lvgl_rotation_action_for` not declared.

- [ ] **Step 3: Declare the interface**

Append to `include/drm_rotation_strategy.h`, after the `choose_drm_rotation_strategy()` declaration:

```cpp
/**
 * @brief What LVGL should be told about rotation for a given strategy
 */
enum class LvglRotationAction {
    APPLY_REQUESTED, ///< Pass the requested angle to lv_display_set_rotation()
    CLEAR_TO_ZERO,   ///< Something else rotates; LVGL must not rotate as well
};

/**
 * @brief Decide whether LVGL carries the rotation, or something else does
 *
 * DRM plane rotation happens on the scanout side, after LVGL has produced its
 * pixels. Setting LVGL's rotation as well applies the transform a second time.
 *
 * @param strategy  Result of choose_drm_rotation_strategy()
 * @return Whether LVGL receives the requested angle or zero
 */
LvglRotationAction lvgl_rotation_action_for(DrmRotationStrategy strategy);

/**
 * @brief Whether the display must render whole frames for this strategy
 *
 * The software path reverses the pixel array in place in the flush callback,
 * which needs the entire buffer present.
 *
 * @param strategy  Result of choose_drm_rotation_strategy()
 * @return true when LV_DISPLAY_RENDER_MODE_FULL is required
 */
bool drm_rotation_needs_full_render(DrmRotationStrategy strategy);
```

- [ ] **Step 4: Implement**

Append to `src/api/drm_rotation_strategy.cpp`:

```cpp
LvglRotationAction lvgl_rotation_action_for(DrmRotationStrategy strategy) {
    if (strategy == DrmRotationStrategy::SOFTWARE) {
        return LvglRotationAction::APPLY_REQUESTED;
    }
    return LvglRotationAction::CLEAR_TO_ZERO;
}

bool drm_rotation_needs_full_render(DrmRotationStrategy strategy) {
    return strategy == DrmRotationStrategy::SOFTWARE;
}
```

- [ ] **Step 5: Run to verify they pass**

```bash
make -j"$(scripts/helix-claim jobs)" test && ./build/bin/helix-tests "[rotation]"
```

Expected: PASS. Confirm the case count rose by 4; a filter that silently matches nothing reports green.

- [ ] **Step 6: Prove the tests can fail**

Invert `lvgl_rotation_action_for` to always return `APPLY_REQUESTED`, rerun `./build/bin/helix-tests "[rotation]"`, confirm the HARDWARE case goes red, then revert. A green suite is not evidence that a test works.

- [ ] **Step 7: Commit**

```bash
git commit -m "feat(display): name the two rotation decisions the DRM backend was inlining" \
  -- include/drm_rotation_strategy.h src/api/drm_rotation_strategy.cpp \
     tests/unit/test_drm_rotation_fallback.cpp
```

---

### Task 2: Stop applying rotation twice

**Files:**
- Modify: `src/api/display_backend_drm.cpp` (`DisplayBackendDRM::set_display_rotation`)
- Modify: `src/application/display_manager.cpp` (the rotation block in `init`)

**Interfaces:**
- Consumes: `lvgl_rotation_action_for()` and `drm_rotation_needs_full_render()` from Task 1.
- Produces: no new symbols. Task 3 depends on this landing first.

- [ ] **Step 1: Route the DRM backend through the decisions**

In `src/api/display_backend_drm.cpp#set_display_rotation`, replace the three-case `switch (strategy)` body with:

```cpp
    if (drm_rotation_needs_full_render(strategy)) {
        lv_display_set_render_mode(display_, LV_DISPLAY_RENDER_MODE_FULL);
    }

    if (lvgl_rotation_action_for(strategy) == LvglRotationAction::CLEAR_TO_ZERO) {
        lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_0);
        lv_display_set_matrix_rotation(display_, false);
    } else {
        lv_display_set_rotation(display_, rot);
    }

    if (strategy == DrmRotationStrategy::HARDWARE) {
#ifndef HELIX_ENABLE_OPENGLES
        lv_linux_drm_set_rotation(display_, drm_rot);
        spdlog::info("[DRM Backend] Plane rotation {}° (LVGL left unrotated)",
                     static_cast<int>(rot) * 90);
#endif
    } else if (strategy == DrmRotationStrategy::SOFTWARE) {
        spdlog::info("[DRM Backend] Software rotation {}° (plane supports 0x{:X})",
                     static_cast<int>(rot) * 90, supported_mask);
    } else {
        spdlog::debug("[DRM Backend] No rotation needed");
    }
```

Keep the existing `#ifndef HELIX_ENABLE_OPENGLES` guard around the `lv_linux_drm_set_rotation` call for now; Task 3 replaces it.

- [ ] **Step 2: Let the backend settle before caching dimensions**

In `src/application/display_manager.cpp`, the rotation block currently sets LVGL rotation, caches the resolution, then calls the backend. The backend may now clear that rotation, so the cache must come last. Reorder to:

```cpp
            } else {
                lv_display_set_rotation(m_display, lv_rot);

                // The backend may clear LVGL's rotation when the scanout plane
                // rotates instead, so read the resolution it settles on.
                m_backend->set_display_rotation(lv_rot, phys_w, phys_h);

                m_width = lv_display_get_horizontal_resolution(m_display);
                m_height = lv_display_get_vertical_resolution(m_display);
            }
```

**What this fixes and what it does not.** It fixes the cancellation: a plane rotating 180 while LVGL also rotates 180 now happens only once. It does **not** make HARDWARE at 90 or 270 correct. Those need LVGL rendering at the swapped resolution while not rotating pixels, and clearing LVGL's rotation un-swaps that resolution. No owned board advertises 90 or 270 on any plane, so the case is unreachable and cannot be verified here. Leave it unreachable: do not add a resolution fix you cannot test. If a board ever reports such a mask, that is the moment to work out the resolution handling, on that hardware.

- [ ] **Step 3: Verify no owned device changes behaviour**

```bash
make -j"$(scripts/helix-claim jobs)" test-run
```

Expected: green. Then confirm the reachability claim still holds by re-reading the measured masks in the spec: `0x0` (Pi DSI, CB1) and `0x21` (U1) never satisfy `supported_mask & requested_drm_rot`, so every owned board still takes SOFTWARE or NONE.

- [ ] **Step 4: Confirm on hardware that rotation is unchanged**

The Pi at 192.168.1.113 is the only owned DRM board with a live panel.

```bash
make pi-docker && make deploy-pi
ssh pbrown@192.168.1.113 'sudo journalctl -u helixscreen -n 60 --no-pager' | grep -iE 'rotat|backend'
```

Expected: identical backend and rotation lines to a pre-change boot. Capture the pre-change log first.

- [ ] **Step 5: Commit**

```bash
git commit -m "fix(display): let one thing rotate the picture, not two (prestonbrown/helixscreen#1275)" \
  -- src/api/display_backend_drm.cpp src/application/display_manager.cpp
```

---

### Task 3: Read the resolved macro, and make drift a build failure

**Files:**
- Modify: `src/api/display_backend_drm.cpp` (five `HELIX_ENABLE_OPENGLES` sites)
- Modify: `include/display_backend_drm.h` (`using_egl_`)
- Modify: `mk/cross.mk` (six DRM targets)

**Interfaces:**
- Consumes: Task 2's corrected rotation ownership. Landing this without Task 2 ships a live double-rotation.
- Produces: `LV_LINUX_DRM_USE_EGL` as the single source of truth for whether EGL is compiled.

- [ ] **Step 1: Add the gate**

Near the top of `src/api/display_backend_drm.cpp`, after the LVGL includes:

```c
// lv_conf_internal.h derives LV_LINUX_DRM_USE_EGL from LV_USE_OPENGLES and
// redefines it with no #ifndef guard, so a value set in lv_conf.h does not
// survive. Ask the preprocessor what it resolved to, never the header.
#if defined(HELIX_ENABLE_OPENGLES) && !LV_LINUX_DRM_USE_EGL
#error "HELIX_ENABLE_OPENGLES set but LVGL resolved LV_LINUX_DRM_USE_EGL to 0"
#endif
```

- [ ] **Step 2: Verify the gate fires**

```bash
make pi-docker 2>&1 | grep -A2 'LV_LINUX_DRM_USE_EGL'
```

Expected: the build **fails** with that `#error`. That failure is the whole bug, now visible. Do not proceed until you have seen it.

- [ ] **Step 3: Withdraw the request**

In `mk/cross.mk`, change `ENABLE_OPENGLES := yes` to `ENABLE_OPENGLES := no` for the six DRM targets: `pi`, `pi-both`, `pi32`, `pi32-both`, `x86`, `x86-both`. Add one comment above the first:

```make
    # No LVGL EGL path is compiled yet; LV_USE_OPENGLES gates it and is 0.
    # Setting this to yes without that trips the #error in display_backend_drm.cpp.
    ENABLE_OPENGLES := no
```

- [ ] **Step 4: Replace every request read with a result read**

In `src/api/display_backend_drm.cpp`, change all five sites from `#ifdef HELIX_ENABLE_OPENGLES` / `#ifndef HELIX_ENABLE_OPENGLES` to `#if LV_LINUX_DRM_USE_EGL` / `#if !LV_LINUX_DRM_USE_EGL`. In `create_display`, set the member from the resolved macro rather than unconditionally:

```cpp
    using_egl_ = (LV_LINUX_DRM_USE_EGL != 0);
    if (using_egl_) {
        spdlog::info("[DRM Backend] GPU-accelerated display active (EGL/OpenGL ES)");
    } else {
        spdlog::info("[DRM Backend] DRM display active (dumb buffers, CPU rendering)");
    }
```

- [ ] **Step 5: Verify the build is green and the log is honest**

```bash
make pi-docker && printf '#include "lv_conf_internal.h"\nX = LV_LINUX_DRM_USE_EGL\n' > /tmp/p.c && \
  gcc -E -I lib/lvgl/src -I . -DHELIX_DISPLAY_DRM -DLV_CONF_INCLUDE_SIMPLE /tmp/p.c | grep '^X'
```

Expected: build succeeds; the preprocessor prints `X = 0`.

- [ ] **Step 6: Confirm on hardware**

```bash
make deploy-pi && ssh pbrown@192.168.1.113 'sudo journalctl -u helixscreen -n 60 --no-pager' \
  | grep -iE 'GPU-accelerated|dumb buffers|Rendering:'
```

Expected: `DRM display active (dumb buffers, CPU rendering)` and `Rendering: CPU (DRM dumb buffers)`. The GPU claim is gone, which is the point — nothing about the pixels changed.

- [ ] **Step 7: Commit**

```bash
git commit -m "fix(display): ask the preprocessor whether EGL is compiled, not the Makefile (prestonbrown/helixscreen#1580)" \
  -- src/api/display_backend_drm.cpp include/display_backend_drm.h mk/cross.mk
```

---

### Task 4: Correct the rotation documentation

**Files:**
- Modify: `docs/user/CONFIGURATION.md`
- Modify: `docs/user/TROUBLESHOOTING.md`

**Interfaces:**
- Consumes: nothing. Independent of Tasks 1-3 and can land in any order.
- Produces: nothing.

- [ ] **Step 1: Find the claims to fix**

```bash
grep -rn "rotate\|rotation\|fbdev" docs/user/CONFIGURATION.md docs/user/TROUBLESHOOTING.md
```

- [ ] **Step 2: Replace the CONFIGURATION.md performance note**

The paragraph beginning `**Performance note (Raspberry Pi / DRM displays):**` under `### rotate` becomes:

```markdown
**Raspberry Pi and other DRM displays rotate by any angle, using the framebuffer path to do it.** No display controller HelixScreen ships to can rotate a plane by 90 or 270 degrees, so when you set a rotation the app switches itself to the framebuffer backend, which rotates in software. This happens automatically and in-process: nothing needs reinstalling, no setting needs changing, and both binaries are already present. The cost is a full-screen redraw each frame rather than only the changed regions, typically under 1ms per frame on a Pi 5.

To avoid that cost entirely, rotate the panel in the kernel instead with a `video=...,rotate=90` parameter on the kernel command line. That applies before HelixScreen starts, so the app renders unrotated at native speed.

Framebuffer displays (AD5M, K1, K2, CC1, AD5X) rotate by any angle with no meaningful performance impact, using a more efficient partial update.
```

- [ ] **Step 3: Replace the TROUBLESHOOTING.md rotation note**

Under `### Display upside down or rotated`, in the `**Manual rotation:**` area, state:

```markdown
**All four values work on every printer.** On a Raspberry Pi or other DRM display, 90 and 270 are handled by switching to the framebuffer path automatically — you do not need to set `HELIX_DISPLAY_BACKEND` or install anything. If the picture stays unrotated after a restart, that switch failed, which means the system has no usable `/dev/fb0`; the log will say `Continuing without rotation`. In that case rotate the panel in the kernel with `video=...,rotate=90` instead.
```

**If `feature/xml-style-transition` has merged first**, the paragraphs to replace are its versions, which say a Pi "supports 180 degrees only" and tell the user to set `HELIX_DISPLAY_BACKEND=fbdev` by hand. Both claims are wrong for the same reason: `display_manager.cpp#try_drm_to_fbdev_fallback` deletes the DRM display, rebuilds on the fbdev backend and rebuilds the input devices without the user doing anything, and the DRM binary already contains that backend because `mk/cross.mk` compiles the DRM targets with both `-DHELIX_DISPLAY_DRM` and `-DHELIX_DISPLAY_FBDEV`.

Do not describe the framebuffer path as a fallback from something better. On this hardware there is nothing better.

- [ ] **Step 4: Verify doc gates pass**

```bash
scripts/quality-checks.sh --staged-only
```

Expected: doc references, links, and index all resolve.

- [ ] **Step 5: Commit**

```bash
git commit -m "docs(display): a sideways panel needs no binary choice (prestonbrown/helixscreen#1581)" \
  -- docs/user/CONFIGURATION.md docs/user/TROUBLESHOOTING.md
```

---

## Out of scope, deliberately

- **Turning EGL on.** That is Phase 2 and needs the probe-selected ladder, because the CB1 fails `gbm_create_device` while running the same `pi-both` binary as the Pi. Phase 1 exists so Phase 2 flips `ENABLE_OPENGLES` back to `yes` and gets a behaviour change instead of a bug reveal.
- **The nanovg draw unit**, and with it `LV_DRAW_TRANSFORM_USE_MATRIX`. Worth noting for Phase 2's design: LVGL's matrix rotation needs a matrix-capable draw unit, so nanovg may give DRM real 90/270 rotation and remove the fbdev swap entirely. Unverified.
- **Context sharing (#1582).** Follows Phase 2; delete the patch if Phase 2 is abandoned.
- **`docs/devel/architecture/14-build-platforms.md`.** Its rotation section lives on the unmerged `feature/xml-style-transition` branch. Correct it there or after that merge, not here, or the two will conflict.
