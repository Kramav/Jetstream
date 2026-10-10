-- remod shop menu probe (CLAUDE.md §10 M3 "Triggers", a new entry in the merchant's menu): read-only. What is the
-- merchant's menu made of, so a talk-topics entry can be added? Guide: spikes/shop_menu_probe.md. No method hooks,
-- and nothing is changed.
--
-- The game's names [dump]: chainsaw.InGameShopManager (AppSingleton) FlowController (chainsaw.InGameShopFlowController:
-- get_CurrPhase, get_CurrStateType -> chainsaw.gui.shop.InGameShopGuiState, close()). The first screen after Interact
-- is chainsaw.InGameShopSelectGuiBehavior: _SelectList (via.gui.SimpleList, items via.gui.SelectItem, SelectedIndex),
-- OnDecided(InGameShopMenuKind: Purchase, Sell, Custom, Reward). Inside, the tab strip chainsaw.gui.shop.TabListGui
-- (_TabElements, TabElementNum, CurrFocusType: InGameShopWindowType). A label is a via.gui.Text (Message / MessageId).
--   always  the shop's phase and state, the select menu's step / selected index / item count, the tab strip's focus:
--           on screen, and in the log when they change
--   F5      write down the select menu's and the tab strip's GUI trees (every element: type, name, visible, text, play
--           state) and the tab elements, into reframework\data\remod_shop_probe.json
-- Results: reframework\data\remod_shop_probe.json and the log.

local RUN = 1
local TAG = "[remod shop probe] "

local last = json.load_file("remod_shop_probe.json")
local results = { run = RUN, notes = {}, trees = {}, previous = last and { run = last.run, notes = last.notes } or nil }
local function save() json.dump_file("remod_shop_probe.json", results) end
local function note(text)
    table.insert(results.notes, string.format("%.1f s: %s", os.clock(), text))
    log.info(TAG .. text)
    save()
end
local function try(f, ...)
    local ok, v = pcall(f, ...)
    if ok then return v end
    return nil
end
local enum_cache = {}
local function enum_name(t, v)
    if v == nil then return "nil" end
    local names = enum_cache[t]
    if not names then
        names = {}
        local td = sdk.find_type_definition(t)
        for _, f in ipairs(td and td:get_fields() or {}) do
            if f:is_static() then names[f:get_data(nil)] = f:get_name() end
        end
        enum_cache[t] = names
    end
    return names[v] or tostring(v)
end
local function instance(t)
    local td = sdk.find_type_definition(t)
    local m = td and td:get_method("get_Instance")
    return m and try(m.call, m, nil)
end
local function find_all(t)
    local s = sdk.find_type_definition("via.SceneManager"):get_method("get_CurrentScene"):call(nil)
    local arr = s and try(s.call, s, "findComponents(System.Type)", sdk.typeof(t))
    return arr and arr:get_elements() or {}
end
local function type_name(o) return o and try(function() return o:get_type_definition():get_full_name() end) or "nil" end

