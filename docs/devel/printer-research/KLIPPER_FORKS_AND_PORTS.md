# Klipper Forks, Ports, and Vendor Reimplementations

Research notes on the state of Klipper "ports" — vendors reimplementing the
Python host (klippy) in compiled languages, the proprietary APIs they ship in
place of Moonraker, and what all of it means for HelixScreen.

**Last updated:** 2026-06-13

> **Why this matters for HelixScreen:** none of these reimplementations change
> the wire we talk to. HelixScreen speaks the **Moonraker API** (WebSocket +
> REST), and every shipping variant below still exposes Moonraker (stock,
> forked, or via a community overlay like Rinkhals). The language klippy was
> rewritten in is mostly invisible to us — it surfaces only as Moonraker quirks
> (missing objects, subscription differences, REST gaps). Treat "what language
> is klippy" as device-support trivia, not an integration blocker.

---

## 1. The landscape

There is **no single "Klipper port" project** competing for a crown. There are
several *independent, mostly vendor-driven* reimplementations of klippy (the
host), each picking its own language, **none of them upstream**. Upstream and
the major community fork remain Python.

| Effort | Language | Layer | Who | Open? | Status |
|--------|----------|-------|-----|-------|--------|
| **Klipper (klipper3d)** | Python + C | host + MCU | Kevin O'Connor et al. | GPL-3.0, open | Canonical. Not being rewritten. |
| **Kalico** (ex Danger-Klipper) | Python + C | host + MCU | KalicoCrew | GPL-3.0, open | Most active community fork. Adds features, **same language**. |
| **Klipper-go / GoKlipper** | Go | host (klippy) | Anycubic | Partly open (proprietary bits encapsulated) | Ships on Kobra 3 / S1 ("K3" base). Source published ~Oct 2025. |
| **gopper** | TinyGo | **MCU** | amken3d (community) | Open | MCU-side port, not a klippy replacement. Experimental. |
| **Anycubic K4 "KlipperC++"** | C++ | host (klippy) | Anycubic | **Closed** | Ships on Kobra X. Not published. RE-derived knowledge only. |
| **Elegoo Centauri Carbon** | C++ (transpiled) | host (klippy) | Elegoo | Source released under GPL pressure; compliance disputed | Python→C++ transpile, original references intact. |

Key takeaway: among the **commercial vendors doing closed reimplementations**,
the trend tilts toward **C++**. The cleanest single data point is Anycubic
itself moving **Go → C++** on its newest flagship (see §3). Go isn't dead — it
runs on every Kobra 3 / S1 in the field — but it looks transitional rather than
where Anycubic is heading.

---

## 2. Elegoo Centauri Carbon — C++ (transpiled klippy)

- The CC1 mainboard runs a monolithic app bundling web UI, camera, API, screen
  UI, and **klippy transpiled from Python to C++**.
- Community RE: heavily modified Klipper at commit `28f60f7e` (616 commits past
  v0.9.1). Original Python references survive in the transpiled output — the
  tell that it is a **mechanical derivative**, not a rewrite.
- The DSP acting as the Klipper MCU runs extended/modified Klipper MCU code;
  the bed board has a custom MCU with non-standard commands.
- GPL-3.0 compliance has been contested publicly; Elegoo released "source" but
  completeness is disputed.

## 3. Anycubic — used BOTH, and switched Go → C++

Anycubic firmware is tracked by the Rinkhals RE community by an internal
"software base" generation:

| Base | klippy language | Models |
|------|-----------------|--------|
| **K3** | **Go** (GoKlipper / Klipper-go) | Kobra 3, 3 V2, 3 Max, Kobra S1 (+ Max) |
| **K4 ("K4Pro")** | **C++** (KlipperC++) | **Kobra X** |

- On the **Kobra X**, Anycubic abandoned its own GoKlipper base and rewrote on a
  **new in-house C++ port** — a *different* codebase from Elegoo's transpile.
- Core userspace processes on K4: `avata_main`, `avata_ui`, `avata_video`
  (control / UI / camera split; "Avata" is Anycubic app branding, not Klipper).
- **K4 enforces mandatory RSA signature verification** on firmware packages
  (validated against a public key via OpenSSL). This closes the SWU-injection
  path that works on every K3-base printer.
- **Rinkhals does not support the Kobra X** — "blocked by RSA signature."
  Documented bypass vectors are the usual last resorts: physical serial/UART
  access, or a software vuln in one of the `avata_*` processes.
