-- remod cutscene runtime (CLAUDE.md Â§10 M3 route 3; the seed of M5's one generic runtime). Plays cutscene files,
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

-- New movies and new sounds (remod's New movie, New sound blocks): their notes, read with the cutscenes; the game's
-- part plays them (its sound_play, movie_start).
local new_movies = {}  -- name -> note (reframework\data\remod_movies\<name>.json)
local MOVIES_DIR = "remod_movies"
local new_sounds = {}  -- name -> note (remod_sounds\<name>.json)
local SOUNDS_DIR = "remod_sounds"

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

-- ---- The game's own part (CLAUDE.md Â§10 "Games") ----
-- Everything that names the game's own code is in reframework\autorun\remod_cutscene\<REFramework's name for the
-- game>.lua (re4.lua ...), shipped beside this script. It's given R, this script's helpers (filled in as they're
-- defined below, so look them up when called), and gives back the game's functions (`game`). No file for the running
-- game: the menu says so and nothing runs.
local R = { now = now, enum = enum, instance = instance, keep = keep, enum_name = enum_name, enum_values = enum_values,
            new_movies = function() return new_movies end, MOVIES_DIR = MOVIES_DIR }
local game, game_problem = nil, nil
do
    local name = reframework:get_game_name()
    local ok, make = pcall(require, "remod_cutscene." .. tostring(name))
    if ok and type(make) == "function" then
        local mok, g = pcall(make, R)
        if mok and type(g) == "table" then game = g else game_problem = "remod_cutscene\\" .. tostring(name) .. ".lua: " .. tostring(g) end
    else
        game_problem = "no remod_cutscene\\" .. tostring(name) .. ".lua (" .. tostring(make):match("^[^\n]*") .. ")"
    end
    if game_problem then log.info("[remod_cutscene] " .. game_problem) end
end

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
            if type(data) == "table" and data.schema_version and data.length then
                table.insert(cutscenes, { file = rel, data = data })
            elseif type(data) == "table" then
                -- what the game wrote down beside them (trigger.json, spot.json, flags.json ...): not cutscenes
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
    on_update("requesting the cutscenes' files", function()  -- resources are made in the game's update (Â§9)
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
    for _, h in ipairs(playing.sounds or {}) do pcall(game.sound_stop, h) end  -- the cutscene's sounds end with it
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

-- Starting and stopping, as asked by a key, the menu or a trigger: in the game's update (Â§9 of remod's CLAUDE.md:
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
-- is pressed then); not again until they've stopped holding. `once`: true (the default) once per save, by its remod
-- flag (below: a save from before it lets it play again); "session" once each time the game runs; false every
-- time (a talk trigger's default: the player asks for it). Never while a cutscene plays or the game is busy (its own movie, a pause). Checked 5 times a second.
local trigger_state = {}  -- cutscene file -> {held, since, fired, done, talk_ready}
local next_check = 0

local function player_position()
    local body = game.player()
    return body and body:call("get_Transform"):call("get_Position")
end

-- ---- remod's own story flags (2026-10-09, trigger test 3 runs 7-8) ----
-- Flags of our own that follow the player's saves as the game's do, without touching the save file: the flags on now,
-- a copy per save slot taken when the game saves there, put back when it loads one (a death retry is a load of the
-- slot played: run 8). In remod_cutscenes\flags.json {on = {name = true}, slots = {"<slot>" = {name = true}}}.
-- A cutscene started by its trigger turns on the flag named after its file ("meet_luis.json": meet_luis), and those its
-- `sets_flags` names ("chose_to_help" on, "!chose_to_help" off); triggers test them in `flags` as "remod:meet_luis" /
-- "!remod:meet_luis". Started by a key or the menu (testing), it sets nothing. The game's save requests come from
-- share.SaveDataManager._CurrentRequest {Command, SlotId} [dump], read each frame without hooks; slot -1 follows
-- every save and load (inferred: system data) and isn't a game slot.
-- ponytail: a save of a slot from before remod gets no copy, so loading it means no remod flags (right: nothing played).
local has_flags = select(2, pcall(fs.glob, DIR .. "[/\\\\]flags\\.json$"))  -- a missing file logs a JSON error
local rflags = type(has_flags) == "table" and #has_flags > 0 and json.load_file(DIR .. "/flags.json") or nil
if type(rflags) ~= "table" then rflags = {} end
rflags.on, rflags.slots = rflags.on or {}, rflags.slots or {}
local function copy_set(t) local o = {} for k in pairs(t or {}) do o[k] = true end return o end
local function rflags_list(t) local o = {} for k in pairs(t) do table.insert(o, k) end table.sort(o) return table.concat(o, ", ") end
local function rflags_write() pcall(json.dump_file, DIR .. "/flags.json", rflags) end
local function rflag_set(name)
    if rflags.on[name] then return end
    rflags.on[name] = true
    rflags_write()
    log.info("[remod_cutscene] remod flag on: " .. name)
end
local function rflag_clear(name)
    if not rflags.on[name] then return end
    rflags.on[name] = nil
    rflags_write()
    log.info("[remod_cutscene] remod flag off: " .. name)
end

local save_watch = {}
local function watch_saves()
    if not game.save_request then return end
    local cmd, slot = game.save_request()
    local key = cmd and (tostring(cmd) .. " " .. tostring(slot))
    if key == save_watch.key then return end
    save_watch.key = key  -- each request acted on once (no request between two: PreIdle / Idle)
    if not slot or slot < 0 then return end
    if cmd == "Save" then
        rflags.slots[tostring(slot)] = copy_set(rflags.on)
        rflags_write()
        log.info("[remod_cutscene] saved to slot " .. slot .. ": remod flags " .. (rflags_list(rflags.on) ~= "" and rflags_list(rflags.on) or "none"))
    elseif cmd == "Load" then
        rflags.on = copy_set(rflags.slots[tostring(slot)])
        rflags_write()
        for _, s in pairs(trigger_state) do s.done, s.icon, s.icon_failed = false, nil, nil end  -- icons copied again
        log.info("[remod_cutscene] loading slot " .. slot .. ": remod flags " .. (rflags_list(rflags.on) ~= "" and rflags_list(rflags.on) or "none"))
    end
end

local function flag_name(c) return (c.file:match("([^/\\]+)%.json$") or c.file) end

local function fired_before(c, t)
    local s = trigger_state[c.file]
    local once = t.once
    if once == nil and (type(t.talk) == "table" or type(t.topic) == "table") then once = false end  -- the player asks: every time
    if once == false then return false end
    if once == "session" then return s.fired end
    return rflags.on[flag_name(c)] == true  -- once per save: its remod flag
end

local function fire(c)
    local s = trigger_state[c.file]
    -- A talk trigger's prompt comes back once the cutscene ends (no walking away first: the player asks each time).
    s.fired, s.done, s.talk_ready = true, type(c.data.trigger.talk) ~= "table" and type(c.data.trigger.topic) ~= "table", false
    rflag_set(flag_name(c))
    for _, f in ipairs(type(c.data.sets_flags) == "table" and c.data.sets_flags or {}) do  -- its named flags
        if f:sub(1, 1) == "!" then rflag_clear(f:sub(2)) else rflag_set(f) end
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
    if #list > 0 then  -- the groups, once: what names flags can take
        local groups, order = {}, {}
        for _, f in ipairs(list) do
            if not groups[f.group] then groups[f.group] = { n = 0, first = f.name }; table.insert(order, f.group) end
            groups[f.group].n = groups[f.group].n + 1
        end
        local parts = {}
        for _, g in ipairs(order) do table.insert(parts, string.format("%s (%d, e.g. %s)", g, groups[g].n, groups[g].first)) end
        log.info("[remod_cutscene] story flags: " .. #list .. " in " .. #order .. " groups: " .. table.concat(parts, "; "))
        if game.flag_diagnostics then pcall(game.flag_diagnostics, order) end
    end
end
local function flag_is_on(name)  -- nil if there's no such flag (yet)
    local own = name:match("^remod:(.+)$")
    if own then return rflags.on[own] == true end
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

-- The story flag the game turns on as its cutscene starts or ends ("Start" / "End"): csa038 turned on
-- EventTimelineStart00_038 and EventTimelineEnd00_038 (trigger test 3 run 4). For other ids the name is inferred
-- (hundreds in the group?), so only a name the game has is given; nil otherwise.
local function event_flag(id, which)
    local n = tonumber(tostring(id):match("^%a+(%d+)$") or "")
    if not n then return nil end
    read_flags()
    for _, name in ipairs({ string.format("EventTimeline%s%02d_%03d", which, n // 100, n),
                            string.format("EventTimeline%s%02d_%03d", which, n // 100, n % 100) }) do
        if flags_by_name and flags_by_name[name] then return name end
    end
end

-- The game's own movie or cutscene: the one playing now, and the last of each kind that ended (for `after`).
local game_now, game_ended = nil, {}  -- { kind, name }; kind -> { name, at }
local function watch_game()
    local ok, kind, name = pcall(game.playing_now)
    if not ok then return end
    if game_now and game_now.name ~= name then
        game_ended[game_now.kind] = { name = game_now.name, at = now() }
        log.info("[remod_cutscene] the game's " .. game_now.kind .. " ended: " .. tostring(game_now.name))
    end
    if kind and not (game_now and game_now.name == name) then
        log.info("[remod_cutscene] the game's " .. kind .. " playing: " .. tostring(name))
    end
    game_now = kind and { kind = kind, name = name } or nil
end

-- A trigger started by talking to a character: a talk trigger's `talk`, or a topic of a character other than the
-- merchant (picked from remod's talk menu, below; its npc and radius are read the same way).
local function talk_of(t)
    if type(t.talk) == "table" then return t.talk end
    if type(t.topic) == "table" and t.topic.npc ~= "merchant" then return t.topic end
end
local function is_merchant_topic(t) return type(t) == "table" and type(t.topic) == "table" and t.topic.npc == "merchant" end
-- Topics in a menu: by their `order` (lower first, 0 if none), then by file name.
local function topic_order(a, b)
    local function o(c) return type(c.data.trigger.topic) == "table" and tonumber(c.data.trigger.topic.order) or 0 end
    if o(a) ~= o(b) then return o(a) < o(b) end
    return a.file < b.file
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
    local who = nil  -- a talk trigger's character: the nearest of its kind in reach (its icon goes on it)
    local talk = talk_of(t)
    if talk then
        if not pos then return false end
        local r, best = talk.radius or 2.5, nil
        for _, ch in ipairs(chars or {}) do
            local dx, dy, dz = pos.x - ch.position.x, pos.y - ch.position.y, pos.z - ch.position.z
            local d = dx * dx + dy * dy + dz * dz
            if ch.kind == talk.npc and d <= r * r and (not best or d < best) then best, who = d, ch end
        end
        if not who then return false end
    end
    return true, who
end

-- What triggers can see, into the log when it changes (every 2 s): characters within 8 m of Leon, the difficulty and
-- the Difficulty story flags read both ways. So a talk or flag trigger that doesn't start says why without the menu.
-- ponytail: always on; it logs only changes, a few lines per area.
local seen = { at = -10 }
local function log_seen(pos)
    if now() - seen.at < 2 then return end
    seen.at = now()
    local near = {}
    local cok, chars = pcall(game.characters)
    for _, ch in ipairs(cok and chars or {}) do
        local d = pos and math.sqrt((pos.x - ch.position.x) ^ 2 + (pos.y - ch.position.y) ^ 2 + (pos.z - ch.position.z) ^ 2)
        if d and d <= 8 then table.insert(near, string.format("%s (%s) %.1f m", tostring(ch.kind), ch.list or "?", d)) end
    end
    local kinds = {}
    for _, n in ipairs(near) do table.insert(kinds, (n:gsub(" [%d.]+ m$", ""))) end
    table.sort(kinds)
    local key = table.concat(kinds, ", ")
    if key ~= seen.near then
        seen.near = key
        log.info("[remod_cutscene] near Leon: " .. (#near > 0 and table.concat(near, ", ") or "nobody within 8 m"))
    end
    read_flags()
    local dok, diff = pcall(game.difficulty)
    local parts = { "difficulty " .. (dok and tostring(diff) or "? " .. tostring(diff)),
                    "story flags read " .. (flags_list and #flags_list or 0) }
    for _, d in ipairs({ "Assisted", "Standard", "Hard", "Professional" }) do
        local f = flags_by_name and flags_by_name["Difficulty" .. d]
        local lok, lon = true, nil
        if f then lok, lon = pcall(game.flag_on, f) end
        local nok, non = pcall(game.named_flag_on, "Difficulty" .. d)
        table.insert(parts, string.format("%s: by list %s, by define %s", d,
            f and (lok and tostring(lon) or "error") or "not listed", nok and tostring(non) or "error " .. tostring(non)))
    end
    local line = table.concat(parts, "; ")
    if line ~= seen.flags then
        seen.flags = line
        log.info("[remod_cutscene] " .. line)
    end
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
    pcall(log_seen, type(pos) == "userdata" and pos or nil)
    local chars = nil
    for _, c in ipairs(cutscenes) do
        local t = c.data.trigger
        if type(t) == "table" and not is_merchant_topic(t) then  -- his topics are picked from his menu (game.update)
            local s = trigger_state[c.file] or {}
            trigger_state[c.file] = s
            if talk_of(t) and not chars then
                local cok, list = pcall(game.characters)
                chars = cok and list or {}
            end
            local now_holds, who = holds(t, where_ok and where or nil, type(pos) == "userdata" and pos or nil, chars)
            if now_holds and not s.held then s.since = now() end
            s.held = now_holds
            if not now_holds then s.done = false end
            local ready = now_holds and not s.done and now() - s.since >= (t.delay or 0) and not fired_before(c, t)
            if talk_of(t) then
                if ready ~= (s.talk_ready or false) then
                    log.info("[remod_cutscene] " .. (c.data.name or c.file) .. ": talk prompt " .. (ready and "shown" or "gone"))
                end
                s.talk_ready = ready  -- the game's icon and Interact start it (talk_icons), else its key (each frame, below)
                s.talk_who = ready and who or nil
                if where_ok and where.stage ~= s.icon_stage then  -- a new area: copy again (and try again)
                    s.icon_stage, s.icon, s.icon_failed = where.stage, nil, nil
                end
            elseif ready then
                fire(c)
                return
            end
        end
    end
end

-- "Make a trigger here": Leon's spot (2 m around him) and the game's names for where he is, into
-- remod_cutscenes\trigger.json; remod's Use trigger puts it into a cutscene file. The menu's other buttons add to it.
local trigger_shown = {}  -- the menu's view of trigger.json: { at, t }
local story_window = {}   -- the menu's typed game cutscene id
local function make_trigger()
    local pos = player_position()
    if not pos then error("no player (load a save first)") end
    trigger_shown.at = nil  -- show it again at once
    local where = game.where()
    json.dump_file(DIR .. "/trigger.json", { near = { position = { pos.x, pos.y, pos.z }, radius = 2.0 },
                                             chapter = where.chapter, stage = where.stage })
end

-- Adds a condition to trigger.json (made if there's none yet): a story flag, a talk, an after.
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
-- each frame a slice of them is read, so a whole pass takes a moment. On by default, changes logged (the log is how
-- Claude finds flags after a play: 2026-10-09); a pass with more than 20 changes (a save loaded) logs a count instead.
local flag_watch = { on = true, at = 1, last = {}, changes = {}, pass = {} }  -- last: flag -> on; changes: { name, group, on, at }
local function watch_flags()
    read_flags()
    if not flags_list or #flags_list == 0 then return end
    for _ = 1, math.min(300, #flags_list) do
        local f = flags_list[flag_watch.at]
        flag_watch.at = flag_watch.at % #flags_list + 1
        local ok, on = pcall(game.flag_on, f)
        if ok then
            local before = flag_watch.last[f]
            if before ~= nil and before ~= on then
                table.insert(flag_watch.changes, 1, { name = f.name, group = f.group, on = on, at = now() })
                if #flag_watch.changes > 20 then table.remove(flag_watch.changes) end
                table.insert(flag_watch.pass, f.group .. "/" .. f.name .. (on and " on" or " off"))
            end
            flag_watch.last[f] = on
        end
        if flag_watch.at == 1 and #flag_watch.pass > 0 then  -- a whole pass done
            local p = flag_watch.pass
            log.info("[remod_cutscene] story flags changed: " .. (#p <= 20 and table.concat(p, ", ")
                or (#p .. " at once (a save loaded?), e.g. " .. table.concat(p, ", ", 1, 5))))
            flag_watch.pass = {}
        end
    end
end

-- A talk trigger's prompt, near the bottom of the screen; row 0, 1, ... stack upwards (two in reach drew on top of
-- each other, trigger test 3).
local function draw_prompt(talk, row)
    local size = imgui.get_display_size()
    local text = "[" .. (talk.key or "T") .. "] " .. (talk.prompt or "Talk")
    local x, y = size.x * 0.5 - #text * 4, size.y * 0.72 - row * 34
    draw.filled_rect(x - 12, y - 6, #text * 8 + 24, 28, 0xA0000000)
    draw.text(text, x, y, 0xFFFFFFFF)
end

-- ---- remod's talk menu: a character's talk topics (trigger.topic with a character's kind, 2026-10-13) ----
-- Interact on a character opens a list of what's ready on them: their topics, and talk triggers (labelled by their
-- prompt); a lone talk trigger starts at once as before. The game has no list dialog a script can open (its
-- GeneralDialogGuiBehavior is Yes / No, opened through callbacks [dump]), so remod draws it. Leon is held while it's
-- open (the operation stop, as in a cutscene). Moved by the game's GUI commands (chainsaw.GuiInputManager MoveUp /
-- MoveDown / Decide / Cancel) or keys (arrows, W / S, Enter, Space, Escape, Backspace); each pick or back logs which,
-- since whether the game's commands come through outside its own menus isn't known. Closes when the game gets busy
-- (its pause menu).
local key_down = {}
local function pressed(vk)  -- a key's press, once (call it every frame for that key)
    local is = reframework:is_key_down(vk)
    local was = key_down[vk]
    key_down[vk] = is
    return is and not was
end
local talk_menu = nil  -- { entries = {c...}, cursor, at, who, by = {source = true} }
local MENU_KEYS = { Up = 0x26, W = 0x57, Down = 0x28, S = 0x53, Enter = 0x0D, Space = 0x20, Escape = 0x1B, Backspace = 0x08 }
local function entry_label(c)
    local t = c.data.trigger
    return type(t.topic) == "table" and t.topic.label or (t.talk.prompt or "Talk")
end
local function talk_pressed(c)  -- Interact (or the stand-in key) on a character with this trigger ready
    local who = trigger_state[c.file].talk_who
    local id = who and who.body and who.body:get_address()
    local entries = {}
    for _, o in ipairs(cutscenes) do
        local s = trigger_state[o.file]
        local w = s and s.talk_ready and s.talk_who
        if w and w.body and w.body:get_address() == id then table.insert(entries, o) end
    end
    if #entries == 0 then entries = { c } end
    table.sort(entries, topic_order)
    if #entries == 1 and type(entries[1].data.trigger.talk) == "table" then fire(entries[1]) return end
    local labels = {}
    for _, e in ipairs(entries) do table.insert(labels, entry_label(e)) end
    log.info("[remod_cutscene] talk menu for " .. tostring(who and who.kind) .. ": " .. table.concat(labels, ", "))
    talk_menu = { entries = entries, cursor = 1, at = now(), who = who, by = {} }
end
local function talk_menu_close(why)
    local by = {}
    for k in pairs(talk_menu.by) do table.insert(by, k) end
    table.sort(by)
    log.info("[remod_cutscene] talk menu closed: " .. why .. (#by > 0 and ("; moved by " .. table.concat(by, ", ")) or ""))
    talk_menu = nil
end
local function talk_menu_frame()  -- each frame while it's open: input, then drawing
    local m = talk_menu
    local k = {}
    for name, vk in pairs(MENU_KEYS) do k[name] = pressed(vk) end  -- every key every frame, so the edges are right
    local bok, busy = pcall(game.busy)
    if playing then talk_menu_close("a cutscene started") return end
    if bok and busy then talk_menu_close("the game is busy (its menu, a movie)") return end
    if now() - m.at < 0.25 then k = {} end  -- the Interact press that opened it
    local function cmd(name)
        if now() - m.at < 0.25 or not game.gui_command then return false end
        local ok, on = pcall(game.gui_command, name)
        return ok and on == true
    end
    local up = cmd("MoveUp") and "the game's up" or k.Up and "Up" or k.W and "W"
    local down = cmd("MoveDown") and "the game's down" or k.Down and "Down" or k.S and "S"
    local pick = cmd("Decide") and "the game's confirm" or k.Enter and "Enter" or k.Space and "Space"
    local back = cmd("Cancel") and "the game's back" or k.Escape and "Escape" or k.Backspace and "Backspace"
    local n = #m.entries
    if up then m.cursor, m.by[up] = (m.cursor - 2) % n + 1, true end
    if down then m.cursor, m.by[down] = m.cursor % n + 1, true end
    if back then talk_menu_close("back (" .. back .. ")") return end
    if pick then
        local c = m.entries[m.cursor]
        talk_menu_close("picked " .. entry_label(c) .. " (" .. pick .. ")")
        fire(c)
        return
    end
    -- Drawn right of centre, like the merchant's: a dark panel, the entry under the cursor lit.
    local size = imgui.get_display_size()
    local w, rh = 420, 38
    local x, y = size.x * 0.6, size.y * 0.5 - (n * rh + 76) / 2
    draw.filled_rect(x, y, w, n * rh + 76, 0xC8000000)
    draw.text("Talk", x + 20, y + 12, 0xFF9AB4C8)
    for i, c in ipairs(m.entries) do
        local ry = y + 40 + (i - 1) * rh
        if i == m.cursor then draw.filled_rect(x + 8, ry - 4, w - 16, rh - 4, 0x5060B0D8) end
        draw.text(entry_label(c), x + 24, ry + 6, i == m.cursor and 0xFFFFFFFF or 0xFFB8B8B8)
    end
    draw.text("Up / Down    Confirm    Back", x + 20, y + 44 + n * rh, 0xFF8C8C8C)
end

-- ---- Talk triggers with the game's own interact icon (spikes/interact_icon_probe.md, runs 2-3, 2026-10-10) ----
-- The game draws its icon for a key trigger (chainsaw.InteractTriggerKey) whose work asks each frame. We copy a key
-- trigger of something loaded nearby (MemberwiseClone; a typewriter's "Use", the merchant's "Button", a file's
-- "Read"...), point its icon at a marker object of ours kept above the character's Head joint, make its work the
-- game's way (generateWork, applyTarget on Leon), and each frame in the game's update ask for the icon
-- (requestDrawKeyIcon) and read the press (checkInput: true one frame per Interact press). So the icon shows the
-- Interact key for keyboard or gamepad, and Interact starts the cutscene. One icon per character: the first ready
-- talk trigger (by file name) on it. If none can be made, the drawn "[T] Talk" prompt and its key stand in.
-- ponytail: each copy is kept for good (handed to the game); one per trigger per area or load, a few small objects.
local ICON_UP = 0.3          -- metres above the Head joint (run 3: looked right)
local ICON_NO_JOINT_UP = 1.9 -- metres above the character's origin when it has no Head joint (the merchant gimmick)
local function talk_icon_marker(name)  -- one per trigger, never destroyed, found by name after Reset Scripts
    local s = sdk.find_type_definition("via.SceneManager"):get_method("get_CurrentScene"):call(nil)
    local go = s and select(2, pcall(s.call, s, "findGameObject(System.String)", sdk.create_managed_string(name)))
    if type(go) ~= "userdata" then go = create_object(name) end
    return keep(go)
end
local function make_talk_icon(c, who)
    local marker = talk_icon_marker("remod_talk_icon_" .. flag_name(c))
    local work, from = game.make_icon(who.body, marker)
    log.info(string.format("[remod_cutscene] %s: the game's interact icon on %s (copied from %s)", c.data.name or c.file,
        tostring(who.kind), tostring(from)))
    return { work = work, marker = marker, body = who.body:get_address() }
end
local function talk_icons()  -- each frame, in the game's update
    if playing or talk_menu or not game.make_icon then return end
    local taken = {}  -- characters that already have an icon this frame
    for _, c in ipairs(cutscenes) do
        local s = trigger_state[c.file]
        local who = s and s.talk_ready and s.talk_who
        local id = who and who.body and who.body:get_address()  -- the same object comes back as a new userdata
        if id and not taken[id] and not s.icon_failed then
            taken[id] = true
            local ok, err = pcall(function()
                if not s.icon or s.icon.body ~= id then s.icon = make_talk_icon(c, who) end
                local x = who.body:call("get_Transform")
                local joint = x:call("getJointByName", "Head")
                local p = joint and joint:call("get_Position")
                local up = p and ICON_UP or ICON_NO_JOINT_UP
                p = p or x:call("get_Position")
                s.icon.marker:call("get_Transform"):call("set_Position", Vector3f.new(p.x, p.y + up, p.z))
                if game.icon_frame(s.icon.work) then
                    log.info("[remod_cutscene] " .. (c.data.name or c.file) .. ": Interact pressed")
                    talk_pressed(c)
                end
            end)
            if not ok then  -- the drawn prompt and its key stand in for it from now on
                s.icon_failed = tostring(err)
                log.info("[remod_cutscene] " .. (c.data.name or c.file) .. ": no game icon (" .. s.icon_failed
                    .. "); the drawn prompt and its key instead")
            end
        end
    end
end
-- The topics whose conditions hold for a character kind's menu (the merchant's: "merchant"), in topic order; the
-- game's part shows them in its menu (game.update) and fires the one picked.
local function topics_ready(npc)
    local where_ok, where = pcall(game.where)
    local out = {}
    for _, c in ipairs(cutscenes) do
        local t = c.data.trigger
        if type(t) == "table" and type(t.topic) == "table" and t.topic.npc == npc then
            trigger_state[c.file] = trigger_state[c.file] or {}
            if holds(t, where_ok and where or nil) and not fired_before(c, t) then table.insert(out, c) end
        end
    end
    table.sort(out, topic_order)
    return out
end
R.topics_ready, R.fire, R.playing = topics_ready, fire, function() return playing end

re.on_pre_application_entry("UpdateBehavior", function()
    if not game then return end
    talk_icons()
    if talk_menu then pcall(game.hold_player) end  -- Leon stays put while he's talking
    if game.update then game.update() end  -- the game's own menus (RE4: the merchant's talk topics)
end)

-- ---- Each frame: keys, motions, letterbox, fades, subtitles ----
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
    local click = pressed(0x01)  -- every frame, so its edge is right when the merchant's menu opens
    if game.frame then pcall(game.frame, click) end
    pcall(watch_saves)
    if flag_watch.on then watch_flags() end
    if talk_menu then
        local ok, err = pcall(talk_menu_frame)
        if not ok and talk_menu then talk_menu_close("a problem: " .. tostring(err)) end
    elseif not playing then  -- talk triggers ready: the game's icon and Interact (talk_icons); a key the file names
        -- also starts it; where the icon couldn't be made, the drawn prompt and its key (T by default) stand in. One
        -- prompt per character (its first ready trigger: Interact or the key opens its talk menu).
        local row, taken = 0, {}
        for _, c in ipairs(cutscenes) do
            local s = trigger_state[c.file]
            local talk = s and s.talk_ready and talk_of(c.data.trigger)
            local id = talk and s.talk_who and s.talk_who.body and s.talk_who.body:get_address()
            if talk and not taken[id or c.file] then
                taken[id or c.file] = true
                if s.icon_failed then
                    draw_prompt(talk, row)
                    row = row + 1
                end
                local key = (s.icon_failed or talk.key) and KEY_CODES[talk.key or "T"]
                if key and pressed(key) then talk_pressed(c) end
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
                local h = try("sound " .. s.id, game.sound_play, new_sounds[s.id].sound)
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
        imgui.text("Not supported in " .. tostring(reframework:get_game_name()) .. " yet: " .. tostring(game_problem))
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
        imgui.text("New movie " .. name .. (game.movie_id(name) and (" (id " .. game.movie_id(name) .. ")") or ""))
    end
    local sound_names = {}
    for name in pairs(new_sounds) do table.insert(sound_names, name) end
    table.sort(sound_names)
    for i, name in ipairs(sound_names) do
        if imgui.button("Play##sound" .. i) then try("sound " .. name, game.sound_play, new_sounds[name].sound) end
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
    local own = rflags_list(rflags.on)
    imgui.text("remod flags on: " .. (own ~= "" and own or "none"))
    if own ~= "" then
        imgui.same_line()
        if imgui.button("Clear") then  -- for trying a once-per-save cutscene again; saved slots keep theirs
            rflags.on = {}
            rflags_write()
            log.info("[remod_cutscene] remod flags cleared from the menu")
        end
    end
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
    if not trigger_shown.at then  -- what trigger.json holds: read again only after a button changes it (a missing
        -- file logged a JSON error every second)
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
            local done = kind == "event" and event_flag(e.name, "End")
            if done then  -- lasting: its story flag stays on, in every save past it
                imgui.same_line()
                if imgui.button("Any time after it") then trigger_add("flags", done) end
            end
        end
    end
    -- Story windows: after one of the game's cutscenes, until the next (its story flags; the id from the log or the
    -- line above, e.g. csa039).
    local _, until_id = imgui.input_text("The game's cutscene (e.g. csa039)", story_window.id or "")
    story_window.id = until_id or ""
    if story_window.id ~= "" then
        local before, after = event_flag(story_window.id, "Start"), event_flag(story_window.id, "End")
        if before then
            if imgui.button("Until it starts") then trigger_add("flags", "!" .. before) end
            imgui.same_line()
        end
        if after then
            if imgui.button("Any time after it##typed") then trigger_add("flags", after) end
        end
        if not before and not after then imgui.text("No story flag found for " .. story_window.id .. " (yet: load a save).") end
    end
    if imgui.tree_node("remod's flags (this mod's cutscenes)") then
        -- Each cutscene's own (on once its trigger started it in this save) and the names their sets_flags give.
        local named = {}
        for i, c in ipairs(cutscenes) do
            local n = flag_name(c)
            if imgui.button("After it##rf" .. i) then trigger_add("flags", "remod:" .. n) end
            imgui.same_line()
            if imgui.button("Until it##rf" .. i) then trigger_add("flags", "!remod:" .. n) end
            imgui.same_line()
            imgui.text(n .. (rflags.on[n] and "  (played in this save)" or ""))
            for _, f in ipairs(type(c.data.sets_flags) == "table" and c.data.sets_flags or {}) do
                named[(tostring(f):gsub("^!", ""))] = true
            end
        end
        local names = {}
        for n in pairs(named) do table.insert(names, n) end
        table.sort(names)
        for i, n in ipairs(names) do
            if imgui.button("Must be on##rn" .. i) then trigger_add("flags", "remod:" .. n) end
            imgui.same_line()
            if imgui.button("Must be off##rn" .. i) then trigger_add("flags", "!remod:" .. n) end
            imgui.same_line()
            imgui.text(n .. (rflags.on[n] and "  (on)" or "  (off)"))
        end
        imgui.tree_pop()
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
                trigger_add("talk", { npc = ch.kind, key = "T", prompt = "Talk", radius = 2.5 })
            end
            imgui.same_line()
            imgui.text(string.format("%s, %.1f m", ch.kind, ch.d))
        end
        imgui.text("Talk trigger: near that character, \"[T] Talk\" shows; T starts the cutscene (key, prompt and\n" ..
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
