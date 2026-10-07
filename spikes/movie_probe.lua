-- remod movie probe: a spike for CLAUDE.md §10 M3, route 2 (play a movie when we choose). Read-only: it looks at
-- the game's movie types and writes what it finds to a text file; it changes nothing in the game.
-- How to run it: spikes/movie_probe.md.
--
-- It writes reframework\data\remod_movie_probe.txt as soon as it loads (the movie types' methods and fields). With
-- WATCH_CALLS it also adds each of their methods the first time the game calls one, e.g. while a movie plays. Watching
-- hooks every method of those types: off by default, since on 2026-10-07 the game crashed at New Game with it on (with
-- REFramework v1.5.9 of March 2025, whose integrity-check bypass failed on the April 2026 game; see movie_probe.md).
--
-- The type names come from the game's exe (re4.exe names them); nothing here guesses how they work.
-- Only REFramework's documented API is used: sdk.find_type_definition, RETypeDefinition, REMethodDefinition,
-- REField, sdk.hook, fs.write.

local WATCH_CALLS = false

local TYPES = {
    "via.movie.Movie",
    "via.movie.MovieManager",
    "via.movie.MovieResource",
    "via.movie.MovieResourceHolder",
    "via.movie.MovieEntry",
    "via.movie.MovieContext",
    "chainsaw.FullScreenMovieGui",
    "chainsaw.FullScreenMovieGui.OpenParam",
    "chainsaw.FullScreenMovieGui.CloseParam",
}

local lines = {}
local function add(line) lines[#lines + 1] = line end
local function save() fs.write("remod_movie_probe.txt", table.concat(lines, "\n") .. "\n") end
local function name_of(t)
    if not t then return "?" end
    local ok, name = pcall(function() return t:get_full_name() end)
    return ok and name or "?"
end

add("remod movie probe")
for _, type_name in ipairs(TYPES) do
    local t = sdk.find_type_definition(type_name)
    if not t then
        add("MISSING " .. type_name)
    else
        add("TYPE " .. name_of(t) .. "  (parent: " .. name_of(t:get_parent_type()) .. ")")
        for _, m in ipairs(t:get_methods()) do
            local ok, params = pcall(function() return table.concat(m:get_param_names(), ", ") end)
            add(string.format("  method %s%s(%s) -> %s", m:is_static() and "static " or "", m:get_name(),
                ok and params or "?", name_of(m:get_return_type())))
        end
        for _, f in ipairs(t:get_fields()) do
            add(string.format("  field %s%s : %s", f:is_static() and "static " or "", f:get_name(), name_of(f:get_type())))
        end
    end
end
save()

if WATCH_CALLS then
    add("CALLS (each method once, in the order the game first called it):")
    save()
    local seen = {}
    for _, type_name in ipairs(TYPES) do
        local t = sdk.find_type_definition(type_name)
        if t then
            for _, m in ipairs(t:get_methods()) do
                local key = type_name .. "." .. m:get_name()
                pcall(sdk.hook, m, function(args)
                    if not seen[key] then
                        seen[key] = true
                        add("  CALLED " .. key)
                        save()
                    end
                end, function(retval) return retval end)
            end
        end
    end
end
