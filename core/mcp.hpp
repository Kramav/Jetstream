#pragma once
// MCP server (CLAUDE.md §10 M2: the AI orchestrator): Model Context Protocol, JSON-RPC 2.0, one message per line, over
// ApiSession. An AI (Claude Code, Claude Desktop) builds, checks, looks at and runs graphs with core's own rules; this
// only translates. `remod mcp` runs it on stdin / stdout.
//
// Approval belongs to the user, never to the AI: no tool takes an "approve" argument. A run whose plan has changes
// needing approval asks McpOptions::approve (the CLI: a Windows dialog only the user can answer) and runs only on yes.
#include "api.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace remod {

struct McpOptions {
    std::filesystem::path noesis;      // convert textures with Noesis (the user's choice), else the built-in converter
    std::filesystem::path game_files;  // the extracted game files: never changed (Guard)
    // Asks the user whether to allow `changes` (one per line, with why each needs approval). True = allowed.
    std::function<bool(const std::string& changes)> approve;
};

class McpServer {
public:
    explicit McpServer(McpOptions options) : options_(std::move(options)) {}
    // One incoming message -> the reply to write (one line), or "" when none is due (a notification).
    std::string handle(const std::string& message);

private:
    McpOptions options_;
    ApiSession api_;
};

}  // namespace remod
