#pragma once
// Graph editing for programs (scripts; later an AI orchestrator / MCP server, CLAUDE.md §10 M2): one JSON request in,
// one JSON reply out. The rules are core's, the same calls the app makes; this only translates. The CLI's `remod api`
// reads one request per line from stdin and writes one reply per line.
//
// Request: {"op": "<name>", ...}. Reply: {"ok": true, ...} or {"ok": false, "error": "<why>"}.
//   types                                  every block type: inputs, outputs, kinds, fields
//   new      [profile]                     an empty graph (profile "re4r" by default)
//   open     file                          load a graph file (older files migrated, as in the app)
//   save     [file]                        write it (to the file opened or last saved if none given)
//   graph                                  blocks (id, type, title, params) and links
//   add      type                          -> id
//   remove   id                            the block and its links
//   set      id, input, value              a typed field (an input's, or an output's field such as Export's "png")
//   link     from, from_port, to, to_port  refused with the reason if the rules don't allow it
//   unlink   from, from_port, to, to_port
//   next     id, port, output (bool)       block types that could attach to that pin, and through which port
//   validate                               every problem that would stop a run
//   preview                                every output's value as known without running (nothing is touched)
// Not here yet: run. It changes files, so it waits for the orchestrator's guardrails (CLAUDE.md §10 M2).
#include "graph.hpp"

#include <filesystem>
#include <string>

namespace remod {

class ApiSession {
public:
    std::string call(const std::string& request);  // never throws: errors are replies

private:
    Graph graph_;
    std::filesystem::path file_;  // where the graph was opened from or last saved to; empty for a new one
};

}  // namespace remod
