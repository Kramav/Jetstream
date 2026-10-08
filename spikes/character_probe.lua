-- remod character probe (CLAUDE.md §10 M3 route 3, other characters in cutscenes): can a script find the other
-- characters, take them over for a cutscene, animate them and place them? Guide: spikes/character_probe.md. No method
-- hooks. What the game has for it [dump]:
--   - chainsaw.CharacterManager lists them: get_PlayerContextList, get_PartnerContextList (Ashley, Luis...),
--     get_DollNpcContextList, get_EnemyContextList; each a CharacterContext with get_BodyGameObject, get_ID (a
--     ContextID, get_DisplayName), get_KindID, get_Position.
--   - chainsaw.OccupiedMediator.requestLock(user, targets, OccupiedMediatorPriority.CUT_SCENE, ...): the game's own way
--     to take characters for a cutscene; requestUnlock gives them back; Context.isLocked() reads it back.
--   F2  next character (the list is read again each press)
--   F3  lock / unlock it for a cutscene (CUT_SCENE priority, Leon as the one asking)
--   F4  restart its current animation (changeMotion, as cutscenes do for Leon)
--   F6  bring it 1.5 m in front of Leon, facing him
-- Results: reframework\data\remod_character_probe.json and the log.

local RUN = 1
local TAG = "[remod character probe] "

local results = { run = RUN, notes = {} }
local function note(text)
    table.insert(results.notes, string.format("%.1f s: %s", os.clock(), text))
    log.info(TAG .. text)
    json.dump_file("remod_character_probe.json", results)
end
local function try(f, ...)
    local ok, v = pcall(f, ...)
    if ok then return v end
    return nil, tostring(v)
end
local function enum(t, name) return sdk.find_type_definition(t):get_field(name):get_data(nil) end
local function instance(t) return sdk.find_type_definition(t):get_method("get_Instance"):call(nil) end

-- ---- The characters ----

local LISTS = { { "get_PlayerContextList", "player" }, { "get_PartnerContextList", "partner" },
                { "get_DollNpcContextList", "doll NPC" }, { "get_EnemyContextList", "enemy" } }

local function characters()
    local cm = sdk.get_managed_singleton("chainsaw.CharacterManager")
    if not cm then error("no CharacterManager (load a save first)") end
    local out = {}
    for _, l in ipairs(LISTS) do
        local list = try(cm.call, cm, l[1])
        for i = 0, (list and list:call("get_Count") or 0) - 1 do
            local ctx = list:call("get_Item", i)
            local body = ctx and try(ctx.call, ctx, "get_BodyGameObject")
            if body then table.insert(out, { ctx = ctx, body = body, what = l[2] }) end
        end
    end
    return out
end

local function leon()
    local cm = sdk.get_managed_singleton("chainsaw.CharacterManager")
    return cm and cm:call("getPlayerContextRef")
end

local function motion_layer(c)
    local m = c.body:call("getComponent(System.Type)", sdk.typeof("via.motion.Motion"))
    return m and m:call("getLayer", 0)
end

local function describe(c)
    local id = try(c.ctx.call, c.ctx, "get_ID")
    local name = id and try(id.call, id, "get_DisplayName") or try(c.body.call, c.body, "get_Name") or "?"
    local p = try(c.ctx.call, c.ctx, "get_Position")
    local l = leon()
    local lp = l and try(l.call, l, "get_Position")
    local dist = (p and lp) and math.sqrt((p.x - lp.x) ^ 2 + (p.y - lp.y) ^ 2 + (p.z - lp.z) ^ 2) or -1
    local layer = try(motion_layer, c)
    local anim = layer and string.format("bank %s motion %s frame %.0f", tostring(layer:call("get_MotionBankID")),
        tostring(layer:call("get_MotionID")), layer:call("get_Frame")) or "no animation layer"
    return string.format("%s (%s, kind %s, body %s) %.1f m away, locked %s, %s", tostring(name), c.what,
        tostring(try(c.ctx.call, c.ctx, "get_KindID")), tostring(try(c.body.call, c.body, "get_Name")), dist,
        tostring(try(c.ctx.call, c.ctx, "isLocked")), anim)
end

local list, index = {}, 0
local function selected()
    return list[index]
end

