#pragma once
// REFramework scripts (CLAUDE.md §10 M2, plan step 2): the game's code as REFramework's SDK dump describes it, and a
// script checked against it, so names an AI (or a person) writes are real before the game runs.
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace remod {

struct GameField {
    std::string name, type;
    bool is_static = false;
};

struct GameMethod {
    std::string name, returns;
    std::vector<std::pair<std::string, std::string>> params;  // {type, name}
    bool is_static = false;
    std::string prototype() const;  // "name(T1, T2)": what REFramework's get_method also matches [official]
};

struct GameType {
    std::string parent;  // "" for none
    std::vector<GameField> fields;
    std::vector<GameMethod> methods;
};

struct GameCode {
    std::unordered_map<std::string, GameType> types;  // by full name, e.g. "chainsaw.PlayerManager"
    const GameType* find(const std::string& name) const;
    // A field or method by name (or a method by prototype), on the type or a parent, as REFramework looks [official,
    // RETypeDefinition.cpp]; nullptr if none.
    const GameField* field(const std::string& type, const std::string& name) const;
    const GameMethod* method(const std::string& type, const std::string& name) const;
};

// REFramework's il2cpp_dump.json (DeveloperTools > ObjectExplorer > Dump SDK, written to the game's folder) [official,
// ObjectExplorer.cpp]: read as a stream, keeping only types, parents, fields and methods (the file is hundreds of MB).
// With `cache` (a file): read from it when it was made from this dump (path, size, write time), else written there.
// Throws std::runtime_error.
GameCode load_game_code(const std::filesystem::path& dump, const std::filesystem::path& cache = {});

// The dump scripts are checked against (Settings::sdk_dump; front ends set it). game_code() loads it on first use (and
// again when the file changes), cached under %LOCALAPPDATA%\remod\game_code; nullptr if none is set. Throws if it can't
// be read. ponytail: one dump for every game, as {game}.
void set_sdk_dump(const std::filesystem::path& dump);
const GameCode* game_code();

// Types and members whose names hold every word of `query` (ignoring case), types first; a query naming a type exactly
// gives it and all its own members. `detail`: a field's type, a method's signature and return type, a type's parent.
struct CodeHit {
    std::string type, member, detail;  // member "" for the type itself
};
std::vector<CodeHit> search_game_code(const GameCode& code, const std::string& query, std::size_t limit = 60);

// A script's problems, by line: syntax (Lua 5.4's own parser; REFramework runs 5.4.3), then with `code`, game names
// in strings: sdk.find_type_definition / get_managed_singleton / create_instance / typeof("<type>"), and :get_method /
// :get_field / :call / :set_field("<name>") on one of those or on a variable set from one in the same file.
// ponytail: names built at run time, and variables passed between files, aren't checked.
struct ScriptProblem {
    int line = 0;
    std::string message;
};
std::vector<ScriptProblem> check_lua(const std::string& source, const std::string& name, const GameCode* code);

}  // namespace remod
