-- remod HUD and freeze probe (CLAUDE.md §10 M3 route 3): how a cutscene hides the HUD and stops the player, from the
-- game's own switches found in the SDK dump. Guide: spikes/hud_freeze_probe.md. No method hooks.
--
-- Runs 1-5 (2026-10-07): HUD option 0 hides the HUD; nothing so far stops Leon while a direction is held (head
-- updater off, EnableOperation in his behaviour tree, the input command groups, forcing his idle). Run 6: the game's
-- own operation stop, which the EnableOperation flag follows: CharacterManager.requestOperationStop(Player_1, layer),
-- as the game does for the inventory, map, files, tutorial and crafting. Sent every frame while on.
--   F5  operation stop on / off
--   F6  next pause layer (Self, Inventory, Tutorial, File, Map, Craft)
--   F8  restart Leon's current animation (changeMotion): does it play while stopped?
-- The text at the top left shows the layer and what the game reads back each frame.
--
-- What it did goes to reframework\data\remod_hud_freeze_probe.json and the log. Script reset turns it off.

local results = { notes = {} }
local function note(text)
    table.insert(results.notes, text)
    log.info("[remod probe] " .. text)
    json.dump_file("remod_hud_freeze_probe.json", results)
end

local function enum(type_name, name)
    return sdk.find_type_definition(type_name):get_field(name):get_data(nil)
end
local PLAYER_1 = enum("chainsaw.CharacterControlIndex", "Player_1")
local LAYERS = { "Self", "Inventory", "Tutorial", "File", "Map", "Craft" }
local layer_index = 1
local stopping = false
local stop_error = nil

local function manager() return sdk.get_managed_singleton("chainsaw.CharacterManager") end
local function player_context()
    local mgr = manager()
    return mgr and mgr:call("getPlayerContextRef")
end

local function request_stop()
    local mgr = manager()
    if not mgr then error("no CharacterManager") end
    mgr:call("requestOperationStop", PLAYER_1, enum("chainsaw.character.PauseLayer", LAYERS[layer_index]))
end

-- Sent before the game's behaviour update each frame, while on.
re.on_pre_application_entry("UpdateBehavior", function()
    if not stopping then return end
    local ok, err = pcall(request_stop)
    if not ok and stop_error ~= tostring(err) then
        stop_error = tostring(err)
        note("requestOperationStop failed: " .. stop_error)
    end
end)

local function read_back()
    local ctx = player_context()
    if not ctx then return "no player" end
    local head = ctx:call("get_HeadUpdater")
    local parts = {}
    for _, getter in ipairs({ "get_OperationEnable", "get_OperationStopState" }) do
        local ok, v = pcall(ctx.call, ctx, getter)
        table.insert(parts, getter:sub(5) .. " " .. (ok and tostring(v) or "?"))
    end
    local ok, v = pcall(head.call, head, "get_EnableOperation")
    table.insert(parts, "EnableOperation " .. (ok and tostring(v) or "?"))
    return table.concat(parts, ", ")
end

local function replay()
    local ctx = player_context()
    local body = ctx and ctx:call("get_BodyGameObject")
    local motion = body and body:call("getComponent(System.Type)", sdk.typeof("via.motion.Motion"))
    local layer = motion and motion:call("getLayer", 0)
    if not layer then return note("F8: no player motion layer") end
    local ok, err = pcall(layer.call, layer,
        "changeMotion(System.UInt32, System.UInt32, System.Single, System.Single, via.motion.InterpolationMode, via.motion.InterpolationCurve)",
        layer:call("get_MotionBankID"), layer:call("get_MotionID"), 0.0, 10.0, 1, 0)
    note("F8: changeMotion " .. (ok and "called" or ("failed: " .. tostring(err))) .. (stopping and " (stopped)" or ""))
end

local KEYS = { F5 = 0x74, F6 = 0x75, F8 = 0x77 }
local down = {}
local function pressed(name)
    local is = reframework:is_key_down(KEYS[name])
    local was = down[name]
    down[name] = is
    return is and not was
end

re.on_frame(function()
    if pressed("F5") then
        stopping = not stopping
        note("F5: operation stop " .. (stopping and "on" or "off") .. " (layer " .. LAYERS[layer_index] .. "); reads " .. read_back())
    end
    if pressed("F6") then
        layer_index = layer_index % #LAYERS + 1
        note("F6: layer " .. LAYERS[layer_index])
    end
    if pressed("F8") then replay() end
    draw.text("remod probe: F5 operation stop (" .. (stopping and "ON" or "off") .. "), F6 layer (" .. LAYERS[layer_index] ..
        "), F8 replay animation. Game reads: " .. read_back(), 40, 40, 0xFFFFFFFF)
end)

re.on_script_reset(function() stopping = false end)
note("loaded (run 6: operation stop)")
