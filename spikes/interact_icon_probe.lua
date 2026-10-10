-- remod interact icon probe (CLAUDE.md §10 M3 "Triggers", talk prompts): can a script show the game's own
-- interact icon (the large key icon the merchant and doors show) for our talk triggers, instead of our drawn text?
-- Guide: spikes/interact_icon_probe.md. No method hooks.
--
-- The game's way [dump]: an object's chainsaw.InteractHolder holds triggers; a key trigger
-- (chainsaw.InteractTriggerKey) gets a work (chainsaw.InteractTriggerKey.WorkKey) while the player is in range, and
-- the work asks for the icon with requestDrawKeyIcon(OverrideDisable); the icon is a chainsaw.FloatIconGuiBehavior
-- (its _OpenParam: WorldPos, KeyType, IconType, TopType, Owner). The icon goes where the trigger's _ObjIconPos
-- (a Transform) is.
--   always  the float icons in the scene and the input on our work, on screen, and in the log when they change
--   F5      write down the interaction holders within 10 m of Leon (the merchant's), their key triggers and the icons
--   F6      our own icon at Ashley's head: a copy of the merchant's key trigger (MemberwiseClone) with _ObjIconPos
--           on a marker object of ours kept above her Head joint and Ashley as its owner, its work from the game's
--           generateWork, requestDrawKeyIcon(None) every frame, checkInput read back; F6 again stops it
-- Results: reframework\data\remod_interact_icon_probe.json and the log. Changes run in the game's update
-- (UpdateBehavior), each logged first (CLAUDE.md §9).

local RUN = 3
local marker_object  -- below (F6)
local TAG = "[remod interact icon probe] "

local last = json.load_file("remod_interact_icon_probe.json")
local results = { run = RUN, notes = {}, previous = last and { run = last.run, notes = last.notes } or nil }
local function note(text)
    table.insert(results.notes, string.format("%.1f s: %s", os.clock(), text))
    log.info(TAG .. text)
    json.dump_file("remod_interact_icon_probe.json", results)
end
local function try(f, ...)
    local ok, v = pcall(f, ...)
    if ok then return v end
    return nil, tostring(v)
end
local function keep(o) if o then o:add_ref_permanent() end return o end
local function type_name(o) return o and try(function() return o:get_type_definition():get_full_name() end) or "nil" end
local function enum_name(t, v)
    local td = sdk.find_type_definition(t)
    for _, f in ipairs(td and td:get_fields() or {}) do
        if f:is_static() and f:get_data(nil) == v then return f:get_name() end
    end
    return tostring(v)
end
local function enum(t, name) return sdk.find_type_definition(t):get_field(name):get_data(nil) end
local function vec(v) return v and string.format("(%.2f, %.2f, %.2f)", v.x, v.y, v.z) or "nil" end
local function go_name(go) return go and try(go.call, go, "get_Name") or "nil" end

local function scene() return sdk.find_type_definition("via.SceneManager"):get_method("get_CurrentScene"):call(nil) end
local function find_all(t)
    local s = scene()
    local arr = s and s:call("findComponents(System.Type)", sdk.typeof(t))
    return arr and arr:get_elements() or {}
end
local function position(go) return go:call("get_Transform"):call("get_Position") end
local function leon()
    local cm = sdk.get_managed_singleton("chainsaw.CharacterManager")
    local ctx = cm and cm:call("getPlayerContextRef")
    return ctx and ctx:call("get_BodyGameObject")
end
local function ashley()
    local cm = sdk.get_managed_singleton("chainsaw.CharacterManager")
    local list = cm and cm:call("get_PartnerContextList")
    for i = 0, (list and list:call("get_Count") or 0) - 1 do
        local ctx = list:call("get_Item", i)
        local kind = ctx and try(ctx.call, ctx, "get_KindID")
        if kind and enum_name("chainsaw.CharacterKindID", kind) == "ch2_a1z0" then return ctx:call("get_BodyGameObject") end
    end
end
local function dist(a, b) return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2) end

-- ---- Changes, in the game's update ----
local queue = {}
local function on_update(what, f) table.insert(queue, { what = what, f = f }) end

