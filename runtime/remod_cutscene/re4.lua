-- remod cutscene runtime: Resident Evil 4 (2023)'s part (CLAUDE.md §10 "Games"). The shared script
-- (reframework\autorun\remod_cutscene.lua) loads it for REFramework's game "re4" and calls these; everything here
-- names RE4's own code (chainsaw.*, share.*). Found by remod's spikes run in game (spikes/*.md), all confirmed there.
-- R: the shared script's helpers (now, enum, instance, keep, enum_name, enum_values, new_movies, topics_ready, fire,
-- playing): the later ones are looked up when called.
return function(R)
local now, enum, instance, keep, enum_name, enum_values = R.now, R.enum, R.instance, R.keep, R.enum_name, R.enum_values

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
local registered = {}  -- name -> id (this game session)

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

-- ---- The game's functions, found by remod's spikes (cutscene_probe.lua ...) run in it ----
local merchants, merchants_at = {}, nil  -- the merchant gimmicks, found every 5 s (game.characters)
local game = {
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
        elseif R.new_movies()[name] then
            id = register_movie(name)
        else
            error("no movie " .. name .. ": not the game's, and no New movie block made one (in " .. R.MOVIES_DIR .. ")")
        end
        local m = { id = id, phase = "loading", since = now(), sound = not f and R.new_movies()[name].sound }
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
        -- Enemies too: the merchant wasn't found among the NPCs and partners (trigger test 3, 2026-10-09).
        for _, getter in ipairs({ "get_DollNpcContextList", "get_PartnerContextList", "get_EnemyContextList" }) do
            local list = mgr and mgr:call(getter)
            for i = 0, (list and list:call("get_Count") or 0) - 1 do
                pcall(function()
                    local ctx = list:call("get_Item", i)
                    local pos = ctx and ctx:call("get_Position")
                    if pos then
                        table.insert(out, { kind = enum_name("chainsaw.CharacterKindID", ctx:call("get_KindID")),
                                            position = pos, list = getter:match("get_(%a-)Context"),
                                            body = ctx:call("get_BodyGameObject") })
                    end
                end)
            end
        end
        -- The merchant isn't a character the manager lists (trigger test 3 run 2: nobody but Ashley near him, in
        -- the NPC, partner and enemy lists); he's a gimmick, chainsaw.GmWeaponMerchant [dump], kind "merchant"
        -- here. Found in the scene every 5 s (ponytail: a scene search, cached; per frame would cost).
        if now() - (merchants_at or -10) > 5 then
            merchants_at, merchants = now(), {}
            local scene = sdk.find_type_definition("via.SceneManager"):get_method("get_CurrentScene"):call(nil)
            local found = scene and scene:call("findComponents(System.Type)", sdk.typeof("chainsaw.GmWeaponMerchant"))
            for _, m in ipairs(found and found:get_elements() or {}) do table.insert(merchants, m) end
        end
        for _, m in ipairs(merchants) do
            local ok = pcall(function()
                local body = m:get_field("NpcBodyObj") or m:call("get_GameObject")
                table.insert(out, { kind = "merchant", position = body:call("get_Transform"):call("get_Position"),
                                    list = "Gimmick", body = body })
            end)
            -- A merchant whose area unloaded throws (and REFramework logs it every check, run 8): search again.
            if not ok then merchants_at = nil end
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
    -- The game's difficulty setting (chainsaw.CampaignManager get_CurrentDifficulty: Rank_10..Rank_40) [dump].
    difficulty = function()
        return enum_name("chainsaw.CampaignManager.Difficulty", instance("chainsaw.CampaignManager"):call("get_CurrentDifficulty"))
    end,
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
}

-- New sounds and new movies, for the shared script.
game.sound_play, game.sound_stop = sound_play, sound_stop
function game.movie_id(name) return registered[name] end  -- a new movie's id this session, if it's registered

-- The save or load the game is doing now: "Save" / "Load" (share.SaveDataManager _CurrentRequest's Command) and its
-- slot (trigger test 3 run 8: every save and load seen, a death retry a Load of the slot played; slot -1 is system data).
function game.save_request()
    local mgr = sdk.get_managed_singleton("share.SaveDataManager")
    local req = mgr and mgr:get_field("_CurrentRequest")
    if not req then return nil end
    return enum_name("share.SaveDataManager.Request.CommandType", req:get_field("Command")), tonumber(req:get_field("SlotId"))
end

-- One of the game's GUI commands pressed this frame ("MoveUp", "MoveDown", "Decide", "Cancel": chainsaw.GuiCommandType).
function game.gui_command(name)
    local gim = instance("chainsaw.GuiInputManager")
    return gim and gim:call("isTrigger(chainsaw.GuiCommandType, System.Boolean)", enum("chainsaw.GuiCommandType", name), false)
end

-- Where the game's own named flags point (ScenarioFlagDefine {Group, Index}): the Difficulty ones read off in trigger
-- test 3, so is Group the _Group array's position? `order`: the story flag groups' names (logged once).
function game.flag_diagnostics(order)
    do
        local td, out = sdk.find_type_definition("chainsaw.ScenarioFlagDefine"), {}
        for _, n in ipairs({ "DifficultyStandard", "DifficultyProfessional", "Ch1f0z0LuisArrivedDemoAfter" }) do
            local fl = td:get_field(n):get_data(nil)
            local g, i = fl:get_field("Group"), fl:get_field("Index")
            table.insert(out, string.format("%s = group %s index %s (the list's group %s there: %s)", n, tostring(g),
                tostring(i), tostring(g), tostring(order[(tonumber(g) or -1) + 1])))
        end
        log.info("[remod_cutscene] named flags: " .. table.concat(out, "; "))
    end
end

-- ---- Talk triggers with the game's own interact icon (spikes/interact_icon_probe.md, runs 2-3, 2026-10-10) ----
local function key_trigger_to_copy()  -- a loaded key trigger, preferring ones known to show the Interact icon
    local s = sdk.find_type_definition("via.SceneManager"):get_method("get_CurrentScene"):call(nil)
    local holders = s and s:call("findComponents(System.Type)", sdk.typeof("chainsaw.InteractHolder"))
    local best, rank = nil, 99
    local prefer = { Use = 1, Button = 2, Read = 3 }
    for _, h in ipairs(holders and holders:get_elements() or {}) do
        local acts = h:get_field("_TrgAct")
        for _, t in ipairs(acts and acts:get_elements() or {}) do
            if t:get_type_definition():get_full_name() == "chainsaw.InteractTriggerKey" then
                local r = prefer[tostring(t:get_field("UniqueName"))] or 9
                if r < rank then best, rank = t, r end
            end
        end
        if rank == 1 then break end
    end
    return best
end
-- The game's Interact icon over `body`, at `marker` (an object of the shared script's, kept above the character's
-- head): a copy of a loaded key trigger (MemberwiseClone) pointed there, its work made the game's way (generateWork,
-- applyTarget on Leon). Gives the work and what it was copied from.
function game.make_icon(body, marker)
    local source = key_trigger_to_copy()
    if not source then error("no key trigger loaded to copy (no typewriter, door, file... in this area)") end
    local copy = keep(source:call("MemberwiseClone"))
    copy:set_field("_ObjIconPos", marker:call("get_Transform"))
    copy:call("set_Owner", body)
    local work = keep(copy:call("generateWork", enum("chainsaw.InteractTrigger.TargetType", "Pl00"),
                                enum("chainsaw.InteractManager.WorkIndex", "Pl1")))
    -- A null in the game's code crashes it or throws each frame (probe run 1): only use a fully linked work.
    if not work or work:get_type_definition():get_full_name() ~= "chainsaw.InteractTriggerKey.WorkKey"
        or not work:call("get_TriggerKey") or not work:get_field("_FloatIconOpenParam") then
        error("the game's work for it isn't linked")
    end
    local ud = keep(sdk.create_instance("chainsaw.collision.GimmickSensorUserData", true))
    ud:call("set_Kind", enum("chainsaw.collision.GimmickSensorUserData.KindType", "Interact"))
    work:call("applyTarget", game.player(), ud)
    return work, string.format("'%s' of %s", tostring(source:get_field("UniqueName")),
        tostring(source:call("get_Owner") and source:call("get_Owner"):call("get_Name")))
end
-- Each frame in the game's update: asks for the icon; true on the frame Interact is pressed (checkInput).
function game.icon_frame(work)
    work:call("requestDrawKeyIcon", enum("chainsaw.InteractTriggerKey.OverrideDisable", "None"))
    return work:call("checkInput") == true
end

-- ---- Talk topics in the merchant's menu (trigger.topic; spikes/merchant_menu_test.md runs 1-3, topic_test.md) ----
-- His first menu's layout comes with remod's spare entries (core gui.*: si_menu_4..7 after the game's four). In the
-- game's update the topics whose conditions hold take the spares in file order: labelled, given the sizes the game
-- gives its own four in code (copied from the 4th: the layout's defaults are 1000 wide), without the icon (the game
-- sets its four's only); the other spares are hidden. Confirming one (the game's Decide, or a click) closes the shop
-- the game's own way, and the cutscene plays once it has closed.
local shop = { slots = {}, open = false }
local function shop_flow()
    local m = instance("chainsaw.InGameShopManager")
    return m and m:call("get_FlowController")
end
local function shop_state(f)
    return enum_name("chainsaw.gui.shop.InGameShopGuiState", f and f:call("get_CurrStateType"))
end
-- The first menu's behaviour: searched for while the shop is open until found, then kept until it closes. Each visit
-- makes a new one (topic test, 2026-10-13: a menu kept from the first visit gave no entries on the second).
local function shop_menu()
    local state = shop_state(shop_flow())
    if state == "Invalid" or state == "nil" then shop.menu, shop.logged = nil, nil return nil end  -- logged each visit
    if not shop.menu then  -- ponytail: a scene search each frame until found (a frame or two); throttle if it lags
        local scene = sdk.find_type_definition("via.SceneManager"):get_method("get_CurrentScene"):call(nil)
        local arr = scene and scene:call("findComponents(System.Type)", sdk.typeof("chainsaw.InGameShopSelectGuiBehavior"))
        shop.menu = arr and arr:get_elements()[1]
    end
    return shop.menu
end
local function gui_parts(item)  -- every element under a menu entry by its path of names ("cursor_center/mat_rect")
    local out = {}
    local function walk(o, path)
        out[path] = o
        local kids = o:call("getChildren(System.Type)", sdk.typeof("via.gui.PlayObject"))
        for _, k in ipairs(kids and kids:get_elements() or {}) do
            walk(k, (path == "" and "" or path .. "/") .. tostring(k:call("get_Name")))
        end
    end
    walk(item, "")
    return out
end
local function vec_text(v)
    local ok, s = pcall(function() return string.format("%.1f %.1f %.1f", v.x, v.y, v.z) end)
    if ok then return s end
    ok, s = pcall(function() return string.format("%.1f %.1f", v.w, v.h) end)
    return ok and s or tostring(v)
end
local function match_parts(model, ours)  -- our entry's parts sized as the game sizes its own (label and root excepted)
    local theirs, mine = gui_parts(model), gui_parts(ours)
    for path, src in pairs(theirs) do
        local dst = mine[path]
        if dst and path ~= "" and not path:match("m_list$") then
            for _, prop in ipairs({ "Size", "Position", "Scale" }) do
                local ok, v = pcall(src.call, src, "get_" .. prop)
                if ok and v ~= nil then
                    local _, w = pcall(dst.call, dst, "get_" .. prop)
                    if vec_text(v) ~= vec_text(w) then pcall(dst.call, dst, "set_" .. prop, v) end
                end
            end
        end
    end
    return mine
end
local function shop_update()  -- each frame, in the game's update
    local ok, err = pcall(function()
        local menu = shop_menu()
        local list = menu and menu:get_field("_SelectList")
        local items = list and list:call("get_Items")
        items = items and items:get_elements() or {}
        shop.open = #items > 4
        if #items == 4 and not shop.said_no_spares and #R.topics_ready("merchant") > 0 then
            shop.said_no_spares = true
            log.info("[remod_cutscene] the merchant's menu has no spare entries for talk topics: install the mod "
                .. "through Fluffy (its menu layout is a game file)")
        end
        if not shop.open then shop.slots, shop.confirm = {}, nil return end
        local ready, labels = R.topics_ready("merchant"), {}
        shop.slots = {}
        for i = 5, #items do
            local c, item = ready[i - 4], items[i]
            if c then
                local parts = match_parts(items[4], item)
                local text, icon = parts["m_list"], parts["icon"]
                if text and text:call("get_Message") ~= c.data.trigger.topic.label then
                    text:call("set_Message", c.data.trigger.topic.label)
                end
                if icon and icon:call("get_Visible") then icon:call("set_Visible", false) end
                if not item:call("get_Visible") then item:call("set_Visible", true) end
                shop.slots[i - 1] = c  -- by the list's selected index (0 = the first entry)
                table.insert(labels, c.data.trigger.topic.label)
            elseif item:call("get_Visible") then
                item:call("set_Visible", false)
            end
        end
        local line = (#labels > 0 and table.concat(labels, ", ") or "none")
            .. (#ready > #items - 4 and string.format(" (%d more than its %d spare entries)", #ready - (#items - 4), #items - 4) or "")
        if line ~= shop.logged then
            shop.logged = line
            log.info("[remod_cutscene] the merchant's menu: talk topics " .. line)
        end
        local f = shop_flow()
        if shop.confirm and shop_state(f) == "Select_Default" then
            local c = shop.slots[list:call("get_SelectedIndex")]
            if c then
                log.info("[remod_cutscene] " .. (c.data.name or c.file) .. ": picked in the merchant's menu ("
                    .. shop.confirm .. "); closing the shop")
                f:call("close")
                shop.pending = { c = c, at = now() }
            end
        end
        shop.confirm = nil
    end)
    if not ok then
        shop.menu = nil  -- searched for again
        if shop.err ~= tostring(err) then
            shop.err = tostring(err)
            log.info("[remod_cutscene] the merchant's menu: " .. shop.err)
        end
    end
end
local function shop_frame(click)  -- each frame: a confirm on the menu, and a picked topic once the shop has closed
    if shop.open then
        local gim = instance("chainsaw.GuiInputManager")
        local decide = gim and gim:call("isTrigger(chainsaw.GuiCommandType, System.Boolean)",
            enum("chainsaw.GuiCommandType", "Decide"), false)
        shop.confirm = (decide and "the game's confirm") or (click and "a click") or shop.confirm
    end
    local p = shop.pending
    if p and not R.playing() then
        local state = shop_state(shop_flow())
        local busy_ok, busy = pcall(game.busy)
        if (state == "Invalid" or state == "nil") and busy_ok and not busy then
            shop.pending = nil
            R.fire(p.c)
        elseif now() - p.at > 10 then
            shop.pending = nil
            log.info("[remod_cutscene] " .. (p.c.data.name or p.c.file) .. ": the shop didn't close (state " .. state
                .. ", busy " .. tostring(busy) .. "); not played")
        end
    end
end

-- Each frame in the game's update, and each frame (on_frame, with whether the mouse was clicked): the merchant's menu.
game.update, game.frame = shop_update, shop_frame

return game
end
