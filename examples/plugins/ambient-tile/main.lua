-- Ambient Tile: room temperature and humidity from Home Assistant or any
-- JSON endpoint, polled over http. The readings, their age and the recent
-- temperature history land on subjects; a bad source is a status line and a
-- growing age, never a toast and never stale data presented as fresh.

local t = helix.i18n.t

local compact = helix.subject.int("compact", 1)
local header_on = helix.subject.int("header_on", 0)
local spark_on = helix.subject.int("spark_on", 0)
local age_on = helix.subject.int("age_on", 0)
local temp_text = helix.subject.string("temp_text", "--")
local rh_text = helix.subject.string("rh_text", "--")
local age_text = helix.subject.string("age_text", t("Never"))
local source_line = helix.subject.string("source_line", "")
local status_line = helix.subject.string("status_line", "")

-- The temperature history the sparklines draw. Capped, in-memory only: it
-- refills within a few polls of a restart.
local WINDOW_MAX = 240
local window = {}

local spark = helix.canvas("spark")
local plot = helix.canvas("plot")

local function draw(c, cw, ch)
    local w, h = cw or 0, ch or 0
    if w > 4 and h > 4 and #window >= 2 then
        local lo, hi = window[1], window[1]
        for _, v in ipairs(window) do
            lo = math.min(lo, v)
            hi = math.max(hi, v)
        end
        if hi - lo < 1 then
            hi = lo + 1
        end
        local pad = (hi - lo) * 0.1
        lo, hi = lo - pad, hi + pad
        -- The newest dot keeps its radius clear of the right edge.
        local xmax = w - 1 - 3
        local pts = {}
        for i, v in ipairs(window) do
            pts[#pts + 1] = (i - 1) * xmax / (#window - 1)
            pts[#pts + 1] = (h - 1) * (1 - (v - lo) / (hi - lo))
        end
        c:polyline(pts, { color = "primary", width = 2, fill = "primary", fill_opa = 15, baseline = h - 1 })
        c:circle(pts[#pts - 1], pts[#pts], 2, { fill = "primary" })
    end
    c:commit()
end

local function redraw()
    local w, h = spark:size()
    draw(spark, w, h)
    w, h = plot:size()
    draw(plot, w, h)
end

spark:on_size(function(w, h)
    draw(spark, w, h)
end)
plot:on_size(function(w, h)
    draw(plot, w, h)
end)

-- ---------------------------------------------------------------- age ----
-- No wall clock in the sandbox: a one-second tick counts from the last good
-- reading, and the age text only re-renders when its wording changes.
local elapsed = -1 -- -1: no reading yet, so the line reads "Never"

local function age_string()
    if elapsed < 0 then
        return t("Never")
    end
    if elapsed < 30 then
        return t("Updated just now")
    end
    if elapsed < 90 then
        return t("Updated 1m ago")
    end
    return t("Updated %dm ago"):format(math.floor((elapsed + 30) / 60))
end

helix.timer.every(1000, function()
    elapsed = elapsed >= 0 and elapsed + 1 or elapsed
    local s = age_string()
    if s ~= age_text:get() then
        age_text:set(s)
    end
end)

-- ----------------------------------------------------------- settings ----

local function host_of(u)
    return (u:gsub("^https?://", ""):gsub("/.*$", ""):gsub("^%w+@", ""))
end

local function refresh_source()
    local src = helix.settings.get("source") or "Home Assistant"
    local url = helix.settings.get("url") or ""
    local label = url ~= "" and host_of(url) or t("(not set)")
    source_line:set(src .. " \u{00B7} " .. label)
end

-- Every destination is checked to its shape before anything reaches the
-- network, so a typo is a status line, not a request to a half-parsed URL.
local function config_error()
    local src = helix.settings.get("source") or "Home Assistant"
    local url = helix.settings.get("url") or ""
    if not url:match("^https?://") then
        return t("not configured")
    end
    if src == "Home Assistant" then
        if (helix.settings.get("token") or "") == "" then
            return t("not configured")
        end
        for _, key in ipairs({ "temp_entity", "humidity_entity" }) do
            local ent = helix.settings.get(key) or ""
            if ent ~= "" and not ent:match("^[%w_.]+$") then
                return t("not configured")
            end
        end
        if (helix.settings.get("temp_entity") or "") == "" then
            return t("not configured")
        end
    else
        for _, key in ipairs({ "temp_pointer", "humidity_pointer" }) do
            local p = helix.settings.get(key) or ""
            if p ~= "" and not p:match("^[%w_.]+$") then
                return t("not configured")
            end
        end
        if (helix.settings.get("temp_pointer") or "") == "" then
            return t("not configured")
        end
    end
    return nil
end

-- ------------------------------------------------------------- fetch -----

-- Dot-separated keys; a numeric segment indexes a JSON array from zero (the
-- decoded Lua table is one-based, so the segment shifts by one).
local function resolve(root, path)
    local node = root
    for seg in path:gmatch("[^%.]+") do
        if type(node) ~= "table" then
            return nil
        end
        local num = tonumber(seg)
        local key = seg
        if num and node[1] ~= nil then
            key = num + 1
        elseif num then
            key = num
        end
        node = node[key]
    end
    if type(node) == "number" then
        return node
    end
    if type(node) == "string" then
        return tonumber(node)
    end
    return nil
end

local function fetch_ha(base, token)
    local out = {}
    for i, key in ipairs({ "temp_entity", "humidity_entity" }) do
        local ent = helix.settings.get(key) or ""
        if ent ~= "" then
            local r, err = helix.http.get(base .. "/api/states/" .. ent,
                { headers = { Authorization = "Bearer " .. token } })
            if not r then
                return nil, err
            end
            if r.status ~= 200 then
                return nil, ("HTTP %d"):format(r.status)
            end
            local j, jerr = helix.json.decode(r.body)
            if not j then
                return nil, jerr
            end
            local v = tonumber(j.state)
            if not v then
                return nil, t("bad data")
            end
            out[i] = v
        end
    end
    return out
end

local function fetch_json(url)
    local r, err = helix.http.get(url)
    if not r then
        return nil, err
    end
    if r.status ~= 200 then
        return nil, ("HTTP %d"):format(r.status)
    end
    local j, jerr = helix.json.decode(r.body)
    if not j then
        return nil, jerr
    end
    return { resolve(j, helix.settings.get("temp_pointer") or ""),
             resolve(j, helix.settings.get("humidity_pointer") or "") }
end

local function apply(temp, rh)
    -- Returns an error string, or nil on success, so a pcall'ed caller reads
    -- one value either way.
    if temp then
        temp_text:set(("%.1f\u{00B0}"):format(temp))
        window[#window + 1] = temp
        if #window > WINDOW_MAX then
            table.remove(window, 1)
        end
    end
    if rh then
        rh_text:set(("%d%%"):format(math.floor(rh + 0.5)))
    end
    if not temp and not rh then
        return t("bad data")
    end
    elapsed = 0
    return nil
end

local function poll_once()
    local miss = config_error()
    if miss then
        status_line:set(miss)
        return
    end
    local src = helix.settings.get("source") or "Home Assistant"
    local url = helix.settings.get("url") or ""
    local out, err
    if src == "Home Assistant" then
        out, err = fetch_ha(url:gsub("/$", ""), helix.settings.get("token") or "")
    else
        out, err = fetch_json(url)
    end
    if out == nil then
        status_line:set(err or t("bad data"))
        return
    end
    local ok, aerr = pcall(apply, out[1], out[2])
    if not ok then
        status_line:set(t("bad data"))
        return
    end
    if aerr then
        status_line:set(aerr)
        return
    end
    status_line:set("")
    age_text:set(age_string())
    redraw()
end

-- -------------------------------------------------------------- loop -----

local poll_timer

local function rearm()
    if poll_timer then
        poll_timer:cancel()
    end
    local s = helix.settings.get("poll_s") or 60
    poll_timer = helix.timer.every(s * 1000, function()
        local ok, err = pcall(poll_once)
        if not ok then
            status_line:set(t("bad data"))
        end
    end)
end

for _, key in ipairs({ "source", "url", "token", "temp_entity", "humidity_entity",
                       "temp_pointer", "humidity_pointer" }) do
    helix.settings.on_change(key, function()
        refresh_source()
        pcall(poll_once)
    end)
end
helix.settings.on_change("poll_s", function() rearm() end)

helix.i18n.on_change(function()
    refresh_source()
    age_text:set(age_string())
    pcall(poll_once) -- subject-carried status re-renders in the new language
end)

helix.widget("tile", {
    on_size = function(cols, rows, w, h)
        compact:set(w < 110 and 1 or 0)
        header_on:set(h >= 90 and 1 or 0)
        spark_on:set(h >= 140 and 1 or 0)
        age_on:set(h >= 140 and 1 or 0)
    end,
})

helix.ui.on("open", function()
    helix.ui.overlay("ambient-tile__detail")
end)

refresh_source()
rearm()
poll_once() -- the top level may await: the first reading does not wait a poll
