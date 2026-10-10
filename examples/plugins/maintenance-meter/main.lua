-- Maintenance Meters: hour meters for consumable parts. Elapsed hours
-- accrue from print time, persist in the plugin store, and survive
-- restarts. Intervals come from settings; an interval of 0 hides that
-- meter everywhere.

local METERS = {
    { key = "filter", name = "HEPA filter", short = "HEPA", setting = "filter_interval_h" },
    { key = "nozzle", name = "Nozzle", short = "Nozzle", setting = "nozzle_interval_h" },
    { key = "rails", name = "linear rails", short = "Rails", setting = "rails_interval_h" },
}

-- One accrual tick per minute of printing; the store write rides the
-- host debounce, so this is at most one small write per minute.
local TICK_MS = 60 * 1000
local TICK_H = TICK_MS / 3600000

local compact = helix.subject.int("compact", 0)
local list_zone = helix.subject.int("list_zone", 0)
local inner = helix.subject.int("inner", 1)
local bars_visible = helix.subject.int("bars", 0)
local big_value = helix.subject.string("big_value", "--")
local big_name = helix.subject.string("big_name", "")
local total_line = helix.subject.string("total_line", "Total print time on this screen: 0h")

local m_on, m_line, m_note, m_left = {}, {}, {}, {}
local l_name, l_hours = {}, {}
for i, m in ipairs(METERS) do
    m_on[i] = helix.subject.int("m" .. i .. "_on", 0)
    m_line[i] = helix.subject.string("m" .. i .. "_line", "")
    m_note[i] = helix.subject.string("m" .. i .. "_note", "")
    m_left[i] = helix.subject.string("m" .. i .. "_left", "")
    l_name[i] = helix.subject.string("l" .. i .. "_name", m.short)
    l_hours[i] = helix.subject.string("l" .. i .. "_hours", "")
end

-- Interval from settings; runtime state from the store.
local total_h = 0.0
for _, m in ipairs(METERS) do
    m.interval = helix.settings.get(m.setting) or 0
    m.used = 0.0
    m.reset_at = nil
    m.notified = false
end

local function load_state()
    local saved = helix.storage.get("state")
    if type(saved) ~= "table" then return end
    total_h = tonumber(saved.total_h) or 0.0
    local stored = saved.meters
    if type(stored) ~= "table" then return end
    for _, m in ipairs(METERS) do
        local s = stored[m.key]
        if type(s) == "table" then
            m.used = tonumber(s.used) or 0.0
            m.reset_at = s.reset_at
            m.notified = s.notified == true
        end
    end
end

local function save_state()
    local saved = { total_h = total_h, meters = {} }
    for _, m in ipairs(METERS) do
        saved.meters[m.key] = { used = m.used, reset_at = m.reset_at, notified = m.notified }
    end
    helix.storage.set("state", saved)
end

local function enabled(m) return m.interval > 0 end

local function frac(m) return math.min(1, m.used / m.interval) end

local function worst()
    local w = nil
    for _, m in ipairs(METERS) do
        if enabled(m) and (w == nil or frac(m) > frac(w)) then w = m end
    end
    return w
end

local function state_color(m)
    local f = frac(m)
    if f >= 1 then return "danger" end
    if f >= 0.75 then return "warning" end
    return "primary"
end

local function hours_left(m)
    return string.format("%dh", math.max(0, math.ceil(m.interval - m.used)))
end

local function h(n) return string.format("%dh", math.floor(n + 0.5)) end

-- The progress arc as 10-degree segments from 12 oclock (270 degrees in
-- screen space) clockwise, one degree of overlap closing each seam.
local function ring(c, cx, cy, r, f, col, width)
    width = width or 6
    c:arc(cx, cy, r, 0, 360, { color = "border", width = width })
    if f <= 0.005 then return end
    local total = math.floor(360 * f + 0.5)
    local drawn, a = 0, 270
    while drawn < total do
        local step = math.min(10, total - drawn)
        local b = a + step
        local na, nb = a % 360, b % 360
        if nb == 0 then nb = 360 end
        if nb > na then
            c:arc(cx, cy, r, math.max(0, na - 1), math.min(360, nb + 1), { color = col, width = width })
        else
            c:arc(cx, cy, r, math.max(0, na - 1), 360, { color = col, width = width })
            c:arc(cx, cy, r, 0, math.min(360, nb + 1), { color = col, width = width })
        end
        a, drawn = b, drawn + step
    end
end

local gauge = helix.canvas("gauge")
local bars = helix.canvas("bars")

-- The value only fits inside the ring once the ring is large enough;
-- below the floor the number is dropped and the caption keeps the name.
local RADIUS_FLOOR = 24

local function refresh_derived()
    for i, m in ipairs(METERS) do
        m_on[i]:set(enabled(m) and 1 or 0)
        if enabled(m) then
            m_line[i]:set(string.format("%dh / %dh", math.floor(m.used + 0.5), m.interval))
            local pct = math.floor((1 - frac(m)) * 100 + 0.5)
            local when = m.reset_at and (" - reset at " .. h(m.reset_at)) or ""
            m_note[i]:set(string.format("%d%% left%s", pct, when))
            m_left[i]:set(hours_left(m))
            l_hours[i]:set(hours_left(m))
        end
    end
    total_line:set("Total print time on this screen: " .. h(total_h))
    local w = worst()
    big_value:set(w and hours_left(w) or "--")
    big_name:set(w and w.name or "")
