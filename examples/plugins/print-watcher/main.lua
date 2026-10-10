-- Print Watcher: print-event notifications to ntfy, Discord, Telegram or a
-- generic webhook. Edges of print_state become events; enabled, unmuted
-- events are queued and posted one at a time (the sandbox allows two
-- in-flight requests; a queue never races that limit). Settings drive the
-- service, destination and per-event toggles; the overlay's switches write
-- back through helix.settings.set.

local muted = helix.subject.int("muted", 0)
local compact = helix.subject.int("compact", 0)
local room = helix.subject.int("room", 1)
local word_vis = helix.subject.int("word_vis", 1)
local has_events = helix.subject.int("has_events", 0)
local event_zone = helix.subject.int("event_zone", 0)
local log_zone = helix.subject.int("log_zone", 0)
local dest_row_on = helix.subject.int("dest_row_on", 0)
local sev = helix.subject.int("sev", 0)
local word = helix.subject.string("word", "armed")
local status_text = helix.subject.string("status_text", "")
local last_event = helix.subject.string("last_event", "")
local last_file = helix.subject.string("last_file", "")
local dest_line = helix.subject.string("dest_line", "")
local dest_hint = helix.subject.string("dest_hint", "")

local LOG_ROWS = 5
local log_on, log_ev, log_det = {}, {}, {}
for i = 1, LOG_ROWS do
    log_on[i] = helix.subject.int("log" .. i .. "_on", 0)
    log_ev[i] = helix.subject.string("log" .. i .. "_ev", "")
    log_det[i] = helix.subject.string("log" .. i .. "_det", "")
end

local EVENTS = { "started", "resumed", "paused", "completed", "cancelled", "failed" }
local ev_on = {}
for _, e in ipairs(EVENTS) do
    ev_on[e] = helix.subject.int("ev_" .. e, helix.settings.get("ev_" .. e) == false and 0 or 1)
end

-- Settings mirrors: on_change pushes every accepted write back onto the
-- subjects, so the tile and the overlay switches always show the persisted
-- truth, whichever surface made the change.
local t = helix.i18n.t

local function mirror_settings()
    muted:set(helix.settings.get("muted") == true and 1 or 0)
    word:set(t(helix.settings.get("muted") == true and "muted" or "armed"))
    for _, e in ipairs(EVENTS) do
        ev_on[e]:set(helix.settings.get("ev_" .. e) == false and 0 or 1)
    end
    local service = helix.settings.get("service") or "ntfy"
    local dest = helix.settings.get("dest") or ""
    dest_line:set(service .. " \u{00B7} " .. (dest ~= "" and dest or t("(not set)")))
    local hints = {
        ntfy = "A topic name (my-printer), or a full URL to a self-hosted server",
        Discord = "A webhook URL: https://discord.com/api/webhooks/...",
        Telegram = "Bot token and chat id as token@chatid",
        Webhook = "Any http(s) URL that accepts a JSON POST",
    }
    dest_hint:set(t(hints[service] or ""))
end

for _, key in ipairs({ "muted", "ev_started", "ev_resumed", "ev_paused", "ev_completed",
                       "ev_cancelled", "ev_failed", "service", "dest", "include_filename" }) do
    helix.settings.on_change(key, function(_) mirror_settings() end)
end

-- The recent-events ring. No wall clock in the sandbox, so an entry is the
-- event, the file and the progress at the moment it fired, plus its delivery
-- result once the post settles.
local ring = {}
local tile_cols, tile_rows = 1, 1

-- The event zone beside the bell needs something in it: before the first
-- event (or after a reload with an empty ring) the bell centers across the
-- whole tile instead of hugging a blank half.
local function apply_event_zone()
    event_zone:set(tile_cols >= 2 and tile_rows == 1 and 1 or 0)
end

local function refresh_log()
    last_event:set(ring[1] and ring[1].title or "")
    last_file:set(ring[1] and ring[1].file or "")
    for i = 1, LOG_ROWS do
        local e = ring[i]
        if e then
            log_on[i]:set(1)
            log_ev[i]:set(t(e.title))
            local det = e.file
            if e.pct then det = det .. " \u{00B7} " .. e.pct .. "%" end
            if e.result then det = det .. " \u{00B7} " .. t(e.result) end
            log_det[i]:set(det)
        else
            log_on[i]:set(0)
        end
    end
    has_events:set(ring[1] and 1 or 0)
    apply_event_zone()
end

local function push_ring(e)
    table.insert(ring, 1, e)
    if #ring > LOG_ROWS then ring[LOG_ROWS + 1] = nil end
end

-- One request in flight; the queue drains as each post settles.
local queue = {}
local sending = false

local function set_status(s, text)
    sev:set(s)
    status_text:set(t(text))
end

-- Destination shape per service, checked before anything reaches the
-- network: a bad destination is a status line, never a G-code-shaped
-- surprise or a POST to a half-parsed URL.
local function service_target()
    local service = helix.settings.get("service") or "ntfy"
    local dest = helix.settings.get("dest") or ""
    if type(dest) ~= "string" or #dest == 0 or #dest > 512 then
        return nil, "destination not set"
    end
    if service == "ntfy" then
        local url = dest:match("^https?://") and dest or ("https://ntfy.sh/" .. dest)
        return { url = url, kind = "ntfy" }
    elseif service == "Discord" then
        if not dest:match("^https://discord%.com/api/webhooks/") and
           not dest:match("^https://discordapp%.com/api/webhooks/") then
            return nil, "not a Discord webhook URL"
        end
        return { url = dest, kind = "content" }
    elseif service == "Telegram" then
        local token, chat = dest:match("^([^@]+)@(.+)$")
        if not token or not chat or not token:match("^%d+:[%w_-]+$") then
            return nil, "want token@chatid"
        end
        return { url = "https://api.telegram.org/bot" .. token .. "/sendMessage",
                 chat = chat, kind = "chat" }
    elseif service == "Webhook" then
        if not dest:match("^https?://") then
            return nil, "want an http(s) URL"
        end
        return { url = dest, kind = "json" }
    end
    return nil, "unknown service"
