-- remod merchant menu test (CLAUDE.md §10 M3 "Triggers", new entries in the merchant's menu). Goes with an edited
-- copy of the menu's layout (cs_ui3510.gui, made by spikes/gui_add_menu_item: entries si_menu_4 and si_menu_5 sharing
-- the other entries' children). Guide: spikes/merchant_menu_test.md. No method hooks.
-- Run 1 (2026-10-10): the 5th entry showed, the cursor reached it, our label took; confirming it did nothing (the game
-- ignores an entry it doesn't know), no crash; its highlight was larger than the others' (a long label?).
-- Run 2 (2026-10-10): both entries showed and took labels; F5 hid / showed the 6th; no confirm was seen (isTrigger
-- Decide read before UpdateBehavior never came true; the user clicked); the highlight on ours was still large,
-- stretching across the screen (the game sizes each entry's parts in code, for its four only).
-- Run 3: our entries copy the 4th entry's parts' sizes and positions each frame (the differences logged once a
-- menu); confirm is looked for in several ways after the update (logged by which fired first).
-- Run 2 as built:
--   always  entries 5 and 6 labelled "Talk" and "Ask about Ashley" (in the game's update); the menu's entry count,
--           selection and shop state on screen and in the log when they change
--   F5      hide / show the 6th entry (topics will show or hide by flags: does the game skip a hidden one?)
--   confirm on entry 5 or 6 (the game's own Decide: GuiInputManager.isTrigger): logged, and the shop closed with the
--           game's own close (FlowController.close), where remod will start that topic's cutscene
-- Results: the log ("[remod merchant menu test]" lines).

local RUN = 3
local TAG = "[remod merchant menu test] "
local LABELS = { [5] = "Talk", [6] = "Ask about Ashley" }

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
local function enum(t, name) return sdk.find_type_definition(t):get_field(name):get_data(nil) end
local function singleton(t) return sdk.find_type_definition(t):get_method("get_Instance"):call(nil) end
local function select_menu()
    local s = sdk.find_type_definition("via.SceneManager"):get_method("get_CurrentScene"):call(nil)
    local arr = s and try(s.call, s, "findComponents(System.Type)", sdk.typeof("chainsaw.InGameShopSelectGuiBehavior"))
    return arr and arr:get_elements()[1]
end
local function flow()
    local m = singleton("chainsaw.InGameShopManager")
    return m and try(m.call, m, "get_FlowController")
end
local function child_named(o, name)  -- depth-first through the GUI tree
    local kids = try(o.call, o, "getChildren(System.Type)", sdk.typeof("via.gui.PlayObject"))
    for _, k in ipairs(kids and kids:get_elements() or {}) do
        if try(k.call, k, "get_Name") == name then return k end
        local found = child_named(k, name)
        if found then return found end
    end
end

-- Every element under an item by its path of names ("cursor_center/cursor_flash"), with the item itself as "".
local function parts(item)
    local out = {}
    local function walk(o, path)
        out[path] = o
        local kids = try(o.call, o, "getChildren(System.Type)", sdk.typeof("via.gui.PlayObject"))
        for _, k in ipairs(kids and kids:get_elements() or {}) do
            walk(k, (path == "" and "" or path .. "/") .. tostring(try(k.call, k, "get_Name")))
        end
    end
    walk(item, "")
    return out
end
local function fmt(v)
    if v == nil then return "nil" end
    local ok, s = pcall(function() return string.format("(%.1f, %.1f, %.1f)", v.x, v.y, v.z) end)
    if ok then return s end
    ok, s = pcall(function() return string.format("<%.1f x %.1f>", v.w, v.h) end)
    return ok and s or tostring(v)
end
-- Our entry looks like the game's 4th: Size (Rect, Scale9Grid), Position and Scale (any transform) of each part copied
-- (labels and the item's own position excluded). The first time a menu opens, what differed is logged.
local function match_parts(model, ours, log_diff)
    local a, b = parts(model), parts(ours)
    local diffs = {}
    for path, src in pairs(a) do
        local dst = b[path]
        if dst and path ~= "" and not path:match("m_list$") then
            for _, prop in ipairs({ "Size", "Position", "Scale" }) do
                local v = try(src.call, src, "get_" .. prop)
                if v ~= nil then
                    local w = try(dst.call, dst, "get_" .. prop)
                    if fmt(v) ~= fmt(w) then
                        if log_diff then table.insert(diffs, path .. "." .. prop .. " " .. fmt(w) .. " -> " .. fmt(v)) end
                        pcall(dst.call, dst, "set_" .. prop, v)
                    end
                end
            end
        end
    end
    return diffs
end

local seen = { line = nil, labelled = {}, hide6 = false, toggle = false, closing = false, diffs_logged = false,
               confirm = nil }
local function read()
    local sel = select_menu()
    local list = sel and try(sel.get_field, sel, "_SelectList")
    local items = list and try(list.call, list, "get_Items")
    local f = flow()
    return { list = list, items = items and items:get_elements() or {}, flow = f,
             index = list and try(list.call, list, "get_SelectedIndex"),
             state = enum_name("chainsaw.gui.shop.InGameShopGuiState", f and try(f.call, f, "get_CurrStateType")),
             phase = enum_name("chainsaw.InGameShopFlowController.Phase", f and try(f.call, f, "get_CurrPhase")) }
end

re.on_pre_application_entry("UpdateBehavior", function()  -- changes to the game's GUI and shop: here
    local ok, err = pcall(function()
        local r = read()
        if #r.items < 6 then seen.labelled, seen.closing, seen.diffs_logged = {}, false, false return end
        for n = 5, 6 do
            local diffs = match_parts(r.items[4], r.items[n], not seen.diffs_logged)
            if not seen.diffs_logged and #diffs > 0 then
                log.info(TAG .. "entry " .. n .. " differed from the 4th: " .. table.concat(diffs, "; "))
            end
        end
        if not seen.diffs_logged then seen.diffs_logged = true; log.info(TAG .. "entries 5 and 6 matched to the 4th") end
        for n, label in pairs(LABELS) do
            local text = child_named(r.items[n], "m_list")
            if text and try(text.call, text, "get_Message") ~= label then
                text:call("set_Message", label)
                if not seen.labelled[n] then seen.labelled[n] = true; log.info(TAG .. "entry " .. n .. " labelled '" .. label .. "'") end
            end
        end
        if seen.toggle then
            seen.toggle, seen.hide6 = false, not seen.hide6
            r.items[6]:call("set_Visible", not seen.hide6)
            log.info(TAG .. "F5: 6th entry " .. (seen.hide6 and "hidden" or "shown") .. "; read back visible "
                .. tostring(try(r.items[6].call, r.items[6], "get_Visible")))
        end
        -- Confirm on one of ours (seen after the update, in on_frame): the game does nothing with it (run 1), so we act.
        if seen.confirm and not seen.closing and r.state == "Select_Default" and (r.index == 4 or r.index == 5) then
            seen.closing = true
            log.info(TAG .. "confirm (" .. seen.confirm .. ") on entry " .. (r.index + 1) .. " ('" .. LABELS[r.index + 1]
                .. "'): closing the shop")
            r.flow:call("close")
            log.info(TAG .. "close() done; state now " .. enum_name("chainsaw.gui.shop.InGameShopGuiState",
                try(r.flow.call, r.flow, "get_CurrStateType")))
        end
        seen.confirm = nil
    end)
    if not ok and seen.err ~= tostring(err) then seen.err = tostring(err); log.info(TAG .. "failed: " .. seen.err) end
end)

local down = {}
local function pressed(vk)
    local is = reframework:is_key_down(vk)
    local was = down[vk]
    down[vk] = is
    return is and not was
end
local line = ""
re.on_frame(function()
    if pressed(0x74) then seen.toggle = true end
    -- Confirm, several ways (run 2: isTrigger read before UpdateBehavior never came true); the first that fires names it.
    local gim = try(function() return sdk.find_type_definition("chainsaw.GuiInputManager"):get_method("get_Instance"):call(nil) end)
    local decide = gim and enum("chainsaw.GuiCommandType", "Decide")
    local trig = gim and try(gim.call, gim, "isTrigger(chainsaw.GuiCommandType, System.Boolean)", decide, false)
    local held = gim and try(gim.call, gim, "isDown(chainsaw.GuiCommandType)", decide)
    local held_edge = held and not seen.held
    seen.held = held
    local mouse, enter, space = pressed(0x01), pressed(0x0D), pressed(0x20)
    local which = (trig and "game confirm") or (held_edge and "game confirm held") or (mouse and "mouse click")
        or (enter and "Enter") or (space and "Space")
    if which and seen.menu_open then  -- only while our entries show (a click in play is a shot)
        seen.confirm = which
        log.info(TAG .. "confirm seen: " .. which .. " (game confirm " .. tostring(trig) .. ", held " .. tostring(held) .. ")")
    end
    local ok, r = pcall(read)
    seen.menu_open = ok and #r.items >= 6
    if ok then
        line = string.format("menu entries %d, selected %s, shop state %s, flow %s", #r.items, tostring(r.index), r.state, r.phase)
        if line ~= seen.line then seen.line = line; log.info(TAG .. line) end
    else
        line = "error: " .. tostring(r)
    end
    draw.text(TAG .. "run " .. RUN .. ": F5 hide / show the 6th entry", 20, 200, 0xFFFFFFFF)
    draw.text(line, 20, 218, 0xFFFFFFFF)
end)

log.info(TAG .. "loaded (run " .. RUN .. ")")
