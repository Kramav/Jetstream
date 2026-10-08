-- remod cutscene runtime (CLAUDE.md §10 M3 route 3; the seed of M5's one generic runtime). Plays cutscene files,
-- which are data, not code: a mod ships this script once and its cutscenes as
-- reframework\data\remod_cutscenes\<name>.json (the format: schemas/cutscene.v0.example.json in remod).
--
-- A cutscene: a length; an optional start key; camera keys (time, position, rotation x y z w, FOV, ease) the camera
-- follows; subtitles; a letterbox; fades; motions (one of the game's animations, by motion bank and id, on the player);
-- movies (one of the game's, by id, full screen with the world paused; the cutscene's time waits while it plays).
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
    return o:add_ref()
end

-- An array holding `values`: created, else a copy of `like` (as long); each set read back.
local function new_array(elem_type, values, like)
    local ok, arr = pcall(sdk.create_managed_array, elem_type, #values)
    if not (ok and arr) and like and like:get_size() == #values then arr = like:call("Clone") end
    if not arr then error("can't make an array of " .. elem_type) end
    arr = arr:add_ref()
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
    p = (ok and p) and p:add_ref() or like:call("duplicate"):add_ref()
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
        b = { resource = res, holder = res:create_holder("via.simplewwise.BankResourceHolder"):add_ref(), kept = {} }
        banks[s.bank] = b
    end
    local c = sound_container()
    if not b.kept[c:get_address()] then  -- a save loaded since: the player's container is a new one
        c:call("get_BankResourceList"):call("Add", b.holder)
        b.kept[c:get_address()] = true
    end
    local info = c:get_field("_TriggerInfoList"):call("get_Item", 0):call("MemberwiseClone"):add_ref()
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
        local info = mm:call("getLoadInfo", mva000):call("MemberwiseClone"):add_ref()
        info:call("set_MovieID", id)
        for _, f in ipairs({ "set_IsChapterStart", "set_IsChapterEnd", "set_IsEnding", "set_IsGameOver",
                             "set_HasNextMovie" }) do info:call(f, false) end
        mm:call("get_MovieLoadTable"):call("get_LoadInfoList"):call("Add", info)
    end
    registered[name] = id
    log.info("[remod_cutscene] new movie " .. name .. " registered as id " .. id)
    return id
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

-- ---- Playing ----
local playing = nil  -- { data, started, fired = {}, fov_before, hud_before }

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
    playing = nil  -- the player's controls come back as hold_player is no longer sent
end

-- While a cutscene plays the player can't be controlled (hold_player, each frame below) and the HUD is hidden; both
-- come back when it ends or stops.
local function play(data)
    stop()
    local cam = camera_parts()
    local fov_before = nil
    if cam then
        local ok, fov = pcall(cam.call, cam, "get_FOV")
        fov_before = ok and fov or nil
    end
    playing = { data = data, started = now(), fired = {}, fov_before = fov_before,
                hud_before = try("hiding the HUD", game.hud, game.hud_off) }
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
    if m.actor and m.actor ~= "player" then return end
    local layer = player_layer()
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
        if not playing then return end
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
            if playing and playing.data == c.data then stop() else play(c.data) end
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
    if imgui.button("Reload cutscenes") then load_all() end
    if load_error then imgui.text("Problem: " .. load_error) end
    if problem then imgui.text("Problem: " .. problem) end
    if #cutscenes == 0 then imgui.text("No cutscenes in reframework\\data\\" .. DIR .. ".") end
    for i, c in ipairs(cutscenes) do
        local name = c.data.name or c.file
        local this = playing and playing.data == c.data
        if imgui.button((this and "Stop##" or "Play##") .. i) then
            if this then stop() else play(c.data) end
        end
        imgui.same_line()
        imgui.text(name .. (c.data.start and c.data.start.key and ("  (" .. c.data.start.key .. ")") or ""))
    end
    local names = {}
    for name in pairs(new_movies) do table.insert(names, name) end
    table.sort(names)
    for i, name in ipairs(names) do  -- each alone, as a cutscene of just that movie
        local this = playing and playing.data.movie_of == name
        if imgui.button((this and "Stop##movie" or "Play##movie") .. i) then
            if this then stop() else play({ name = name, movie_of = name, length = 0.01, movies = { { t = 0, id = name } } }) end
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
    imgui.text("F10: add the camera as a key to " .. DIR .. "\\recording.json" ..
        (recording and (" (" .. #recording.keys .. " keys)") or ""))
    if recording and imgui.button("Start a new recording") then recording = nil end
    imgui.tree_pop()
end)

re.on_script_reset(stop)
load_all()

-- For remod's tests (REFramework ignores what a script returns).
return { camera_at = camera_at }
