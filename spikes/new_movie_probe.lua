-- remod new movie probe (CLAUDE.md §10 M3 route 2): plays OUR movie under a NEW movie id, inside a cinematic state,
-- and checks the player comes back exactly as left. Guide: spikes/new_movie_probe.md. No method hooks.
--
-- The movie (`rmd001`, made by spikes/make_new_movie.ps1, installed with Fluffy) is new files at new paths: two MP4s
-- and two prefabs copied from mva000's with the paths changed. Nothing of the game's is replaced.
-- The id: the game finds a movie through movie catalogs registered with chainsaw.AppEventManager (ids are UInt16);
-- this registers a catalog of its own holding rmd001 under an unused id, then asks chainsaw.MovieMediator to load and
-- play that id, as the game does for its own movies [dump]. Run 2: the MovieResource, catalog entry, catalog and
-- arrays are created, else copied from mva000's (run 1: create_userdata gave an empty MovieResource, and a plain
-- create_instance of the entry type gave nil); each step says which it did.
-- The cinematic state: the game's own pause for event movies (share.PauseManager, PauseType.EventMovie), the
-- operation stop every frame (chainsaw.CharacterManager.requestOperationStop), the HUD option off. Before, during
-- (every half second) and after, it reads Leon's position, rotation, health, money, kills, animation and the game's
-- clocks, and shows what changed.
--   F5  play rmd001 (the new id)
--   F6  stop now and restore
--   F7  the same with the game's own mva000 (a comparison, if F5 fails)
-- Results: reframework\data\remod_new_movie_probe.json and the log.

local RUN = 3
local TAG = "[remod new movie probe] "
local OWNER = "remod"

local results = { run = RUN, notes = {} }
local function save() json.dump_file("remod_new_movie_probe.json", results) end
local function note(text)
    table.insert(results.notes, string.format("%.1f s: %s", os.clock(), text))
    log.info(TAG .. text)
    save()
end

local function enum(type_name, name) return sdk.find_type_definition(type_name):get_field(name):get_data(nil) end
-- A game singleton: REFramework's list, else its AppSingleton`1 get_Instance (found through the parent).
local function instance(type_name)
    local o = sdk.get_managed_singleton(type_name)
    if o then return o end
    local td = sdk.find_type_definition(type_name)
    local m = td and td:get_method("get_Instance")
    return m and m:call(nil)
end
local function try(f, ...)
    local ok, v = pcall(f, ...)
    if ok then return v end
    return nil, tostring(v)
end
local function str(s) return sdk.create_managed_string(s) end

-- ---- Player state ----

local function player_context()
    local cm = sdk.get_managed_singleton("chainsaw.CharacterManager")
    return cm and cm:call("getPlayerContextRef")
end

local function snapshot()
    local s = {}
    local ctx = player_context()
    if ctx then
        try(function() local p = ctx:call("get_Position"); s.pos = { p.x, p.y, p.z } end)
        try(function() local r = ctx:call("get_Rotation"); s.rot = { r.x, r.y, r.z, r.w } end)
        try(function()
            local hp = ctx:call("get_HitPoint")
            s.hp, s.hp_max = hp:call("get_CurrentHitPoint"), hp:call("get_DefaultHitPoint")
        end)
        try(function()
            local body = ctx:call("get_BodyGameObject")
            local layer = body:call("getComponent(System.Type)", sdk.typeof("via.motion.Motion")):call("getLayer", 0)
            s.motion = { layer:call("get_MotionBankID"), layer:call("get_MotionID"), layer:call("get_Frame") }
        end)
    end
    try(function() s.ptas = instance("chainsaw.InventoryManager"):call("get_CurrPTAS") end)
    try(function() s.kills = instance("chainsaw.GameStatsManager"):call("getKillCount") end)
    try(function()
        local c = instance("share.GameClock")
        s.clock_game, s.clock_playing = c:call("get_GameElapsedTime"), c:call("get_ActualPlayingTime")
        s.clock_demo, s.clock_pause = c:call("get_DemoSpendingTime"), c:call("get_PauseSpendingTime")
    end)
    return s
end

