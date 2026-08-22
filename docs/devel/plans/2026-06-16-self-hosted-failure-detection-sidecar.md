# Self-Hosted Local Print-Failure Detection (Sidecar) — Feasibility & Deferred Plan

**Date:** 2026-06-16
**Status:** PARKED / deferred — strategic feasibility captured, not scheduled. Un-parking criteria in §9.
**Relation to shipped work:** Builds on the `DetectionSource` abstraction shipped for the Snapmaker U1 (`docs/devel/printers/SNAPMAKER_U1_SUPPORT.md` § `defect_detection`). That adapter consumes a detector someone else runs. This doc is about HelixScreen **running its own detector** on printers whose stock AI is closed/unobservable.

---

## 1. Why this doc exists

The U1 adapter was cheap (~600 LOC) because Snapmaker did the hard parts — camera capture, NPU inference, a trained model — **and** published the result into Moonraker (`print_stats.exception.code==2`). We just read a field.

The question this doc answers: the Creality K1C / K2 Plus (and others) clearly have AI-detection *hardware* — why can't HelixScreen implement its own middleware + AI engine on them? Short answer: **we can, but it's a categorically larger project than the U1 adapter, the hardware is less reachable than it looks, and it substantially overlaps with Obico.** The `DetectionSource` abstraction is already the right consumer for it.

## 2. Finding that forces the issue: K1C/K2 stock detection is closed and Moonraker-unobservable

On-device + web investigation (2026-06-15), read-only on the project K1C (192.168.30.182) and K2 Plus (192.168.1.74):

