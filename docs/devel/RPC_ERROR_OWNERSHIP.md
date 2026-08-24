# RPC Error Ownership

Who tells the user that a JSON-RPC call failed.

One Klipper rejection can reach the screen through three unrelated code paths. Exactly one of
them must speak: zero is a silent failure, two is a stack of duplicate toasts on top of the
same event. This document is the contract for deciding which one, and the rule that makes the
decision computable at the call site.

The decision itself is one function — `helix::rpc_error_policy::decide()`
(`include/rpc_error_policy.h:87-121`). Nothing else may re-derive it.

---

## The three surfaces

| # | Surface | Where it fires | What it can do |
|---|---------|----------------|----------------|
| 1 | **The caller's own error callback** | Wherever the caller passed `on_error` | Whatever it wants — a contextual toast next to the button the user pressed, a modal, a step-model failure state |
| 2 | **The Request Tracker's generic fallback** | `MoonrakerRequestTracker::route_response()`, `src/api/moonraker_request_tracker.cpp:226-232` — emits `MoonrakerEventType::RPC_ERROR` as `"Printer command '<method>' failed: <msg>"` | One toast, raw method name and raw Klipper text. The last resort |
| 3 | **`GcodeErrorRouter`**, from Klipper's `!!` broadcast | `src/application/gcode_error_router.cpp` `process_line()` → `decide_presentation()` (`:179-186`) | The richest surface: classifies severity, hands the line to every AMS backend's `classify_error()` first, and escalates a CRITICAL fault to a recovery modal with buttons |

Surfaces 1 and 2 both hang off the JSON-RPC error reply. Surface 3 comes down an entirely
different transport — `notify_gcode_response` — from the same Klipper dispatcher call,
milliseconds apart. Neither transport can see the other, which is why the tie is broken by a
recorded intent plus a short correlation window rather than by ordering.

**Surface 3 outranks surface 2 for `printer.gcode.script`.** When the failure will be
broadcast as `!!`, the generic fallback stands down entirely. The router's report is strictly
better: it knows severity, it lets the backend that owns the hardware interpret the line
first, and mid-print it can put a Resume / Unload / Reset dialog on screen instead of a
sentence. Only `printer.gcode.script` has that channel —
`rpc_error_policy::method_has_broadcast_channel()` (`include/rpc_error_policy.h:64-66`) keeps
the method-name literal in one place. Every other RPC (`server.files.*`, `machine.*`,
`printer.objects.subscribe`, …) has no second channel, so for those the generic fallback is
the only thing standing between a failure and silence.

---

## The decision

```cpp
struct CallerIntent {
    bool silent          = false;  // caller opted out of ALL automatic error UI
    bool surfaces_errors = false;  // caller's on_error actually SHOWS a human something
};

struct RequestFacts {
    bool has_broadcast_channel = false;  // Klipper will also emit `!!`
    bool suppress_all          = false;  // global mute (teardown/shutdown)
};

struct Decision {
    bool emit_generic_toast;  // surface 2 fires
    bool record_for_dedup;    // surface 3 stands down for a matching `!!`
};
```

`CallerIntent` is what the caller promised; `RequestFacts` is what is true about the request
regardless of anyone's promise. Keeping them apart is what stops a caller from being able to
assert its way past `suppress_all`, and stops the tracker from inferring a promise from a
method name.

### The matrix

With `suppress_all = false`:

| `silent` | `surfaces_errors` | `has_broadcast_channel` | `emit_generic_toast` | `record_for_dedup` | Who reports |
|---|---|---|---|---|---|
| false | false | false | **yes** | yes | Surface 2 — the generic fallback |
| false | false | **true** | no | no | Surface 3 — `GcodeErrorRouter` |
| false | **true** | false | no | yes | Surface 1 — the caller |
| false | **true** | **true** | no | yes | Surface 1; surface 3 dedups against the record |
| **true** | false | false | no | no | Nobody — deliberate, the caller opted out |
| **true** | false | **true** | no | no | Surface 3 — `silent` mutes *us*, not Klipper |
| **true** | **true** | false | no | yes | Surface 1 |
| **true** | **true** | **true** | no | yes | Surface 1; surface 3 dedups |

`suppress_all = true` short-circuits to `{false, false}` for every input. Nothing is surfaced
*and* nothing is recorded: the router is muted by the same shutdown flag, and a record written
during teardown would sit in the 1.5 s correlation window past a reconnect and eat the first
real error after recovery.

