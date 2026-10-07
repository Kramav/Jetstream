#include "script_ai.hpp"

#include "process.hpp"
#include "settings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace remod {

namespace fs = std::filesystem;

namespace {

fs::path on_path(const wchar_t* name) {
    wchar_t found[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr);
    return n > 0 && n < MAX_PATH ? fs::path(found) : fs::path();
}

void write_text(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
    if (!out.flush()) throw std::runtime_error("couldn't write " + file.string());
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

// What Claude is told, in a file of its own: no quoting problems on a command line, whatever the user typed.
std::string request_text(const ScriptAsk& ask) {
    std::error_code ec;
    const bool exists = fs::is_regular_file(ask.script, ec);
    const fs::path modules = ask.script.parent_path() / ask.script.stem();
    std::string t = "# Write a REFramework script\n\n"
                    "You are writing a Lua script for REFramework, the mod framework, for " +
                    (ask.game.empty() ? std::string("an RE Engine game") : ask.game) +
                    ". The user tests it in game and may edit it by hand.\n\n"
                    "The script: " + u8(ask.script) + "\n";
    t += exists ? "It exists: read it first. It may hold the user's own edits: keep them unless the request says "
                  "otherwise, and change only what the request needs.\n"
                : "It doesn't exist yet: write it from scratch.\n";
    if (fs::is_directory(modules, ec))
        t += "Its modules (loaded with require) are in " + u8(modules) +
             ": read them if you need to. Only the main script can be changed.\n";
    t += "\n## What the user wants\n\n" + ask.request +
         "\n\n## How\n\n"
         "- REFramework runs every .lua directly in reframework/autorun when the game starts and when the user presses "
         "Reset Scripts in its menu. It runs Lua 5.4.\n"
         "- Its API (re, sdk, imgui, draw, json, fs, log): read the pages you need in the REFramework book, "
         "https://cursey.github.io/reframework-book/ (api/general/README.html first, then api/re.html, api/sdk.html, "
         "api/imgui.html ...), instead of guessing.\n"
         "- Every game type, method and field the script names: find it with the search_game_code tool, never from "
         "memory. If it says there's no SDK dump, the names can't be checked: say so in your note.\n"
         "- When the script is ready, check its whole text with the check_script tool and fix every problem until it "
         "reports none.\n"
         "- Write it for a person who will read and edit it: short comments on what each part does, settings at the "
         "top.\n"
         "- Options the user can change: a window drawn in re.on_draw_ui (REFramework's menu), saved with "
         "json.dump_file / json.load_file under the script's name.\n"
         "- Don't write or edit any file: remod writes the script from your answer.\n"
         "\n## Answer\n\n"
         "First a short note for the user: what the script does, what to try in game, anything you couldn't check. "
         "Then the whole script in one ```lua block, and nothing after it.\n";
    return t;
}

}  // namespace

fs::path find_claude() {
    for (const wchar_t* name : {L"claude.exe", L"claude.cmd"})
        if (fs::path p = on_path(name); !p.empty()) return p;
    std::error_code ec;
    wchar_t profile[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const fs::path home(profile);
    if (const fs::path native = home / ".local" / "bin" / "claude.exe"; fs::is_regular_file(native, ec))
        return native;
    fs::path newest;
    fs::file_time_type newest_time{};
    for (const auto& e : fs::directory_iterator(home / ".vscode" / "extensions", ec)) {
        if (!e.path().filename().string().starts_with("anthropic.claude-code-")) continue;
        const fs::path exe = e.path() / "resources" / "native-binary" / "claude.exe";
        if (const auto t = fs::last_write_time(exe, ec); !ec && (newest.empty() || t > newest_time))
            newest = exe, newest_time = t;
    }
    return newest;
}

std::string per_use_billing(const fs::path& settings) {
    for (const wchar_t* name : {L"ANTHROPIC_API_KEY", L"CLAUDE_CODE_USE_BEDROCK", L"CLAUDE_CODE_USE_VERTEX"})
        if (GetEnvironmentVariableW(name, nullptr, 0) > 0) return fs::path(name).string() + " is set";
    fs::path file = settings;
    if (file.empty()) {
        wchar_t profile[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return "";
        file = fs::path(profile) / ".claude" / "settings.json";
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return "";
    if (j.contains("apiKeyHelper")) return "an apiKeyHelper in " + file.filename().string();
    if (const auto env = j.find("env"); env != j.end() && env->is_object())
        for (const char* name : {"ANTHROPIC_API_KEY", "CLAUDE_CODE_USE_BEDROCK", "CLAUDE_CODE_USE_VERTEX"})
            if (env->contains(name)) return std::string(name) + " in " + file.filename().string();
    return "";
}

fs::path find_remod_cli(const fs::path& dir) {
    std::error_code ec;
    for (const fs::path p : {dir / "remod.exe", dir.parent_path() / "cli" / "remod.exe"})
        if (fs::is_regular_file(p, ec)) return p;
    return {};
}

std::pair<std::string, std::string> split_answer(const std::string& answer) {
    size_t open = answer.rfind("```lua");
    if (open == std::string::npos) {  // a plain fence: the one before the last ``` (its close)
        const size_t close = answer.rfind("```");
        open = close == std::string::npos || close == 0 ? std::string::npos : answer.rfind("```", close - 1);
    }
    if (open == std::string::npos) throw std::runtime_error("Claude didn't answer with a script: " + answer);
    const size_t body = answer.find('\n', open);
    const size_t end = body == std::string::npos ? std::string::npos : answer.find("```", body);
    if (end == std::string::npos) throw std::runtime_error("Claude's script was cut off: " + answer.substr(open));
    std::string notes = answer.substr(0, open);
    while (!notes.empty() && std::isspace(static_cast<unsigned char>(notes.back()))) notes.pop_back();
    return {answer.substr(body + 1, end - body - 1), notes};
}

ScriptAnswer ask_claude(const ScriptAsk& ask, std::chrono::minutes timeout) {
    if (const std::string why = per_use_billing(); !why.empty())
        throw std::runtime_error("Write with Claude is off: Claude Code here would bill per use (" + why +
                                 "). It runs only on a Claude subscription.");
    if (ask.claude.empty())
        throw std::runtime_error("Claude Code isn't installed (no claude.exe on PATH, in .local\\bin or in VS Code's "
                                 "extension): install it from https://claude.com/claude-code and sign in");
    if (ask.remod.empty()) throw std::runtime_error("remod.exe (for Claude's tools) isn't beside the app");
    if (ask.request.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::runtime_error("say what the script should do first");
    if (ask.script.empty()) throw std::runtime_error("type the script's file name first, e.g. scripts\\my_mod.lua");

    const fs::path cache = default_cache_dir();
    if (cache.empty()) throw std::runtime_error("LOCALAPPDATA isn't set");
    const fs::path dir = cache.parent_path() / "ai";
    write_text(dir / "request.md", request_text(ask));
    write_text(dir / "mcp.json",
               nlohmann::json{{"mcpServers", {{"remod", {{"command", u8(ask.remod)}, {"args", {"mcp"}}}}}}}.dump(2));

    // Read-only tools, the REFramework book, and remod's two checks; nothing that writes or runs commands. dontAsk:
    // anything else is refused, never asked (no one is there to answer).
    const std::vector<std::wstring> args{
        L"-p", L"Follow the instructions in request.md (in this folder), and answer as it says.",
        L"--output-format", L"json",
        L"--tools", L"Read,Grep,Glob,WebFetch",
        L"--allowedTools", L"Read", L"Grep", L"Glob", L"WebFetch(domain:cursey.github.io)",
        L"mcp__remod__search_game_code", L"mcp__remod__check_script",
        L"--permission-mode", L"dontAsk",
        L"--mcp-config", (dir / "mcp.json").wstring(), L"--strict-mcp-config",
        L"--add-dir", fs::absolute(ask.script).parent_path().wstring()};
    ProcessResult r;
    if (ask.claude.extension() == ".cmd") {  // npm's claude.cmd: through cmd.exe, the line as is
        std::wstring line = L"cmd.exe /d /s /c \"" + quote_arg(ask.claude.wstring());
        for (const auto& a : args) {
            if (a.find_first_of(L"&|<>^%!\"") != std::wstring::npos)
                throw std::runtime_error("can't pass this to claude.cmd safely: install Claude Code's native build");
            line += L" " + quote_arg(a);
        }
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        r = run_process_line(fs::path(sys) / L"cmd.exe", line + L"\"", timeout, false, {}, dir);
    } else {
        r = run_process(ask.claude, args, timeout, false, {}, dir);
    }

    // Its reply is one JSON line (stderr shares the pipe: take the last line that parses).
    nlohmann::json reply;
    std::istringstream lines(r.output);
    for (std::string line; std::getline(lines, line);)
        if (!line.empty() && line[0] == '{')
            if (auto j = nlohmann::json::parse(line, nullptr, false); !j.is_discarded() && j.contains("result")) reply = j;
    if (reply.is_null())
        throw std::runtime_error("Claude Code didn't answer (exit " + std::to_string(r.exit_code) + "): " +
                                 r.output.substr(0, 1500));
    const std::string result = reply.value("result", std::string());
    if (reply.value("is_error", false) || r.exit_code != 0) throw std::runtime_error("Claude Code: " + result);

    ScriptAnswer answer;
    std::tie(answer.script, answer.notes) = split_answer(result);
    const GameCode* code = nullptr;
    try {
        code = game_code();
    } catch (const std::exception&) {  // the dump's trouble is reported by the block's run
    }
    answer.problems = check_lua(answer.script, ask.script.filename().string(), code);
    return answer;
}

std::string file_stamp(const fs::path& file) {
    std::error_code ec;
    const auto t = fs::last_write_time(file, ec);
    return ec ? std::string() : std::to_string(t.time_since_epoch().count());
}

fs::path save_script(const fs::path& script, const std::string& text, const std::string& stamp) {
    std::error_code ec;
    if (file_stamp(script) != stamp) {  // edited (or made) by hand meanwhile: theirs stays
        const fs::path aside = script.parent_path() / (script.stem().string() + "_claude.lua");
        write_text(aside, text);
        return aside;
    }
    if (fs::is_regular_file(script, ec))
        fs::copy_file(script, fs::path(script) += ".bak", fs::copy_options::overwrite_existing);
    write_text(script, text);
    return script;
}

}  // namespace remod