-- ---- The float icons now ----
local function icons()
    local out = {}
    for _, g in ipairs(find_all("chainsaw.FloatIconGuiBehavior")) do  -- a pool of ~20 (run 1); idle ones left out
        local p = try(g.get_field, g, "_OpenParam")
        local owner = p and try(p.get_field, p, "Owner")
        local step = enum_name("chainsaw.FloatIconGuiBehavior.Step", try(g.call, g, "get_CurrStep"))
        if owner or step ~= "Invalid" then table.insert(out, string.format("%s step %s key %s icon %s top %s text %s; param pos %s owner %s",
            go_name(try(g.call, g, "get_GameObject")),
            enum_name("chainsaw.FloatIconGuiBehavior.Step", try(g.call, g, "get_CurrStep")),
            enum_name("chainsaw.GimmickKeyType", try(g.call, g, "get_CurrKeyType")),
            enum_name("chainsaw.gui.FloatIconType", try(g.call, g, "get_CurrIconType")),
            enum_name("chainsaw.gui.FloatIconTopType", try(g.call, g, "get_CurrTopType")),
            tostring(try(g.call, g, "get_LastIconTextMessage")),
            vec(p and try(p.get_field, p, "WorldPos")), go_name(owner))) end
    end
    return out
end

-- ---- F5: what's near ----
local function key_triggers(holder)
    local out = {}
    for _, t in ipairs((try(holder.get_field, holder, "_TrgAct") or { get_elements = function() return {} end }):get_elements()) do
        if type_name(t) == "chainsaw.InteractTriggerKey" then table.insert(out, t) end
    end
    return out
end
local function describe_key(t)
    local function f(n) return try(t.get_field, t, n) end
    return string.format("'%s' type %s input %s default %s icon top %s range %s icon pos object '%s' (%s) npc pos '%s' "
        .. "priority %s display %s hide %s near NPC %s owner %s",
        tostring(f("UniqueName")), enum_name("chainsaw.InteractTrigger.TriggerType", f("<Type>k__BackingField")),
        tostring(f("_InputKey")), tostring(f("_DefaultInputKey")),
        enum_name("chainsaw.gui.FloatIconTopType", f("<IconTop>k__BackingField")), tostring(f("RangeSize")),
        tostring(f("ObjectNameIconPos")), go_name(f("_ObjIconPos") and try(f("_ObjIconPos").call, f("_ObjIconPos"), "get_GameObject")),
        tostring(f("ObjectNameNpcPos")), tostring(f("<KeyPriority>k__BackingField")),
        tostring(f("<DisplayPriority>k__BackingField")), tostring(f("<HideDisplay>k__BackingField")),
        tostring(f("<IsConditionNearNPC>k__BackingField")), go_name(try(t.call, t, "get_Owner")))