- Asymmetry worth noting: Anycubic **open-sourced** the Go base
  ([Klipper-go](https://github.com/ANYCUBIC-3D/Klipper-go),
  [K3-klipper-mcu](https://github.com/ANYCUBIC-3D/K3-klipper-mcu)) but **locked
  down** the C++ one. So "C++ winning commercially" is also "C++ winning as the
  more *closed* option" — which is exactly what fuels the GPL fight.

> **Confidence:** the K3/K4 + GoKlipper/KlipperC++ naming and the `avata_*` /
> RSA details come from the **Rinkhals reverse-engineering community**
> (PR #422, docs), **not** Anycubic documentation. Anycubic has not published
> the K4 C++ port. Treat K4 internals as RE-derived.

---

## 4. The GPL question — can they keep the C++ port closed and signed?

Short version: **almost certainly not, on two independent grounds — unless it
were a genuine clean-room reimplementation, which it demonstrably is not.**

These are **two separate obligations**, often conflated under "locking down":

**(a) Closed source.** A *port / transpile* of klippy is a **derivative work**
(a translation is the textbook example). GPL-3.0 §4–6 then require corresponding
source be offered to anyone receiving binaries. Closed = violation. This is the
fight already underway with Elegoo.

**(b) The RSA signing lock — anti-tivoization (GPLv3 §6).** GPLv3 was written
specifically to kill the TiVo trick: ship the source, but sign the firmware so a
*modified* build won't run. For a **"User Product"** (a consumer 3D printer is
squarely one), GPLv3 requires also shipping the **Installation Information**
(signing keys or equivalent) needed to install and run a modified version. So
even if Anycubic published the K4 source tomorrow, the **RSA lock by itself**
would still violate GPLv3. This clause is the practical difference between
GPLv2 projects (e.g. the Linux kernel — no anti-tivoization) and **Klipper,
which is GPL-3.0** — so it bites here.

### Why "clean room" is not an available defense

Clean room defeats **copyright** (and GPL is just a copyright license): if a
walled-off team reimplements with *no access to and no copying of* the original
source, the result is not a derivative work and GPL never attaches. But clean
room only works when there is a **stable external interface/specification to
reimplement *to***, separate from the implementation behind it. (Canonical
case: Phoenix reimplementing the IBM PC BIOS against the documented BIOS call
interface.)

**Klipper has no such contract for the part that matters.** The interfaces that
*do* exist don't get you Klipper:

- **G-code in** — standard, documented. But "a thing that eats G-code and drives
  steppers" is Marlin/RepRapFirmware, not *Klipper*.
- **Host↔MCU protocol** — the one real candidate (defined messaging protocol,
  queryable data dictionary, compressed move queues). Reimplementing it
  reproduces the *transport*, not the brain.
- **printer.cfg** — a compatibility surface, not a spec; it lists knobs, not
  behavior.

What actually *is* Klipper — lookahead/trapezoidal motion planning, kinematics,
pressure advance, input shaping, stepper time-compression — **has no external
specification.** The behavior *is* the source. Any faithful reproduction
necessarily traces back to the GPL code → derivative.

The tell: vendors didn't want "a printer firmware," they wanted **Klipper
specifically** (config compatibility, motion quality, ecosystem). That's *why*
they ported/transpiled instead of reimplementing. If clean-room compatibility
had been viable, the cheap path was to write to the MCU protocol and stop —
nobody did.

### Why "can't" is squishy in practice

"Can't" describes the license terms. **Enforcement is separate:** GPL is
enforced only by copyright holders (here Kevin O'Connor + Klipper contributors,
not the FSF) choosing to sue. Nobody has taken Elegoo or Anycubic to court; the
pressure so far is public-shaming compliance campaigns
([freethecode.lol](https://freethecode.lol/)). Accurate framing: *they're very
likely in violation on both counts and doing it anyway*, because enforcement
cost falls on a small group of volunteer copyright holders against large
manufacturers.

> Not legal advice — derivative-work questions are fact-specific. But on the
> facts the RE community has (ported/transpiled, not clean-room), the lockdown
> is not compatible with GPL-3.0.

---

## 5. Practical implications for HelixScreen

- **Moonraker is the abstraction that saves us.** We don't care what language
  klippy is in as long as Moonraker is reachable. Stock/forked/overlay all work.
- **Go base (Kobra 3 / S1):** reachable via Rinkhals → standard Moonraker. ACE
  Pro backend already supported. Watch for `klipper-go` WebSocket/object quirks
  (may need REST polling for some objects) — see
  [ANYCUBIC_KOBRA_COREXY_RESEARCH.md](ANYCUBIC_KOBRA_COREXY_RESEARCH.md).
- **C++ closed + signed (Kobra X / K4):** effectively **not targetable** today.
  No Moonraker overlay can be installed past the RSA wall without a serial/vuln
  bypass. Park Kobra X support until the RE community breaks the signing chain.
- **Elegoo CC1:** OpenCentauri (COSMOS = full Klipper/Kalico replacement;
  Patched = overlay on stock) is the path to a sane Moonraker. The C++ transpile
  itself isn't something we integrate against directly.
- **Community forks of a stock printer are the sharper problem** — they keep the
  hardware identity while swapping a vendor module underneath, so model
  detection cannot see them and a stock-schema parser fails silently rather than
  loudly. See §8.

---

## 6. How vendors skip Moonraker (and what they expose instead)

**The enabling fact: klippy has its own API; Moonraker is just one client of
it.** Started with `-a`, klippy opens a Unix domain socket (`/tmp/klippy_uds`)
speaking a simple JSON protocol (messages terminated by ASCII `0x03`). Moonraker
is "only" a web server that connects to that socket and re-exposes it as
HTTP/WebSocket JSON-RPC. So a vendor can drop Moonraker two ways:

1. **Keep klippy, write your own daemon** that connects to `/tmp/klippy_uds`
   directly and exposes a proprietary external API.
2. **Replace klippy entirely** with a reimplementation (GoKlipper, the C++
   transpile) that owns the control loop *and* its own API — no socket boundary.

Either way the external-facing API is theirs to invent, and all three big
vendors converged on **MQTT-flavored, cloud-first** protocols (phone app ↔ cloud
broker ↔ printer pub/sub) — which is precisely what Moonraker's LAN/HTTP-first
design was *not* built for.

| Vendor | klippy | External API | Transport | Auth | RE status |
|--------|--------|--------------|-----------|------|-----------|
| **Elegoo** (Centauri Carbon) | C++ transpile (internal) | **SDCP v3** (ChituBox / cbd-tech) | MQTT-over-WebSocket + UDP discovery, **port 3030** | none on LAN WS | Well RE'd; multiple Python clients + HA integration |
| **Anycubic** (Kobra / GoKlipper) | GoKlipper (`gklib`) | **`gkapi`** — OctoPrint-compatible-ish endpoints + MQTT | WebSocket + MQTT | vendor | Partial; Go decompiles poorly (unnamed fns, concatenated strings) |
| **Creality** (K1 / K2) | Klipper (modified) | proprietary "app-server" / web-server | WebSocket (local) + MQTT (cloud), **port 9999** | vendor/cloud | RE'd by `ha_creality_ws` (K1/K2/K3/Hi) |

Cross-cutting observations:

- **Everyone reaches for MQTT** for cloud/app connectivity. Dropping Moonraker
  is about **ecosystem control** (own the app + cloud relationship, telemetry,
  keep third-party clients out), **not** performance — Moonraker is cheap and
  I/O-bound (see §7).
- **Numeric command codes + per-printer semantics.** SDCP explicitly warns the
  same cmd code can mean different things on different printers — these are
  app-to-firmware control channels, not interop standards.
- **SDCP is the cross-line bet.** Elegoo unified FDM *and* resin (Saturn/Mars)
  on SDCP, so it's the most documented and stable of the three.

**Rinkhals' approach (Anycubic):** rather than speak `gkapi` natively, Rinkhals
runs **real Moonraker and monkey-patches it** to translate between Moonraker's
expectations and GoKlipper's protocol — because GoKlipper does **not** natively
do `printer.objects.subscribe` / `notify_status_update`. This is why those
printers need an overlay to become HelixScreen-targetable.

> **HelixScreen consequence:** on all of these *stock* firmwares the Moonraker
> JSON-RPC API we speak is **absent**. Today the paths are: a community Moonraker
> shim (Rinkhals on Anycubic), a full replacement (OpenCentauri COSMOS on Elegoo,
> Guilouz/Guppy on Creality), or — speculative — us speaking the vendor protocol
> natively (see §7). Of the native options, **SDCP is by far the most viable
> target**: documented, RE'd, LAN-accessible, no auth, shared across Elegoo's line.

## 7. Future consideration: MQTT transport vs Moonraker JSON-over-HTTP

Notes for a possible future where HelixScreen talks to a printer over something
other than Moonraker's WebSocket JSON-RPC — e.g. MQTT (SDCP) or a vendor API.

**Will Moonraker itself be rewritten in a lighter language?** No public effort
as of 2026-06, and little motivation: Moonraker is **I/O-bound** (it shuffles
JSON over WS/HTTP and proxies klippy over a unix socket), unlike klippy which is
**CPU-bound** soft-real-time motion math where Python's GIL genuinely hurts on
cheap SBCs. The rewrite pressure that produced GoKlipper / the C++ transpile
applies to klippy, **not** Moonraker. The real risk to us is not "Moonraker gets
rewritten" but "a vendor ships something that isn't Moonraker at all" (§6).

**What adding an MQTT/SDCP backend would take.** Per the Moonraker-coupling
assessment (2026-06): the codebase has a genuine transport abstraction —
`IMoonrakerClient::send_jsonrpc(method, params)` is generic, and the Klipper
**object model** (`print_stats`, `toolhead`, `extruder`, …) is normalized at a
single boundary (`PrinterState::update_from_status()`). Effort scales with how
different the target is from Moonraker, **not** with our codebase size:

- Moonraker-compatible JSON-RPC over WS (same method names): **~3–4 days** — a
  new `IMoonrakerClient` subclass.
- Different method namespaces, same object model, still WS+JSON-RPC: **~7–9
  days** — add a method-name-provider indirection.
- **MQTT/SDCP or HTTP-only REST with no native subscriptions: ~2–3 weeks** —
  this is the relevant tier. The hard parts are (a) a **polling adapter** to
  synthesize `notify_status_update` since the push model is baked into
  `PrinterState`, (b) translating SDCP's numeric cmd codes ↔ our gcode/object
  expectations, and (c) graceful no-op for Moonraker-only features
  (`server.database.*`, the custom `server.helix.print_modified` plugin).
- Plus, for a **vendor** protocol (not a clean SDCP impl), the real cost is
  up-front **reverse-engineering of an undocumented/closed wire format** — that
  bound is the vendor's protocol, not our code.

**Known leak through the abstraction:** `notify_gcode_response` is registered at
~24 scattered call sites (not centralized) — the one place a transport swap
would ripple beyond `src/api/`. Worth centralizing before any such effort.

**Recommendation:** not now. If pursued, target **SDCP first** (documented,
shared across Elegoo's whole line, no auth) as the proof-of-concept for a
non-Moonraker backend, and centralize `notify_gcode_response` handling as
prerequisite cleanup. Gate the whole thing on real demand (Elegoo-owner
interest in the field), not speculation.

---

## 8. The inverse case: community reimplementations of a *vendor's* closed module

Everything above is a **vendor** reimplementing Klipper. The inverse also
happens, and it hits us harder: the **community** reimplementing a *vendor's
closed Klipper module* on top of an otherwise-mainline fork.

**Worked example — Creality K2 Plus CFS.** [`Jacob10383/kalico`](https://github.com/Jacob10383/kalico)
is a fork of KalicoCrew's Kalico for the K2 Plus. Stock Klipper is replaced
wholesale, and Creality's closed CFS module (`box_wrapper.cpython-39.so`) is
replaced by a clean-room `box.py` + friends. The printer keeps its hardware
identity; only the software module changes.

Consequences, in rough order of how much they cost us:

1. **Model detection is useless for this.** Every K2 heuristic still trips —
   `box`, `motor_control`, `fan_feedback`, the hostname. `PrinterDetector`
   reports a stock K2 Plus, correctly, because that *is* the hardware. Anything
   that varies with **firmware** rather than hardware has to be detected from
   the payload or the macro list.
2. **The vendor object keeps its name and changes its entire shape.** The
   reimplemented `box` object shares **zero keys** with Creality's. A parser
   written against the stock schema doesn't error — it finds none of its keys
   and reports an empty system, which surfaces as a working UI showing nothing.
   Silent, not loud.
3. **Schema and command dialect vary independently.** We had been treating
   "which macro dialect" as implied by the printer model (K1 vs K2). The fork
   broke that: new schema *and* new dialect, neither inferable from the model,
   and in principle mix-and-matchable. That forced `CfsSchema` and
   `CfsMacroVariant` apart into two axes.
4. **"Not on GitHub" is not "not published."** The `box.py` here is untracked in
   the Klipper repo and in no public repo, and GitHub code search finds nothing.
   It is nonetheless fetchable: the firmware installs from a content-addressed
   object store (`firmware.jacobean.xyz`) whose index lists every Klipper extra
   by SHA, so one module can be pulled without the 300 MB rootfs. The lesson is
   to **follow the install path**, not the source-hosting convention — and to
   distrust a zero-hit code search, which here reflected a legacy search API
   that simply had not indexed the repo.
   Inferring the command surface from user macros instead got one command
   materially wrong: those macros aliased a `BOX_CHANGE` the module does not
   define, while the real tool change is a registered `T<n>`.

**Design rules this produced:**

- Detect **capabilities from payloads**, model from hardware. Never let a model
  string stand in for a firmware feature.
- When a parser finds none of its expected keys, that is a **signal**, not an
  empty result — log it loudly enough to diagnose from a bundle.
- Keep "what shape is the data" and "what commands does it take" as separate
  axes from the start; they correlate until they don't.
- Read-only support is a legitimate shipping state. Displaying a community
  port's filament system while refusing to drive it beats both extremes — and
  it is the correct state to sit in *while* you go find the module, rather than
  shipping a guessed command set.
- Prefer an **explicit API version** over inferring commands from the status
  layout. This module publishes `api_version`; the `slots[]` structure selects
  the parser, while the version selects the command dialect.

**Footnote worth knowing:** this firmware ships **HelixScreen as its stock
display**, pinned by version and SHA in its installer. Community ports are not
only a compatibility burden — some of them are downstream distributors, and a
parser gap here surfaced as an empty panel on every one of their installs.

Details and the full field mapping: `CREALITY_CFS_K2_INTERNALS.md`
§ "Community Kalico port".

---

## Sources

- [ANYCUBIC-3D/Klipper-go](https://github.com/ANYCUBIC-3D/Klipper-go) · [K3-klipper-mcu](https://github.com/ANYCUBIC-3D/K3-klipper-mcu)
- [jbatonnet/Rinkhals](https://github.com/jbatonnet/Rinkhals) · [PR #422 — Kobra X / K4Pro](https://github.com/jbatonnet/Rinkhals/pull/422/files) · [docs](https://jbatonnet.github.io/Rinkhals/)
- [OpenCentauri](https://github.com/OpenCentauri) · [docs.opencentauri.cc](https://docs.opencentauri.cc/software/)
- [All3DP — Elegoo releases Centauri Carbon firmware code, questions remain](https://all3dp.com/4/elegoo-releases-centauri-carbon-firmware-code-but-questions-remain/)
- [freethecode.lol — Centauri Carbon GPL compliance](https://freethecode.lol/)
- [Kalico (Danger-Klipper rename) — Hackaday](https://hackaday.com/2024/12/11/danger-klipper-fork-renamed-to-kalico/)
- [amken3d/gopper (TinyGo MCU port)](https://github.com/amken3d/gopper)
- GPLv3 anti-tivoization: GPL-3.0 §6 ("Installation Information" / User Products)
- [Klipper API Server (klippy unix socket `/tmp/klippy_uds`)](https://www.klipper3d.org/API_Server.html)
- [Elegoo SDCP — Centauri Carbon implementation](https://github.com/WalkerFrederick/sdcp-centauri-carbon) · [SDCP WebSocket API docs (OpenCentauri)](https://docs.opencentauri.cc/software/api/) · [RemmyLee/carbon dev docs](https://github.com/RemmyLee/carbon)
- [Rinkhals — GoKlipper / gkapi RE](https://jbatonnet.github.io/Rinkhals/firmware/goklipper/) · [Moonraker integration (monkey-patch bridge)](https://deepwiki.com/jbatonnet/Rinkhals/5.2-moonraker-integration)
- [Creality K1/K2 WebSocket — `ha_creality_ws` HA integration](https://github.com/3dg1luk43/ha_creality_ws) · [bessw/moonraker-Creality-K1](https://github.com/bessw/moonraker-Creality-K1)
