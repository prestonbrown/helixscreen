# Coral-Accelerated Local Failure + Bed Detection (Voron / `helix-detect` sidecar) — Spec

**Date:** 2026-06-16
**Status:** PARKED 2026-06-16 — design complete, not scheduled. Parked on the open **camera-mount generalization** question (below): a small self-trained model may not generalize across the wildly varying camera mounts Voron users run, which is exactly Obico's data-diversity moat. Un-park by running the reframed Phase 0 (treat our Voron camera as a held-out, never-trained-on view; if a public-data-only model detects spaghetti on it, generalization is plausible). Bed-occupancy (classical-CV reference-diff) is mount-agnostic and unaffected.
**Author:** Preston (solo maintainer)

**Relation to prior work:**
- Consumes the shipped `DetectionSource` / `DetectionManager` framework (`docs/devel/printers/SNAPMAKER_U1_SUPPORT.md` § `defect_detection`, merged to main).
- **Un-parks** `docs/devel/plans/2026-06-16-self-hosted-failure-detection-sidecar.md`, scoped to its friendly-hardware case. That doc parked the sidecar because the *fleet's* hardware is hostile (closed vendor NPUs, possibly-unreachable K1 camera-module NPU, MIPS / memory-dieted SoCs, an unproven CPU-viability gamble). **A Voron host + Coral USB Edge TPU deletes every one of those blockers**, so this spec executes that doc's §8 phased plan on the hardware where it is tractable.

---

## 1. Goal & audience

Detect, during a print on a camera-equipped Voron-class printer with a **Coral USB Edge TPU**:
1. **Spaghetti / print-failure** (continuous, mid-print).
2. **Non-empty / dirty bed** before a print starts (one-shot, pre-first-layer).

…and surface both through HelixScreen's existing detection response UI, while the detector itself protects the print (self-pause) even when the screen is off.

**Audience:** not personal-only. Anyone with a Coral should be able to install and try it. **Coral is the supported acceleration path; a CPU fallback exists but is best-effort/experimental.** Fully local, no cloud, no account.

**Explicit non-goals (this spec):**
- No in-process inference in the HelixScreen binary (rejected on main; would bloat the single UI binary shipped to MIPS/AD5M targets).
- No per-SoC vendor-NPU work (K1 Ingenic / K2 Allwinner). Coral only; CPU fallback. Vendor NPUs remain deferred per the parked doc §8 Phase 3.
- No cloud detector. (Obico/OctoEverywhere remain separate deferred `DetectionSource` extension points.)
- HelixScreen does **not** own the primary pause — `DeferToSource` policy default, with the existing backstop.

---

## 2. Architecture overview

Two deliverables.

```
                       ┌──────────────────────── Klipper host (Voron) ──────────────────────┐
[chamber MJPEG webcam] → [ helix-detect (Python daemon) ]
                          ├ frame grabber (reads existing MJPEG stream)
                          ├ inference backend (pluggable, auto-selected)
                          │    • CoralBackend  — pycoral / libedgetpu, INT8 Edge TPU   (primary)
                          │    • CpuBackend    — tflite-runtime + XNNPACK              (fallback)
                          ├ spaghetti detector  (Obico open model, debounced)
                          ├ bed-occupancy checker (classical CV reference-diff, no model)
                          └ Moonraker client:
                               • printer.print.pause   (self-pause; protects print w/ screen off)
                               • registers as agent, broadcasts notify_agent_event {kind, confidence, ts}
                       └────────────────────────────────────────────────────────────────────┘
                                                  │  notify_agent_event (over Moonraker WS)
                                                  ▼
                       ┌──────────────────────── HelixScreen ───────────────────────────────┐
                          [ SidecarSource : DetectionSource ]  → DetectionManager → existing
                          (subscribes to agent events,            (policy + backstop)   response
                           maps payload → DetectionEvent)                               modal
                       └────────────────────────────────────────────────────────────────────┘
```

**Deliverable A — `helix-detect` sidecar** (new; separate Python project / repo subdir). The hard new work.
**Deliverable B — `SidecarSource`** (new `DetectionSource` in HelixScreen). Small; reuses shipped modal/policy/backstop/settings.

---

## 3. Deliverable A — `helix-detect` sidecar

**Language:** Python 3. Rationale: separate host process (not the cross-compiled UI binary, so none of the C++ cross-compile constraints apply); `pycoral` / `tflite-runtime` are first-class in Python; matches the Klipper/`moonraker-obico` ecosystem; fast iteration on thresholds.