-- ---- The state, read every 0.25 s ----
local function state_line()
    local parts = {}
    local mgr = instance("chainsaw.InGameShopManager")
    local flow = mgr and try(mgr.call, mgr, "get_FlowController")
    table.insert(parts, "shop phase " .. enum_name("chainsaw.InGameShopManager.Phase", mgr and try(mgr.call, mgr, "get_CurrPhase")))
    table.insert(parts, "flow phase " .. enum_name("chainsaw.InGameShopFlowController.Phase", flow and try(flow.call, flow, "get_CurrPhase")))
    table.insert(parts, "state " .. enum_name("chainsaw.gui.shop.InGameShopGuiState", flow and try(flow.call, flow, "get_CurrStateType")))
    for i, sel in ipairs(find_all("chainsaw.InGameShopSelectGuiBehavior")) do
        local list = try(sel.get_field, sel, "_SelectList")
        local items = list and try(list.call, list, "get_Items")
        table.insert(parts, string.format("select %d: step %s, selected %s of %s", i,
            enum_name("chainsaw.InGameShopSelectGuiBehavior.Step", try(sel.call, sel, "get_CurrStep")),
            tostring(list and try(list.call, list, "get_SelectedIndex")), tostring(items and #items:get_elements())))
    end
    table.insert(parts, "shop windows " .. #find_all("chainsaw.InGameShopGuiBehavior"))
    return table.concat(parts, "; ")
end

-- ---- F5: GUI trees ----
local function text_of(o)
    if type_name(o) ~= "via.gui.Text" then return nil end
    return tostring(try(o.call, o, "get_Message")) .. " [msg " .. tostring(try(function() return o:call("get_MessageId"):call("ToString") end)) .. "]"
end
local function walk(o, depth, out)
    if not o or depth > 12 or #out > 600 then return end
    table.insert(out, string.format("%s%s '%s' visible %s%s%s", string.rep("  ", depth), type_name(o),
        tostring(try(o.call, o, "get_Name")), tostring(try(o.call, o, "get_Visible")),
        (function() local t = text_of(o) return t and (" text: " .. t) or "" end)(),
        (function() local s = try(o.call, o, "get_PlayState") return s and s ~= "" and (" state " .. s) or "" end)()))
    local kids = try(o.call, o, "getChildren(System.Type)", sdk.typeof("via.gui.PlayObject"))
    for _, k in ipairs(kids and kids:get_elements() or {}) do walk(k, depth + 1, out) end
end
local function gui_tree(behaviour, label)
    local go = try(behaviour.call, behaviour, "get_GameObject")
    local gui = go and try(go.call, go, "getComponent(System.Type)", sdk.typeof("via.gui.GUI"))
    local view = gui and try(gui.call, gui, "get_View")
    local out = {}
    walk(view, 0, out)
    results.trees[label] = out
    note(string.format("F5: %s on '%s': %d GUI elements", label, tostring(go and try(go.call, go, "get_Name")), #out))
end
local function write_down()
    for i, sel in ipairs(find_all("chainsaw.InGameShopSelectGuiBehavior")) do
        gui_tree(sel, "select menu " .. i)
        local list = try(sel.get_field, sel, "_SelectList")
        local items = list and try(list.call, list, "get_Items")
        for j, it in ipairs(items and items:get_elements() or {}) do
            note(string.format("F5: select %d item %d '%s' visible %s enabled %s selected %s", i, j,
                tostring(try(it.call, it, "get_Name")), tostring(try(it.call, it, "get_Visible")),
                tostring(try(it.call, it, "get_Enabled")), tostring(try(it.call, it, "get_Selected"))))
        end
    end
    -- The shop window (chainsaw.InGameShopGuiBehavior [dump]); its tab strip (TabListGui) sits in a TabGui's
    -- _TabListGui, inside the window's GUI tree.
    local n = 0
    for _, b in ipairs(find_all("chainsaw.InGameShopGuiBehavior")) do
        n = n + 1
        gui_tree(b, "shop window " .. n)
    end
    if n == 0 then note("F5: no shop window now (open Buy, Sell or another tab first)") end
    save()
end

-- ---- Each frame ----
local down = {}
local function pressed(vk)
    local is = reframework:is_key_down(vk)
    local was = down[vk]
    down[vk] = is
    return is and not was
end
local line, line_at = "", 0
re.on_frame(function()
    if pressed(0x74) then pcall(write_down) end
    if os.clock() - line_at > 0.25 then
        line_at = os.clock()
        local ok, l = pcall(state_line)
        l = ok and l or ("error: " .. tostring(l))
        if l ~= line then line = l; note(l) end
    end
    draw.text(TAG .. "run " .. RUN .. " (read-only): F5 write down the menu", 20, 200, 0xFFFFFFFF)
    draw.text(line, 20, 218, 0xFFFFFFFF)
end)

note("loaded (run " .. RUN .. ")")
