-- remod cutscene runtime (CLAUDE.md §10 M3 route 3; the seed of M5's one generic runtime). Plays cutscene files,
-- which are data, not code: a mod ships this script once and its cutscenes as
-- reframework\data\remod_cutscenes\<name>.json (the format: schemas/cutscene.v0.example.json in remod).
--
-- A cutscene: a length; an optional start key; camera keys (time, position, rotation x y z w, FOV, ease) the camera
-- follows; subtitles; a letterbox; fades; actors (other characters, as puppets built from the game's files: see
-- "Actors" below); motions (one of the game's animations, by motion bank and id, on the player or an actor); movies
-- (one of the game's, by id, full screen with the world paused; the cutscene's time waits while it plays).
-- REFramework's menu > Script Generated UI > remod cutscenes: play or stop each, reload, and record camera keys: F10
-- adds the camera as it is now as a key to remod_cutscenes\recording.json (the time between presses becomes the time
-- between keys). Frame shots with REFramework's free camera, then press F10.

local DIR = "remod_cutscenes"
local RECORD_KEY = 0x79  -- F10

-- ---- Time: os.clock when REFramework's Lua has it, else frames at 60 per second ----
local frames = 0
local function now() return (os and os.clock) and os.clock() or frames / 60 end

local function enum(t, name) return sdk.find_type_definition(t):get_field(name):get_data(nil) end
-- An AppSingleton`1 of the game's, by its get_Instance (on the parent).
local function instance(t) return sdk.find_type_definition(t):get_method("get_Instance"):call(nil) end

-- Kept for good. REFramework frees what a script made once the script no longer refers to it, and all of it when the
-- script resets (Reset Scripts, or loading again at game start), unless add_ref_permanent [REFramework book,
-- REManagedObject]. Everything handed to the game gets it: the game went on using freed resource holders, motion banks
-- and layers and crashed (2026-10-08: the character probe's F8 after Reset Scripts, the actor test's first play).
-- ponytail: never released; each is made once per game session, so it's a few small objects.
local function keep(o)
    o:add_ref_permanent()
    return o
end

-- The game's pause for event movies, so the world stands still while one plays (RE4R).
local function event_pause(on)
    instance("share.PauseManager"):call(
        (on and "requestStartPause" or "requestEndPause") .. "(share.PauseManager.PauseType, System.String, System.Action)",
        enum("share.PauseManager.PauseType", "EventMovie"), sdk.create_managed_string("remod_cutscene"), nil)
end

-- ---- New movies (remod's New movie block; docs/re4r_movies.md, proven 2026-10-07) ----
-- A mod's new movie is new files at new paths (MP4s and prefabs made like mva000's) and a note in
-- reframework\data\remod_movies\<name>.json. The first time one is played it's registered with the game under a new
-- id, in a catalog of its own, built from copies of mva000's objects with its own prefabs.
local new_movies = {}  -- name -> note (from the files)
local registered = {}  -- name -> id (this game session)
local MOVIES_DIR = "remod_movies"

-- A new object of a game type: created (REFramework's "simplify", else nil for some types), else a copy of `like`.
local function new_object(type_name, like)
    local ok, o = pcall(sdk.create_instance, type_name, true)
    if not (ok and o) then o = like and like:call("MemberwiseClone") end
    if not o then error("can't make a " .. type_name) end
    return keep(o)
end

-- An array holding `values`: created, else a copy of `like` (as long); each set read back.
local function new_array(elem_type, values, like)
    local ok, arr = pcall(sdk.create_managed_array, elem_type, #values)
    if not (ok and arr) and like and like:get_size() == #values then arr = like:call("Clone") end
    if not arr then error("can't make an array of " .. elem_type) end
    arr = keep(arr)
    for i, v in ipairs(values) do
        arr:call("SetValue(System.Object, System.Int32)", v, i - 1)
        local back = arr:call("GetValue(System.Int32)", i - 1)
        if not back or back:get_address() ~= v:get_address() then error("array element " .. (i - 1) .. " didn't take") end
    end
    return arr
end

-- A prefab on `like`'s path with mva000 changed to `name`. The path is taken from the game's, never typed: a leading
-- "@" (a file with a platform suffix, .pfb.17.x64) must stay, or the game doesn't find the file.
local function new_prefab(like, name)
    local ok, p = pcall(sdk.create_instance, "via.Prefab", true)
    p = keep((ok and p) and p or like:call("duplicate"))
    p:call("set_Path", sdk.create_managed_string((like:call("get_Path"):gsub("mva000/mva000", name .. "/" .. name))))
    if not p:call("get_Exist") then error("the game doesn't see " .. p:call("get_Path") .. ": is the mod installed?") end
    return p
end

-- ---- New sounds (remod's New sound and New movie blocks; docs/re4r_movies.md §4, proven 2026-10-07) ----
-- A new sound is a bank of its own; its note gives the bank's path (as the game writes them, with "@") and its event.
-- It plays through the player's sound container as the game plays its own: a copy of one of the container's trigger
-- infos with the event changed (soundlib.SoundContainer.trigger). The bank is loaded once, as a resource the
-- container keeps.
local new_sounds = {}  -- name -> note (remod_sounds\<name>.json)
local SOUNDS_DIR = "remod_sounds"
local banks = {}       -- bank path -> {resource, holder, kept = {container address = true}}

local function sound_container()
    local cm = sdk.get_managed_singleton("chainsaw.CharacterManager")
    local ctx = cm and cm:call("getPlayerContextRef")
    local body = ctx and ctx:call("get_BodyGameObject")
    local c = body and body:call("getComponent(System.Type)", sdk.typeof("chainsaw.SoundContainerApp"))
    if not c then error("no sound container on the player (load a save first)") end
    return c
end

-- A sound ({bank, event}) played now; gives what sound_stop takes.
local function sound_play(s)
    local b = banks[s.bank]
    if not b then
        local res = sdk.create_resource("via.simplewwise.BankResource", s.bank)
        if not res then error("can't load " .. s.bank .. ": is the mod installed?") end
        res = res:add_ref()
        b = { resource = res, holder = keep(res:create_holder("via.simplewwise.BankResourceHolder")), kept = {} }
        banks[s.bank] = b
    end
    local c = sound_container()
    if not b.kept[c:get_address()] then  -- a save loaded since: the player's container is a new one
        c:call("get_BankResourceList"):call("Add", b.holder)
        b.kept[c:get_address()] = true
    end
    local info = keep(c:get_field("_TriggerInfoList"):call("get_Item", 0):call("MemberwiseClone"))
    info:set_field("_EventId", s.event)
    info:set_field("_TriggerId", s.event)
    return { container = c, request = c:call("trigger(soundlib.SoundTriggerInfo)", info), trigger = s.event }
end

-- Stops it (the game's own stop by request, else by trigger). [Not seen in game yet.]
local function sound_stop(h)
    local go = h.container:call("get_GameObject")
    local ok = pcall(function()
        sdk.find_type_definition("soundlib.SoundManager"):get_method("stopEventByRequestId"):call(nil, go, h.request, 0)
    end)
    if not ok then
        pcall(h.container.call, h.container, "stopTriggered(System.UInt32, via.GameObject, System.UInt32)", h.trigger, go, 0)
    end
end

local function register_movie(name)
    if registered[name] then return registered[name] end
    local mva000 = enum("chainsaw.MovieDefine.ID", "mva000")
    local aem = instance("chainsaw.AppEventManager")
    local list = aem:get_field("_AppEventCatalogList")
    local used, catalog, entry = {}, nil, nil
    for i = 0, list:call("get_Count") - 1 do
        local c = list:call("get_Item", i)
        if c and c:get_type_definition():get_full_name() == "chainsaw.MovieCatalog" then
            local arr = c:call("get_ResourceArray")
            for j = 0, arr:get_size() - 1 do
                local e = arr:get_element(j)
                used[e:call("get_ID")] = true
                if e:call("get_ID") == mva000 then catalog, entry = c, e end
            end
        end
    end
    if not entry then error("mva000 isn't registered yet (load a save first)") end
    for _, f in ipairs(sdk.find_type_definition("chainsaw.MovieDefine.ID"):get_fields()) do
        if f:is_static() then used[f:get_data(nil)] = true end
    end
    -- The game's ids are 10000 + the mva number (mva924 the last): ours from 10950, the first free.
    local id = 10950
    while used[id] do id = id + 1 end

    local base = entry:call("get_Data")
    local res = new_object("chainsaw.MovieResource", base)
    res:call("set_MoviePrefab", new_prefab(base:call("get_MoviePrefab"), name))
    local extra = base:call("get_ExceptionalMoviePrefabs")
    res:call("set_ExceptionalMoviePrefabs", new_array("via.Prefab", { new_prefab(extra:get_element(0), name) }, extra))
    res:call("set_DisplayType", base:call("get_DisplayType"))
    local option = new_object("chainsaw.MovieResource.OptionParam", base:call("get_Option"))
    option:call("set_WwiseTriggerID", 0)  -- not mva000's sound: new movies are silent for now
    res:call("set_Option", option)

    local mine = new_object(entry:get_type_definition():get_full_name(), entry)
    mine:call("set_ID", id)
    mine:call("set_Data", res)
    mine:call("set_FollowData", entry:call("get_FollowData"))
    local cat = new_object("chainsaw.MovieCatalog", catalog)
    cat:call("set_KeyName", sdk.create_managed_string("remod_movie_" .. name))
    cat:set_field("_KeyNameHash", 0x7e3d0000 + id)
    cat:call("set_Kind", catalog:call("get_Kind"))
    cat:call("set_ResourceArray", new_array(mine:get_type_definition():get_full_name(), { mine },
        catalog:call("get_ResourceArray")))
    aem:call("registerCatalog", cat, catalog:call("get_Kind"))
    local found = aem:call("getMovieResource", id)
    if not found or found:get_address() ~= res:get_address() then error("the game didn't take the new movie's catalog") end

    -- A load table entry like mva000's, its chapter flags off.
    local mm = instance("chainsaw.MovieMediator")
    if not mm:call("getLoadInfo", id) then
        local info = keep(mm:call("getLoadInfo", mva000):call("MemberwiseClone"))
        info:call("set_MovieID", id)
        for _, f in ipairs({ "set_IsChapterStart", "set_IsChapterEnd", "set_IsEnding", "set_IsGameOver",
                             "set_HasNextMovie" }) do info:call(f, false) end
        mm:call("get_MovieLoadTable"):call("get_LoadInfoList"):call("Add", info)
    end
    registered[name] = id
    log.info("[remod_cutscene] new movie " .. name .. " registered as id " .. id)
    return id
end

-- An enum value's name (an enum's static fields), e.g. a ChapterID's "chap01_01"; the number if none.
local enum_names = {}
local function enum_name(type_name, value)
    local names = enum_names[type_name]
    if not names then
        names = {}
        for _, f in ipairs(sdk.find_type_definition(type_name):get_fields()) do
            if f:is_static() then names[f:get_data(nil)] = f:get_name() end
        end
        enum_names[type_name] = names
    end
    return names[value] or tostring(value)
end

-- An enum's values by name (its static fields, value__ left out).
local enum_value_lists = {}
local function enum_values(type_name)
    local values = enum_value_lists[type_name]
    if not values then
        values = {}
        for _, f in ipairs(sdk.find_type_definition(type_name):get_fields()) do
            if f:is_static() then values[f:get_name()] = f:get_data(nil) end
        end
        enum_value_lists[type_name] = values
    end
    return values
end

-- What differs between games, found by remod's spikes/cutscene_probe.lua run in that game.
local GAMES = {
    re4 = {
        -- Where the camera is set each frame ("after X": once step X is done, else just before it). Set only at
        -- BeginRendering the view still followed the mouse, though the transform read back as ours (the view is taken
        -- earlier); set at each step from the game's own camera update (LateUpdateBehavior) on, it holds (2026-10-07).
        camera_hooks = { "after LateUpdateBehavior", "PrepareRendering", "BeforeLockSceneRendering", "LockScene",
                         "BeginRendering" },
        -- The player's GameObject, as EMV Engine (MIT) finds it in RE4R.
        player = function()
            local mgr = sdk.get_managed_singleton("chainsaw.CharacterManager")
            local ctx = mgr and mgr:call("getPlayerContextRef")
            return ctx and ctx:call("get_BodyGameObject")
        end,
        -- The partners' GameObjects (Ashley, Luis when he's one), for an actor that hides them [spikes/character_probe].
        partners = function()
            local mgr = sdk.get_managed_singleton("chainsaw.CharacterManager")
            local list = mgr and mgr:call("get_PartnerContextList")
            local out = {}
            for i = 0, (list and list:call("get_Count") or 0) - 1 do
                local ctx = list:call("get_Item", i)
                local body = ctx and ctx:call("get_BodyGameObject")
                if body then table.insert(out, body) end
            end
            return out
        end,
        -- Keep the player from being controlled, sent every frame while a cutscene plays: the game's own operation stop
        -- (as for its menus), layer Self. He stops by himself even with a direction held, and control comes back when
        -- it's no longer sent (spikes/hud_freeze_probe run 6, 2026-10-07). Switching off his head updater instead kept
        -- his last action (he walked on).
        hold_player = function()
            local mgr = sdk.get_managed_singleton("chainsaw.CharacterManager")
            if not mgr then error("no CharacterManager") end
            mgr:call("requestOperationStop", enum("chainsaw.CharacterControlIndex", "Player_1"),
                enum("chainsaw.character.PauseLayer", "Self"))
        end,
        -- The game's Display HUD option (chainsaw.OptionManager, OptionID.DisplayUI) set to `value`; gives the one it
        -- had. Values (probe): 0 none, 1 and 3 crosshair and damage edges, 2 everything.
        hud = function(value)
            local om = sdk.get_managed_singleton("chainsaw.OptionManager")
            if not om then error("no OptionManager") end
            local id = sdk.find_type_definition("chainsaw.option.OptionID"):get_field("DisplayUI"):get_data(nil)
            local before = om:call("getCurrentOptionValue", id)
            om:call("setCurrentOptionValue", id, value)
            return before
        end,
        hud_off = 0,
        -- A game movie by its id's name (chainsaw.MovieDefine.ID, e.g. "mva000"), played as the game plays its own
        -- (chainsaw.MovieMediator: load, then play) with the world paused. Seen with mva000 (spikes/new_movie_probe F7,
        -- 2026-10-07): full screen, Leon's position, health and animation unchanged while it played.
        movie_start = function(name)
            local f = sdk.find_type_definition("chainsaw.MovieDefine.ID"):get_field(name)
            local id
            if f then
                id = f:get_data(nil)
            elseif new_movies[name] then
                id = register_movie(name)
            else
                error("no movie " .. name .. ": not the game's, and no New movie block made one (in " .. MOVIES_DIR .. ")")
            end
            local m = { id = id, phase = "loading", since = now(), sound = not f and new_movies[name].sound }
            event_pause(true)
            instance("chainsaw.MovieMediator"):call("load", m.id)
            return m
        end,
        -- true once it has played to the end.
        movie_update = function(m)
            local mm = instance("chainsaw.MovieMediator")
            if m.phase == "loading" then
                if mm:call("IsLoaded", m.id) then
                    mm:call("play", m.id, nil, nil)
                    m.phase, m.since = "starting", now()
                    if m.sound then  -- a new movie's own sound; if it fails, the movie plays on without it
                        local ok, h = pcall(sound_play, m.sound)
                        if ok then m.playing_sound = h else m.sound_error = h end
                    end
                elseif now() - m.since > 20 then
                    error("not loaded after 20 s")
                end
            elseif m.phase == "starting" then
                if mm:call("isPlaying") then m.phase = "playing" elseif now() - m.since > 15 then error("didn't start") end
            elseif not mm:call("isPlaying") then
                return true
            end
            return false
        end,
        -- Where the player is, by the game's names [dump: chainsaw.CampaignManager]: its chapter (ChapterID), and its
        -- StageIdentifier's location (LocID), area (AreaID) and stage (StageID). For triggers. [Not seen in game yet.]
        where = function()
            local cm = instance("chainsaw.CampaignManager")
            local si = cm:call("get_CurrentStageIdentifier")
            return { chapter = enum_name("chainsaw.ChapterID", cm:call("get_CurrentChapter")),
                     location = enum_name("chainsaw.LocID", si:get_field("_Location")),
                     area = enum_name("chainsaw.AreaID", si:get_field("_Area")),
                     stage = enum_name("chainsaw.StageID", si:get_field("_Stage")) }
        end,
        -- The game is busy with something a trigger shouldn't start over: its own movie, or a pause (menus).
        busy = function()
            return instance("chainsaw.MovieMediator"):call("isPlaying") or instance("share.PauseManager"):call("isPaused()")
        end,
        -- The characters about (partners, and NPCs such as the merchant): { kind, position }, the kind by the game's
        -- name (chainsaw.CharacterKindID, e.g. ch3_a8z0) [dump: CharacterManager get_DollNpcContextList /
        -- get_PartnerContextList; CharacterContext get_KindID, get_Position]. For talk triggers. [Not seen in game yet.]
        characters = function()
            local mgr = sdk.get_managed_singleton("chainsaw.CharacterManager")
            local out = {}
            for _, getter in ipairs({ "get_DollNpcContextList", "get_PartnerContextList" }) do
                local list = mgr and mgr:call(getter)
                for i = 0, (list and list:call("get_Count") or 0) - 1 do
                    local ctx = list:call("get_Item", i)
                    local pos = ctx and ctx:call("get_Position")
                    if pos then
                        table.insert(out, { kind = enum_name("chainsaw.CharacterKindID", ctx:call("get_KindID")), position = pos })
                    end
                end
            end
            return out
        end,
        -- Every story flag: { name, group (its group's name), g, index } [dump: chainsaw.ScenarioFlagManager _Group;
        -- its Group get_Name / get_Variables / isOn(index); via.userdata.UserVariables getVariableCount / getVariable;
        -- Variable get_Name]. [inferred: a flag's index in its group is its variable's index; not seen in game yet]
        story_flags = function()
            local mgr = instance("chainsaw.ScenarioFlagManager")
            local groups = mgr and mgr:get_field("_Group")
            local out = {}
            for _, g in ipairs(groups and groups:get_elements() or {}) do
                local vars = g:call("get_Variables")
                local gname = tostring(g:call("get_Name"))
                for i = 0, (vars and vars:call("getVariableCount") or 0) - 1 do
                    local v = vars:call("getVariable", i)
                    local name = v and v:call("get_Name")
                    if name then table.insert(out, { name = tostring(name), group = gname, g = g, index = i }) end
                end
            end
            return out
        end,
        flag_on = function(f) return f.g:call("isOn", f.index) end,
        -- One of the flags the game's code names itself (chainsaw.ScenarioFlagDefine's static Flag {DataName, Group,
        -- Index}, e.g. DifficultyHard, Ch1f0z0LuisArrivedDemoAfter), by ScenarioFlagManager checkFlag(group, index); nil
        -- if it names none [dump; not seen in game yet].
        named_flag_on = function(name)
            local f = sdk.find_type_definition("chainsaw.ScenarioFlagDefine"):get_field(name)
            if not f or not f:is_static() then return nil end
            local flag = f:get_data(nil)
            return instance("chainsaw.ScenarioFlagManager"):call("checkFlag(System.Int32, System.Int32)",
                flag:get_field("Group"), flag:get_field("Index"))
        end,
        -- The save's play time (share.GameClock, counts while playing; a save loaded brings its own): for "once per save".
        play_time = function() return instance("share.GameClock"):call("get_ActualPlayingTime") end,
        -- The game's own movie or cutscene playing now: "movie" or "event", and its id's name (e.g. mva000, csa012), or
        -- nil [dump: MovieMediator / TimelineEventMediator isPlaying and getWork(ID); MovieWork get_IsPlaying;
        -- TimelineEventWork _EventPhase Playing]. Its id is looked for only while one plays. [Not seen in game yet.]
        playing_now = function()
            for _, k in ipairs({ { "movie", "chainsaw.MovieMediator", "chainsaw.MovieDefine.ID" },
                                  { "event", "chainsaw.TimelineEventMediator", "chainsaw.TimelineEventDefine.ID" } }) do
                local med = instance(k[2])
                if med and med:call("isPlaying") then
                    local phase = k[1] == "event" and enum("chainsaw.TimelineEventWork.EventPhase", "Playing")
                    for name, id in pairs(enum_values(k[3])) do
                        local w = med:call("getWork", id)
                        if w and (phase and w:get_field("_EventPhase") == phase or not phase and w:call("get_IsPlaying")) then
                            return k[1], name
                        end
                    end
                end
            end
            return nil
        end,
        movie_stop = function(m)
            local mm = instance("chainsaw.MovieMediator")
            if m.phase ~= "loading" and mm:call("isPlaying") then pcall(mm.call, mm, "requestSkip", m.id) end
            if m.playing_sound then sound_stop(m.playing_sound) end
            pcall(mm.call, mm, "unload", m.id)
            event_pause(false)
        end,
    },
}
local game = GAMES[reframework:get_game_name()]

-- ---- Cutscene files ----
local cutscenes = {}  -- { file, data }
local load_error = nil

local function load_all()
    cutscenes, load_error = {}, nil
    local ok, files = pcall(fs.glob, DIR .. "[/\\\\].*\\.json$")
    if not ok or not files then
        load_error = "couldn't list " .. DIR .. ": " .. tostring(files)
        return
    end
    for _, path in ipairs(files) do
        local rel = path:match("[/\\]data[/\\](.*)$") or path  -- relative to reframework\data, as json.load_file wants
        if not rel:match("recording%.json$") then
            local data = json.load_file(rel)
            if type(data) == "table" then
                table.insert(cutscenes, { file = rel, data = data })
            else
                load_error = "couldn't read " .. rel
            end
        end
    end
    table.sort(cutscenes, function(a, b) return a.file < b.file end)

    for dir, into in pairs({ [MOVIES_DIR] = {}, [SOUNDS_DIR] = {} }) do
        local ok2, notes = pcall(fs.glob, dir .. "[/\\\\].*\\.json$")
        for _, path in ipairs(ok2 and notes or {}) do
            local rel = path:match("[/\\]data[/\\](.*)$") or path
            local note = json.load_file(rel)
            if type(note) == "table" and type(note.name) == "string" then into[note.name] = note end
        end
        if dir == MOVIES_DIR then new_movies = into else new_sounds = into end
    end
end

-- ---- The camera ----
local function camera_parts()
    local cam = sdk.get_primary_camera()
    if not cam then return nil end
    local go = cam:call("get_GameObject")
    return cam, go and go:call("get_Transform")
end

local function smooth(u) return u * u * (3 - 2 * u) end
local function lerp(a, b, u) return a + (b - a) * u end

-- The pose at time t between the keys around it. A key's ease says how the camera arrives at it: "smooth" (default),
-- "linear", or "cut" (stays on the key before, then jumps).
local function camera_at(keys, t)
    if #keys == 0 then return nil end
    local a, b, u = keys[#keys], keys[#keys], 0
    if t <= keys[1].t then
        a, b = keys[1], keys[1]
    else
        for i = 1, #keys - 1 do
            if t <= keys[i + 1].t then
                a, b = keys[i], keys[i + 1]
                u = (t - a.t) / math.max(b.t - a.t, 0.000001)
                local ease = b.ease or "smooth"
                if ease == "smooth" then u = smooth(u) elseif ease == "cut" then u = 0 end
                break
            end
        end
    end
    local pos = Vector3f.new(lerp(a.position[1], b.position[1], u), lerp(a.position[2], b.position[2], u),
        lerp(a.position[3], b.position[3], u))
    local qa = Quaternion.new(a.rotation[4], a.rotation[1], a.rotation[2], a.rotation[3])
    local qb = Quaternion.new(b.rotation[4], b.rotation[1], b.rotation[2], b.rotation[3])
    local fov = (a.fov and b.fov) and lerp(a.fov, b.fov, u) or nil
    return pos, qa:slerp(qb, u), fov
end

-- ---- Actors: other characters, as puppets (spikes/character_probe.md, runs 4-12) ----
-- An actor is a puppet built from the game's files, as the game's own events build theirs, from a definition in
-- reframework\data\remod_puppets\<name>.json: skeleton, motion banks, and parts (a mesh and its material each; a
-- parent_joint, else it follows the skeleton's joints of the same names). What the probe found, all kept here:
--   - from files, a character needn't be loaded in the level (Luis anywhere);
--   - a Mesh handed a file that hasn't loaded yet never shows, and a new one made once it has does; so every actor's
--     files are requested when the cutscenes are read, the parts are made when one plays, and a part not ready to draw
--     1 s later is replaced by a fresh one (run 12: each needed one, ready at once);
--   - never destroyed (building after a destroy crashed the game twice): put away (hidden, not updating) when a
--     cutscene ends and used again, found by its name after Reset Scripts;
--   - its idle started at once (a new Motion plays nothing).
-- ponytail: Strands hair isn't built (from files it didn't show; definitions use the plain hair mesh), nor chains.
local PUPPETS_DIR = "remod_puppets"
local puppet_defs = {}  -- name -> definition
local preloaded = {}    -- name -> holders, or { error = text }
local anim_holders = {}  -- an animation file's game path -> its resource holder (cutscenes' animation_files)
local put_away = {}     -- name -> its root GameObject
local STUCK = "remod_stuck_part"

local function component(go, t) return go:call("getComponent(System.Type)", sdk.typeof(t)) end
local function add_component(go, t) return go:call("createComponent(System.Type)", sdk.typeof(t)) end

local create_method = nil
local function create_object(name)
    create_method = create_method or sdk.find_type_definition("via.GameObject"):get_method("create(System.String)")
    return keep(create_method:call(nil, sdk.create_managed_string(name)))
end

-- A GameObject and every one under it.
local function tree(go)
    local out = { go }
    local function walk(x)
        while x do
            table.insert(out, x:call("get_GameObject"))
            walk(x:call("get_Child"))
            x = x:call("get_Next")
        end
    end
    walk(go:call("get_Transform"):call("get_Child"))
    return out
end

local function set_drawn(go, on)
    for _, o in ipairs(tree(go)) do o:call("set_DrawSelf", on) end
end
local function set_active(go, on)
    for _, o in ipairs(tree(go)) do
        o:call("set_DrawSelf", on)
        o:call("set_UpdateSelf", on)
    end
end

-- A resource holder for a game file (no natives/STM, no suffix); nil without a path. Never hand a nil to the game:
-- that crashes it.
local function holder(rtype, path)
    if type(path) ~= "string" or path == "" then return nil end
    local res = sdk.create_resource(rtype, path)
    if not res then error("not in the game's files: " .. path) end
    res:add_ref()  -- a resource isn't managed by REFramework: this one is the game's own count
    return keep(res:create_holder(rtype .. "Holder"))
end

-- Every file a definition names, requested now (they load in under a second).
local function preload(def)
    local h = { parts = {}, dynamic = {} }
    h.skeleton = holder("via.motion.SkeletonResource", def.skeleton)
    h.bank = holder("via.motion.MotionBankResource", def.motion_bank)
    for _, path in ipairs(def.dynamic_banks or {}) do table.insert(h.dynamic, holder("via.motion.MotionBankResource", path)) end
    for i, part in ipairs(def.parts or {}) do
        h.parts[i] = { mesh = holder("via.render.MeshResource", part.mesh),
                       material = holder("via.render.MeshMaterialResource", part.material) }
    end
    return h
end

local function add_motion(go, bank, banks, layers)
    local m = add_component(go, "via.motion.Motion")
    if bank then m:call("set_MotionBankAsset", bank) end
    m:call("setDynamicMotionBankCount", #banks)
    for i, d in ipairs(banks) do m:call("setDynamicMotionBank", i - 1, d) end
    m:call("setLayerCount", layers)
    for l = 0, layers - 1 do
        if not m:call("getLayer", l) then m:call("setLayer", l, keep(sdk.create_instance("via.motion.TreeLayer"))) end
    end
end

local function make_part(parent, name, mesh, material, parent_joint, bank, banks)
    local go = create_object("remod_puppet_" .. name)
    if mesh then
        local m = add_component(go, "via.render.Mesh")
        m:call("setMesh", mesh)
        if material then m:call("set_Material", material) end
    end
    add_motion(go, bank, banks, 0)
    local x = go:call("get_Transform")
    x:call("set_Parent", parent)
    x:call("set_LocalPosition", Vector3f.new(0, 0, 0))
    x:call("set_LocalRotation", Quaternion.new(1, 0, 0, 0))
    if parent_joint then x:call("set_ParentJoint", tostring(parent_joint)) else x:call("set_SameJointsConstraint", true) end
    return go
end

local function build_puppet(name, def, h)
    local root = create_object("remod_puppet_" .. name)
    local banks = {}
    for _, bh in ipairs(h.dynamic) do
        local d = keep(sdk.create_instance("via.motion.DynamicMotionBank") or sdk.create_instance("via.motion.DynamicMotionBank", true))
        d:call("set_MotionBank", bh)
        table.insert(banks, d)
    end
    if h.skeleton then add_component(root, "via.motion.DummySkeleton"):call("set_SkeletonResourceHandle", h.skeleton) end
    add_motion(root, h.bank, banks, tonumber(def.layers) or 13)
    local rx = root:call("get_Transform")
    for i, part in ipairs(def.parts or {}) do
        make_part(rx, tostring(part.name or "part"), h.parts[i].mesh, h.parts[i].material, part.parent_joint, h.bank, banks)
    end
    return root
end

-- The safety net: each mesh part not ready to draw 1 s after it shows is replaced by a fresh one with the same files
-- and motion banks; the stuck one is hidden and renamed, never destroyed. Up to 5 times.
local watching = {}  -- { go, mesh, t0, tries, actor }
local function watch_parts(root, actor)
    for _, go in ipairs(tree(root)) do
        local m = component(go, "via.render.Mesh")
        if m and go:call("get_Name") ~= STUCK then
            table.insert(watching, { go = go, mesh = m, t0 = now(), tries = 0, actor = actor })
        end
    end
end

local function fresh_part(go, m)
    local x = go:call("get_Transform")
    local new = create_object(go:call("get_Name"))
    local nm = add_component(new, "via.render.Mesh")
    nm:call("setMesh", m:call("getMesh"))
    local mat = m:call("get_Material")
    if mat then nm:call("set_Material", mat) end
    local motion = component(go, "via.motion.Motion")
    if motion then
        local banks = {}
        for b = 0, motion:call("getDynamicMotionBankCount") - 1 do table.insert(banks, motion:call("getDynamicMotionBank", b)) end
        add_motion(new, motion:call("get_MotionBankAsset"), banks, 0)
    end
    local nx = new:call("get_Transform")
    nx:call("set_Parent", x:call("get_Parent"))
    nx:call("set_LocalPosition", x:call("get_LocalPosition"))
    nx:call("set_LocalRotation", x:call("get_LocalRotation"))
    local pj = x:call("get_ParentJoint")
    if pj and tostring(pj) ~= "" then nx:call("set_ParentJoint", pj) end
    nx:call("set_SameJointsConstraint", x:call("get_SameJointsConstraint"))
    set_active(go, false)
    go:call("set_Name", STUCK)
    return new, nm
end

-- Where an actor stands: its position and rotation (written down in game), else offset [right, up, forward] metres
-- from the player (default 1.5 m in front), facing him. Right is (-forward z, forward x): the first actor test
-- (2026-10-08) put the actor at -0.8 on his right with (forward z, -forward x).
local function actor_place(a)
    if a.position then
        local p, r = a.position, a.rotation
        return Vector3f.new(p[1], p[2], p[3]), Quaternion.new(r[4], r[1], r[2], r[3])
    end
    local body = game.player()
    if not body then error("no player to place it by") end
    local x = body:call("get_Transform")
    local lp, lq = x:call("get_Position"), x:call("get_Rotation")
    local fx, fz = 2 * (lq.x * lq.z + lq.w * lq.y), 1 - 2 * (lq.x * lq.x + lq.y * lq.y)  -- his +z, flat
    local len = math.sqrt(fx * fx + fz * fz)
    if len < 1e-6 then fx, fz, len = 0, 1, 1 end
    fx, fz = fx / len, fz / len
    local o = a.offset or { 0, 0, 1.5 }
    local px, py, pz = lp.x - fz * o[1] + fx * o[3], lp.y + o[2], lp.z + fx * o[1] + fz * o[3]
    local yaw = math.atan(lp.x - px, lp.z - pz)
    return Vector3f.new(px, py, pz), Quaternion.new(math.cos(yaw / 2), 0, math.sin(yaw / 2), 0)
end

local function scene_find(name)
    local scene = sdk.find_type_definition("via.SceneManager"):get_method("get_CurrentScene"):call(nil)
    return scene and scene:call("findGameObject(System.String)", name)
end

-- The actor's puppet out and in place, in its idle; gives its root.
local function start_actor(a)
    local def = puppet_defs[a.puppet]
    if not def then error("no definition " .. PUPPETS_DIR .. "\\" .. tostring(a.puppet) .. ".json") end
    local root = put_away[a.puppet] or scene_find("remod_puppet_" .. a.puppet)
    put_away[a.puppet] = nil
    if root then
        set_active(root, true)
    else
        local h = preloaded[a.puppet]
        if not h or h.error then h = preload(def) end  -- not requested earlier: the safety net catches it
        root = build_puppet(a.puppet, def, h)
    end
    watch_parts(root, a.name)
    local pos, rot = actor_place(a)
    local x = root:call("get_Transform")
    x:call("set_Position", pos)
    x:call("set_Rotation", rot)
    local idle = type(def.idle) == "table" and def.idle or { bank = 1000, motion = 160 }
    local layer = component(root, "via.motion.Motion"):call("getLayer", 0)
    layer:call("changeMotion(System.UInt32, System.UInt32, System.Single, System.Single, via.motion.InterpolationMode, via.motion.InterpolationCurve)",
        idle.bank, idle.motion, 0.0, 0.0, 1, 0)
    return root
end

-- ---- Animation previewer (user, 2026-10-08) ----
-- In the menu: pick who (Leon, or a puppet brought out in front of him), one of its motion banks and an animation by
-- name; it plays at once, looping, with its numbers. "Use in a cutscene" writes them to remod_cutscenes\animation.json
-- for remod's cutscene editor (Add picked animation). The lists are the game's own [dump: via.motion.Motion
-- getActiveMotionBank / getMotionCount / getMotionInfoByIndex, via.motion.MotionInfo get_MotionID / get_MotionName /
-- get_MotionEndFrame]. While Leon previews one, he's held as in a cutscene, so his controls don't take over.
local preview = { who = "player", root = nil, banks = nil, bank = nil, motions = nil, filter = "", picked = nil }

local function preview_motion()
    local root = preview.who == "player" and game.player() or preview.root
    return root and component(root, "via.motion.Motion")
end

local function preview_put_away()
    preview.building = false
    if preview.root then
        pcall(set_active, preview.root, false)
        put_away[preview.who] = preview.root
    end
    preview.root, preview.banks, preview.bank, preview.motions, preview.picked = nil, nil, nil, nil, nil
end

-- Work that changes the game's objects (building a puppet, changing a Motion's banks) waits for the game's own update
-- (UpdateBehavior, below) instead of running from the menu: building Luis from the menu crashed the game on one of
-- its worker threads (user, 2026-10-08; nothing logged by us), while the same build from a cutscene's key works.
local in_update = {}  -- { what, f }
local function on_update(what, f, quiet) table.insert(in_update, { what = what, f = f, quiet = quiet }) end

local function preview_choose(who)
    local old_root, old_who = preview.root, preview.who
    preview.root, preview.banks, preview.bank, preview.motions, preview.picked = nil, nil, nil, nil, nil
    preview.who, preview.problem, preview.building = who, nil, true
    on_update("previewing " .. who, function()
        if old_root then  -- the one shown before, put away for next time
            pcall(set_active, old_root, false)
            put_away[old_who] = old_root
        end
        preview.building = false
        if who == "player" or preview.who ~= who then return end  -- Leon, or another picked meanwhile
        preview.root = start_actor({ name = "preview", puppet = who, offset = { 0, 0, 2 } })
        preview.since = now()
    end)
end

-- The banks it has: {id, name}, by id.
local function preview_banks()
    local m = preview_motion()
    local out, seen = {}, {}
    for i = 0, (m and m:call("getActiveMotionBankCount") or 0) - 1 do
        local b = m:call("getActiveMotionBank", i)
        local id = b and b:call("get_BankID")
        if id and not seen[id] then
            seen[id] = true
            local list = b:call("get_MotionList")
            local path = list and list:call("get_ResourcePath")
            local name = path and tostring(path):match("([^/]+)%.motlist") or tostring(b:call("get_Name"))
            table.insert(out, { id = id, name = name })
        end
    end
    table.sort(out, function(a, c) return a.id < c.id end)
    return out
end

-- A bank's animations: {id, name, frames}.
local function preview_motions(bank)
    local m = preview_motion()
    local info = sdk.create_instance("via.motion.MotionInfo") or sdk.create_instance("via.motion.MotionInfo", true)
    local out = {}
    for i = 0, m:call("getMotionCount", bank) - 1 do
        if m:call("getMotionInfoByIndex(System.UInt32, System.UInt32, via.motion.MotionInfo)", bank, i, info) then
            table.insert(out, { id = info:call("get_MotionID"), name = tostring(info:call("get_MotionName")),
                                frames = info:call("get_MotionEndFrame") })
        end
    end
    return out
end

local function preview_play(mo)
    local layer = preview_motion():call("getLayer", 0)
    layer:call("changeMotion(System.UInt32, System.UInt32, System.Single, System.Single, via.motion.InterpolationMode, via.motion.InterpolationCurve)",
        preview.bank, mo.id, 0.0, 0.0, 1, 0)
    layer:call("set_WrapMode", enum("via.motion.WrapMode", "Loop"))
    layer:call("set_Speed", 1.0)
    preview.picked = { bank = preview.bank, motion = mo.id, name = mo.name, frames = mo.frames }
end

-- An animation file (.motlist: a game cutscene's own, later a new one) put on a character as a bank of our own number,
-- with no .motbank: a via.motion.DynamicMotionBank given the file (set_MotionList) and our number (set_OverwriteBankID,
-- set_BankID) [dump], added to its Motion's dynamic banks. Spike: spikes/event_animation_test.md. The file is requested
-- first and added a second later (a resource handed over before it loaded never took, for meshes: character probe
-- run 11). Each step's result goes in file_notes, shown in the menu.
local CUTSCENE_FILES = {  -- csa012 (Luis and Leon); the spike's defaults
    player = "_Chainsaw/Event/cs/csa012/csa012_s00/chara/cha000_00/cha000_00.motlist",
    luis = "_Chainsaw/Event/cs/csa012/csa012_s00/chara/cha300_00/cha300_00.motlist",
}
local loading_files = {}  -- { motion, holder, bank, path, t0 }
local file_notes = {}

local function add_motion_file(f)
    local m = f.motion
    if m:call("getMotionCount", f.bank) > 0 then
        return "bank " .. f.bank .. " is already there: pick another number"
    end
    local d = keep(sdk.create_instance("via.motion.DynamicMotionBank") or sdk.create_instance("via.motion.DynamicMotionBank", true))
    d:call("set_MotionList", f.holder)
    d:call("set_OverwriteBankID", true)
    d:call("set_BankID", f.bank)
    local banks = {}
    for i = 0, m:call("getDynamicMotionBankCount") - 1 do table.insert(banks, m:call("getDynamicMotionBank", i)) end
    table.insert(banks, d)
    m:call("setDynamicMotionBankCount", #banks)
    for i, b in ipairs(banks) do m:call("setDynamicMotionBank", i - 1, b) end
    local n = m:call("getMotionCount", f.bank)
    if n > 0 then return string.format("bank %d: %d animations", f.bank, n) end
    m:call("setupMotionBank")
    n = m:call("getMotionCount", f.bank)
    return string.format("bank %d: %d animations after setupMotionBank (0 before)", f.bank, n)
end

local function add_motion_files()
    for i = #loading_files, 1, -1 do
        local f = loading_files[i]
        if now() - f.t0 > 1 then
            table.remove(loading_files, i)
            local ok, note = pcall(add_motion_file, f)
            table.insert(file_notes, f.path .. ": " .. (ok and note or ("failed: " .. tostring(note))))
            if ok and f.motion:call("getMotionCount", f.bank) > 0 then
                preview.files = preview.files or {}  -- for Use in a cutscene: the cutscene loads the file too
                preview.files[f.who .. "#" .. f.bank] = f.path
            end
            log.info("[remod_cutscene] " .. file_notes[#file_notes])
            preview.banks, preview.motions = nil, nil  -- list them again
        end
    end
end

-- The definitions, and every actor's files requested now, so its parts show when its cutscene plays. After load_all.
local function load_puppets()
    puppet_defs = {}
    local ok, files = pcall(fs.glob, PUPPETS_DIR .. "[/\\\\].*\\.json$")
    for _, path in ipairs(ok and files or {}) do
        local rel = path:match("[/\\]data[/\\](.*)$") or path
        local def = json.load_file(rel)
        local name = rel:match("([^/\\]+)%.json$")
        if name and type(def) == "table" and type(def.parts) == "table" then puppet_defs[name] = def end
    end
    on_update("requesting the cutscenes' files", function()  -- resources are made in the game's update (§9)
        for _, c in ipairs(cutscenes) do
            for _, a in ipairs(type(c.data.actors) == "table" and c.data.actors or {}) do
                local def = puppet_defs[a.puppet]
                if def and not preloaded[a.puppet] then
                    local pok, h = pcall(preload, def)
                    preloaded[a.puppet] = pok and h or { error = tostring(h) }
                    if not pok then log.error("[remod_cutscene] puppet " .. tostring(a.puppet) .. ": " .. tostring(h)) end
                end
            end
            for _, af in ipairs(type(c.data.animation_files) == "table" and c.data.animation_files or {}) do
                if type(af.file) == "string" and not anim_holders[af.file] then
                    local hok, h = pcall(holder, "via.motion.MotionListResource", af.file)
                    if hok then anim_holders[af.file] = h
                    else log.error("[remod_cutscene] animation file " .. af.file .. ": " .. tostring(h)) end
                end
            end
        end
    end)
end

-- A cutscene's animation file on its character, as its bank (once: a puppet reused, or Leon a second time, has it).
-- ponytail: a bank number already on the character from elsewhere (the previewer, another file) is taken as this one.
local function animation_file_on(af, actors)
    local who = af.actor or "player"
    local body = who == "player" and game.player() or (actors[who] and actors[who].root)
    if not body then error("no " .. who .. " to put it on") end
    local m = component(body, "via.motion.Motion")
    if m:call("getMotionCount", af.bank) > 0 then return end
    local h = anim_holders[af.file] or holder("via.motion.MotionListResource", af.file)
    anim_holders[af.file] = h
    local note = add_motion_file({ motion = m, holder = h, bank = af.bank })
    if m:call("getMotionCount", af.bank) == 0 then error(note .. " (not loaded yet? play it again)") end
end

-- ---- Playing ----
local playing = nil  -- { data, started, fired = {}, fov_before, hud_before, actors = {name -> {root, puppet}}, hidden }

local problem = nil  -- the last one, shown in the menu; each also logged once (log.* reaches the log without
                     -- REFramework's "Log Lua Errors to Disk")
local function report(what, err)
    problem = what .. ": " .. tostring(err)
    log.error("[remod_cutscene] " .. problem)
end

-- f(...) for a part that may fail in a game update; nil and reported if it does.
local function try(what, f, ...)
    local ok, result = pcall(f, ...)
    if ok then return result end
    report(what, result)
end

local function stop()
    if not playing then return end
    if playing.movie then try("stopping the movie", game.movie_stop, playing.movie.m) end
    for _, h in ipairs(playing.sounds or {}) do pcall(sound_stop, h) end  -- the cutscene's sounds end with it
    if playing.fov_before then
        local cam = camera_parts()
        if cam then pcall(cam.call, cam, "set_FOV", playing.fov_before) end
    end
    if playing.hud_before then try("showing the HUD", game.hud, playing.hud_before) end
    for _, a in pairs(playing.actors or {}) do  -- put away for next time, never destroyed
        try("putting away " .. a.puppet, set_active, a.root, false)
        put_away[a.puppet] = a.root
    end
    watching = {}
    for _, body in ipairs(playing.hidden or {}) do pcall(set_drawn, body, true) end
    playing = nil  -- the player's controls come back as hold_player is no longer sent
end

-- While a cutscene plays the player can't be controlled (hold_player, each frame below) and the HUD is hidden; both
-- come back when it ends or stops.
local function play(data)
    stop()
    preview_put_away()  -- its puppet may be one of the actors
    local cam = camera_parts()
    local fov_before = nil
    if cam then
        local ok, fov = pcall(cam.call, cam, "get_FOV")
        fov_before = ok and fov or nil
    end
    playing = { data = data, started = now(), fired = {}, fov_before = fov_before,
                hud_before = try("hiding the HUD", game.hud, game.hud_off), actors = {}, hidden = {} }
    for _, a in ipairs(data.actors or {}) do
        local root = try("actor " .. tostring(a.name), start_actor, a)
        if root then playing.actors[a.name] = { root = root, puppet = a.puppet } end
        if a.hides == "partner" and #playing.hidden == 0 then  -- the real ones, while it plays
            for _, body in ipairs(try("finding the partners", game.partners) or {}) do
                try("hiding a partner", set_drawn, body, false)
                table.insert(playing.hidden, body)
            end
        end
    end
    for _, af in ipairs(data.animation_files or {}) do  -- before any motion uses their banks
        try("animation file " .. tostring(af.file), animation_file_on, af, playing.actors)
    end
end

-- Starting and stopping, as asked by a key, the menu or a trigger: in the game's update (§9 of remod's CLAUDE.md:
-- game objects change only there), so a press toggles once the game gets to it.
local function request_toggle(data, what)
    on_update((what or "playing") .. " " .. tostring(data.name or data.movie_of), function()
        if playing and playing.data == data then stop() else play(data) end
    end)
end

-- The cutscene's time; it stands still while a movie plays.
local function elapsed()
    if not playing then return 0 end
    return playing.movie and playing.movie.t or (now() - playing.started)
end

-- A movie at its time: the timeline waits until it has played (or failed: reported, and the cutscene goes on).
local function end_movie()
    try("stopping the movie", game.movie_stop, playing.movie.m)
    playing.started = now() - playing.movie.t
    playing.movie = nil
end

local function run_movies(t)
    if playing.movie then
        local ok, done = pcall(game.movie_update, playing.movie.m)
        if not ok then report("movie " .. playing.movie.id, done) end
        if playing.movie.m.sound_error then
            report("movie " .. playing.movie.id .. "'s sound", playing.movie.m.sound_error)
            playing.movie.m.sound_error = nil
        end
        if not ok or done then end_movie() end
        return
    end
    for i, m in ipairs(playing.data.movies or {}) do
        if not playing.fired["movie" .. i] and t >= m.t then
            playing.fired["movie" .. i] = true
            if not game.movie_start then return report("movie " .. m.id, "movies aren't supported in this game yet") end
            local started = try("movie " .. m.id, game.movie_start, m.id)
            if started then playing.movie = { t = m.t, id = m.id, m = started } end
            return
        end
    end
end

-- An animation on an actor ("player" only, for now).
local function player_layer()
    local player = game.player()
    local motion = player and player:call("getComponent(System.Type)", sdk.typeof("via.motion.Motion"))
    return motion and motion:call("getLayer", 0)
end

-- What the player is playing now, for writing motions: "bank 1000, motion 160, frame 76 of 2433".
local function current_motion()
    local layer = player_layer()
    if not layer then return "no player" end
    return string.format("bank %d, motion %d, frame %.0f of %.0f", layer:call("get_MotionBankID"),
        layer:call("get_MotionID"), layer:call("get_Frame"), layer:call("get_EndFrame"))
end

local function start_motion(m)
    local layer
    if m.actor and m.actor ~= "player" then
        local a = playing.actors[m.actor]
        local motion = a and component(a.root, "via.motion.Motion")
        layer = motion and motion:call("getLayer", 0)
    else
        layer = player_layer()
    end
    if not layer then return end
    pcall(layer.call, layer,
        "changeMotion(System.UInt32, System.UInt32, System.Single, System.Single, via.motion.InterpolationMode, via.motion.InterpolationCurve)",
        m.bank, m.motion, m.frame or 0.0, m.blend or 10.0, 1, 0)
end

local function set_camera()
    local pos, rot, fov = camera_at(playing.data.camera, elapsed())
    local cam, xform = camera_parts()
    if not pos then return end
    if not xform then error("no primary camera") end
    xform:call("set_Position", pos)
    xform:call("set_Rotation", rot)
    if fov then cam:call("set_FOV", fov) end
end

local function hold_camera()
    if not playing or not playing.data.camera then return end
    local ok, err = pcall(set_camera)
    if not ok and playing.camera_problem ~= err then  -- once per play, not every frame
        playing.camera_problem = err
        report("camera", err)
    end
end

if game then
    for _, hook in ipairs(game.camera_hooks) do
        local after = hook:match("^after (.+)$")
        if after then re.on_application_entry(after, hold_camera) else re.on_pre_application_entry(hook, hold_camera) end
    end
    -- The player held before the game's behaviour update, every frame of a cutscene (reported once per play).
    re.on_pre_application_entry("UpdateBehavior", function()
        if #in_update > 0 then
            local work = in_update
            in_update = {}
            for _, w in ipairs(work) do
                if not w.quiet then log.info("[remod_cutscene] " .. w.what) end  -- logged first, in case the game goes down
                local ok, err = pcall(w.f)
                if not ok then
                    report(w.what, err)
                    preview.problem = w.what .. ": " .. tostring(err)
                end
            end
        end
        if #loading_files > 0 then add_motion_files() end
        if not playing then
            if preview.who == "player" and preview.picked then pcall(game.hold_player) end  -- previewing on Leon
            return
        end
        local ok, err = pcall(game.hold_player)
        if not ok and playing.hold_problem ~= err then
            playing.hold_problem = err
            report("holding the player", err)
        end
    end)
end

-- ---- Recording camera keys ----
local recording = nil  -- { started, keys }

local function record_key()
    local cam, xform = camera_parts()
    if not xform then return end
    recording = recording or { started = now(), keys = {} }
    local p, q = xform:call("get_Position"), xform:call("get_Rotation")
    local ok, fov = pcall(cam.call, cam, "get_FOV")
    table.insert(recording.keys, {
        t = math.floor((now() - recording.started) * 100 + 0.5) / 100,
        position = { p.x, p.y, p.z },
        rotation = { q.x, q.y, q.z, q.w },
        fov = ok and fov or nil,
        ease = "smooth",
    })
    local last = recording.keys[#recording.keys].t
    json.dump_file(DIR .. "/recording.json", { schema_version = 0, name = "Recording", length = last, camera = recording.keys })
end

-- ---- Triggers: a cutscene starts by itself (its file's "trigger") ----
-- Every condition it names must hold: near (Leon within radius metres of a spot); the game's names for where he is
-- (chapter, location, area, stage, as the menu's Now line shows); talk (near a character of that kind: a prompt shows
-- and its key starts it); flags (story flags on, or off with "!"); after (one of the game's movies or cutscenes ended
-- in the last 10 s). It starts when they become true and have held for `delay` seconds (a talk trigger: when its key
-- is pressed then); not again until they've stopped holding. `once`: true (the default) once per save, by the save's
-- play time (loading a save from before it lets it play again); "session" once each time the game runs; false every
-- time. Never while a cutscene plays or the game is busy (its own movie, a pause). Checked 5 times a second.
local trigger_state = {}  -- cutscene file -> {held, since, fired, done, talk_ready}
local next_check = 0

local function player_position()
    local body = game.player()
    return body and body:call("get_Transform"):call("get_Position")
end

-- Once per save: the play time each cutscene file last started at by its trigger, in remod_cutscenes\fired.json.
-- ponytail: one record per cutscene, not per save slot; a save from another playthrough with more play time counts
-- as after it.
local fired_at = nil
local function fired_before(c, t)
    local s = trigger_state[c.file]
    if t.once == false then return false end
    if t.once == "session" then return s.fired end
    fired_at = fired_at or (json.load_file(DIR .. "/fired.json") or {})
    local at = fired_at[c.file]
    if not at then return false end
    local ok, pt = pcall(game.play_time)
    return not ok or pt >= at  -- the play time unknown: taken as after it
end

local function fire(c)
    local s = trigger_state[c.file]
    s.fired, s.done, s.talk_ready = true, true, false
    local ok, pt = pcall(game.play_time)
    if ok then
        fired_at = fired_at or (json.load_file(DIR .. "/fired.json") or {})
        fired_at[c.file] = pt
        pcall(json.dump_file, DIR .. "/fired.json", fired_at)
    end
    log.info("[remod_cutscene] " .. (c.data.name or c.file) .. " triggered")
    if not (playing and playing.data == c.data) then request_toggle(c.data, "triggered") end
end

-- Story flags by name ("Name", or "Group/Name" when two groups share one), read once the game has them.
local flags_by_name, flags_list, flags_read_at = nil, nil, -100
local function read_flags()
    if flags_list and #flags_list > 0 or now() - flags_read_at < 10 then return end
    flags_read_at = now()
    local ok, list = pcall(game.story_flags)
    if not ok then return end
    flags_list, flags_by_name = list, {}
    for _, f in ipairs(list) do
        flags_by_name[f.group .. "/" .. f.name] = f
        if not flags_by_name[f.name] then flags_by_name[f.name] = f end
    end
end
local function flag_is_on(name)  -- nil if there's no such flag (yet)
    read_flags()
    local f = flags_by_name and flags_by_name[name]
    if not f then  -- one the game's code names itself, if it's that
        local nok, on = pcall(game.named_flag_on, name)
        if nok then return on end
        return nil
    end
    local ok, on = pcall(game.flag_on, f)
    if not ok then return nil end
    return on
end

-- The game's own movie or cutscene: the one playing now, and the last of each kind that ended (for `after`).
local game_now, game_ended = nil, {}  -- { kind, name }; kind -> { name, at }
local function watch_game()
    local ok, kind, name = pcall(game.playing_now)
    if not ok then return end
    if game_now and game_now.name ~= name then game_ended[game_now.kind] = { name = game_now.name, at = now() } end
    game_now = kind and { kind = kind, name = name } or nil
end

local function holds(t, where, pos, chars)
    if t.near then
        if not pos then return false end
        local p = t.near.position
        local dx, dy, dz = pos.x - p[1], pos.y - p[2], pos.z - p[3]
        if dx * dx + dy * dy + dz * dz > t.near.radius * t.near.radius then return false end
    end
    for _, key in ipairs({ "chapter", "location", "area", "stage" }) do
        if t[key] and (not where or where[key] ~= t[key]) then return false end
    end
    if type(t.after) == "table" then
        local kind = t.after.movie and "movie" or "event"
        local e = game_ended[kind]
        if not e or e.name ~= (t.after.movie or t.after.event) or now() - e.at > 10 then return false end
    end
    for _, f in ipairs(type(t.flags) == "table" and t.flags or {}) do
        local off = f:sub(1, 1) == "!"
        local on = flag_is_on(off and f:sub(2) or f)
        if on == nil or on == off then return false end
    end
    if type(t.talk) == "table" then
        if not pos then return false end
        local r, near = t.talk.radius or 2.5, false
        for _, ch in ipairs(chars or {}) do
            local dx, dy, dz = pos.x - ch.position.x, pos.y - ch.position.y, pos.z - ch.position.z
            if ch.kind == t.talk.npc and dx * dx + dy * dy + dz * dz <= r * r then near = true end
        end
        if not near then return false end
    end
    return true
end

local function check_triggers()
    if playing or not game.where or now() < next_check then return end
    next_check = now() + 0.2
    watch_game()
    local busy_ok, busy = pcall(game.busy)
    if busy_ok and busy then
        for _, s in pairs(trigger_state) do s.talk_ready = false end
        return
    end
    local where_ok, where = pcall(game.where)
    local pos = select(2, pcall(player_position))
    local chars = nil
    for _, c in ipairs(cutscenes) do
        local t = c.data.trigger
        if type(t) == "table" then
            local s = trigger_state[c.file] or {}
            trigger_state[c.file] = s
            if type(t.talk) == "table" and not chars then
                local cok, list = pcall(game.characters)
                chars = cok and list or {}
            end
            local now_holds = holds(t, where_ok and where or nil, type(pos) == "userdata" and pos or nil, chars)
            if now_holds and not s.held then s.since = now() end
            s.held = now_holds
            if not now_holds then s.done = false end
            local ready = now_holds and not s.done and now() - s.since >= (t.delay or 0) and not fired_before(c, t)
            if type(t.talk) == "table" then
                s.talk_ready = ready  -- its key starts it (each frame, below)
            elseif ready then
                fire(c)
                return
            end
        end
    end
end

-- "Make a trigger here": Leon's spot (2 m around him) and the game's names for where he is, into
-- remod_cutscenes\trigger.json; remod's Use trigger puts it into a cutscene file. The menu's other buttons add to it.
local function make_trigger()
    local pos = player_position()
    if not pos then error("no player (load a save first)") end
    local where = game.where()
    json.dump_file(DIR .. "/trigger.json", { near = { position = { pos.x, pos.y, pos.z }, radius = 2.0 },
                                             chapter = where.chapter, stage = where.stage })
end

-- Adds a condition to trigger.json (made if there's none yet): a story flag, a talk, an after.
local trigger_shown = {}  -- the menu's view of trigger.json: { at, t }
local function trigger_add(key, value)
    trigger_shown.at = nil  -- show it again at once
    local t = json.load_file(DIR .. "/trigger.json")
    if type(t) ~= "table" then t = {} end
    if key == "flags" then
        t.flags = type(t.flags) == "table" and t.flags or {}
        table.insert(t.flags, value)
    else
        t[key] = value
    end
    json.dump_file(DIR .. "/trigger.json", t)
end

-- Story flags as they change while you play, for finding the one a moment sets (the menu's Story flags > Watch):
-- each frame a slice of them is read, so a whole pass takes a moment.
local flag_watch = { on = false, at = 1, last = {}, changes = {} }  -- last: flag -> on; changes: { name, group, on, at }
local function watch_flags()
    read_flags()
    if not flags_list or #flags_list == 0 then return end
    for _ = 1, math.min(500, #flags_list) do
        local f = flags_list[flag_watch.at]
        flag_watch.at = flag_watch.at % #flags_list + 1
        local ok, on = pcall(game.flag_on, f)
        if ok then
            local before = flag_watch.last[f]
            if before ~= nil and before ~= on then
                table.insert(flag_watch.changes, 1, { name = f.name, group = f.group, on = on, at = now() })
                if #flag_watch.changes > 20 then table.remove(flag_watch.changes) end
            end
            flag_watch.last[f] = on
        end
    end
end

-- A talk trigger's prompt, near the bottom of the screen.
local function draw_prompt(talk)
    local size = imgui.get_display_size()
    local text = "[" .. (talk.key or "G") .. "] " .. (talk.prompt or "Talk")
    local x, y = size.x * 0.5 - #text * 4, size.y * 0.72
    draw.filled_rect(x - 12, y - 6, #text * 8 + 24, 28, 0xA0000000)
    draw.text(text, x, y, 0xFFFFFFFF)
end

-- ---- Each frame: keys, motions, letterbox, fades, subtitles ----
local key_down = {}
local function pressed(vk)
    local is = reframework:is_key_down(vk)
    local was = key_down[vk]
    key_down[vk] = is
    return is and not was
end

local KEY_CODES = { F1 = 0x70, F2 = 0x71, F3 = 0x72, F4 = 0x73, F5 = 0x74, F6 = 0x75, F7 = 0x76, F8 = 0x77,
                    F9 = 0x78, F11 = 0x7A, F12 = 0x7B }
for i = 0, 25 do KEY_CODES[string.char(65 + i)] = 0x41 + i end  -- A-Z, for talk triggers

local function draw_overlays(data, t)
    local size = imgui.get_display_size()
    local w, h = size.x, size.y
    local bar = (data.letterbox or 0) * h
    if bar > 0 then
        draw.filled_rect(0, 0, w, bar, 0xFF000000)
        draw.filled_rect(0, h - bar, w, bar, 0xFF000000)
    end
    for _, s in ipairs(data.subtitles or {}) do
        if t >= s.t and t < s["until"] then
            draw.text(s.text, w * 0.1, h - math.max(bar, h * 0.06) - 30, 0xFFFFFFFF)
        end
    end
    for _, f in ipairs(data.fades or {}) do  -- black, from / to opacity 0-1
        if t >= f.t and t < f["until"] then
            local u = (t - f.t) / math.max(f["until"] - f.t, 0.000001)
            local alpha = math.floor(math.max(0, math.min(1, lerp(f.from, f.to, u))) * 255)
            draw.filled_rect(0, 0, w, h, alpha << 24)
        end
    end
end

re.on_frame(function()
    frames = frames + 1
    if not game then return end
    if pressed(RECORD_KEY) then record_key() end
    for _, c in ipairs(cutscenes) do
        local key = c.data.start and KEY_CODES[c.data.start.key]
        if key and pressed(key) then
            request_toggle(c.data)
        end
    end
    check_triggers()
    if flag_watch.on then watch_flags() end
    if not playing then  -- talk triggers ready: their prompt, and their key starts them
        for _, c in ipairs(cutscenes) do
            local s = trigger_state[c.file]
            local talk = s and s.talk_ready and c.data.trigger.talk
            if talk then
                draw_prompt(talk)
                local key = KEY_CODES[talk.key or "G"]
                if key and pressed(key) then fire(c) end
            end
        end
    end
    for i = #watching, 1, -1 do  -- actors' (and the previewer's) parts not yet ready to draw
        local w = watching[i]
        local ok, ready = pcall(w.mesh.call, w.mesh, "get_ReadyToDraw")
        if not ok or ready or w.tries >= 5 then
            if ok and not ready then report("actor " .. w.actor, "a part never became ready to draw") end
            table.remove(watching, i)
        elseif now() - w.t0 > 1 then
            local fok, go, m = pcall(fresh_part, w.go, w.mesh)
            if fok then
                w.go, w.mesh, w.tries, w.t0 = go, m, w.tries + 1, now()
            else
                report("actor " .. w.actor, go)
                table.remove(watching, i)
            end
        end
    end
    if not playing then return end
    run_movies(elapsed())
    local t = elapsed()
    if not playing.movie and t >= (playing.data.length or 0) then
        stop()
        return
    end
    for i, m in ipairs(playing.data.motions or {}) do
        if not playing.fired[i] and t >= m.t then
            playing.fired[i] = true
            start_motion(m)
        end
    end
    for i, s in ipairs(playing.data.sounds or {}) do  -- New sound blocks' sounds, by name
        if not playing.fired["sound" .. i] and t >= s.t then
            playing.fired["sound" .. i] = true
            if not new_sounds[s.id] then
                report("sound " .. s.id, "no New sound block made it (in " .. SOUNDS_DIR .. ")")
            else
                local h = try("sound " .. s.id, sound_play, new_sounds[s.id].sound)
                if h then
                    playing.sounds = playing.sounds or {}
                    table.insert(playing.sounds, h)
                end
            end
        end
    end
    -- Not over a movie once it shows (while it loads they cover the wait, e.g. a fade held black).
    -- ponytail: so no subtitles over a movie.
    if not (playing.movie and playing.movie.m.phase ~= "loading") then draw_overlays(playing.data, t) end
end)

re.on_draw_ui(function()
    if not imgui.tree_node("remod cutscenes") then return end
    if not game then
        imgui.text("Not supported in " .. tostring(reframework:get_game_name()) .. " yet.")
        imgui.tree_pop()
        return
    end
    if imgui.button("Reload cutscenes") then
        load_all()
        load_puppets()
    end
    if load_error then imgui.text("Problem: " .. load_error) end
    if problem then imgui.text("Problem: " .. problem) end
    if #cutscenes == 0 then imgui.text("No cutscenes in reframework\\data\\" .. DIR .. ".") end
    for i, c in ipairs(cutscenes) do
        local name = c.data.name or c.file
        local this = playing and playing.data == c.data
        if imgui.button((this and "Stop##" or "Play##") .. i) then request_toggle(c.data) end
        imgui.same_line()
        local s = trigger_state[c.file]
        imgui.text(name .. (c.data.start and c.data.start.key and ("  (" .. c.data.start.key .. ")") or "") ..
            (c.data.trigger and ("  starts by itself" .. (s and s.fired and " (did this session)" or "")) or ""))
    end
    local names = {}
    for name in pairs(new_movies) do table.insert(names, name) end
    table.sort(names)
    for i, name in ipairs(names) do  -- each alone, as a cutscene of just that movie
        local this = playing and playing.data.movie_of == name
        if imgui.button((this and "Stop##movie" or "Play##movie") .. i) then
            request_toggle(this and playing.data or { name = name, movie_of = name, length = 0.01, movies = { { t = 0, id = name } } })
        end
        imgui.same_line()
        imgui.text("New movie " .. name .. (registered[name] and (" (id " .. registered[name] .. ")") or ""))
    end
    local sound_names = {}
    for name in pairs(new_sounds) do table.insert(sound_names, name) end
    table.sort(sound_names)
    for i, name in ipairs(sound_names) do
        if imgui.button("Play##sound" .. i) then try("sound " .. name, sound_play, new_sounds[name].sound) end
        imgui.same_line()
        imgui.text("New sound " .. name)
    end
    local ok, now_playing = pcall(current_motion)
    imgui.text("Leon's animation now: " .. (ok and now_playing or "unknown"))
    -- Where he is, for triggers.
    local wok, w = pcall(game.where)
    local bok, busy = pcall(game.busy)
    imgui.text(wok and string.format("Now: chapter %s, location %s, area %s, stage %s%s", w.chapter, w.location, w.area,
        w.stage, bok and busy and " (busy: triggers wait)" or "") or ("Now: unknown (" .. tostring(w) .. ")"))
    if imgui.button("Make a trigger here") then
        local mok, err = pcall(make_trigger)
        problem = mok and nil or ("making the trigger: " .. tostring(err))
    end
    imgui.same_line()
    imgui.text("Leon's spot and where he is, into " .. DIR .. "\\trigger.json (remod: Use trigger)")
    if imgui.button("Write down Leon's spot") then  -- an actor's position and rotation, to paste into its cutscene
        local sok, err = pcall(function()
            local x = game.player():call("get_Transform")
            local p, q = x:call("get_Position"), x:call("get_Rotation")
            json.dump_file(DIR .. "/spot.json", { position = { p.x, p.y, p.z }, rotation = { q.x, q.y, q.z, q.w } })
        end)
        problem = sok and nil or ("writing down the spot: " .. tostring(err))
    end
    imgui.same_line()
    imgui.text("into " .. DIR .. "\\spot.json: an actor's position and rotation")
    -- More conditions for the trigger: each button adds one to trigger.json (made if there's none yet).
    if now() - (trigger_shown.at or -10) > 1 then  -- what trigger.json holds, read once a second
        trigger_shown.at, trigger_shown.t = now(), json.load_file(DIR .. "/trigger.json")
    end
    local tf, parts = trigger_shown.t, {}
    if type(tf) == "table" then
        if tf.near then table.insert(parts, "near a spot") end
        if type(tf.talk) == "table" then table.insert(parts, "talking to " .. tostring(tf.talk.npc)) end
        if type(tf.flags) == "table" then table.insert(parts, table.concat(tf.flags, ", ")) end
        if type(tf.after) == "table" then table.insert(parts, "after " .. tostring(tf.after.movie or tf.after.event)) end
        for _, key in ipairs({ "chapter", "location", "area", "stage" }) do
            if tf[key] then table.insert(parts, key .. " " .. tostring(tf[key])) end
        end
    end
    imgui.text("trigger.json: " .. (#parts > 0 and table.concat(parts, "; ") or "nothing yet"))
    imgui.same_line()
    if imgui.button("Start a new trigger") then
        json.dump_file(DIR .. "/trigger.json", {})
        trigger_shown.at = nil
    end
    if game_now then imgui.text("The game's " .. game_now.kind .. " playing now: " .. game_now.name) end
    for _, kind in ipairs({ "movie", "event" }) do
        local e = game_ended[kind]
        if e then
            imgui.text(string.format("The game's last %s: %s, ended %.0f s ago", kind == "movie" and "movie" or "cutscene",
                e.name, now() - e.at))
            imgui.same_line()
            if imgui.button("Start after it##" .. kind) then trigger_add("after", { [kind] = e.name }) end
        end
    end
    if imgui.tree_node("Characters near Leon (talk triggers)") then
        local cok, chars = pcall(game.characters)
        local pos = select(2, pcall(player_position))
        local near = {}
        for _, ch in ipairs(cok and type(pos) == "userdata" and chars or {}) do
            local dx, dy, dz = pos.x - ch.position.x, pos.y - ch.position.y, pos.z - ch.position.z
            local d = math.sqrt(dx * dx + dy * dy + dz * dz)
            if d <= 15 then table.insert(near, { kind = ch.kind, d = d }) end
        end
        table.sort(near, function(a, b) return a.d < b.d end)
        if #near == 0 then imgui.text(cok and "Nobody within 15 m." or ("Couldn't list them: " .. tostring(chars))) end
        for i, ch in ipairs(near) do
            if imgui.button("Talk trigger##talk" .. i) then
                trigger_add("talk", { npc = ch.kind, key = "G", prompt = "Talk", radius = 2.5 })
            end
            imgui.same_line()
            imgui.text(string.format("%s, %.1f m", ch.kind, ch.d))
        end
        imgui.text("Talk trigger: near that character, \"[G] Talk\" shows; G starts the cutscene (key, prompt and\n" ..
                   "radius can be changed in the cutscene file).")
        imgui.tree_pop()
    end
    if imgui.tree_node("Story flags") then
        local diff = {}  -- flags the game's code names: a check that reading flags works at all
        for _, d in ipairs({ "Assisted", "Standard", "Hard", "Professional" }) do
            local dok, on = pcall(game.named_flag_on, "Difficulty" .. d)
            table.insert(diff, d .. " " .. (dok and (on == true and "ON" or on == false and "off" or "?") or ("error: " .. tostring(on))))
        end
        imgui.text("Difficulty flags: " .. table.concat(diff, ", ") .. "  (one should be ON: this save's difficulty)")
        local _, on = imgui.checkbox("Watch: list flags as they change while you play", flag_watch.on)
        flag_watch.on = on
        for i, ch in ipairs(flag_watch.changes) do
            if imgui.button("Add##fw" .. i) then trigger_add("flags", (ch.on and "" or "!") .. ch.name) end
            imgui.same_line()
            imgui.text(string.format("%s %s  (%s, %.0f s ago)", ch.on and "on: " or "off:", ch.name, ch.group, now() - ch.at))
        end
        local _, find = imgui.input_text("Find a flag", flag_watch.find or "")
        flag_watch.find = find or ""
        read_flags()
        if flag_watch.find ~= "" and flags_list then
            local needle, shown = flag_watch.find:lower(), 0
            for _, f in ipairs(flags_list) do
                if shown < 20 and f.name:lower():find(needle, 1, true) then
                    shown = shown + 1
                    local fok, state = pcall(game.flag_on, f)
                    if imgui.button("Must be on##ff" .. shown) then trigger_add("flags", f.name) end
                    imgui.same_line()
                    if imgui.button("Must be off##ff" .. shown) then trigger_add("flags", "!" .. f.name) end
                    imgui.same_line()
                    imgui.text(string.format("%s  (%s, now %s)", f.name, f.group, fok and (state and "on" or "off") or "?"))
                end
            end
        end
        imgui.text((flags_list and #flags_list or 0) .. " story flags. Each button adds to " .. DIR .. "\\trigger.json.")
        imgui.tree_pop()
    end
    local puppet_names = {}
    for name in pairs(puppet_defs) do
        local h = preloaded[name]
        table.insert(puppet_names, name .. (h and h.error and " (files: " .. h.error .. ")" or ""))
    end
    table.sort(puppet_names)
    imgui.text("Puppets (actors), in " .. PUPPETS_DIR .. ": " .. (#puppet_names > 0 and table.concat(puppet_names, ", ") or "none"))
    if imgui.tree_node("Animations") then
        local ok, err = pcall(function()
            -- Who: Leon, or a puppet out in front of him.
            local whos, labels = { "player" }, { "Leon" }
            local names = {}
            for name in pairs(puppet_defs) do table.insert(names, name) end
            table.sort(names)
            for _, name in ipairs(names) do
                table.insert(whos, name)
                table.insert(labels, name)
            end
            local at = 1
            for i, w in ipairs(whos) do if w == preview.who then at = i end end
            local changed, picked = imgui.combo("Who", at, labels)
            if changed and whos[picked] ~= preview.who then preview_choose(whos[picked]) end
            if preview.problem then imgui.text("Problem: " .. preview.problem) end
            if preview.building or (preview.root and now() - (preview.since or 0) < 0.5) then
                imgui.text("Bringing " .. (preview.who == "player" and "Leon" or preview.who) .. " out...")  -- its banks are read once the game has updated it
                return
            end
            if not preview_motion() then
                imgui.text(preview.who == "player" and "No player yet (load a save)." or "Pick it again to bring it out.")
                return
            end
            -- An animation file of our choosing, as a bank of our own number.
            preview.file = preview.file or {}
            local file = preview.file[preview.who] or CUTSCENE_FILES[preview.who] or ""
            local _, typed = imgui.input_text("Animation file", file)
            preview.file[preview.who] = typed or file
            local _, bank_text = imgui.input_text("As bank", preview.file_bank or "9000")
            preview.file_bank = bank_text or "9000"
            if imgui.button("Add this animation file") then
                local bank = math.tointeger(tonumber(preview.file_bank))
                local path, motion, who = preview.file[preview.who], preview_motion(), preview.who
                if not bank or bank < 0 then
                    table.insert(file_notes, "As bank must be a whole number")
                else
                    table.insert(file_notes, path .. ": requested, added in a second")
                    on_update("requesting " .. path, function()
                        local hok, h = pcall(holder, "via.motion.MotionListResource", path)
                        if not hok or not h then
                            table.insert(file_notes, path .. ": " .. tostring(h or "no file named"))
                        else
                            table.insert(loading_files, { motion = motion, holder = h, bank = bank, path = path, t0 = now(),
                                                          who = who })
                        end
                    end)
                end
            end
            imgui.same_line()
            imgui.text("(no natives/STM, no suffix)")
            for i = math.max(1, #file_notes - 4), #file_notes do imgui.text(file_notes[i]) end
            if not preview.banks or #preview.banks == 0 then preview.banks = preview_banks() end  -- a new puppet's load
            local bank_labels, bank_at = {}, 1
            for i, b in ipairs(preview.banks) do
                table.insert(bank_labels, b.id .. "  " .. b.name)
                if b.id == preview.bank then bank_at = i end
            end
            if #preview.banks == 0 then
                imgui.text("It has no motion banks yet.")
                return
            end
            local bchanged, bpicked = imgui.combo("Bank", bank_at, bank_labels)
            if bchanged or not preview.bank then
                preview.bank, preview.motions = preview.banks[bpicked or 1].id, nil
            end
            preview.motions = preview.motions or preview_motions(preview.bank)
            local _, filter = imgui.input_text("Find", preview.filter)
            preview.filter = filter or ""
            local shown, needle = 0, preview.filter:lower()
            for _, mo in ipairs(preview.motions) do
                if needle == "" or mo.name:lower():find(needle, 1, true) or tostring(mo.id) == needle then
                    shown = shown + 1
                    if shown <= 40 and imgui.button(string.format("%d  %s  (%.0f frames)##m%d", mo.id, mo.name, mo.frames, mo.id)) then
                        on_update("previewing " .. mo.name, function() preview_play(mo) end)
                    end
                end
            end
            if shown > 40 then imgui.text(string.format("... and %d more: type in Find to narrow it", shown - 40)) end
            imgui.text(string.format("%d animations in bank %d", #preview.motions, preview.bank))
            local p = preview.picked
            if p then
                local layer = preview_motion():call("getLayer", 0)
                imgui.text(string.format("Playing: bank %d, motion %d (%s)", p.bank, p.motion, p.name))
                local frame = layer:call("get_Frame")
                local fchanged, f = imgui.slider_float("Frame", frame, 0, math.max(layer:call("get_EndFrame"), 1))
                if fchanged then
                    on_update("frame", function()  -- quiet: not logged every frame of a drag
                        layer:call("set_Speed", 0.0)
                        layer:call("set_Frame", f)
                    end, true)
                end
                local paused = layer:call("get_Speed") == 0
                if imgui.button(paused and "Play" or "Pause") then
                    on_update(paused and "play" or "pause", function() layer:call("set_Speed", paused and 1.0 or 0.0) end)
                end
                imgui.same_line()
                if imgui.button("Use in a cutscene") then
                    json.dump_file(DIR .. "/animation.json", { actor = preview.who, bank = p.bank, motion = p.motion,
                                                              name = p.name, frame = math.floor(frame),
                                                              file = (preview.files or {})[preview.who .. "#" .. p.bank] })
                end
                imgui.same_line()
                imgui.text("into " .. DIR .. "\\animation.json (remod's cutscene editor: Add picked animation)")
            end
            if imgui.button(preview.who == "player" and "Stop previewing" or "Put it away") then
                on_update("putting the preview away", preview_put_away)
            end
        end)
        if not ok then imgui.text("Problem: " .. tostring(err)) end
        imgui.tree_pop()
    end
    imgui.text("F10: add the camera as a key to " .. DIR .. "\\recording.json" ..
        (recording and (" (" .. #recording.keys .. " keys)") or ""))
    if recording and imgui.button("Start a new recording") then recording = nil end
    imgui.tree_pop()
end)

re.on_script_reset(function()
    stop()
    preview_put_away()
end)
load_all()
load_puppets()

-- For remod's tests (REFramework ignores what a script returns).
return { camera_at = camera_at }
