-- remod cutscene runtime (CLAUDE.md §10 M3 route 3; the seed of M5's one generic runtime). Plays cutscene files,
-- which are data, not code: a mod ships this script once and its cutscenes as
-- reframework\data\remod_cutscenes\<name>.json (the format: schemas/cutscene.v0.example.json in remod).
--
-- A cutscene: a length; an optional start key; camera keys (time, position, rotation x y z w, FOV, ease) the camera
-- follows; subtitles; a letterbox; fades; motions (one of the game's animations, by motion bank and id, on the player).
-- REFramework's menu > Script Generated UI > remod cutscenes: play or stop each, reload, and record camera keys: F10
-- adds the camera as it is now as a key to remod_cutscenes\recording.json (the time between presses becomes the time
-- between keys). Frame shots with REFramework's free camera, then press F10.

local DIR = "remod_cutscenes"
local RECORD_KEY = 0x79  -- F10

-- What differs between games. Values marked TBD-spike come from remod's spikes/cutscene_probe.lua run in that game.
local GAMES = {
    re4 = {
        camera_hook = "BeginRendering",  -- TBD-spike: the engine step after which nothing moves the camera again
        -- The player's GameObject, as EMV Engine (MIT) finds it in RE4R.
        player = function()
            local mgr = sdk.get_managed_singleton("chainsaw.CharacterManager")
            local ctx = mgr and mgr:call("getPlayerContextRef")
            return ctx and ctx:call("get_BodyGameObject")
        end,
    },
}
local game = GAMES[reframework:get_game_name()]

-- ---- Time: os.clock when REFramework's Lua has it, else frames at 60 per second ----
local frames = 0
local function now() return (os and os.clock) and os.clock() or frames / 60 end

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
local playing = nil  -- { data, started, fired = {}, fov_before }

local function stop()
    if playing and playing.fov_before then
        local cam = camera_parts()
        if cam then pcall(cam.call, cam, "set_FOV", playing.fov_before) end
    end
    playing = nil
end

local function play(data)
    local cam = camera_parts()
    local fov_before = nil
    if cam then
        local ok, fov = pcall(cam.call, cam, "get_FOV")
        fov_before = ok and fov or nil
    end
    playing = { data = data, started = now(), fired = {}, fov_before = fov_before }
end

local function elapsed() return playing and (now() - playing.started) or 0 end

-- An animation on an actor ("player" only, for now).
local function start_motion(m)
    if m.actor and m.actor ~= "player" then return end
    local player = game.player()
    local motion = player and player:call("getComponent(System.Type)", sdk.typeof("via.motion.Motion"))
    local layer = motion and motion:call("getLayer", 0)
    if not layer then return end
    pcall(layer.call, layer,
        "changeMotion(System.UInt32, System.UInt32, System.Single, System.Single, via.motion.InterpolationMode, via.motion.InterpolationCurve)",
        m.bank, m.motion, m.frame or 0.0, m.blend or 10.0, 1, 0)
end

if game then
    re.on_pre_application_entry(game.camera_hook, function()
        if not playing or not playing.data.camera then return end
        local pos, rot, fov = camera_at(playing.data.camera, elapsed())
        local cam, xform = camera_parts()
        if not pos or not xform then return end
        xform:call("set_Position", pos)
        xform:call("set_Rotation", rot)
        if fov then cam:call("set_FOV", fov) end
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
    local t = elapsed()
    if t >= (playing.data.length or 0) then
        stop()
        return
    end
    for i, m in ipairs(playing.data.motions or {}) do
        if not playing.fired[i] and t >= m.t then
            playing.fired[i] = true
            start_motion(m)
        end
    end
    draw_overlays(playing.data, t)
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
    imgui.text("F10: add the camera as a key to " .. DIR .. "\\recording.json" ..
        (recording and (" (" .. #recording.keys .. " keys)") or ""))
    if recording and imgui.button("Start a new recording") then recording = nil end
    imgui.tree_pop()
end)

re.on_script_reset(stop)
load_all()

-- For remod's tests (REFramework ignores what a script returns).
return { camera_at = camera_at }
