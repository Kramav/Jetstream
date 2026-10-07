-- remod cutscene probe (CLAUDE.md §10 M3 route 3): answers the engine questions a cutscene runtime depends on, in one
-- short session. Install with a Lua script block's Test in game, load a save, then press Reset Scripts.
--
--   F5  write down the camera (its object, position, rotation, FOV, components) and how the player is found
--   F6  camera override test: freezes the view, trying one engine hook after another (3 s each, shown on screen).
--       Move Leon and the mouse meanwhile. If the view stays perfectly still for one, press F7 while it's shown.
--   F7  "this hook holds": records the hook shown now
--   F8  restart Leon's current animation from frame 0 (changeMotion): watch whether he visibly restarts it
--
-- Everything goes to reframework\data\remod_cutscene_probe.json (remod reads it from the game's folder) and the log.
-- Read-only except for the camera while F6 runs and Leon's animation on F8; nothing is saved to the game.

local results = { game = reframework:get_game_name(), notes = {} }

local function save()
    json.dump_file("remod_cutscene_probe.json", results)
end

local function note(text)
    table.insert(results.notes, text)
    log.info("[remod probe] " .. text)
    save()
end

local function vec(v) return v and { v.x, v.y, v.z } or nil end
local function quat(q) return q and { q.x, q.y, q.z, q.w } or nil end

-- The camera's GameObject and Transform, through REFramework's primary camera (a via.Camera component).
local function camera_parts()
    local cam = sdk.get_primary_camera()
    if not cam then return nil end
    local go = cam:call("get_GameObject")
    local xform = go and go:call("get_Transform")
    return cam, go, xform
end

-- The player's GameObject in RE4R, as EMV Engine (MIT) finds it: CharacterManager:getPlayerContextRef():get_BodyGameObject.
-- Other ways are tried and written down too, for other games later.
local function find_player()
    local found = {}
    for _, name in ipairs({ "chainsaw.CharacterManager", "chainsaw.PlayerManager" }) do
        local mgr = sdk.get_managed_singleton(name)
        found[name] = mgr ~= nil
        if mgr then
            local ok, ctx = pcall(mgr.call, mgr, "getPlayerContextRef")
            if ok and ctx then
                local ok2, body = pcall(ctx.call, ctx, "get_BodyGameObject")
                if ok2 and body then return body, name .. ":getPlayerContextRef():get_BodyGameObject()", found end
            end
        end
    end
    return nil, nil, found
end

-- F5
local function describe()
    local cam, go, xform = camera_parts()
    if not cam then
        note("F5: no primary camera")
        return
    end
    local c = {}
    c.object = go and go:call("get_Name")
    c.position = xform and vec(xform:call("get_Position"))
    c.rotation = xform and quat(xform:call("get_Rotation"))
    local ok, fov = pcall(cam.call, cam, "get_FOV")
    c.fov = ok and fov or nil
    c.components = {}
    local ok2, comps = pcall(go.call, go, "get_Components")
    if ok2 and comps then
        for _, comp in ipairs(comps:get_elements()) do
            table.insert(c.components, comp:get_type_definition():get_full_name())
        end
    end
    local parent = xform and xform:call("get_Parent")
    c.parent = parent and parent:call("get_GameObject"):call("get_Name") or nil
    results.camera = c

    local player, how, managers = find_player()
    results.player = { found_by = how, managers = managers, object = player and player:call("get_Name") or nil }
    if player then
        local pxf = player:call("get_Transform")
        results.player.position = vec(pxf:call("get_Position"))
    end

    -- Does fs.glob list files in reframework\data, and how are they named? (The runtime finds cutscene files so.)
    local ok3, files = pcall(fs.glob, "remod_cutscene_probe.*")
    results.glob = ok3 and files or ("failed: " .. tostring(files))
    results.clock = os and os.clock and os.clock() or "no os.clock"
    note("F5: camera " .. tostring(c.object) .. ", fov " .. tostring(c.fov) .. ", player " .. tostring(how))
end

-- F6 / F7: which hook's camera override holds.
local HOOKS = { "UpdateBehavior", "LateUpdateBehavior", "UpdateMotion", "PrepareRendering",
                "BeforeLockSceneRendering", "LockScene", "BeginRendering" }
local test = nil  -- { pos, rot, index, started, readback = { hook = {held, moved} } }

-- Seconds: os.clock if REFramework's Lua has it (F5 writes down whether it does), else frames at 60 per second.
local frames = 0
local function now() return (os and os.clock) and os.clock() or frames / 60 end

local function current_hook()
    return test and HOOKS[test.index] or nil
end

local function start_test()
    local _, _, xform = camera_parts()
    if not xform then
        note("F6: no camera to test")
        return
    end
    test = { pos = xform:call("get_Position"), rot = xform:call("get_Rotation"), index = 1, started = now() }
    results.hooks = results.hooks or {}
    note("F6: camera override test started")
end

for _, hook in ipairs(HOOKS) do
    re.on_pre_application_entry(hook, function()
        if current_hook() ~= hook then return end
        local _, _, xform = camera_parts()
        if xform then
            xform:call("set_Position", test.pos)
            xform:call("set_Rotation", test.rot)
        end
    end)
end

-- Read back after the frame's rendering began: did anything move the camera after our hook?
re.on_application_entry("BeginRendering", function()
    local hook = current_hook()
    if not hook then return end
    local _, _, xform = camera_parts()
    if not xform then return end
    local d = (xform:call("get_Position") - test.pos):length()
    local r = results.hooks[hook] or { frames = 0, moved_frames = 0, max_moved = 0 }
    r.frames = r.frames + 1
    if d > 0.001 then r.moved_frames = r.moved_frames + 1 end
    if d > r.max_moved then r.max_moved = d end
    results.hooks[hook] = r
end)

-- F8: restart the player's current animation from frame 0.
local function replay()
    local player = find_player()
    if not player then
        note("F8: no player found")
        return
    end
    local motion = player:call("getComponent(System.Type)", sdk.typeof("via.motion.Motion"))
    if not motion then
        note("F8: the player has no via.motion.Motion")
        return
    end
    local layer = motion:call("getLayer", 0)
    local m = {}
    for _, getter in ipairs({ "get_MotionBankID", "get_MotionID", "get_Frame", "get_EndFrame", "get_Speed" }) do
        local ok, v = pcall(layer.call, layer, getter)
        m[getter] = ok and v or ("failed: " .. tostring(v))
    end
    local ok, err = pcall(layer.call, layer,
        "changeMotion(System.UInt32, System.UInt32, System.Single, System.Single, via.motion.InterpolationMode, via.motion.InterpolationCurve)",
        m.get_MotionBankID, m.get_MotionID, 0.0, 10.0, 1, 0)
    m.change_motion = ok and "called" or ("failed: " .. tostring(err))
    results.motion = m
    note("F8: changeMotion " .. m.change_motion .. " (bank " .. tostring(m.get_MotionBankID) .. ", motion " ..
        tostring(m.get_MotionID) .. ")")
end

-- Keys, once per press (Windows virtual-key codes).
local KEYS = { F5 = 0x74, F6 = 0x75, F7 = 0x76, F8 = 0x77 }
local down = {}
local function pressed(name)
    local is = reframework:is_key_down(KEYS[name])
    local was = down[name]
    down[name] = is
    return is and not was
end

re.on_frame(function()
    frames = frames + 1
    if pressed("F5") then describe() end
    if pressed("F6") then
        if test then
            test = nil
            note("F6: test stopped")
        else
            start_test()
        end
    end
    if pressed("F7") and current_hook() then
        results.holds = results.holds or {}
        table.insert(results.holds, current_hook())
        note("F7: holds with " .. current_hook())
    end
    if pressed("F8") then replay() end

    if test then
        if now() - test.started > 3 then
            test.index = test.index + 1
            test.started = now()
            if test.index > #HOOKS then
                test = nil
                note("F6: test finished")
                return
            end
        end
        draw.text("remod probe: camera held by " .. current_hook() .. " (" .. test.index .. " of " .. #HOOKS ..
            "). Move Leon and the mouse: if the view stays perfectly still, press F7.", 40, 40, 0xFFFFFFFF)
    end
end)

re.on_draw_ui(function()
    if imgui.tree_node("remod cutscene probe") then
        imgui.text("F5 camera and player, F6 camera hook test (F7: this one holds), F8 replay Leon's animation.")
        imgui.text("Results: reframework\\data\\remod_cutscene_probe.json")
        imgui.tree_pop()
    end
end)

re.on_script_reset(function() test = nil end)
note("loaded")