Two properties are worth reading off the table directly:

- **`emit_generic_toast` is false whenever anyone else might speak.** `caller_owns_report =
  silent || surfaces_errors`; the fallback fires only when that is false *and* there is no
  broadcast channel.
- **`record_for_dedup` is `surfaces_errors || emit_generic_toast`** — "someone definitely
  showed this to a human", never "someone might have".

---

## `silent` does not mean the user was told

`silent` means *no automatic toast from us*. It is what internal probes, polls, and long AMS
macros with out-of-band completion tracking pass so the tracker does not narrate their
plumbing. It says nothing about whether a human learned anything.

So a merely-`silent` request records **nothing** for dedup. If it did, the `!!` copy — which
in the `printer.gcode.script` case is the only remaining signal — would be suppressed for a
failure nobody ever saw. `include/rpc_error_correlation.h:26-29` states the same rule from the
ledger's side.

The corollary catches people out: `silent = true` on a `gcode.script` send is not a way to
keep a failure quiet. Klipper still broadcasts `!!`, the router still reports it, and the
user still gets a toast — just not one worded by us. Suppressing the report entirely is not
something a caller can ask for.

---

## Capture the intent *before* any wrapper

**`CallerIntent` must be built from the caller's own callbacks, at the point they arrive, and
before any internal code wraps them.**

Several layers wrap `on_error` unconditionally — to settle an in-flight counter, to reset an
action to IDLE, to marshal a background callback to the main thread. After any of those, the
callback we hold is non-null for *every* call, including the many that passed `nullptr`.
Deriving `surfaces_errors` at that point reads our own bookkeeping as a promise the caller
never made, and silences surface 3 for an error that then reaches nobody.

The reference shape is `helix::ensure_homed_then()`
(`src/printer/toolhead_homing.cpp:60-86`):

```cpp
// Capture the caller's error-reporting intent BEFORE on_error is moved into
// the wrapper below.
const bool caller_surfaces = (on_error != nullptr);

api->execute_gcode("G28",
                   guard.bg_cb("ensure_homed_then::g28_done",  /* … */),
                   guard.bg_cb("ensure_homed_then::g28_error",
                               [on_error = std::move(on_error)](const MoonrakerError& err) {
                                   spdlog::warn("[ensure_homed_then] G28 failed: {}", err.message);
                                   if (on_error) { on_error(err); }
                               }),
                   IMoonrakerAPI::HOMING_TIMEOUT_MS, /*silent=*/false,
                   /*on_queued=*/nullptr,
                   /*caller_surfaces_errors=*/caller_surfaces);
```

The `bg_cb` wrapper is always non-null and always logs. Read after the `std::move`, every G28
in the tree would claim to surface its own errors; read before it, only the calls that really
passed a callback do.

The same capture-then-forward appears at every layer that wraps:

| Site | Captured at |
|------|-------------|
| `MoonrakerAPI::execute_gcode()` | `src/api/moonraker_api_controls.cpp:373-375` — before the activity-counter wrapping |
| `MoonrakerMotionAPI::execute_gcode()` | `src/api/moonraker_motion_api.cpp:401-403` — also the site that sets `silent = (on_error != nullptr)` |
| `AmsSubscriptionBackend::dispatch_payload()` | `src/printer/ams_subscription_backend.cpp:529-533` — `caller_surfaces_errors.value_or(on_error != nullptr)` |
| `AmsSubscriptionBackend::ensure_homed_then()` | `src/printer/ams_subscription_backend.cpp:446-449` — the G28 leg forwards the *caller's* answer, not the wrapper's |
| `LedController::send_led_command()` / strobe | `src/led/led_controller.cpp:1023`, `:1050` — `caller_surfaces_errors && (on_error != nullptr)` |

### The parameter

`caller_surfaces_errors` is the trailing parameter on the sends that can reach
`printer.gcode.script`:

| Method | Declared |
|--------|----------|
| `execute_gcode()` | `include/i_moonraker_api.h:206-213` |
| `set_temperature()` | `include/i_moonraker_api.h:176-181` |
| `set_led()` | `include/i_moonraker_api.h:190-196` |
| `set_strobe_frequency()` | `include/i_moonraker_sub_apis.h:348-353` |

