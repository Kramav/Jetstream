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
--   F8  build the next puppet from a definition: game file paths only, no live character needed (run 8, see
--       from_files): Luis (built in), then each reframework\data\remod_puppets\*.json (a mod's new character)
--   F9  write down the selected live character's definition (the files it really uses) into the results
--   F11 put every puppet away (hidden, not updating), as Reset Scripts does; F8 reuses it. Puppets are never
--       destroyed: run 9 crashed the game building after a destroy, twice (see build_next).
-- Results: reframework\data\remod_character_probe.json and the log.

local RUN = 12
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

-- ---- Run 8: puppets from files ----
-- Run 7's copy shares a live character's resources. Every setter it used takes a resource holder [dump]
-- (Mesh.setMesh / set_Material, DummySkeleton.set_SkeletonResourceHandle, Motion.set_MotionBankAsset,
-- DynamicMotionBank.set_MotionBank, Strands.set_Strand / set_StrandBindingData), and sdk.create_resource(type, path)
-- loads one from the game's files [REFramework book], so a definition (paths) is enough: any character, loaded in
-- the level or not. Luis's parts are the ones the game's event csa012 builds its ch2a3z0 puppet from; his motion
-- banks are his body prefab's (appsystem/character/ch2a3z0).

local G = "_Chainsaw/Character/ch/cha3/cha300/"
local DEFINITIONS = {
    { name = "luis (cha300)", skeleton = "_Chainsaw/Character/ch/cha3/cha3.fbxskel",
      motion_bank = "_Chainsaw/AppSystem/Character/ch2Common/Motion/ch2CommonBank.motbank",
      dynamic_banks = { "_Chainsaw/Animation/ch/cha3/motbank/cha3.motbank" }, layers = 13,
      parts = { { name = "body", mesh = G .. "00/cha300_00.mesh", material = G .. "00/cha300_00.mdf2" },
                { name = "head", mesh = G .. "10/cha300_10.mesh", material = G .. "10/cha300_10.mdf2" },
                { name = "hair", mesh = G .. "20/cha300_20.mesh", material = G .. "20/cha300_20.mdf2" } } },
}
local PUPPET_DIR = "remod_puppets"  -- under reframework\data: a mod ships its characters' definitions there
local captured = {}  -- F9's definitions, buildable by F8 too

local function definitions()
    local out = {}
    for _, d in ipairs(DEFINITIONS) do table.insert(out, d) end
    local ok, files = pcall(fs.glob, PUPPET_DIR .. "[/\\\\].*\\.json$")
    for _, path in ipairs(ok and files or {}) do
        local def = json.load_file(path:match("[/\\]data[/\\](.*)$") or path)
        if type(def) == "table" and type(def.parts) == "table" then
            def.name = tostring(def.name or path)
            table.insert(out, def)
        end
    end
    for _, d in ipairs(captured) do table.insert(out, d) end
    return out
end

-- A resource holder for a file, or nil and why. Never hand a nil to a game method: that crashes the game (run 1).
local function holder(rtype, path)
    if type(path) ~= "string" or path == "" then return nil, "no path" end
    local res = sdk.create_resource(rtype, path)
    if not res then return nil, "not loaded: " .. path end
    res:add_ref()
    local h = res:create_holder(rtype .. "Holder")
    if not h then return nil, "no holder: " .. path end
    return h:add_ref()
end

-- A puppet's root object is named after its definition, so a put-away one can be found again after Reset Scripts.
local function key_of(def)
    return "remod_puppet_" .. (tostring(def.name):gsub("[^%w]", "_"))
end

local function from_files(def)
    local rec = { name = def.name, problems = {}, parts = {} }
    local function load(rtype, path, what)
        local ok, h, why = pcall(holder, rtype, path)
        if ok and h then return h end
        table.insert(rec.problems, what .. ": " .. tostring(ok and why or h))
    end
    local create = sdk.find_type_definition("via.GameObject"):get_method("create(System.String)")
    local new = function(name) return create:call(nil, sdk.create_managed_string("remod_puppet_" .. name)):add_ref() end
    local add = function(go, t) return go:call("createComponent(System.Type)", sdk.typeof(t)) end

    local bank = def.motion_bank and load("via.motion.MotionBankResource", def.motion_bank, "motion bank")
    local dynamic = {}
    for _, path in ipairs(def.dynamic_banks or {}) do
        local h = load("via.motion.MotionBankResource", path, "dynamic bank")
        local d = h and (sdk.create_instance("via.motion.DynamicMotionBank") or sdk.create_instance("via.motion.DynamicMotionBank", true))
        if d then
            d = d:add_ref()
            d:call("set_MotionBank", h)
            table.insert(dynamic, d)
        elseif h then
            table.insert(rec.problems, "couldn't make a DynamicMotionBank")
        end
    end
    local function add_motion(go, layers)
        local m = add(go, "via.motion.Motion")
        if bank then m:call("set_MotionBankAsset", bank) end
        m:call("setDynamicMotionBankCount", #dynamic)
        for i, d in ipairs(dynamic) do m:call("setDynamicMotionBank", i - 1, d) end
        m:call("setLayerCount", layers)
        for l = 0, layers - 1 do
            if not m:call("getLayer", l) then m:call("setLayer", l, sdk.create_instance("via.motion.TreeLayer"):add_ref()) end
        end
    end

    local pos, rot = front_of_leon()
    local root = create:call(nil, sdk.create_managed_string(key_of(def))):add_ref()
    local rx = root:call("get_Transform")
    rx:call("set_Position", pos)
    rx:call("set_Rotation", rot)
    local skel = load("via.motion.SkeletonResource", def.skeleton, "skeleton")
    if skel then add(root, "via.motion.DummySkeleton"):call("set_SkeletonResourceHandle", skel) end
    add_motion(root, tonumber(def.layers) or 13)

    for _, part in ipairs(def.parts) do
        local name = tostring(part.name or "part")
        local go = new(name)
        local mesh = load("via.render.MeshResource", part.mesh, name .. " mesh")
        local mat = load("via.render.MeshMaterialResource", part.material, name .. " material")
        if mesh then
            local m = add(go, "via.render.Mesh")
            m:call("setMesh", mesh)
            if mat then m:call("set_Material", mat) end
        end
        add_motion(go, 0)  -- run 7: the live body part's Motion has no layers and follows all the same
        local s = part.strands
        if type(s) == "table" then
            local strand = load("via.render.StrandsResource", s.strand, name .. " strands")
            local binding = load("via.render.StrandsBindingResource", s.binding, name .. " strand binding")
            local smat = load("via.render.MeshMaterialResource", s.material, name .. " strand material")
            if strand then
                local c = add(go, "via.render.Strands")
                c:call("set_Strand", strand)
                if binding then c:call("set_StrandBindingData", binding) end
                if smat then c:call("set_Material", smat) end
            end
        end
        local x = go:call("get_Transform")
        x:call("set_Parent", rx)
        x:call("set_LocalPosition", Vector3f.new(0, 0, 0))
        x:call("set_LocalRotation", Quaternion.new(1, 0, 0, 0))
        if part.parent_joint then x:call("set_ParentJoint", tostring(part.parent_joint)) else x:call("set_SameJointsConstraint", true) end
        table.insert(rec.parts, string.format("%s: mesh %s, material %s%s", name, mesh and "loaded" or "missing",
            mat and "loaded" or "missing", s and ", strands" or ""))
    end
    rec.dynamic_banks = #dynamic
    results.from_files = results.from_files or {}
    table.insert(results.from_files, rec)
    return rec, root
end

-- Run 9: building again after a puppet was destroyed crashed the game twice (Luis after Reset Scripts, rmc001 after
-- F11), 10-18 ms after the build; building twice with nothing destroyed was fine. So puppets are never destroyed:
-- put away (hidden, not updating), and reused by the next F8 of the same definition, found by name after a reset.
local put_away = {}  -- key -> root GameObject
local function set_active(go, on)
    for _, o in ipairs(tree(go)) do
        o:call("set_DrawSelf", on)
        o:call("set_UpdateSelf", on)
    end
end
local function scene_find(name)
    local scene = sdk.find_type_definition("via.SceneManager"):get_method("get_CurrentScene"):call(nil)
    return scene and scene:call("findGameObject(System.String)", name)
end

-- Run 10: F8's puppet animates and is drawn but isn't seen; F5's copy of it, new parts given the same mesh and
-- material holders later, is. Run 11: parts whose files the game already had (rmc001's head, hair, accessory: the
-- live Ashley's) were ready to draw at once; the others (all of Luis, rmc001's new body) never were (get_MeshReady
-- false), not after setting the mesh again nor switching the part off and on. So a Mesh handed a resource that
-- hasn't loaded yet stays broken, and a fresh one made once it has works (F5). Holders have no load state [dump]. So
-- each part is watched: not ready after 1 s, a fresh part takes its place (the stuck one hidden, renamed, never
-- destroyed: run 9), up to 5 times.
local STUCK = "remod_stuck_part"
local watching = {}  -- { mesh, go, name, t0, tries }
local function watch_meshes(root, label)
    for _, go in ipairs(tree(root)) do
        local m = component(go, "via.render.Mesh")
        local name = tostring(go:call("get_Name"))
        if m and name ~= STUCK then
            table.insert(watching, { mesh = m, go = go, name = label .. "/" .. name, t0 = os.clock(), tries = 0 })
        end
    end
end
local function fresh_part(go, m)
    local x = go:call("get_Transform")
    local create = sdk.find_type_definition("via.GameObject"):get_method("create(System.String)")
    local new = create:call(nil, sdk.create_managed_string(tostring(go:call("get_Name")))):add_ref()
    local nm = new:call("createComponent(System.Type)", sdk.typeof("via.render.Mesh"))
    nm:call("setMesh", m:call("getMesh"))
    local mat = m:call("get_Material")
    if mat then nm:call("set_Material", mat) end
    share_motion(go, new)
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
local function check_meshes()
    for i = #watching, 1, -1 do
        local w = watching[i]
        local age = os.clock() - w.t0
        if try(w.mesh.call, w.mesh, "get_ReadyToDraw") then
            table.remove(watching, i)
            note(string.format("%s: ready to draw %s (%.1f s)", w.name,
                w.tries == 0 and "by itself" or ("after " .. w.tries .. " fresh part(s)"), age))
        elseif age > 1 and w.tries >= 5 then
            table.remove(watching, i)
            note(string.format("%s: never ready to draw, %d fresh parts tried (mesh ready %s, material ready %s)", w.name,
                w.tries, tostring(try(w.mesh.call, w.mesh, "get_MeshReady")),
                tostring(try(w.mesh.call, w.mesh, "get_MaterialReady"))))
        elseif age > 1 then
            w.go, w.mesh = fresh_part(w.go, w.mesh)
            w.tries, w.t0 = w.tries + 1, os.clock()
        end
    end
end

-- Shown in front of Leon, selected, and in its idle: run 9 built Luis but he wasn't seen until F7, as his layers had
-- no motion (bank 0, motion -1), so no pose (run 8's part 1 pressed F7 8 s after F8).
local function show_puppet(root, def)
    local pos, rot = front_of_leon()
    local x = root:call("get_Transform")
    x:call("set_Position", pos)
    x:call("set_Rotation", rot)
    local p = { body = root, what = "puppet: " .. def.name, key = key_of(def) }
    table.insert(puppets, p)
    list, index = characters(), 0
    for i, c in ipairs(list) do if c == p then index = i end end  -- select it, ready for F7 / F4 / F6
    watch_meshes(root, def.name)
    local idle = type(def.idle) == "table" and def.idle or { bank = 1000, motion = 160 }
    local layer = motion_layer(p)
    if not layer then return "no animation layer" end
    layer:call("changeMotion(System.UInt32, System.UInt32, System.Single, System.Single, via.motion.InterpolationMode, via.motion.InterpolationCurve)",
        idle.bank, idle.motion, 0.0, 0.0, 1, 0)
    return string.format("idle %d / %d started", idle.bank, idle.motion)
end

local def_index = 0
local function build_next()
    local defs = definitions()
    def_index = def_index % #defs + 1
    local def = defs[def_index]
    local key = key_of(def)
    for _, p in ipairs(puppets) do
        if p.key == key then return note("F8: " .. def.name .. " is already out (F11 puts it away)") end
    end
    local root, how = put_away[key], "put away earlier"
    if not root then root, how = try(scene_find, key), "found in the scene after a reset" end
    if root then
        put_away[key] = nil
        set_active(root, true)
        return note(string.format("F8: reused %s (%s, nothing built); %s", def.name, how, show_puppet(root, def)))
    end
    local rec, built = from_files(def)
    note(string.format("F8: built %s from files (%d of %d definitions), %d problem(s)%s; %s; %s", rec.name, def_index,
        #defs, #rec.problems, #rec.problems > 0 and (": " .. table.concat(rec.problems, "; ")) or "",
        table.concat(rec.parts, "; "), show_puppet(built, def)))
end

-- The files a live character uses, as a definition F8 can build (compare with the hand-written ones).
local function path_of(h)
    local p = h and try(h.call, h, "get_ResourcePath")
    return (p and tostring(p) ~= "") and tostring(p) or nil
end
local function capture()
    local c = selected()
    if not c or not c.ctx then return note("F9: pick a live character first (F2)") end
    local def = { name = "live " .. tostring(c.body:call("get_Name")), parts = {}, dynamic_banks = {} }
    local skel = component(c.body, "via.motion.DummySkeleton")
    def.skeleton = skel and path_of(skel:call("get_SkeletonResourceHandle"))
    local m = component(c.body, "via.motion.Motion")
    if m then
        def.motion_bank = path_of(m:call("get_MotionBankAsset"))
        def.layers = m:call("getLayerCount")
        for b = 0, m:call("getDynamicMotionBankCount") - 1 do
            local d = m:call("getDynamicMotionBank", b)
            local p = d and (path_of(try(d.call, d, "get_MotionBank")) or path_of(try(d.call, d, "get_MotionList")))
            table.insert(def.dynamic_banks, p or "?")
        end
    end
    for i, go in ipairs(tree(c.body)) do
        local mesh = component(go, "via.render.Mesh")
        local name = tostring(go:call("get_Name"))
        if i > 1 and mesh and not name:find("raytrace") then
            local x = go:call("get_Transform")
            local part = { name = name, mesh = path_of(mesh:call("getMesh")), material = path_of(mesh:call("get_Material")) }
            if not x:call("get_SameJointsConstraint") then
                local pj = x:call("get_ParentJoint")
                if pj and tostring(pj) ~= "" then part.parent_joint = tostring(pj) end
            end
            local s = component(go, "via.render.Strands")
            if s then
                part.strands = { strand = path_of(try(s.call, s, "get_Strand")),
                                 binding = path_of(try(s.call, s, "get_StrandBindingData")),
                                 material = path_of(try(s.call, s, "get_Material")) }
            end
            table.insert(def.parts, part)
        end
    end
    table.insert(captured, def)
    results.captured = captured
    note(string.format("F9: wrote down %s: skeleton %s, motion bank %s, %d dynamic banks, %d parts", def.name,
        tostring(def.skeleton), tostring(def.motion_bank), #def.dynamic_banks, #def.parts))
end

-- ---- Keys and screen ----

-- Hides every puppet out now, for F8 to reuse (never destroyed: see build_next). F5's copies have no key: hidden only.
local function remove_puppets()
    local n = #puppets
    for _, c in ipairs(puppets) do
        pcall(set_active, c.body, false)
        if c.key then put_away[c.key] = c.body end
    end
    puppets, list, index = {}, {}, 0
    return n
end

local KEYS = { F2 = 0x71, F3 = 0x72, F4 = 0x73, F5 = 0x74, F6 = 0x75, F7 = 0x76, F8 = 0x77, F9 = 0x78, F11 = 0x7A }
local ACTIONS = { F2 = next_character, F3 = toggle_hide, F4 = replay, F5 = spawn_puppet, F6 = bring, F7 = idle,
                  F8 = build_next, F9 = capture,
                  F11 = function() note(string.format("F11: put away %d puppet(s)", remove_puppets())) end }
local down = {}
local function pressed(name)
    local is = reframework:is_key_down(KEYS[name])
    local was = down[name]
    down[name] = is
    return is and not was
end

re.on_frame(function()
    local ok, err = pcall(check_meshes)
    if not ok then watching = {}; note("watching the meshes failed: " .. tostring(err)) end
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
            "F5 build a puppet copy, F6 bring in front of Leon, F7 stand idle, F8 build from files, F9 write down, F11 remove puppets",
        string.format("selected %d of %d: %s", index, #list, tostring(now)),
        "last: " .. (results.notes[#results.notes] or "-"),
    }
    for i, line in ipairs(lines) do draw.text(line, 40, 20 + 18 * i, 0xFFFFFFFF) end
end)

re.on_script_reset(function()  -- show what we hid, put away our puppets (the next F8 finds them by name)
    for _, c in pairs(hidden) do pcall(set_drawn, c, true) end
    remove_puppets()
    hidden = {}
end)

note("loaded (run " .. RUN .. ")")
