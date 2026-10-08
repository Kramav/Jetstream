-- remod character probe (CLAUDE.md §10 M3 route 3, other characters in cutscenes): can a script put other characters
-- in a cutscene, animate them and place them? Guide: spikes/character_probe.md. No method hooks.
--
-- Runs 1-3 tried to stop the live Ashley: OccupiedMediator.requestLock (CUT_SCENE) took the request but she kept
-- moving, and requestUnlock crashed the game; her think switch (PartnerBaseContext.set_ThinkEnable, held off every
-- frame) didn't stop her following either. The game's own cutscenes don't use the live characters at all [dump]: an
-- event has puppets (chainsaw.TimelineEventPuppetChara, copying the character's costume and accessories) built from
-- event meshes such as _chainsaw/event/resource/mesh/mesh07/ch2a1z0_00__body.pfb. Run 4 does the same: hide the
-- live one, spawn a puppet of our own.
--   F2  next character (the list is read again each press; puppets we spawned are in it)
--   F3  hide / show it (DrawSelf on it and every object under it)
--   F4  restart its current animation (changeMotion, as cutscenes do for Leon)
--   F5  build a puppet copy of it 1.5 m in front of Leon (see spawn_puppet), and write down its parts
--   F6  bring it 1.5 m in front of Leon, facing him
--   F7  play the standing idle (bank 1000 motion 160, Leon's and Ashley's)
-- Results: reframework\data\remod_character_probe.json and the log.

local RUN = 7
local TAG = "[remod character probe] "

-- Keep the last run's notes: a crash means a relaunch, and loading again would write over them (run 1 lost F2's).
local last = json.load_file("remod_character_probe.json")
local results = { run = RUN, notes = {}, previous = last and { run = last.run, notes = last.notes } or nil }
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

-- ---- The characters ----

local LISTS = { { "get_PlayerContextList", "player" }, { "get_PartnerContextList", "partner" },
                { "get_DollNpcContextList", "doll NPC" }, { "get_EnemyContextList", "enemy" } }
local puppets = {}  -- { body = GameObject, what = "puppet" }, spawned by F5

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
    for _, p in ipairs(puppets) do table.insert(out, p) end
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

-- Every object under (and including) a GameObject, through its transform's children.
local function tree(go)
    local out = {}
    local function walk(xform)
        while xform do
            table.insert(out, xform:call("get_GameObject"))
            walk(xform:call("get_Child"))
            xform = xform:call("get_Next")
        end
    end
    table.insert(out, go)
    walk(go:call("get_Transform"):call("get_Child"))
    return out
end

local function describe(c)
    local id = c.ctx and try(c.ctx.call, c.ctx, "get_ID")
    local name = id and try(id.call, id, "get_DisplayName") or "-"
    local p = try(function() return c.body:call("get_Transform"):call("get_Position") end)
    local l = leon()
    local lp = l and try(l.call, l, "get_Position")
    local dist = (p and lp) and math.sqrt((p.x - lp.x) ^ 2 + (p.y - lp.y) ^ 2 + (p.z - lp.z) ^ 2) or -1
    local layer = try(motion_layer, c)
    local anim = layer and try(function()
        return string.format("bank %s motion %s frame %.0f", tostring(layer:call("get_MotionBankID")),
            tostring(layer:call("get_MotionID")), layer:call("get_Frame"))
    end) or "no animation layer"
    return string.format("%s (%s, body %s) %.1f m away, drawn %s, %s", tostring(name), c.what,
        tostring(try(c.body.call, c.body, "get_Name")), dist, tostring(try(c.body.call, c.body, "get_DrawSelf")), anim)
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

-- ---- Hiding, spawning, placing, animating ----

local hidden = {}  -- body address -> the character

local function set_drawn(c, on)
    local objects = tree(c.body)
    for _, go in ipairs(objects) do go:call("set_DrawSelf", on) end
    return #objects
end

local function toggle_hide()
    local c = selected()
    if not c then return note("F3: pick a character first (F2)") end
    local key = c.body:get_address()
    if hidden[key] then
        hidden[key] = nil
        return note(string.format("F3: shown again (%d objects): %s", set_drawn(c, true), describe(c)))
    end
    hidden[key] = c
    note(string.format("F3: hidden (%d objects): %s", set_drawn(c, false), describe(c)))
end

-- 1.5 m in front of Leon, and the rotation facing him.
local function front_of_leon()
    local l = leon()
    local lp, lq = l:call("get_Position"), l:call("get_Rotation")
    local fx, fz = 2 * (lq.x * lq.z + lq.w * lq.y), 1 - 2 * (lq.x * lq.x + lq.y * lq.y)  -- his +z, flat
    local len = math.sqrt(fx * fx + fz * fz)
    if len < 1e-6 then fx, fz, len = 0, 1, 1 end
    local x, z = lp.x + fx / len * 1.5, lp.z + fz / len * 1.5
    local yaw = math.atan(lp.x - x, lp.z - z)
    return Vector3f.new(x, lp.y, z), Quaternion.new(math.cos(yaw / 2), 0, math.sin(yaw / 2), 0)