It defaults to `true`, which is right for the common case — a UI callback that toasts. **A
callback that only `spdlog`s, only resets internal state, or does both must pass `false`.**
`AmsSubscriptionBackend::handle_dispatch_error()`
(`src/printer/ams_subscription_backend.cpp:480-493`) is exactly that shape by default: with no
caller `on_error` it logs and sets `AmsAction::IDLE`, which no user sees.

Direct `send_jsonrpc()` callers do not pass an intent at all. The tracker infers
`CallerIntent{silent, error_cb != nullptr}` for them
(`src/api/moonraker_request_tracker.cpp:41`), which is correct there: those are non-gcode RPCs
with no `!!` channel, so "supplied a callback" and "will report it" amount to the same thing.
The gcode paths, which *do* have a second channel, always state their intent explicitly.

---

## What the record does

`Decision::record_for_dedup` writes the raw Klipper `error.message` into
`helix::rpc_error_correlation` (`src/api/moonraker_request_tracker.cpp:250-252`). When the
matching `!!` line arrives, `already_reported_via_rpc()`
(`src/application/gcode_error_router.cpp:173-177`) checks it and the router stays quiet.

Details that matter when reading that code:

- **Exact-string match, 1.5 s window** (`src/api/rpc_error_correlation.cpp:23`). The two
  channels are causally tied — the window absorbs network jitter, nothing more. Substring
  matching would mask unrelated errors sharing a phrase.
- **The recorded identity is Klipper's raw wording**, before `clean_error_text()` rewrites it.
  The router therefore looks up `raw_detail` first and falls back to `detail`, so a message
  the cleaner touched ("Must home axis first" → "Must home axes first") still matches.
- **Only the router's plain-TOAST arm defers and re-checks.** `present_deferred_toast()`
  (`src/application/gcode_error_router.cpp:323-345`) re-runs the lookup when its timer fires,
  which is what catches an RPC reply that lands *after* the `!!` line. A CRITICAL modal fires
  immediately and does not defer — which is the other half of why the generic fallback must
  stand down for `gcode.script` unconditionally rather than racing.

Do not confuse this ledger with `fault_surface_correlation`
(`src/application/fault_surface_correlation.cpp`, 3 s). They are different pairs: this one
dedups RPC↔`!!`; that one dedups the AMS error channels A↔B. See
`FILAMENT_MANAGEMENT.md` § "Two error channels".

---

## The residual gap

One case reports nothing, knowingly:

> A `printer.gcode.script` request that fails with a JSON-RPC error which Klipper does **not**
> mirror as a `!!` broadcast — a Moonraker-level rejection rather than a gcode-dispatcher one
> — whose caller passed `caller_surfaces_errors = false`.

`has_broadcast_channel` is keyed on the method, not on whether Klipper actually broadcast, so
the generic fallback stands down and the promised third surface never speaks.

That set is small because the guards in front of the send reject those conditions *before* a
request is ever tracked: `MoonrakerAPI::execute_gcode()` refuses gcode while Klippy is
`SHUTDOWN`/`ERROR` (`src/api/moonraker_api_controls.cpp:228-244`), refuses app-initiated
homing during an active print, and refuses discretionary motion behind a blocking op;
`MoonrakerMotionAPI::execute_gcode()` is stricter still and refuses any motion unless Klippy is
`READY` (`src/api/moonraker_motion_api.cpp:406-419`). Each of those calls the caller's
`on_error` and returns without a request, so it never reaches this decision at all.

Closing the gap properly means knowing at reply time whether the `!!` actually came — a
reverse correlation window with a timer, i.e. the opposite of the one that exists. That was
judged not worth its complexity against the residue above. If a bundle ever shows a
`gcode.script` failure with no user-visible report and no `!!` in the log, this is the shape to
look for.

---

## The gate

`scripts/check_gcode_error_ownership.py`, run at `--max-allowed 0` from
`scripts/quality-checks.sh:1311-1336`.

It scans `src/` for `execute_gcode()` calls with an inline `[…](const MoonrakerError&)` lambda
whose body is *entirely* logging or empty, and which pass neither `caller_surfaces_errors` nor
an annotation. That is the false claim the whole policy turns on, and it is invisible in
review because the call site looks handled — there *is* an error callback, it just writes to a
log.

