#pragma once
// Runs external tools as subprocesses (CLAUDE.md §7): timeout, captured output, exit code.
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace remod {

struct ProcessError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ProcessResult {
    int exit_code = 0;
    // ponytail: stdout and stderr merged into one pipe; split them if a tool's parsing ever needs it.
    std::string output;
};

// Quotes one argument so the child's CommandLineToArgvW sees it unchanged.
std::wstring quote_arg(const std::wstring& arg);

// Runs exe with args (no shell). The caller checks exit_code.
// Throws ProcessError if the process can't start or exceeds `timeout` (the process tree is killed).
// private_desktop: run it on an invisible desktop of its own, so none of its windows reach the user's screen;
// if it opens a dialog (e.g. an error MessageBox that would wait forever), it is killed at once and the
// ProcessError carries the dialog's text.
// env: variables to set (or override) for the child only, on top of this process's environment.
ProcessResult run_process(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                          std::chrono::milliseconds timeout, bool private_desktop = false,
                          const std::vector<std::pair<std::wstring, std::wstring>>& env = {});

}  // namespace remod
