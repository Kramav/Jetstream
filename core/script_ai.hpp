#pragma once
// Write with AI (CLAUDE.md §10 M2, plan step 3): Claude Code (`claude -p`) writes or changes a REFramework script from
// the user's words, with remod's own tools (search_game_code, check_script) and read-only access, and answers with the
// whole script; remod writes the file. The file stays plain Lua the user can edit by hand (user, 2026-10-07: "always
// have a way for human editing"): Claude reads it first and builds on the user's edits.
#include "game_code.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace remod {

// Claude Code's program: claude.exe / claude.cmd on PATH, else its native install (%USERPROFILE%\.local\bin), else the
// newest VS Code extension's own (anthropic.claude-code-*\resources\native-binary\claude.exe). Empty if none.
std::filesystem::path find_claude();

// remod.exe (the CLI, whose `mcp` gives Claude the tools) for a front end in `dir`: beside it (the release zip), else
// the build tree's ..\cli. Empty if none.
std::filesystem::path find_remod_cli(const std::filesystem::path& dir);

struct ScriptAsk {
    std::filesystem::path script;  // the .lua (it may not exist yet)
    std::string request;           // what the user wants: the whole script, or a change
    std::string game;              // e.g. "Resident Evil 4 (2023)"
    std::filesystem::path claude;  // find_claude()
    std::filesystem::path remod;   // find_remod_cli()
};

struct ScriptAnswer {
    std::string script;  // the whole new script
    std::string notes;   // what Claude said to the user (what it did, what to test in game)
    std::vector<ScriptProblem> problems;  // check_lua on the result (game names too when an SDK dump is set)
};

// Why Claude Code here would bill per use, or "" if it wouldn't: an API key (ANTHROPIC_API_KEY in the environment or in
// its settings' env), an apiKeyHelper, Bedrock or Vertex (user, 2026-10-07: "disable that if you are charging me per
// use"). On a Claude subscription a call counts toward the plan's limits instead. `settings`: Claude Code's user
// settings file (%USERPROFILE%\.claude\settings.json by default).
std::string per_use_billing(const std::filesystem::path& settings = {});

// Runs Claude Code once (minutes; up to `timeout`), only ever because the user asked (user, 2026-10-07: no automated
// calls). Refused when per_use_billing() says why. Writes nothing but its own request and MCP config files under
// %LOCALAPPDATA%\remod\ai. Throws std::runtime_error with Claude's own words when it fails or gives no script.
ScriptAnswer ask_claude(const ScriptAsk& ask, std::chrono::minutes timeout = std::chrono::minutes(15));

// A file's write time as text, "" if it's missing: taken when asking, compared when saving.
std::string file_stamp(const std::filesystem::path& file);

// Writes `text` to `script`, keeping the previous version as <script>.bak. If the file changed since `stamp` (the user
// edited it while Claude worked), the user's file is left alone and the answer goes to <stem>_claude.lua beside it.
// Returns the file written.
std::filesystem::path save_script(const std::filesystem::path& script, const std::string& text, const std::string& stamp);

// The whole script in Claude's answer (its last ```lua block, else its last ``` block) and the words before it.
// Throws if there's none.
std::pair<std::string, std::string> split_answer(const std::string& answer);

}  // namespace remod