Detection is deliberately conservative. Any mention of a notification, modal, or subject
(`UI_MARKERS` in the script) means the callback plausibly reaches a human and the site is left
alone; a gate that cries wolf gets switched off and then protects nothing. Non-gcode RPCs are
out of scope entirely — they have no second channel, so the fallback is their only surface and
this rule does not apply. `*_mock.*` files are skipped, matching
`tests/shell/test_code_lint.bats`.

**Passing `caller_surfaces_errors` with any value silences the gate**, because supplying it is
itself the deliberate choice. For the rare site where the parameter cannot be threaded, the
escape hatch is an annotation in the call or within ~400 characters above it:

```cpp
// ERROR_OWNERSHIP_OK: cleanup failure is expected; router must stay quiet
```

Behaviour is pinned by `tests/unit/test_rpc_error_policy.cpp` (the matrix),
`tests/unit/test_gcode_error_ownership.cpp` and `tests/unit/test_gcode_error_ownership_more.cpp`
(end-to-end surface selection), and `tests/unit/test_rpc_error_correlation_normalization.cpp`
(the raw-vs-cleaned lookup identity).

---

## Adding a new send: the checklist

1. Does it reach `printer.gcode.script`? If yes, the `!!` router is a live surface and
   everything below applies. If not, the generic fallback is the only surface — leave it
   alone.
2. Capture `caller_surfaces_errors` from the caller's `on_error` **before** wrapping anything.
3. Pass `false` when the callback only logs or only resets internal state. A `spdlog::warn` is
   not a report.
4. Use `silent` only to mean "no automatic toast from us". Do not reach for it to hide a
   failure — it cannot.
5. If the send is behind a helper that takes its own callbacks, thread the caller's answer
   through rather than re-deriving it (`std::optional<bool>` is the pattern
   `AmsSubscriptionBackend` uses, `include/ams_subscription_backend.h:111-123`).
6. Never re-implement the matrix. Call `rpc_error_policy::decide()` — the mock's inline gcode
   path does exactly that (`src/api/moonraker_client_mock_print.cpp:91-121`) so its behaviour
   cannot drift from the real tracker's.

## What was measured on hardware

The policy above is enforced by unit tests, and flipping `caller_surfaces_errors` back to
`true` in any of the fixed sites makes the dedup record appear and `GcodeErrorRouter` go quiet
— the mechanism is real and mutation-proven at that level.

It has **not** been shown to change what a user sees. On a CB1 / BoxTurtle, `LANE_UNLOAD` and
`SET_LED` were each shadowed by a macro calling `action_raise_error` (so the failure is raised
before any hardware acts), and the send was driven from the UI through two of the log-only
sites this policy governs — `NativeBackend::set_color` and `AmsBackendAfc::dispatch_lane_unload`.
Both surfaced a toast. **The same test on v0.99.107, which predates the policy, also surfaced a
toast on both paths.** No difference was observable.

Why v0.99.107 did not suppress is unresolved: it carries the dedup commit (`bdf32d07d`) and its
AFC dispatch does pass a non-null error callback, so by the model above it should have recorded
and muted the router. It did not, and that build has no usable logging to say why.

Two things follow, and they are the reason this section exists:

- Do not describe this policy as fixing a user-visible silent failure. What it demonstrably
  fixes is intent derived *after* an internal wrapper (see above), which is a real defect
  independent of whether the dedup engages.
- If you are about to rely on the dedup actually firing — for instance by declaring
  `caller_surfaces_errors=true` to deliberately suppress the `!!` copy — verify it on hardware
  rather than from this document. The one time it was measured, it did not behave as the code
  says it should.

## Related

- `include/rpc_error_policy.h` — the contract, and the four drifted copies it replaced
- `include/rpc_error_correlation.h` — the RPC↔`!!` ledger
- `MOONRAKER_ARCHITECTURE.md` § "MoonrakerAPI (Domain Logic Layer)" — `execute_gcode()`'s three
  dispositions and the `on_queued` contract
- `FILAMENT_MANAGEMENT.md` § "Two error channels" — the AMS Channel A / Channel B pair that
  sits downstream of `GcodeErrorRouter`
- `LED_CONTROL.md` § the `toggle_all()` settle triple — a per-branch example of declaring both
  `on_queued` and error ownership
- `REVIEW_RUBRIC.md` § "Tier 2 — Silent failures"