| | Snapmaker U1 | Creality K1C | Creality K2 Plus |
|---|---|---|---|
| Stock detection | Open `defect_detection.py` | Closed `cx_ai_middleware`/`ai_engine`/`detection` (`/usr/bin`) | Open wrapper `load_ai.py` → **closed** `ai_engine` binary |
| Inference | Local (Rockchip NPU) | Local (Ingenic; likely in the AI **camera module**) | Local (Allwinner `sun8iw20`, likely on-board NPU) |
| **Result in Moonraker** | **`print_stats.exception.code==2`** | **None** — private ubus / `/tmp/ai_*_uds`; only generic `state==paused` | **None** — `load_ai` object returns `{}` for print defects (it's CFS-waste, not spaghetti); generic `state==paused` |
| Replicate by *reading*? | EASY | **NONE** | **NONE** |

Key device evidence: K1C `/usr/bin/cx_ai_middleware`, `/etc/init.d/S70cx_ai_middleware`, thresholds in `/usr/data/creality/userdata/config/user_print_refer.json` (`pastaTruth:62.5`); K2 `klippy/extras/load_ai.py` shelling out to `ai_engine`. Tellingly, **Obico and OctoEverywhere both bolt their own AI onto these printers** — which would be pointless if Creality's result were locally consumable.

**Conclusion:** there is no `K1StockSource`/`K2StockSource` worth writing — nothing honest to read. If we want detection on these printers, HelixScreen must **run its own**.

## 3. Why a sidecar (not in-UI, not stock-consume)

- **Not in the HelixScreen binary.** In-process fleet inference is already rejected: AD5M (14 MB memory diet), K1/AD5X MIPS32, CC1 armv7l have no headroom, and bundling a TFLite/ncnn runtime + model bloats the single UI binary that ships to *every* target. A detector must be an **optional component installed only on capable printers**.
- **Consumed via `DetectionSource`.** The sidecar writes results to a Moonraker-observable channel (a custom Moonraker component object, or a localhost socket/HTTP); HelixScreen reads it through a new `SidecarSource` adapter. The abstraction shipped for the U1 already supports this (`attributable`, `confidence`, policy, presenter) — no UI/policy rework needed.

## 4. The hardware reality (the actual blockers)

"It has the hardware" understates the difficulty. Using an NPU is not "it's there, call it" — each vendor has a closed toolchain you must convert your model through, per SoC:

- **K1C (Ingenic MIPS):** the AI camera is a *self-contained module* that most likely runs inference on the **camera's own processor**, not the mainboard. If so, that NPU is physically unreachable — we'd get only frames out and be stuck on the **mainboard MIPS CPU**, where ML-runtime support (TFLite/ncnn) is poor. Ingenic's NPU SDK ("Magik") is closed and MIPS-targeted.
- **K2 Plus (Allwinner `sun8iw20`, likely V853-class):** the NPU is on the mainboard (it runs `ai_engine` there), so it's reachable *in principle* — but only via Allwinner's closed NN SDK + model-conversion toolchain.
- **Exact silicon is unconfirmed** on both (no authoritative teardown). Any NPU plan starts with identifying the chip and obtaining a per-vendor closed SDK — the expensive, uncertain part.
- **CPU fallback:** a quantized YOLO-class detector at ~1 frame / few-seconds is plausible on the **armv7l K2 CPU**; on the **MIPS K1** it's a slog and competes with Klipper for cycles.

Net: NPU acceleration is vendor-locked and per-SoC (and possibly impossible on the K1). **CPU-only is the realistic starting point**, and it's weak on the K1.

## 5. Proposed architecture (if/when un-parked)

```
[printer MJPEG webcam] --frames--> [detector sidecar] --result--> [Moonraker-observable channel] --> [HelixScreen SidecarSource]
                                    (CPU, open model, low fps)      (custom object / localhost socket)   (existing DetectionSource)
```

- **Frame source (tractable):** the chamber webcam is already an MJPEG stream HelixScreen decodes (`camera_stream.cpp`). The sidecar reads the same stream. The nozzle AI cam (first-layer) is separate and may be locked to the vendor pipeline — spaghetti only needs a chamber/bed view.
- **Runtime:** ncnn or TFLite on CPU, quantized, low frame rate (1 frame / N seconds). NPU acceleration is a **per-SoC optimization added later**, not a prerequisite.
- **Result channel:** prefer a Moonraker custom component that exposes a queryable object / `notify_*` (so it looks like the U1 path to HelixScreen) — falling back to a localhost socket the `SidecarSource` polls.
- **Response:** unchanged — `DetectionManager` policy + the existing response modal (resume/abort/tune). The sidecar self-pauses via Moonraker's pause API (defer-to-source default), HelixScreen surfaces + backstops.
- **Packaging:** an optional installer component, gated on "printer has a camera + adequate CPU." Not shipped to MIPS toasters by default.

## 6. The model

Creality's/Snapmaker's weights are proprietary/encrypted — not reusable. Realistic source: **Obico's open-source spaghetti model** (The Spaghetti Detective is open; weights are available under their license — verify license terms before redistribution). Alternatives: train our own (large effort, needs a labeled failure dataset) or another open failure-detection model.

## 7. The honest gut-check: build-our-own vs. integrate Obico

A CPU sidecar running an open model **is essentially Obico's on-device half, rebuilt.** Obico already exists, is open, runs this class of model, and ships a `moonraker-obico` plugin.

| | Integrate Obico | Build our own sidecar |
|---|---|---|
| Effort | Low (consume existing) | High (months; per-SoC NPU is a long tail) |
| Cloud/account | Needs Obico server (cloud or self-hosted ML) | Fully local, no account |
| UX integration | Their stack; we mostly mirror status | Native, tight HelixScreen UX |
| Coverage | Any printer + internet (or self-host) | Any printer with camera + CPU, **offline** |
| Detection quality | Mature | We own accuracy/false-positive tuning |

The differentiators for build-our-own are **fully-local + no-account + native UX + offline**, and it works uniformly on *any* camera-equipped printer (not just K1/K2). That's real, but it's a large investment for something Obico approximates today.

## 8. Phased plan (when un-parked)

- **Phase 0 — CPU spike on the K2 (most capable of the three).** Stand up an off-device or on-device ncnn/TFLite run of an open spaghetti model against the K2's MJPEG stream; measure real frame-rate, CPU load (vs Klipper), and accuracy/false-positive rate. **Go/no-go gate** — if CPU-only is unusable even on the K2, the whole effort needs NPU work and likely isn't worth it.
- **Phase 1 — sidecar daemon + Moonraker result channel.** Package the detector as an installable component; expose results as a Moonraker-observable object; self-pause via Moonraker.
- **Phase 2 — `SidecarSource` adapter in HelixScreen.** New `DetectionSource` consuming the channel; reuse the existing policy + response modal. (Small — the abstraction is ready.)
- **Phase 3 (optional, per-SoC) — NPU acceleration.** Only for SoCs with a reachable NPU + obtainable SDK (K2/Allwinner candidate; K1/Ingenic likely not). Each is its own integration project.

## 9. Un-park criteria

Revisit if any of:
- Phase-0 spike shows usable CPU-only detection on mid-tier printers — then it's a tractable, high-value feature.
- A printer we support exposes its stock detection in Moonraker on a future firmware (re-check Creality's Klipper fork periodically) — then it's a cheap stock adapter, not a sidecar.
- Obico integration is requested first — likely the better near-term ROI; this sidecar becomes the "offline/no-account" alternative rather than the first move.

## 10. Risks

- NPU SDKs closed/unobtainable per vendor; K1 NPU possibly inside the camera module (unreachable).
- CPU inference too slow on weak SoCs (esp. MIPS K1) — Phase 0 gates this.
- Open-model license terms for redistribution (Obico model) — verify before shipping.
- Maintenance burden of a second deployable component + a model, across heterogeneous SoCs.
- Overlap with Obico — risk of reinventing a mature tool for marginal differentiated value.

## References
- Shipped U1 adapter: `docs/devel/printers/SNAPMAKER_U1_SUPPORT.md` § `defect_detection`.
- Device evidence: K1C `/usr/bin/cx_ai_middleware`, `/etc/init.d/S70cx_ai_middleware`, `/usr/data/creality/userdata/config/user_print_refer.json`; K2 `klippy/extras/load_ai.py`.
- Obico (open detector + `moonraker-obico`): https://www.obico.io
- Creality "edge AI" framing: https://wiki.creality.com/en/k1-flagship-series/k1-max/quick-start-guide/ai-feature-description