end

local function bring()
    local c = selected()
    if not c then return note("F6: pick a character first (F2)") end
    local pos, rot = front_of_leon()
    local xform = c.body:call("get_Transform")
    xform:call("set_Position", pos)
    xform:call("set_Rotation", rot)
    note("F6: moved in front of Leon: " .. describe(c))
end

local function components(go)
    local names = {}
    local arr = go:call("get_Components")
    for i = 0, (arr and arr:call("get_Length") or 0) - 1 do
        local comp = arr:call("GetValue(System.Int32)", i)
        table.insert(names, comp and comp:get_type_definition():get_full_name() or "?")
    end
    return table.concat(names, ", ")
end

local function component(go, t)
    return go:call("getComponent(System.Type)", sdk.typeof(t))
end

local function holder_path(h)
    return h and tostring(try(h.call, h, "get_ResourcePath")) or "-"
end

local function share_motion(from, to)
    local live = component(from, "via.motion.Motion")
    if not live then return end
    local motion = to:call("createComponent(System.Type)", sdk.typeof("via.motion.Motion"))
    local n = live:call("getDynamicMotionBankCount")
    motion:call("setDynamicMotionBankCount", n)
    for b = 0, n - 1 do motion:call("setDynamicMotionBank", b, live:call("getDynamicMotionBank", b)) end
    local asset = live:call("get_MotionBankAsset")
    if asset then motion:call("set_MotionBankAsset", asset) end
    -- Run 6: a new Motion has no layers (F4 / F7: "no animation layer"). Give it as many as the live one, each a new
    -- TreeLayer.
    local layers = live:call("getLayerCount")
    motion:call("setLayerCount", layers)
    for l = 0, layers - 1 do
        if not motion:call("getLayer", l) then motion:call("setLayer", l, sdk.create_instance("via.motion.TreeLayer"):add_ref()) end
    end
    return string.format("%d layers (layer 0 %s)", layers, motion:call("getLayer", 0) and "made" or "missing")
end

-- Every set_X with a get_X on the component's own type, copied from the live one; returns copied, failed.
local function copy_properties(from, to, skip)
    local copied, failed = 0, {}
    for _, m in ipairs(from:get_type_definition():get_methods()) do
        local name = m:get_name()
        local prop = name:match("^set_(.+)$")
        if prop and m:get_num_params() == 1 and not skip[prop] and from:get_type_definition():get_method("get_" .. prop) then
            local ok, err = pcall(function() to:call(name, from:call("get_" .. prop)) end)
            if ok then copied = copied + 1 else table.insert(failed, prop .. ": " .. tostring(err)) end
        end
    end
    return copied, failed
end