### 3.1 Frame grabber
- Reads the **same chamber MJPEG stream HelixScreen already decodes** (resolved from Moonraker's `webcam` config). No nozzle/first-layer AI-cam dependency — spaghetti needs only a bed/chamber view.
- Pulls at a low rate (see cadence below); decodes to RGB; letterbox/resize to the model's input tensor.

### 3.2 Inference backend (pluggable, auto-selected)
- Interface: `infer(frame_rgb) -> list[Detection{bbox, score, class}]`.
- `CoralBackend` — `pycoral` + `libedgetpu`, INT8-quantized Edge TPU model. **Primary path.** ~15ms/frame.
- `CpuBackend` — `tflite-runtime` + XNNPACK, same model un-/dynamically-quantized. **Fallback.** ~1–4 fps; flagged experimental.
- Startup probes for a Coral (`edgetpu` device enumeration) → CoralBackend; else CpuBackend with a logged warning.

### 3.3 Spaghetti detector
- **Model:** **our own small YOLO** (YOLOv8n / YOLO11n) fine-tuned on open CC BY 4.0 datasets (§6), exported to Edge TPU via Ultralytics' official `export(format="edgetpu")`. **We do not port Obico's model** — it is a ~50M-param YOLOv2 *Darknet* net at 416×416 that has resisted Coral conversion for 6+ years (Obico issue #96), is above the Edge-TPU input-size ceiling, and its weights are unlicensed. Obico's `ml_api` and `CookiezRGood`'s CPU `.tflite` are used only as **baselines to beat** in Phase 0a, never shipped.
- **Cadence:** 1 frame / 2–5s, **only while `print_stats.state == printing`** (idle = no inference, no CPU/Coral burn).
- **False-positive control (the key lever):** confidence threshold **plus** temporal debounce — require `N` consecutive sampled frames over threshold (configurable) before firing. Optionally require detections in roughly the same image region across frames.
- On confirmed detection → §3.6 act + publish, `kind = Spaghetti`, real `confidence`.

### 3.4 Bed-occupancy checker (classical CV — separate path, no model, no Coral)
- Runs **once at print start**, before the first layer (gate on the `printing` transition / start-of-print, before extrusion).
- **Reference-diff:** compare the current frame against a stored **empty-bed reference image** (per-printer). Align (the camera is fixed, so a static homography/crop is enough), normalize for lighting, diff, threshold the **changed-area ratio**. Over threshold → bed not clear.
- On positive → §3.6 act + publish, `kind = DirtyBed`.
- **Reference capture:** explicit user action — "bed is clear now → capture reference." Triggered from HelixScreen settings (§4.4) via a sidecar command (agent method / config webhook), or a sidecar CLI. No reference ⇒ bed check disabled (logged), spaghetti still works.

### 3.5 Moonraker client
- Long-lived WS connection to Moonraker (the daemon's own connection — independent of HelixScreen).
- Subscribes to `print_stats` to gate spaghetti (printing) and bed-check (print start).
- **Registers as a Moonraker agent**; on detection broadcasts `notify_agent_event` with a typed payload:
  ```jsonc
  { "agent": "helix_detect",
    "event": "detection",
    "data": { "kind": "spaghetti"|"dirty_bed", "confidence": 0.0-1.0,
              "ts": <epoch>, "frame_seq": <n> } }
  ```
  Chosen over the parked doc's "custom Moonraker component object / localhost socket": no Moonraker component to install, typed payload, visible to all Moonraker clients, survives screen-off.

### 3.6 Acting on a detection
- **Self-pause first:** call `printer.print.pause` (spaghetti) or hold the print before first layer (dirty bed). This protects the print regardless of HelixScreen state — matches the `DeferToSource` model the framework defaults to.
- **Then publish** the agent event (above) so HelixScreen (and any client) can attribute it.

### 3.7 Config & tune
- Config file on the host (`helix-detect.cfg` / TOML): enable flags (spaghetti, bed), spaghetti threshold + debounce `N` + cadence, sensitivity preset, bed changed-area threshold, reference-image path, webcam stream URL override.
- Tune surface: accept live updates (sensitivity/threshold, enable toggles, reference capture) from HelixScreen — via a Moonraker agent method or a small localhost control endpoint. Mirrors the U1's `defect_detection/config` tune affordance so `can_tune()` works.

### 3.8 Packaging
- systemd service. Optional installer component, gated on "camera present + Coral present (or CPU fallback acknowledged)." Not shipped to MIPS/memory-dieted targets by default.
- Distinct from the HelixScreen binary release; its own install path (KIAUH-style script or a HelixScreen installer add-on).

---

## 4. Deliverable B — HelixScreen `SidecarSource`

A new `DetectionSource`, mirroring `U1StockSource` (`include/u1_stock_detection_source.h`).

### 4.1 New files
- `include/sidecar_detection_source.h`, `src/printer/sidecar_detection_source.cpp` — `helix::detection::SidecarSource : DetectionSource`.

### 4.2 Behavior
- `id()` → `"sidecar"`.
- `available()` → true when the `helix_detect` agent is connected (query Moonraker's agent/connection list; refresh via the existing `DetectionManager::refresh_capabilities()` post-connect hook).
- Subscribes to `notify_agent_event` filtered to `agent == "helix_detect"`; maps payload → `DetectionEvent`:
  - `kind`: `"spaghetti"→Spaghetti`, `"dirty_bed"→DirtyBed`.
  - `confidence`: **set** (real float) — unlike `U1StockSource`, which reports none.
  - `attributable = true`.
  - `already_paused`: from current `print_stats.state` (sidecar self-paused, so normally true).
  - `message`: human string built from kind/confidence.
- `can_tune()` → true; forwards sensitivity/threshold/reference-capture to the sidecar (§3.7).
- `self_pauses()` → true.
- WS callback marshals to main thread per the framework contract (events fire on main thread). Use `AsyncLifetimeGuard` + `tok.defer(...)` — no bare `this` in WS callbacks ([L072]); no `if (tok.expired()) return;` then member access ([L081]).

### 4.3 Registration & reuse
- Constructed and registered in `DetectionManager` at startup alongside `U1StockSource` (`src/application/application.cpp`).
- **Reuses unchanged:** `ui_spaghetti_detection_modal` (resume/abort/tune), `DetectionPolicy` (`DeferToSource` default), backstop-pause via `AbortManager`, presenter wiring.
- Bed-occupancy reuses `DetectionKind::DirtyBed` (already defined in `detection_source.h`). Surfaced only around print start. Title/copy differ from spaghetti ("Bed not clear — print held"); actions: Clear & retry / Cancel / Tune. May warrant a small modal title/string variation, not a new modal.

### 4.4 Settings
- Extend the existing detection settings (`include/settings_manager.h` already has `detection_enabled` + `detection_policy_u1`): add `detection_policy_sidecar` (Off/NotifyOnly/DeferToSource) parallel to the U1 one, plus the tune controls (spaghetti sensitivity, bed enable, **"capture empty-bed reference"** action).
- i18n: "Coral", "Obico", "Moonraker", "Klipper", "Voron" are product names — not translated ([L070]); surrounding sentences are.

---

## 5. Phasing

- **Phase 0a — instant CPU baseline (hours, no training).** On the dev box, run an existing local detector (`CookiezRGood` `.tflite`, or Obico `ml_api` Docker) against Voron footage. Gate: **does local YOLO spaghetti detection work at all on our camera angle/lighting, at a tolerable false-positive rate?** If no → stop; the feature is moot.
- **Phase 0b — train + Edge-TPU spike (the real go/no-go).** Fine-tune YOLOv8n/v11n on the CC BY 4.0 datasets (§6), `export(format="edgetpu")` at ~320px, run on the Coral (dev box). Three gates: **(1) compiles mostly onto the TPU** — read the `edgetpu_compiler` op-partition report; one early unsupported op dumps the rest to CPU; **(2) speed** acceptable; **(3) accuracy/FP after INT8** vs the float model on our footage. Failure modes to expect: single-partition CPU fallback, shared-scale INT8 confidence collapse (→ DeGirum multi-scale output fix), input-size ceiling. If 0b fails but 0a passed → fall back to **CPU sidecar, no Coral**.
- **Phase 1 — sidecar daemon:** frame grab + Coral/CPU backend + spaghetti detect + debounce + Moonraker self-pause + agent-event broadcast + config. CPU fallback included.
- **Phase 2 — bed-occupancy:** reference capture + diff check + pre-print gate + agent event.
- **Phase 3 — `SidecarSource` adapter** in HelixScreen (small; framework ready).
- **Phase 4 (optional)** — installer component + settings/tune polish + docs.

Each phase is independently testable. Phase 3 can proceed against a mocked agent-event payload before the daemon is complete.

---

## 6. Testing

- **Sidecar (Python):** unit-test backend selection, debounce state machine (N-consecutive), bed reference-diff thresholding (synthetic occupied/empty frames), and agent-event payload shape. Record a short fixture clip of a real spaghetti failure for regression.
- **`SidecarSource` (C++):** mirror `tests/unit/test_detection_source.cpp` — feed synthetic `notify_agent_event` payloads, assert `DetectionEvent` mapping (kind, confidence set, attributable, already_paused). Tag thread/WS tests `[slow]` ([L052]). Capability/`available()` gating via the existing `apply_objects_list_for_test`-style seam.
- **On-device:** validate on the project Voron with a Coral; confirm self-pause fires, agent event surfaces the modal, backstop triggers if pause is suppressed.

---

## 7. Risks & open items

- **Edge-TPU compilation may not map** — single-partition rule means one early unsupported op runs the whole rest on CPU. Mitigate with a small model (YOLOv8n/v11n), TPU-friendly input (~320px, under the ceiling), ReLU6 over SiLU, and reading the compiler's op-partition report in Phase 0b. Fallback: CPU sidecar.
- **INT8 accuracy collapse** — shared-scale quantization of detector outputs wrecks confidence/box precision. Mitigate with the DeGirum-style per-output-tensor quantization + CPU-side decode/NMS (NMS/sigmoid aren't TPU-mappable anyway). Measured in Phase 0b against the float model.
- **Training-data licensing** — primary sets are CC BY 4.0 (`spaghetti-3d`, `3d-printing-flaws`, `fdm-failures-spaghetti`) — attribution required, redistributable. `Javiai/failures-3D-print` is **Unknown-license** (eval only). `CookiezRGood`'s `.tflite` has **no license** — baseline comparison only, never shipped. Our trained weights are ours to license.
- **False positives** — the make-or-break UX risk; debounce + threshold tuning is the mitigation. A too-eager detector that pauses good prints is worse than none.
- **Bed-diff robustness** to lighting/chamber-LED changes — normalize; consider requiring the reference and current frame under comparable lighting; expose threshold.
- **Overlap with Obico** — acknowledged. Differentiators: fully-local, no-account, offline, native HelixScreen UX, any-camera-printer uniformity. With a Coral the build is tractable; we knowingly rebuild a slice of Obico's on-device half.
- **Frame access contention** — sidecar reading the MJPEG stream concurrently with HelixScreen's decode; both are independent HTTP consumers of the same stream, should be fine, confirm on-device.
- **Coral availability/quirks** — USB Coral throttling/thermals; driver/runtime versioning across host OSes. Document supported runtime versions.

---

## 8. References
- Shipped framework: `docs/devel/printers/SNAPMAKER_U1_SUPPORT.md` § `defect_detection`; `include/detection_source.h`, `include/detection_manager.h`, `include/u1_stock_detection_source.h`.
- Parked feasibility (un-parked here): `docs/devel/plans/2026-06-16-self-hosted-failure-detection-sidecar.md`.
- Obico (baseline detector; YOLOv2-Darknet, AGPL code / unlicensed weights): https://github.com/TheSpaghettiDetective/obico-server — Coral non-feasibility: issue #96 (open 2019–2025, no success).
- CPU baseline `.tflite` (no license — eval only): https://github.com/CookiezRGood/klipper-print-failure-detection
- Edge-TPU export path: https://docs.ultralytics.com/integrations/edge-tpu · reference impl https://github.com/jveitchmichaelis/edgetpu-yolo · INT8 accuracy fix https://www.ultralytics.com/blog/deploying-quantized-yolov8-models-on-edge-devices-with-degirum
- Training datasets (CC BY 4.0): https://universe.roboflow.com/3d-printing-failure/spaghetti-3d · https://universe.roboflow.com/spaghettidetect/3d-printing-flaws · https://universe.roboflow.com/bu-engme500-vision-project/fdm-failures-spaghetti
- Coral Edge TPU model requirements: https://coral.ai/docs/edgetpu/models-intro
- Moonraker agent events: Moonraker docs § "Websocket Connections / Identifying connections / Agents".