end

local function message_for(e)
    -- A composed event reads "Event: file (pct)"; the test button's message
    -- already says what it is, so it carries no event-word prefix.
    if e.raw then return t(e.raw) end
    local file = helix.settings.get("include_filename") ~= false and e.file or ""
    local what = t(e.title)
    if file ~= "" then what = what .. ": " .. file end
    if e.pct then
        what = what .. string.format(" (%d%%)", e.pct)
    end
    return what
end

local function drain()
    if sending then return end
    local e = table.remove(queue, 1)
    if not e then return end
    local target, err = service_target()
    if not target then
        e.result = "bad dest"
        push_ring(e)
        refresh_log()
        set_status(2, err or "bad destination")
        -- Keep draining: one bad destination must not wedge later events.
        return drain()
    end
    sending = true
    local msg = message_for(e)
    local body, headers
    if target.kind == "ntfy" then
        body = msg
        headers = { ["X-Title"] = t(e.title) }
    elseif target.kind == "content" then
        body = helix.json.encode({ content = msg })
        headers = { ["Content-Type"] = "application/json" }
    elseif target.kind == "chat" then
        body = helix.json.encode({ chat_id = target.chat, text = msg })
        headers = { ["Content-Type"] = "application/json" }
    else
        body = helix.json.encode({ event = e.key, filename = e.file, progress = e.pct })
        headers = { ["Content-Type"] = "application/json" }
    end
    local res, perr = helix.http.post(target.url, { body = body, headers = headers })
    sending = false
    if res and res.status and res.status >= 200 and res.status < 300 then
        e.result = "sent"
        set_status(1, "sent")
    else
        e.result = "failed"
        local why = perr or ((res and res.status) and ("HTTP " .. res.status) or "no answer")
        set_status(2, why)
        helix.log.warn("notify failed: " .. why)
    end
    push_ring(e)
    refresh_log()
    if #queue > 0 then drain() end
end

local function enqueue(e)
    queue[#queue + 1] = e
    drain()
end

-- Edge detection over the six states the screen reports; each interesting
-- edge is one event.
local TITLES = {
    started = "Started", resumed = "Resumed", paused = "Paused",
    completed = "Completed", cancelled = "Cancelled", failed = "Failed",
}

local function classify(prev, cur)
    if cur == prev then return nil end
    if cur == "printing" then
        return prev == "paused" and "resumed" or "started"
    end
    if cur == "paused" then return "paused" end
    if cur == "complete" then return "completed" end
    if cur == "cancelled" then return "cancelled" end
    if cur == "error" then return "failed" end
    return nil
end

local prev_state = helix.printer.get("print_state")
helix.printer.watch("print_state", function(cur)
    local ev = classify(prev_state, cur)
    prev_state = cur
    if not ev then return end
    if helix.settings.get("ev_" .. ev) == false then return end
    if helix.settings.get("muted") == true then return end
    enqueue({
        key = ev,
        title = TITLES[ev],
        file = helix.printer.get("filename") or "",
        pct = helix.printer.get("progress") or nil,
    })
end)

helix.widget("tile", {
    on_size = function(cols, rows, w, hgt)
        compact:set(w < 190 and 1 or 0)
        -- a micro-height tile cannot stack the word under the glyph or the
        -- filename under the event; the glyph carries the identity alone
        room:set(hgt >= 80 and 1 or 0)
        -- the word under the bell only on a compact 1x1: wide tiers carry it
        -- in the header, micro-height tiers have no room for it
        word_vis:set((w < 190 and hgt >= 80) and 1 or 0)
        tile_cols, tile_rows = cols, rows
        if rows >= 2 then
            log_zone:set(1)
            local n = (cols >= 4) and LOG_ROWS or 3
            for i = 1, LOG_ROWS do
                log_on[i]:set(i <= n and (ring[i] ~= nil) and 1 or 0)
            end
            dest_row_on:set(cols >= 4 and 1 or 0)
        else
            log_zone:set(0)
            dest_row_on:set(0)
        end
        refresh_log()
    end,
})

helix.ui.on("open", function()
    helix.ui.overlay("print-watcher__detail", { title = "Print Watcher" })
end)

helix.ui.on("test", function()
    enqueue({ key = "test", title = "Test", file = "Print Watcher test",
              raw = "Print Watcher test", pct = nil })
end)

-- A language change re-renders every subject-carried string; XML literals
-- re-resolve through their translation tags on their own.
helix.i18n.on_change(function()
    mirror_settings()
    refresh_log()
end)

-- A switch tap only announces itself; Lua cannot read widget state, so the
-- handler flips the setting it mirrors and lets on_change snap the switch.
helix.ui.on("flip", function(arg)
    local now = helix.settings.get(arg)
    if type(now) ~= "boolean" then return end
    local ok, err = pcall(helix.settings.set, arg, not now)
    if not ok then
        helix.log.warn("set " .. arg .. " failed: " .. tostring(err))
    end
end)

mirror_settings()
refresh_log()