local function next_character()
    list = characters()
    if #list == 0 then return note("F2: no characters found") end
    index = index % #list + 1
    results.characters = {}
    for i, c in ipairs(list) do results.characters[i] = describe(c) end
    note(string.format("F2: %d of %d: %s", index, #list, describe(list[index])))
end

-- ---- Taking one over ----

local locks = {}  -- body address -> the UnlockParam requestLock gave

local function toggle_lock()
    local c = selected()
    if not c then return note("F3: pick a character first (F2)") end
    local om = instance("chainsaw.OccupiedMediator")
    local key = c.body:get_address()
    if locks[key] then
        om:call("requestUnlock(chainsaw.OccupiedMediator.UnlockParam)", locks[key])
        locks[key] = nil
        return note("F3: unlock requested: " .. describe(c))
    end
    local targets = sdk.create_managed_array("chainsaw.ContextID", 1):add_ref()
    targets:call("SetValue(System.Object, System.Int32)", c.ctx:call("get_ID"), 0)
    local unlock = om:call("requestLock(chainsaw.ContextID, chainsaw.ContextID[], chainsaw.OccupiedMediatorPriority, " ..
        "System.Action, System.Action, chainsaw.OccupiedMediator.LockRequestOption)", leon():call("get_ID"), targets,
        enum("chainsaw.OccupiedMediatorPriority", "CUT_SCENE"), nil, nil, nil)
    if unlock then locks[key] = unlock:add_ref() end
    note("F3: lock requested (" .. (unlock and "got an unlock handle" or "no handle back") .. "): " .. describe(c))
end

local function replay()
    local c = selected()
    if not c then return note("F4: pick a character first (F2)") end
    local layer = motion_layer(c)
    if not layer then return note("F4: it has no animation layer") end
    layer:call("changeMotion(System.UInt32, System.UInt32, System.Single, System.Single, via.motion.InterpolationMode, via.motion.InterpolationCurve)",
        layer:call("get_MotionBankID"), layer:call("get_MotionID"), 0.0, 10.0, 1, 0)
    note("F4: changeMotion called: " .. describe(c))
end

local function bring()
    local c = selected()
    if not c then return note("F6: pick a character first (F2)") end
    local l = leon()
    local lp, lq = l:call("get_Position"), l:call("get_Rotation")
    -- Leon's forward (his rotation applied to +z), flat.
    local fx, fz = 2 * (lq.x * lq.z + lq.w * lq.y), 1 - 2 * (lq.x * lq.x + lq.y * lq.y)
    local len = math.sqrt(fx * fx + fz * fz)
    if len < 1e-6 then fx, fz, len = 0, 1, 1 end
    local x, z = lp.x + fx / len * 1.5, lp.z + fz / len * 1.5
    local yaw = math.atan(lp.x - x, lp.z - z)  -- facing Leon
    local xform = c.body:call("get_Transform")
    xform:call("set_Position", Vector3f.new(x, lp.y, z))
    xform:call("set_Rotation", Quaternion.new(math.cos(yaw / 2), 0, math.sin(yaw / 2), 0))
    note("F6: moved in front of Leon: " .. describe(c))
end

-- ---- Keys and screen ----

local KEYS = { F2 = 0x71, F3 = 0x72, F4 = 0x73, F6 = 0x75 }
local ACTIONS = { F2 = next_character, F3 = toggle_lock, F4 = replay, F6 = bring }
local down = {}
local function pressed(name)
    local is = reframework:is_key_down(KEYS[name])
    local was = down[name]
    down[name] = is
    return is and not was
end

re.on_frame(function()
    for key, f in pairs(ACTIONS) do
        if pressed(key) then
            local ok, err = pcall(f)
            if not ok then note(key .. " failed: " .. tostring(err)) end
        end
    end
    local c = selected()
    local now = c and try(describe, c) or "none (F2)"
    local lines = {
        "remod character probe (run " .. RUN .. "): F2 next character, F3 lock/unlock, F4 restart its animation, " ..
            "F6 bring it in front of Leon",
        string.format("selected %d of %d: %s", index, #list, tostring(now)),
        "last: " .. (results.notes[#results.notes] or "-"),
    }
    for i, line in ipairs(lines) do draw.text(line, 40, 20 + 18 * i, 0xFFFFFFFF) end
end)

re.on_script_reset(function()  -- give back anything still locked
    local om = try(instance, "chainsaw.OccupiedMediator")
    for _, u in pairs(locks) do pcall(om.call, om, "requestUnlock(chainsaw.OccupiedMediator.UnlockParam)", u) end
    locks = {}
end)

note("loaded (run " .. RUN .. ")")