-- What differs between two snapshots, as text; clocks reported as seconds gone by (microseconds [inferred]).
local function compare(a, b)
    local out = {}
    if a.pos and b.pos then
        local dx, dy, dz = b.pos[1] - a.pos[1], b.pos[2] - a.pos[2], b.pos[3] - a.pos[3]
        table.insert(out, string.format("moved %.4f m", math.sqrt(dx * dx + dy * dy + dz * dz)))
    end
    if a.rot and b.rot then
        local dot = math.abs(a.rot[1] * b.rot[1] + a.rot[2] * b.rot[2] + a.rot[3] * b.rot[3] + a.rot[4] * b.rot[4])
        table.insert(out, string.format("turned %.2f deg", math.deg(2 * math.acos(math.min(1, dot)))))
    end
    for _, k in ipairs({ "hp", "hp_max", "ptas", "kills" }) do
        if a[k] ~= b[k] then table.insert(out, string.format("%s %s -> %s", k, tostring(a[k]), tostring(b[k]))) end
    end
    if a.motion and b.motion and (a.motion[1] ~= b.motion[1] or a.motion[2] ~= b.motion[2]) then
        table.insert(out, string.format("animation %d/%d -> %d/%d", a.motion[1], a.motion[2], b.motion[1], b.motion[2]))
    end
    for _, k in ipairs({ "clock_game", "clock_playing", "clock_demo", "clock_pause" }) do
        if a[k] and b[k] then table.insert(out, string.format("%s +%.2f s", k:sub(7), (b[k] - a[k]) / 1e6)) end
    end
    return table.concat(out, ", ")
end

-- ---- The new movie id ----

local registered = nil  -- {id = n, catalog = obj, resource = obj}

local function movie_catalogs()
    local aem = instance("chainsaw.AppEventManager")
    local list = aem:get_field("_AppEventCatalogList")
    local out = {}
    for i = 0, list:call("get_Count") - 1 do
        local c = list:call("get_Item", i)
        if c and c:get_type_definition():get_full_name() == "chainsaw.MovieCatalog" then table.insert(out, c) end
    end
    return aem, out
end

-- Every catalog's ids and the enum's values, for the report and for an unused id.
local function survey()
    local aem, catalogs = movie_catalogs()
    local used, info = {}, {}
    for _, c in ipairs(catalogs) do
        local arr = c:call("get_ResourceArray")
        local ids = {}
        for i = 0, (arr and arr:get_size() or 0) - 1 do
            local id = arr:get_element(i):call("get_ID")
            used[id] = true
            table.insert(ids, id)
        end
        table.insert(info, { key = c:call("get_KeyName"), kind = c:call("get_Kind"), ids = ids })
    end
    for _, f in ipairs(sdk.find_type_definition("chainsaw.MovieDefine.ID"):get_fields()) do
        if f:is_static() then used[f:get_data(nil)] = true end
    end
    results.catalogs = info
    results.mva000 = { enum = enum("chainsaw.MovieDefine.ID", "mva000") }
    return aem, catalogs, used
end

