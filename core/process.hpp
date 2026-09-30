#pragma once
// Runs external tools as subprocesses (CLAUDE.md §7): timeout, captured output, exit code.
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
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
ProcessResult run_process(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                          std::chrono::milliseconds timeout);

}  // namespace remod
