#include "process.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

using Catch::Matchers::ContainsSubstring;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path system32(const wchar_t* exe) {
    wchar_t dir[MAX_PATH];
    GetSystemDirectoryW(dir, MAX_PATH);
    return fs::path(dir) / exe;
}

}  // namespace

TEST_CASE("quote_arg follows CommandLineToArgvW rules") {
    CHECK(remod::quote_arg(L"plain") == L"plain");
    CHECK(remod::quote_arg(L"C:\\no\\spaces") == L"C:\\no\\spaces");
    CHECK(remod::quote_arg(L"") == L"\"\"");
    CHECK(remod::quote_arg(L"a b") == L"\"a b\"");
    CHECK(remod::quote_arg(L"a\"b") == L"\"a\\\"b\"");
    CHECK(remod::quote_arg(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
    CHECK(remod::quote_arg(L"a\\\\\"b") == L"\"a\\\\\\\\\\\"b\"");
}

TEST_CASE("quote_arg round-trips through CommandLineToArgvW") {
    for (const std::wstring arg : {L"plain", L"", L"a b", L"a\"b", L"C:\\dir with space\\", L"a\\\\\"b", L"\\\\server\\x y"}) {
        const std::wstring cmd = L"x.exe " + remod::quote_arg(arg);
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmd.c_str(), &argc);
        REQUIRE(argc == 2);
        CHECK(std::wstring(argv[1]) == arg);
        LocalFree(argv);
    }
}

TEST_CASE("run_process captures output and exit code") {
    const auto ok = remod::run_process(system32(L"cmd.exe"), {L"/c", L"echo hello& echo oops 1>&2"}, 10s);
    CHECK(ok.exit_code == 0);
    CHECK_THAT(ok.output, ContainsSubstring("hello") && ContainsSubstring("oops"));

    CHECK(remod::run_process(system32(L"cmd.exe"), {L"/c", L"exit 3"}, 10s).exit_code == 3);
}

TEST_CASE("run_process kills the whole process tree on timeout") {
    // cmd spawns ping as a grandchild that holds the output pipe; the call must still return promptly.
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_WITH(remod::run_process(system32(L"cmd.exe"), {L"/c", L"ping -n 30 127.0.0.1"}, 300ms),
                      ContainsSubstring("timed out"));
    CHECK(std::chrono::steady_clock::now() - start < 10s);
}

TEST_CASE("run_process on a private desktop: dialogs are caught at once, never shown") {
    // A MessageBox on the invisible desktop would wait forever; it must end the run quickly, with its text.
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_WITH(
        remod::run_process(system32(L"WindowsPowerShell\\v1.0\\powershell.exe"),
                           {L"-NoProfile", L"-Command",
                            L"Add-Type -AssemblyName System.Windows.Forms; "
                            L"[System.Windows.Forms.MessageBox]::Show('texture broke', 'Plugin error')"},
                           60s, true),
        ContainsSubstring("stopped with a dialog") && ContainsSubstring("Plugin error") &&
            ContainsSubstring("texture broke"));
    CHECK(std::chrono::steady_clock::now() - start < 30s);

    // Normal programs still run and report their exit code there.
    CHECK(remod::run_process(system32(L"cmd.exe"), {L"/c", L"exit 4"}, 10s, true).exit_code == 4);
}

TEST_CASE("run_process adds environment variables for the child only") {
    const auto r = remod::run_process(system32(L"cmd.exe"), {L"/c", L"echo [%REMOD_TEST_VAR%] [%SystemRoot%]"}, 10s,
                                      false, {{L"REMOD_TEST_VAR", L"set for child"}});
    CHECK_THAT(r.output, ContainsSubstring("[set for child]"));
    CHECK_THAT(r.output, !ContainsSubstring("[%SystemRoot%]"));  // the rest of the environment is inherited
    const auto plain = remod::run_process(system32(L"cmd.exe"), {L"/c", L"echo [%REMOD_TEST_VAR%]"}, 10s);
    CHECK_THAT(plain.output, ContainsSubstring("[%REMOD_TEST_VAR%]"));  // not leaked into our own environment
}

TEST_CASE("run_process reports a missing executable") {
    CHECK_THROWS_AS(remod::run_process("C:/does/not/exist.exe", {}, 1s), remod::ProcessError);
}
