<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# LAN Client Authorization

Some printer firmwares put a **pairing broker** in front of their own control
channel. A slicer or phone app that wants in does not get in on network reach
alone: it files a request, and the firmware asks the printer's **own
touchscreen** to approve it. The screen is the second factor.

HelixScreen replaces the stock touchscreen on such machines, which makes it the
thing being asked. Implement nothing and the request is broadcast to nobody:
the firmware waits on a screen that no longer answers, and the client sits on
"requesting connection" until it times out, with nothing on the printer to say
why.

Today exactly one firmware family does this — Snapmaker's, via a Moonraker
component called `client_manager`, which is what admits Snapmaker Orca and the
Snapmaker App on a U1.

## Where the code lives

| File | Role |
|------|------|
| `include/lan_client_authorization.h` / `src/printer/lan_client_authorization.cpp` | **The only place a vendor is named.** Provider table: which notification, how to parse it, what the answer looks like. Pure — no LVGL, no client, fully unit-tested |
| `include/lan_client_auth_router.h` / `src/application/lan_client_auth_router.cpp` | Generic: subscribes, prompts, sends the answer. Names no firmware |
| `tests/unit/test_lan_client_authorization.cpp` | `[lanauth]` |

Owned by `Application` as `m_lan_client_auth_router`, alongside
`GcodeErrorRouter` and `GcodeNarrationRouter`, and torn down with them **before**
`MoonrakerClient` — its destructor unregisters callbacks that touch the client.

## No capability gate, by design

There is no discovery probe, no `PrinterState` flag, and no subscription-builder
entry. **The notification is its own capability probe:** a firmware without a
broker never sends one, so the subscription is inert and costs a map entry.
That keeps discovery, `PrinterState` and the subscription builder free of any
knowledge that this feature exists.

The answer is inherently matched to the asker, too — the decision RPC is only
ever sent in reply to a request that arrived, so it can only reach the firmware
that defined it.

## The handshake

The client talks JSON-RPC to the firmware's broker **over MQTT**; the screen only
ever sees the websocket half.

```
client                      moonraker/client_manager               HelixScreen
  |-- confirm_lan_status ----------->|                                  |
  |<-- "unauthorized" ---------------|                                  |
  |-- request_lan_auth ------------->|                                  |
  |<-- "authorizing" ----------------|                                  |
  |                                  |-- notify_client_access --------->|  WEBSOCKET
  |                                  |                                  |  (prompt)
  |                                  |<-- server.client_manager.approve |
  |                    mints the client an mTLS cert, stores it         |
  |<-- cert + key + CA over MQTT ---------------------------------------|
  |-- reconnects to the broker over mTLS -->  paired.                   |
```

`notify_client_access` on the wire — Moonraker derives the notify name from the
component's event as `notify_` + `event_name.split(':')[-1]`, and wraps the
payload in the params array:

```json
{"jsonrpc":"2.0","method":"notify_client_access",
 "params":[{"id":"0","clientid":"orca-<uuid>","app_id":"orca-1787643423061664"}]}
```

The `clientid` prefix (`orca-` / `app-`) is the **only** thing in the payload
that says who is asking, which is why the prompt names the product from it.

## Things that are quietly dangerous to get wrong

- **Never send a null `clientid`.** Moonraker's `get_str` is `str(val)`, so a
  JSON null authorizes a client literally named `"None"`. A request that
  arrives without one is dropped, never answered.
- **`id` may arrive as a number.** The stock screen formats it back with an
  unquoted `"id":%s`, so the field round-trips as a JSON number even though the
  notification declares it a string. Both shapes are read.
- **Denying must go on the wire.** It is what turns the client's silent
  ~70-second wait into an immediate refusal. Silence is not a denial.
- **Answering late is safe and useful.** The firmware stores the minted
  credentials, so a client that already gave up picks them up on its next
  attempt. Nothing expires a pending request.
- **Re-approving an existing client is idempotent** — the component
  short-circuits and re-publishes the stored credentials.
- **The prompt can vanish without a button.** `Modal::rebuild_top` hides a
  non-rebuildable dialog on a breakpoint or theme change, so the router tracks
  `LV_EVENT_DELETE` rather than assuming a button caused the teardown. Without
  that, one dismissal would wedge the gate shut against every later request.
- **A Deny suppresses re-prompting for one minute — deliberately.** A denied
  client never enters the firmware's registry, so every reconnect of the
  running slicer files a fresh request and the screen would re-prompt forever.
  `decide(false)` records the client for
  `lan_auth::denial_suppression_window` (60s); repeats within the window are
  dropped with a log line. This is NOT the wedge above: it keys on an explicit
  Deny only (a dismissal answered nothing and leaves the gate open), it is
  per-client, and it expires. An approval clears that client's entry.

## Threading

The notification arrives on the **WebSocket thread**. Registration wraps the
handler in `lifetime_.bg_cb(...)`, which hops to the main thread and re-checks a
generation snapshot on the way in — building a modal straight from the
WebSocket thread would be the invariant-1 crash. See `THREADING.md`.

The modal's button callbacks take a raw `void*`, and `Application` *replaces*
the router on a printer switch. Each callback re-checks its `user_data` against
the active instance, so a click — or the `DELETE` that arrives after the exit
animation — landing on a prompt whose owner is gone does nothing.

## Not covered: the cloud account path

Snapmaker's component has a second, separate flow for binding a **cloud account**
rather than a LAN client: `notify_user_access` / `refresh_pin_code`, surfaced on
the stock screen as a rotating PIN under "Mobile Login". It depends on the cloud
MQTT agent being connected, plus region checks and account login, and is a much
larger job than the LAN approval.

It is not needed to pair Orca or the phone app: while the printer is in LAN mode
(`link_mode: 1`) **both** come in over the LAN path — a request from the phone
app arrives as `request_lan_auth` with an `app-` client id, not as a cloud user.

The stock screen also shows the LAN **access code** under "Desktop Login", which
the user types into Orca to start pairing. HelixScreen has no equivalent surface
yet; on current firmware the code is pinned to `12345678`, so this is a
discoverability gap rather than a blocker.

## Adding a firmware

Add one `Provider` row to the table in `lan_client_authorization.cpp`:
notification method, decision method, a parse function, and a params builder.
No call site changes.

## Testing it against a real printer

Only an **unbound** client triggers the prompt — a previously-paired slicer
reconnects silently. On a Snapmaker U1, the bound-client registry is
`<printer>:/home/lava/printer_data/mqtt/client.json`; back it up and clear the `clients`
object to re-trigger pairing, or pair from a fresh client.

Watch the firmware side while you do it:

```bash
ssh root@<printer-ip> 'tail -f /home/lava/printer_data/logs/moonraker.log | grep client_manager'
```

Unanswered looks like this — the request arrives and nothing follows:

```
LAN auth request from clientid: app-<uuid>, app_id: app-<n>
notify screen new client access request
```

Answered adds `approve connection: ... approve: 1` and an `ack topic[...]` line.