end

local function render()
    local c = gauge
    local w, hgt = c:size()
    if w > 8 and hgt > 8 then
        local r = math.floor(math.min(w, hgt * 0.66) / 2) - 6
        if r < 12 then r = 12 end
        inner:set(r >= RADIUS_FLOOR and 1 or 0)
        -- the stroke scales down with the ring so a micro cell does not
        -- carry a band sized for a dashboard tier
        local sw = 6
        if r < 15 then
            sw = 3
        elseif r < 24 then
            sw = 4
        end
        local m = worst()
        if m then
            ring(c, math.floor(w / 2), math.floor(hgt / 2), r, frac(m), state_color(m), sw)
        else
            ring(c, math.floor(w / 2), math.floor(hgt / 2), r, 0, "primary", sw)
        end
    end
    c:commit()
end

local function render_bars()
    local c = bars
    local w, hgt = c:size()
    if w > 8 and hgt > 8 then
        local live = {}
        for _, m in ipairs(METERS) do
            if enabled(m) then live[#live + 1] = m end
        end
        local cellw = math.floor(w / math.max(1, #live))
        for i, m in ipairs(live) do
            local x0 = (i - 1) * cellw + 2
            local bw = cellw - 8
            c:rect(x0, 4, bw, 5, { fill = "border" })
            c:rect(x0, 4, math.max(1, math.floor(bw * frac(m) + 0.5)), 5, { fill = state_color(m) })
            c:text(x0, 12, m.short .. " " .. hours_left(m), { font = "xs", color = "text_muted" })
        end
    end
    c:commit()
end

gauge:on_size(function() render() end)
bars:on_size(function() render_bars() end)

local rings = { helix.canvas("ring1"), helix.canvas("ring2"), helix.canvas("ring3") }
local function draw_ring(i)
    local rc, m = rings[i], METERS[i]
    local w, hgt = rc:size()
    if w > 8 and hgt > 8 then
        local r = math.floor(math.min(w, hgt) / 2) - 5
        if enabled(m) then
            ring(rc, math.floor(w / 2), math.floor(hgt / 2), r, frac(m), state_color(m), 5)
        end
    end
    rc:commit()
end
for i in ipairs(rings) do
    rings[i]:on_size(function() draw_ring(i) end)
end

-- Accrual: the printer state arrives by watch, the clock by timer. Each
-- tick while printing adds one interval to every enabled meter and the
-- screen total; crossing the interval fires the due toast once, until a
-- reset clears the latch.
local printing = false
helix.printer.watch("print_state", function(v)
    printing = (v == "printing")
end)

local function redraw_all()
    refresh_derived()
    render()
    render_bars()
    for i in ipairs(rings) do draw_ring(i) end
end

local function tick()
    if not printing then return end
    total_h = total_h + TICK_H
    helix.log.debug(string.format("accrual tick: total %.2fh", total_h))
    for _, m in ipairs(METERS) do
        if enabled(m) then
            m.used = m.used + TICK_H
            if not m.notified and m.used >= m.interval then
                m.notified = true
                helix.ui.toast(m.name .. " due - " .. m.interval .. "h reached", "warning")
            end
        end
    end
    save_state()
    redraw_all()
end

helix.timer.every(TICK_MS, tick)

-- An interval change re-derives everything the meter feeds: visibility,
-- list rows, colors, the ring. Setting an interval to 0 hides the meter.
for i, m in ipairs(METERS) do
    helix.settings.on_change(m.setting, function(v)
        helix.log.debug("interval change: " .. m.key .. " -> " .. tostring(v))
        m.interval = v or 0
        redraw_all()
    end)
end

helix.widget("tile", {
    on_size = function(cols, rows, w, hgt)
        compact:set(w < 190 and 1 or 0)
        if cols >= 2 and rows == 1 then
            list_zone:set(1)
            bars_visible:set(0)
        elseif rows >= 2 then
            list_zone:set(0)
            bars_visible:set(1)
        else
            list_zone:set(0)
            bars_visible:set(0)
        end
        redraw_all()
    end,
})

helix.ui.on("open", function()
    helix.ui.overlay("maintenance-meter__detail", { title = "Maintenance" })
end)

-- A reset discards the user's accrued hours for that meter, so it asks
-- first. The stamp is the screen total at reset: the sandbox has no wall
-- clock, and hours-of-printing is the clock this plugin keeps anyway.
helix.ui.on("reset", function(arg)
    for _, m in ipairs(METERS) do
        if m.key == arg then
            helix.ui.confirm("Reset " .. m.name, string.format("Start a new %dh interval?", m.interval), {
                confirm_text = "Reset",
                on_confirm = function()
                    m.used = 0.0
                    m.reset_at = total_h
                    m.notified = false
                    save_state()
                    redraw_all()
                    helix.ui.toast(m.name .. " meter reset", "success")
                end,
            })
            return
        end
    end
end)

load_state()
save_state()
redraw_all()