-- Run 4 spawned the event mesh prefab: only a Transform and chainsaw.TimelineEventMeshSettings (a mesh swap the event
-- applies to a puppet), so nothing showed. The puppets are objects in each event's .scn, built from plain resources
-- (.mesh, .mdf2, .fbxskel, .motlist, .chain, .jmap). Run 5 found Ashley's layout: her root ch2a1z0_body has no mesh
-- (a Motion, a via.motion.DummySkeleton, the AI); the meshes are children (body, head, hair, ac2100_10; hair has a
-- raytrace_mesh child), each with a Motion of its own. Run 6 copies that: a root with the DummySkeleton's skeleton and
-- the root's motion banks, and under it each part with a mesh, sharing its mesh and material, with the live part's
-- SameJointsConstraint / ParentJoint (how RE Engine parts follow their parent's joints) and its offsets.
local function spawn_puppet()
    local c = selected()
    if not c then return note("F5: pick a character first (F2)") end
    local create = sdk.find_type_definition("via.GameObject"):get_method("create(System.String)")
    local new = function(name) return create:call(nil, sdk.create_managed_string("remod_puppet_" .. name)):add_ref() end
    local pos, rot = front_of_leon()
    local record, copies, made, strands = {}, {}, 0, {}
    for i, go in ipairs(tree(c.body)) do
        local name = tostring(go:call("get_Name"))
        local mesh = component(go, "via.render.Mesh")
        local xform = go:call("get_Transform")
        local parent = xform:call("get_Parent")
        local entry = {
            name = name,
            parent = parent and tostring(try(function() return parent:call("get_GameObject"):call("get_Name") end)) or "-",
            components = components(go),
            mesh = mesh and holder_path(mesh:call("getMesh")) or "-",
            material = mesh and holder_path(mesh:call("get_Material")) or "-",
            same_joints = tostring(try(xform.call, xform, "get_SameJointsConstraint")),
            parent_joint = tostring(try(xform.call, xform, "get_ParentJoint")),
        }
        table.insert(record, entry)
        local copy
        if i == 1 then
            copy = new(name)
            local cx = copy:call("get_Transform")
            cx:call("set_Position", pos)
            cx:call("set_Rotation", rot)
            local skel = component(go, "via.motion.DummySkeleton")
            if skel then
                copy:call("createComponent(System.Type)", sdk.typeof("via.motion.DummySkeleton"))
                    :call("set_SkeletonResourceHandle", skel:call("get_SkeletonResourceHandle"))
            end
            entry.motion = share_motion(go, copy)
        elseif mesh and not name:find("raytrace") then
            copy = new(name)
            local m = copy:call("createComponent(System.Type)", sdk.typeof("via.render.Mesh"))
            m:call("setMesh", mesh:call("getMesh"))
            m:call("set_Material", mesh:call("get_Material"))
            entry.motion = share_motion(go, copy)
            -- Run 6 had no hair: it's drawn by via.render.Strands, not the mesh. Copy all its settings; its target
            -- mesh is pointed at our copy once every part exists.
            local live_strands = component(go, "via.render.Strands")
            if live_strands then
                local s = copy:call("createComponent(System.Type)", sdk.typeof("via.render.Strands"))
                local n, failed = copy_properties(live_strands, s, { StrandTargetMesh = true })
                entry.strands = string.format("%d settings copied, %d failed: %s", n, #failed, table.concat(failed, "; "))
                table.insert(strands, { live = live_strands, copy = s, entry = entry })
            end
            local cx = copy:call("get_Transform")
            local into = (parent and copies[parent:call("get_GameObject"):get_address()]) or copies[c.body:get_address()]
            cx:call("set_Parent", into:call("get_Transform"))
            cx:call("set_LocalPosition", xform:call("get_LocalPosition"))
            cx:call("set_LocalRotation", xform:call("get_LocalRotation"))
            local pj = xform:call("get_ParentJoint")
            if pj and tostring(pj) ~= "" then cx:call("set_ParentJoint", pj) end
            cx:call("set_SameJointsConstraint", xform:call("get_SameJointsConstraint"))
            made = made + 1
        end
        if copy then copies[go:get_address()] = copy; entry.copied = true end
    end
    for _, s in ipairs(strands) do
        local ok, err = pcall(function()
            local ref = s.live:call("get_StrandTargetMesh")
            local target = ref:call("get_Target")
            -- Its copy; a part we didn't copy (raytrace_mesh) falls back to its parent's copy.
            local to = target and copies[target:get_address()]
            if target and not to then
                local p = target:call("get_Transform"):call("get_Parent")
                to = p and copies[p:call("get_GameObject"):get_address()]
            end
            s.entry.strand_target = target and tostring(target:call("get_Name")) or "-"
            if to then
                ref:call(".ctor", to)
                s.copy:call("set_StrandTargetMesh", ref)
                s.entry.strand_target = s.entry.strand_target .. " -> " .. tostring(to:call("get_Name"))
            end
        end)
        if not ok then s.entry.strand_target = "failed: " .. tostring(err) end
    end
    results.parts_of = { character = describe(c), parts = record }
    local root = copies[c.body:get_address()]
    local p = { body = root, what = "puppet" }
    table.insert(puppets, p)
    note(string.format("F5: built a copy with %d mesh parts, its animation %s: %s", made, tostring(record[1].motion),
        describe(p)))
end

local function change(c, bank, motion, key)
    local layer = motion_layer(c)
    if not layer then return note(key .. ": it has no animation layer") end
    layer:call("changeMotion(System.UInt32, System.UInt32, System.Single, System.Single, via.motion.InterpolationMode, via.motion.InterpolationCurve)",
        bank, motion, 0.0, 10.0, 1, 0)
    note(key .. ": changeMotion called: " .. describe(c))
end

local function replay()
    local c = selected()
    if not c then return note("F4: pick a character first (F2)") end
    local layer = motion_layer(c)
    if not layer then return note("F4: it has no animation layer") end
    change(c, layer:call("get_MotionBankID"), layer:call("get_MotionID"), "F4")
end

local function idle()
    local c = selected()
    if not c then return note("F7: pick a character first (F2)") end
    change(c, 1000, 160, "F7")
end

-- ---- Keys and screen ----

local KEYS = { F2 = 0x71, F3 = 0x72, F4 = 0x73, F5 = 0x74, F6 = 0x75, F7 = 0x76 }
local ACTIONS = { F2 = next_character, F3 = toggle_hide, F4 = replay, F5 = spawn_puppet, F6 = bring, F7 = idle }
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
        "remod character probe (run " .. RUN .. "): F2 next, F3 hide/show, F4 restart its animation, " ..
            "F5 build a puppet copy, F6 bring in front of Leon, F7 stand idle",
        string.format("selected %d of %d: %s", index, #list, tostring(now)),
        "last: " .. (results.notes[#results.notes] or "-"),
    }
    for i, line in ipairs(lines) do draw.text(line, 40, 20 + 18 * i, 0xFFFFFFFF) end
end)

re.on_script_reset(function()  -- show what we hid, remove what we spawned
    for _, c in pairs(hidden) do pcall(set_drawn, c, true) end
    for _, c in ipairs(puppets) do pcall(function() c.body:call("destroy", c.body) end) end
    hidden, puppets = {}, {}
end)

note("loaded (run " .. RUN .. ")")