end
local function write_down()
    local me = leon()
    if not me then return note("F5: no Leon (load a save first)") end
    local here = position(me)
    local n = 0
    for _, h in ipairs(find_all("chainsaw.InteractHolder")) do
        local go = try(h.call, h, "get_GameObject")
        local d = go and try(function() return dist(position(go), here) end)
        if d and d <= 10 then
            n = n + 1
            local keys = key_triggers(h)
            note(string.format("F5: holder on '%s' %.1f m: %d key trigger(s)", go_name(go), d, #keys))
            for i, t in ipairs(keys) do note("F5:   key " .. i .. ": " .. describe_key(t)) end
        end
    end
    note("F5: " .. n .. " holder(s) within 10 m; float icons: " .. table.concat(icons(), " | "))
    local merchants = find_all("chainsaw.GmWeaponMerchant")
    for _, m in ipairs(merchants) do
        note("F5: merchant gimmick on '" .. go_name(try(m.call, m, "get_GameObject")) .. "', body '"
            .. go_name(try(m.get_field, m, "NpcBodyObj")) .. "'")
    end
end

-- ---- F6: our icon at Ashley ----
local ours = { on = false, work = nil, trigger = nil, input = nil, err = nil }
local MARKER_UP = 0.3  -- metres above her Head joint
local marker = nil
-- One marker for good (never destroyed: CLAUDE.md §9), found again by name after Reset Scripts.
function marker_object()
    if marker then return marker end
    local s = scene()
    marker = s and try(s.call, s, "findGameObject(System.String)", sdk.create_managed_string("remod_icon_marker"))
    if not marker then
        marker = sdk.find_type_definition("via.GameObject"):get_method("create(System.String)")
            :call(nil, sdk.create_managed_string("remod_icon_marker"))
        note("F6: marker object made")
    end
    return keep(marker)
end
local function place_marker()  -- each frame, in the game's update
    local a = ours.ashley
    local joint = a and try(function() return a:call("get_Transform"):call("getJointByName", "Head") end)
    local p = joint and joint:call("get_Position")
    if not p then
        if not ours.no_joint then ours.no_joint = true; note("no Head joint on Ashley: the marker stays put") end
        return
    end
    marker:call("get_Transform"):call("set_Position", Vector3f.new(p.x, p.y + MARKER_UP, p.z))
end
local function merchant_key()
    local merchants = find_all("chainsaw.GmWeaponMerchant")
    local mgo = merchants[1] and merchants[1]:call("get_GameObject")
    if not mgo then return nil, "no merchant in the scene" end
    local best, best_d = nil, 1e9
    for _, h in ipairs(find_all("chainsaw.InteractHolder")) do
        local go = try(h.call, h, "get_GameObject")
        local d = go and try(function() return dist(position(go), position(mgo)) end)
        if d and d < best_d and #key_triggers(h) > 0 then best, best_d = h, d end
    end
    if not best or best_d > 5 then return nil, "no holder with a key trigger within 5 m of the merchant" end
    return key_triggers(best)[1], string.format("holder on '%s' %.1f m from the merchant", go_name(best:call("get_GameObject")), best_d)
end
local function start_ours()
    local key, where = merchant_key()
    if not key then return note("F6: " .. where) end
    note("F6: copying the merchant's key trigger (" .. where .. "): " .. describe_key(key))
    local a = ashley()
    if not a then return note("F6: no Ashley in the partner list") end
    -- Run 2: on her "head" part the icon showed at her feet (her parts sit at her origin; the skeleton moves the
    -- mesh). Run 3: a marker object of ours, moved each frame to her Head joint plus MARKER_UP (below).
    local marker = marker_object()
    local copy = keep(key:call("MemberwiseClone"))
    if not copy then return note("F6: MemberwiseClone gave nil") end
    copy:set_field("_ObjIconPos", marker:call("get_Transform"))
    -- Ashley as the owner, not the merchant (who may not be loaded where a talk trigger is).
    local ook, oerr = pcall(copy.call, copy, "set_Owner", a)
    note("F6: copy made, icon on our marker, owner -> " .. (ook and go_name(try(copy.call, copy, "get_Owner")) or ("failed: " .. tostring(oerr))))
    ours.ashley = a
    -- Run 1 made a WorkKey with create_instance and setup(copy): TriggerKey stayed nil and checkInput threw every frame
    -- (caught; no icon). Run 2: the game's own maker, InteractTriggerActivated.generateWork(TargetType, WorkIndex),
    -- then applyTarget(Leon, a sensor userdata of our own) as the game's sensors do.
    local me = leon()
    if not me then return note("F6: no Leon") end
    local work = keep(copy:call("generateWork", enum("chainsaw.InteractTrigger.TargetType", "Pl00"),
                                enum("chainsaw.InteractManager.WorkIndex", "Pl1")))
    if not work then return note("F6: generateWork gave nil") end
    note("F6: generateWork gave " .. type_name(work) .. "; TriggerKey " .. type_name(try(work.call, work, "get_TriggerKey"))
        .. ", Trigger " .. type_name(try(work.call, work, "get_Trigger")) .. ", TriggerAct "
        .. type_name(try(work.call, work, "get_TriggerAct")) .. ", open param "
        .. type_name(try(work.get_field, work, "_FloatIconOpenParam")))
    local ud = keep(sdk.create_instance("chainsaw.collision.GimmickSensorUserData", true))
    if ud then pcall(ud.call, ud, "set_Kind", enum("chainsaw.collision.GimmickSensorUserData.KindType", "Interact")) end
    local aok, aerr = pcall(work.call, work, "applyTarget", me, ud)
    note("F6: applyTarget(Leon, our userdata " .. type_name(ud) .. "): " .. (aok and "done" or tostring(aerr))
        .. "; TargetObj " .. go_name(try(work.call, work, "get_TargetObj")) .. ", Status "
        .. enum_name("chainsaw.InteractTriggerActivated.WorkAct.StatusType", try(work.call, work, "get_Status")))
    -- A null handed to the game's code crashes it or throws every frame (run 1): only ask once it's all linked.
    if type_name(work) ~= "chainsaw.InteractTriggerKey.WorkKey" or not try(work.call, work, "get_TriggerKey")
        or not try(work.get_field, work, "_FloatIconOpenParam") then
        return note("F6: the work isn't a linked WorkKey: stopped before asking the game")
    end
    ours.work, ours.trigger, ours.on, ours.err, ours.input = work, copy, true, nil, nil
end

re.on_pre_application_entry("UpdateBehavior", function()
    if #queue > 0 then
        local work = queue
        queue = {}
        for _, w in ipairs(work) do
            log.info(TAG .. w.what)  -- first, in case the game goes down
            local ok, err = pcall(w.f)
            if not ok then note(w.what .. " failed: " .. tostring(err)) end
        end
    end
    if ours.on and ours.work then  -- the request, every frame (the game's works ask each frame they're in range)
        pcall(place_marker)
        local ok, err = pcall(ours.work.call, ours.work, "requestDrawKeyIcon", enum("chainsaw.InteractTriggerKey.OverrideDisable", "None"))
        local iok, pressed = true, nil
        if ok then iok, pressed = pcall(ours.work.call, ours.work, "checkInput") end
        if not ok or not iok then  -- stop at the first error (run 1 threw every frame)
            ours.on, ours.err = false, tostring(ok and pressed or err)
            note((ok and "checkInput" or "requestDrawKeyIcon") .. " failed, stopped: " .. ours.err)
        elseif tostring(pressed) ~= ours.input then
            ours.input = tostring(pressed)
            if pressed == true then ours.pressed_at, ours.presses = os.clock(), (ours.presses or 0) + 1 end
            note("our work's checkInput: " .. ours.input)
        end
    end
end)

-- ---- Keys and the read-back ----
local down = {}
local function pressed(vk)
    local is = reframework:is_key_down(vk)
    local was = down[vk]
    down[vk] = is
    return is and not was
end
local shown, shown_at = {}, 0
re.on_frame(function()
    if pressed(0x74) then on_update("F5: writing down what's near", write_down) end
    if pressed(0x75) then
        if ours.on then
            ours.on = false
            note("F6: our icon stopped (no more requests)")
        else
            on_update("F6: our icon at Ashley", start_ours)
        end
    end
    if os.clock() - shown_at > 0.5 then
        shown_at = os.clock()
        shown = icons()
        local line = table.concat(shown, " | ")
        if line ~= results.icons_line then results.icons_line = line; note("float icons: " .. (line ~= "" and line or "none")) end
    end
    local y = 200
    draw.text(TAG .. "run " .. RUN .. ": F5 write down, F6 our icon at Ashley (" .. (ours.on and "ON" or "off") .. ")", 20, y, 0xFFFFFFFF)
    for _, s in ipairs(shown) do y = y + 18; draw.text(s, 20, y, 0xFFFFFFFF) end
    if ours.on then y = y + 18; draw.text("our work's checkInput: " .. tostring(ours.input) .. (ours.err and ("; request: " .. ours.err) or ""), 20, y, 0xFF80FFFF) end
    if ours.pressed_at and os.clock() - ours.pressed_at < 1 then  -- what a talk trigger would start on
        y = y + 24; draw.text("INTERACT PRESSED on our icon (" .. ours.presses .. ")", 20, y, 0xFF40FF40)
    end
end)

note("loaded (run " .. RUN .. ")")
