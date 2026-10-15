-- remod full check: a script with a mistake on purpose, so Game errors and Fix with Claude have a real error to read.
-- Two seconds after it starts it indexes nil, once.
local frames = 0

re.on_frame(function()
    frames = frames + 1
    if frames == 120 then
        local settings = nil
        print(settings.volume)  -- the mistake: settings is nil
    end
end)

re.on_draw_ui(function()
    imgui.text("remod full check script: loaded, " .. frames .. " frames")
end)
