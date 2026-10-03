#pragma once
// Graph editing for programs (scripts; later an AI orchestrator / MCP server, CLAUDE.md §10 M4): one JSON request in,
// one JSON reply out. The rules are core's, the same calls the app makes; this only translates. The CLI's `remod api`
// reads one request per line from stdin and writes one reply per line.
//
// Request: {"op": "<name>", ...}. Reply: {"ok": true, ...} or {"ok": false, "error": "<why>"}.
//   types                                  every block type: inputs, outputs, kinds, fields, and an "ai" note on when to
//                                          use it
//   new      [profile]                     an empty graph (profile "re4r" by default)
//   open     file                          load a graph file (older files migrated, as in the app)
//   save     [file]                        write it (to the file opened or last saved if none given)
//   graph                                  blocks (id, type, title, params) and links
//   add      type                          -> id
//   remove   id                            the block and its links
//   set      id, input, value              a typed field (an input's, or an output's field such as Export's "png"; a
//                                          Files in folder block's "show": which file previews show)
//   link     from, from_port, to, to_port  refused with the reason if the rules don't allow it
//   unlink   from, from_port, to, to_port
//   next     id, port, output (bool)       block types that could attach to that pin, and through which port
//   validate                               every problem that would stop a run
//   preview                                every output's value as known without running (nothing is touched), and
//                                          "lists": each list block's files (name, item)
//   image    id, [size]                    a block's picture as it is now, as the app's thumbnails work it out: "png"
//                                          (base64), width, height; size 16 to 1024 (default 512)
//   plan     [noesis], [game_files]        the file changes a run would make, each judged by the guardrails (Guard):
//                                          ok, needs approval (removing a file; writing outside the graph's folder),
//                                          refused (game_files, the folder in Noesis's RE plugin's NativesPath.txt,
//                                          Noesis's folder). noesis: convert textures with Noesis (optional; the
//                                          built-in converter otherwise). Blocks whose changes are decided
//                                          only in the run (decided_in_run) may change files inside the graph's
//                                          folder only, and remove nothing.
//   run      plan, [approve], [noesis], [game_files]  runs against that plan: refused if the changes differ now, if any is
//                                          refused, or if one needs approval and `approve` isn't true. The guardrails
//                                          are checked again at every change. `approve` is the user's answer: the
//                                          program (an MCP server) must ask the user, never decide it itself. A run
//                                          stopped at an Edit image says "paused": hand control back to the user.
//   edit_done id, [done], [item]           marks an Edit image step done (the user finished editing); `item`: one
//                                          item of a step repeated for a list (its "item" in the run's nodes)
// plan and run need the graph saved: its folder is where a run may write freely.
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
