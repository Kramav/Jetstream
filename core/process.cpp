#include "process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <thread>

namespace remod {

namespace {

struct Handle {
    HANDLE h = nullptr;
    explicit Handle(HANDLE x = nullptr) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { close(); }
    void close() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
};

std::string win_error(DWORD code) {
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string out = msg ? msg : "error " + std::to_string(code);
    LocalFree(msg);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

}  // namespace

std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    // Backslashes are literal unless they precede a quote (CommandLineToArgvW rules).
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L'"';
}

ProcessResult run_process(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                          std::chrono::milliseconds timeout) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_raw = nullptr, write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &sa, 0)) throw ProcessError("CreatePipe: " + win_error(GetLastError()));
    Handle read(read_raw), write(write_raw);
    SetHandleInformation(read.h, HANDLE_FLAG_INHERIT, 0);
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));

    // Job object: killing it kills the whole process tree, including children the tool spawns.
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul.h;
    si.hStdOutput = write.h;
    si.hStdError = write.h;

    std::wstring cmd = quote_arg(exe.wstring());
    for (const auto& a : args) cmd += L" " + quote_arg(a);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        nullptr, &si, &pi))
        throw ProcessError("failed to start " + exe.string() + ": " + win_error(GetLastError()));
    Handle process(pi.hProcess), thread(pi.hThread);
    AssignProcessToJobObject(job.h, process.h);  // while suspended, so its children land in the job too
    ResumeThread(thread.h);
    write.close();  // only the child holds the write end now; ReadFile hits EOF when the tree exits

    std::string output;
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read.h, buf, sizeof(buf), &n, nullptr) && n > 0) output.append(buf, n);
    });

    const bool timed_out = WaitForSingleObject(process.h, static_cast<DWORD>(timeout.count())) == WAIT_TIMEOUT;
    TerminateJobObject(job.h, 1);  // on timeout: kill the tree; otherwise: reap leftover children
    reader.join();

    if (timed_out)
        throw ProcessError(exe.filename().string() + " timed out after " + std::to_string(timeout.count()) +
                           " ms and was killed. Output:\n" + output);
    DWORD code = 0;
    GetExitCodeProcess(process.h, &code);
    return {static_cast<int>(code), std::move(output)};
}

}  // namespace remod
