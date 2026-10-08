-- remod sound probe (CLAUDE.md §10 M3, sound for new movies): can a script play sounds on command? Guide:
-- spikes/sound_probe.md. No method hooks.
--
--   F7  play one of the game's own sounds: a trigger from a sound container (Leon's if found), as the game does
--       (soundlib.SoundContainer.trigger(SoundTriggerInfo)); F8 picks the next trigger.
--   F5  play a BRAND-NEW sound: our bank remod_snd001 (made by `remod new-sound` from ch_csa404_se with every id new,
--       holding 3 s of beeps), loaded as a resource and kept by the container, then its event posted through a copy of
--       one of the container's trigger infos with our event id.
-- Each press shows what the game answered: the request id, then whether Wwise is playing it (Driver
-- getPlayingIdByRequestId: 0 = not playing, e.g. its bank isn't loaded).
-- The mod also has a new movie with sound in its file (rmd002): REFramework menu > Script Generated UI > remod cutscenes
-- > Play beside "New movie rmd002".
-- Results: reframework\data\remod_sound_probe.json and the log.

local RUN = 1
local TAG = "[remod sound probe] "
local NOTE = "remod_sounds/remod_snd001.json"

local results = { run = RUN, notes = {} }
local function note(text)
    table.insert(results.notes, string.format("%.1f s: %s", os.clock(), text))
    log.info(TAG .. text)
    json.dump_file("remod_sound_probe.json", results)
end
local function try(f, ...)
    local ok, v = pcall(f, ...)
    if ok then return v end
    return nil, tostring(v)
end

-- ---- A sound container ----

local container, container_from = nil, nil
local function find_container()
    if container then return container end
    local cm = sdk.get_managed_singleton("chainsaw.CharacterManager")
    local ctx = cm and cm:call("getPlayerContextRef")
    local body = ctx and ctx:call("get_BodyGameObject")
    for _, t in ipairs({ "chainsaw.SoundContainerApp", "soundlib.SoundContainer" }) do
        local c = body and try(body.call, body, "getComponent(System.Type)", sdk.typeof(t))
        if c then
            container, container_from = c:add_ref(), "Leon (" .. t .. ")"
            return container
        end
    end
    -- Else the first one in the scene that has triggers.
    local scene = try(function()
        return sdk.call_native_func(sdk.get_native_singleton("via.SceneManager"),
            sdk.find_type_definition("via.SceneManager"), "get_CurrentScene")
    end)
    local all = scene and try(scene.call, scene, "findComponents(System.Type)", sdk.typeof("chainsaw.SoundContainerApp"))
    for i = 0, (all and all:get_size() or 0) - 1 do
        local c = all:get_element(i)
        local list = c:get_field("_TriggerInfoList")
        if list and list:call("get_Count") > 0 then
            container, container_from = c:add_ref(), "the scene's " .. i + 1 .. " of " .. all:get_size()
            return container
        end
    end
    error("no sound container found (Leon's or the scene's)")
end

local function triggers()
    local list = find_container():get_field("_TriggerInfoList")
    return list, list and list:call("get_Count") or 0
end

-- ---- Playing, and asking Wwise whether it plays ----

local pending = {}  -- {label, request, at}: checked 0.3 s later
local function playing_id(request)
    return sdk.find_type_definition("via.simplewwise.Driver"):get_method("getPlayingIdByRequestId"):call(nil, request)
end
local function play(info, label)
    local req = find_container():call("trigger(soundlib.SoundTriggerInfo)", info)
    note(string.format("%s: trigger %s, event %s -> request %s", label, tostring(info:get_field("_TriggerId")),
        tostring(info:get_field("_EventId")), tostring(req)))
    table.insert(pending, { label = label, request = req, at = os.clock() + 0.3 })
end

local index = 0
local function game_sound()
    local list, n = triggers()
    if n == 0 then return note("F7: the container has no triggers") end
    local info = list:call("get_Item", index % n)
    play(info, string.format("F7 game sound %d of %d (%s)", index % n + 1, n, container_from))
end

-- ---- The new sound ----

local bank = nil  -- {path, holder}
local function load_bank(path)
    if bank then return bank end
    local tried = {}
    for _, p in ipairs({ path, (path:gsub("^@", "")) }) do  -- as the game writes it, then without the "@"
        local res, err = try(sdk.create_resource, "via.simplewwise.BankResource", p)
        table.insert(tried, p .. ": " .. (res and "resource" or ("nil " .. tostring(err))))
        if res then
            res = res:add_ref()
            local holder, herr = try(res.create_holder, res, "via.simplewwise.BankResourceHolder")
            table.insert(tried, "holder: " .. (holder and "made" or ("failed " .. tostring(herr))))
            if holder then
                holder = holder:add_ref()
                local _, aerr = try(function() find_container():call("get_BankResourceList"):call("Add", holder) end)
                table.insert(tried, "kept by the container: " .. (aerr and ("failed " .. aerr) or "yes"))
                bank = { path = p, resource = res, holder = holder }
                break
            end
        end
    end
    note("loading our bank: " .. table.concat(tried, "; "))
    if not bank then error("couldn't load " .. path) end
    return bank
end

local function new_sound()
    local n = json.load_file(NOTE)
    if not n then return note("F5: " .. NOTE .. " not found: is the probe mod installed?") end
    load_bank(n.bank)
    local list, count = triggers()
    if count == 0 then return note("F5: the container has no trigger to copy") end
    local info = list:call("get_Item", 0):call("MemberwiseClone"):add_ref()
    info:set_field("_EventId", n.event)
    info:set_field("_TriggerId", n.event)
    play(info, "F5 new sound (event " .. n.event .. ")")
end

-- ---- Keys and screen ----

local KEYS = { F5 = 0x74, F7 = 0x76, F8 = 0x77 }
local down = {}
local function pressed(name)
    local is = reframework:is_key_down(KEYS[name])
    local was = down[name]
    down[name] = is
    return is and not was
end

local last = "-"
re.on_frame(function()
    for key, f in pairs({ F5 = new_sound, F7 = game_sound }) do
        if pressed(key) then
            local ok, err = pcall(f)
            if not ok then note(key .. " failed: " .. tostring(err)) end
        end
    end
    if pressed("F8") then
        index = index + 1
        local _, n = triggers()
        note("F8: trigger " .. (n > 0 and (index % n + 1) or 0) .. " of " .. n .. " next")
    end
    for i = #pending, 1, -1 do
        local p = pending[i]
        if os.clock() >= p.at then
            table.remove(pending, i)
            local id = try(playing_id, p.request)
            last = p.label .. ": Wwise playing id " .. tostring(id) .. (id == 0 and " (NOT playing)" or "")
            note(last)
        end
    end
    local lines = {
        "remod sound probe (run " .. RUN .. "): F7 a game sound (F8 next), F5 a brand-new sound",
        "container: " .. (container_from or "not looked for yet") .. (bank and (" | our bank: " .. bank.path) or ""),
        "last: " .. last,
    }
    for i, line in ipairs(lines) do draw.text(line, 40, 20 + 18 * i, 0xFFFFFFFF) end
end)

note("loaded (run " .. RUN .. ")")