-- A new object of a game type: created (with REFramework's "simplify", which its book says to use when a plain
-- create returns nil; run 1's plain create of the catalog entry type did), else a copy of `like`, one of the game's
-- (MemberwiseClone: shallow, so what it points to stays shared until replaced). Says which it did.
local function new_object(type_name, like)
    local o = try(sdk.create_instance, type_name, true)
    if o then return o:add_ref(), "created" end
    o = like and try(like.call, like, "MemberwiseClone")
    if o then return o:add_ref(), "copied" end
    error("can't make a " .. type_name)
end

-- A managed array of `values`: created, else a copy of `like` (the same length) with the values put in. Each element
-- read back, so a set that didn't take is an error, not a silent nil.
local function new_array(elem_type, values, like)
    local arr = try(sdk.create_managed_array, elem_type, #values)
    local how = "created"
    if not arr and like and like:get_size() == #values then arr, how = try(like.call, like, "Clone"), "copied" end
    if not arr then error("can't make an array of " .. elem_type) end
    arr = arr:add_ref()
    for i, v in ipairs(values) do
        arr:call("SetValue(System.Object, System.Int32)", v, i - 1)
        local back = arr:call("GetValue(System.Int32)", i - 1)
        if not back or back:get_address() ~= v:get_address() then error("array element " .. (i - 1) .. " didn't take") end
    end
    return arr, how
end

-- A via.Prefab on the game's `like`'s path with mva000 changed to rmd001, so a leading "@" stays: it marks a file with
-- a platform version (".pfb.17.x64" on disk; run 2 left it off and the game didn't find the 4K prefab). Created, else
-- a duplicate of `like`; then pointed at the path.
local function new_prefab(like)
    local path = like:call("get_Path"):gsub("mva000/mva000", "rmd001/rmd001")
    local p, how = try(sdk.create_instance, "via.Prefab", true), "created"
    if p then p = p:add_ref() else p, how = like:call("duplicate"):add_ref(), "duplicated" end
    p:call("set_Path", str(path))
    note(string.format("prefab %s (%s): path %s, exists %s", path, how, tostring(try(p.call, p, "get_Path")),
        tostring(try(p.call, p, "get_Exist"))))
    return p
end

local function register()
    if registered then return registered end
    local aem, catalogs, used = survey()
    if #catalogs == 0 then error("no movie catalog registered yet (load a save first)") end
    local first = catalogs[1]  -- Movie_1st: mva000 alone
    local template = first:call("get_ResourceArray"):get_element(0)
    local kind = first:call("get_Kind")
    -- The game's ids are 10000 + the mva number (mva000 10000, mva201 10201): ours is the first free one from 10950.
    local id = 10950
    while used[id] do id = id + 1 end
    note(string.format("%d movie catalogs; mva000 is enum %d, catalog id %d; new id %d", #catalogs,
        results.mva000.enum, template:call("get_ID"), id))

    -- The MovieResource: mva000's, copied, with our prefabs and its own option (sound trigger cleared). Built from the
    -- game's object rather than loaded from our .user: run 1's create_userdata gave an empty one.
    local base = template:call("get_Data")
    note(string.format("mva000's MovieResource: prefab %s, display %s", tostring(try(function()
        return base:call("get_MoviePrefab"):call("get_Path") end)), tostring(try(base.call, base, "get_DisplayType"))))
    local res, how = new_object("chainsaw.MovieResource", base)
    local prefab_4k = new_prefab(base:call("get_MoviePrefab"))
    res:call("set_MoviePrefab", prefab_4k)
    local base_extra = base:call("get_ExceptionalMoviePrefabs")
    if not (base_extra and base_extra:get_size() > 0) then error("mva000 has no 1080p prefab to copy") end
    local fhd = new_prefab(base_extra:get_element(0))
    local extra, how_extra = new_array("via.Prefab", { fhd }, base_extra)
    res:call("set_ExceptionalMoviePrefabs", extra)
    res:call("set_DisplayType", base:call("get_DisplayType"))
    local option = new_object("chainsaw.MovieResource.OptionParam", base:call("get_Option"))
    option:call("set_WwiseTriggerID", 0)
    res:call("set_Option", option)
    note(string.format("our MovieResource (%s): prefab %s, 1080p prefabs %s, display %s", how,
        tostring(try(function() return res:call("get_MoviePrefab"):call("get_Path") end)), how_extra,
        tostring(res:call("get_DisplayType"))))

    -- One catalog entry, of the type the game's are.
    local entry_type = template:get_type_definition():get_full_name()
    local entry, how_entry = new_object(entry_type, template)
    entry:call("set_ID", id)
    entry:call("set_Data", res)
    entry:call("set_FollowData", template:call("get_FollowData"))
    local arr, how_arr = new_array(entry_type, { entry }, first:call("get_ResourceArray"))
    note(string.format("entry %s (%s, id reads %s), array %s", entry_type, how_entry, tostring(entry:call("get_ID")), how_arr))

    -- Our own catalog (a new one, else a copy of Movie_1st), registered as the game registers its own.
    local cat, how_cat = new_object("chainsaw.MovieCatalog", first)
    cat:call("set_KeyName", str("remod_movie"))
    cat:set_field("_KeyNameHash", 0x7e3d0001)
    cat:call("set_Kind", kind)
    cat:call("set_ResourceArray", arr)
    aem:call("registerCatalog", cat, kind)
    note("registered our catalog (" .. how_cat .. ")")
    local found = try(aem.call, aem, "getMovieResource", id)
    note("AppEventManager.getMovieResource(" .. id .. "): " ..
        (found and (found:get_address() == res:get_address() and "ours" or "another") or "nothing"))

    -- The movie load table: if it has no entry for the id, a copy of mva000's with its chapter flags cleared.
    local mm = instance("chainsaw.MovieMediator")
    if not try(mm.call, mm, "getLoadInfo", id) then
        local info_base = try(mm.call, mm, "getLoadInfo", results.mva000.enum)
        if info_base then
            local ok2, e2 = pcall(function()
                local info = info_base:call("MemberwiseClone"):add_ref()
                info:call("set_MovieID", id)
                for _, f in ipairs({ "set_IsChapterStart", "set_IsChapterEnd", "set_IsEnding", "set_IsGameOver",
                                     "set_HasNextMovie" }) do info:call(f, false) end
                mm:call("get_MovieLoadTable"):call("get_LoadInfoList"):call("Add", info)
            end)
            note("load table entry " .. (ok2 and "added (from mva000's)" or ("failed: " .. tostring(e2))))
        else
            note("no load table entry for mva000 either: none added")
        end
    end
    registered = { id = id, catalog = cat, resource = res }
    return registered
end

-- ---- The cinematic state ----

local cine = nil  -- {id, phase, started, before, during = {worst}, hud_before, ...}
local PAUSE = nil

local function pause_manager() return instance("share.PauseManager") end
local function set_pause(on)
    PAUSE = PAUSE or enum("share.PauseManager.PauseType", "EventMovie")
    local pm = pause_manager()
    if on then
        pm:call("requestStartPause(share.PauseManager.PauseType, System.String, System.Action)", PAUSE, str(OWNER), nil)
    else
        pm:call("requestEndPause(share.PauseManager.PauseType, System.String, System.Action)", PAUSE, str(OWNER), nil)
    end
end
local function set_hud(value)
    local om = sdk.get_managed_singleton("chainsaw.OptionManager")
    local id = enum("chainsaw.option.OptionID", "DisplayUI")
    local before = om:call("getCurrentOptionValue", id)
    om:call("setCurrentOptionValue", id, value)
    return before
end

local check_later = nil  -- {before, at}: compared again a second after restoring

local function restore(why)
    if not cine then return end
    local c = cine
    cine = nil  -- the operation stop ends with it
    local mm = instance("chainsaw.MovieMediator")
    if c.phase ~= "ended" then try(mm.call, mm, "requestSkip", c.id) end
    local errors = {}
    local _, e1 = try(set_pause, false)
    table.insert(errors, e1)
    if c.hud_before then
        local _, e2 = try(set_hud, c.hud_before)
        table.insert(errors, e2)
    end
    local after = snapshot()
    results.last = { id = c.id, label = c.label, why = why, before = c.before, after = after,
                     while_playing = c.worst, changed_after = compare(c.before, after), errors = errors }
    note("restored (" .. why .. "): " .. results.last.changed_after)
    check_later = { before = c.before, at = os.clock() + 1 }
end

local function start(id, label)
    if cine then return note("already playing") end
    local ok, err = pcall(function()
        local before = snapshot()
        cine = { id = id, label = label, phase = "loading", started = os.clock(), before = before, worst = "" }
        cine.hud_before = set_hud(0)
        set_pause(true)
        instance("chainsaw.MovieMediator"):call("load", id)
        note(label .. ": cinematic on, loading id " .. id)
    end)
    if not ok then
        note(label .. " failed to start: " .. tostring(err))
        restore("start failed")
    end
end

-- Every frame before the game's behaviour update: the operation stop, as the cutscene runtime does.
re.on_pre_application_entry("UpdateBehavior", function()
    if not cine then return end
    local cm = sdk.get_managed_singleton("chainsaw.CharacterManager")
    if cm then
        pcall(cm.call, cm, "requestOperationStop", enum("chainsaw.CharacterControlIndex", "Player_1"),
            enum("chainsaw.character.PauseLayer", "Self"))
    end
end)

local function phase_text(mm, id)
    local work = try(mm.call, mm, "getWork", id)
    return work and tostring(try(work.get_field, work, "_MoviePhase")) or "no work"
end

local function step()
    local c = cine
    local mm = instance("chainsaw.MovieMediator")
    local t = os.clock() - c.started
    if c.phase == "loading" then
        if mm:call("IsLoaded", c.id) then
            mm:call("play", c.id, nil, nil)
            c.phase, c.play_at = "playing", os.clock()
            note(c.label .. string.format(": loaded after %.1f s, play called", t))
        elseif t > 20 then
            note(c.label .. ": not loaded after 20 s (phase " .. phase_text(mm, c.id) .. ")")
            restore("load timed out")
        end
    elseif c.phase == "playing" then
        local playing = mm:call("isPlaying")
        if playing and not c.seen_playing then
            c.seen_playing = true
            note(c.label .. string.format(": playing after %.1f s", os.clock() - c.play_at))
        end
        if c.seen_playing and not playing then
            c.phase = "ended"
            note(c.label .. ": ended")
            try(mm.call, mm, "unload", c.id)
            restore("movie ended")
        elseif not c.seen_playing and os.clock() - c.play_at > 15 then
            note(c.label .. ": not playing 15 s after play (phase " .. phase_text(mm, c.id) .. ")")
            restore("play timed out")
        elseif os.clock() - c.play_at > 600 then
            restore("10 minutes")
        end
    end
    if cine and os.clock() >= (c.next_sample or 0) then  -- what changes while it plays
        c.next_sample = os.clock() + 0.5
        c.worst = compare(c.before, snapshot())
    end
end

-- ---- Keys and screen ----

local KEYS = { F5 = 0x74, F6 = 0x75, F7 = 0x76 }
local down = {}
local function pressed(name)
    local is = reframework:is_key_down(KEYS[name])
    local was = down[name]
    down[name] = is
    return is and not was
end

re.on_frame(function()
    if pressed("F5") then
        local r, err = try(register)
        if r then start(r.id, "rmd001 (new id)") else note("F5: registering failed: " .. tostring(err)) end
    end
    if pressed("F6") then restore("F6") end
    if pressed("F7") then start(enum("chainsaw.MovieDefine.ID", "mva000"), "mva000 (game's)") end
    if cine then
        local ok, err = pcall(step)
        if not ok then
            note("error: " .. tostring(err))
            restore("error")
        end
    end
    local p = check_later
    if p and os.clock() >= p.at then
        check_later = nil
        results.last.changed_after_1s = compare(p.before, snapshot())
        note("1 s after: " .. results.last.changed_after_1s)
    end

    local mm = instance("chainsaw.MovieMediator")
    local pm = pause_manager()
    local lines = {
        "remod new movie probe (run " .. RUN .. "): F5 play rmd001 (new id), F6 stop and restore, F7 the game's mva000",
        "state: " .. (cine and (cine.label .. ", " .. cine.phase .. string.format(" %.1f s", os.clock() - cine.started)) or "normal play") ..
            " | paused (EventMovie) " .. tostring(pm and try(pm.call, pm, "isPaused(share.PauseManager.PauseType)",
                PAUSE or enum("share.PauseManager.PauseType", "EventMovie"))) ..
            " | movie playing " .. tostring(mm and try(mm.call, mm, "isPlaying")),
        "while playing, changed: " .. (cine and cine.worst or "-"),
        "last result: " .. (results.last and (results.last.why .. ": " .. results.last.changed_after ..
            (results.last.changed_after_1s and (" | 1 s later: " .. results.last.changed_after_1s) or "")) or "-"),
        "last note: " .. (results.notes[#results.notes] or "-"),
    }
    for i, line in ipairs(lines) do draw.text(line, 40, 20 + 18 * i, 0xFFFFFFFF) end
end)

re.on_script_reset(function() restore("script reset") end)
note("loaded (run " .. RUN .. ")")
