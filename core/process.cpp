#include "process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
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

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring window_text(HWND h) {
    wchar_t buf[1024];
    return std::wstring(buf, size_t(GetWindowTextW(h, buf, 1024)));
}

// Title and message of the first standard dialog (class #32770, e.g. a MessageBox) on `desktop`, or "".
std::string find_dialog(HDESK desktop) {
    std::string found;
    EnumDesktopWindows(
        desktop,
        [](HWND h, LPARAM out) -> BOOL {
            wchar_t cls[16];
            if (!IsWindowVisible(h) || !GetClassNameW(h, cls, 16) || std::wstring(cls) != L"#32770") return TRUE;
            std::wstring text = window_text(h);
            EnumChildWindows(
                h,
                [](HWND c, LPARAM t) -> BOOL {
                    wchar_t ccls[16];
                    if (GetClassNameW(c, ccls, 16) && std::wstring(ccls) == L"Static")
                        if (const auto s = window_text(c); !s.empty()) *reinterpret_cast<std::wstring*>(t) += L": " + s;
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&text));
            *reinterpret_cast<std::string*>(out) = narrow(text);
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// This process's environment with `overrides` applied, as a CreateProcess block ("k=v\0...\0\0"), sorted by name.
std::wstring environment_block(const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
    auto name_of = [](const std::wstring& entry) { return entry.substr(0, entry.find(L'=', 1)); };
    std::vector<std::wstring> entries;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);
            const bool overridden = std::any_of(overrides.begin(), overrides.end(), [&](const auto& o) {
                return _wcsicmp(name_of(entry).c_str(), o.first.c_str()) == 0;
            });
            if (!overridden) entries.push_back(std::move(entry));
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : overrides) entries.push_back(name + L"=" + value);
    std::sort(entries.begin(), entries.end(),
              [&](const std::wstring& a, const std::wstring& b) { return _wcsicmp(name_of(a).c_str(), name_of(b).c_str()) < 0; });
    std::wstring out;
    for (const auto& e : entries) out += e + L'\0';
    return out + L'\0';
}

}  // namespace

ProcessResult run_process(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                          std::chrono::milliseconds timeout, bool private_desktop,
                          const std::vector<std::pair<std::wstring, std::wstring>>& env,
                          const std::filesystem::path& cwd) {
    std::wstring cmd = quote_arg(exe.wstring());
    for (const auto& a : args) cmd += L" " + quote_arg(a);
    return run_process_line(exe, std::move(cmd), timeout, private_desktop, env, cwd);
}

ProcessResult run_process_line(const std::filesystem::path& exe, std::wstring cmd, std::chrono::milliseconds timeout,
                               bool private_desktop, const std::vector<std::pair<std::wstring, std::wstring>>& env,
                               const std::filesystem::path& cwd) {
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

    // A desktop of our own: the child's windows (error MessageBoxes included) never reach the user's screen,
    // and can be found and read.
    static std::atomic<int> desktop_counter{0};
    std::wstring desktop_name =
        L"remod_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(desktop_counter++);
    HDESK desktop = nullptr;
    if (private_desktop) {
        desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop) throw ProcessError("CreateDesktop: " + win_error(GetLastError()));
        si.lpDesktop = desktop_name.data();
    }
    struct DesktopCloser {
        HDESK d;
        ~DesktopCloser() {
            if (d) CloseDesktop(d);
        }
    } desktop_closer{desktop};

    std::wstring env_block = env.empty() ? std::wstring() : environment_block(env);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        env.empty() ? nullptr : env_block.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
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

    // Wait in short steps so a dialog on the private desktop is noticed right away instead of at the timeout.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timed_out = false;
    std::string dialog;
    while (WaitForSingleObject(process.h, desktop ? 200 : static_cast<DWORD>(timeout.count())) == WAIT_TIMEOUT) {
        if (desktop && !(dialog = find_dialog(desktop)).empty()) break;
        if (!desktop || std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
    }
    TerminateJobObject(job.h, 1);  // on timeout/dialog: kill the tree; otherwise: reap leftover children
    reader.join();

    if (!dialog.empty())
        throw ProcessError(exe.filename().string() + " stopped with a dialog and was closed. The dialog said: " + dialog);
    if (timed_out)
        throw ProcessError(exe.filename().string() + " timed out after " + std::to_string(timeout.count()) +
                           " ms and was killed. Output:\n" + output);
    DWORD code = 0;
    GetExitCodeProcess(process.h, &code);
    return {static_cast<int>(code), std::move(output)};
}

}  // namespace remod
