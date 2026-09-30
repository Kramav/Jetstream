#include "package.hpp"

#include "process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace remod {

namespace {

void check_mod_name(const std::string& name) {
    if (name.empty() || name == "." || name == "..") throw PackageError("mod name is empty or invalid");
    for (unsigned char c : name)
        if (c < 32 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos)
            throw PackageError("mod name '" + name + "' contains a character not allowed in Windows folder names");
    if (name.back() == '.' || name.back() == ' ')
        throw PackageError("mod name '" + name + "' must not end with '.' or ' '");
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Windows MAX_PATH; RE mods are known to hit it (CLAUDE.md §1).
constexpr size_t kMaxPath = 259;

fs::path system_tar() {
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) throw PackageError("could not locate the Windows system directory");
    return fs::path(dir) / L"tar.exe";
}

// Zips `folder` so the archive root holds the folder itself (published-mod layout, CLAUDE.md §9).
// Uses the bsdtar that ships with Windows 10 1803+; no bundled zip library.
// ponytail: bsdtar's handling of non-ASCII file names is untested here; switch to a zip library if they break.
void zip_folder(const fs::path& folder, const fs::path& zip) {
    const fs::path tar = system_tar();
    if (!fs::is_regular_file(tar)) throw PackageError("Windows tar.exe not found (requires Windows 10 1803 or later)");
    const auto r = run_process(tar,
                               {L"-a", L"-c", L"-f", zip.wstring(), L"-C", folder.parent_path().wstring(),
                                folder.filename().wstring()},
                               std::chrono::minutes(10));
    if (r.exit_code != 0 || !fs::is_regular_file(zip) || fs::file_size(zip) == 0)
        throw PackageError("zip failed (tar exit " + std::to_string(r.exit_code) + "): " + r.output);
}

// A previous build of this mod: its modinfo.ini has name=<display_name>.
bool is_our_folder(const fs::path& folder, const std::string& display_name) {
    std::ifstream ini(folder / "modinfo.ini");
    for (std::string line; std::getline(ini, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "name=" + display_name) return true;
    }
    return false;
}

// A previous zip of this mod: every entry is under <mod_name>/ and <mod_name>/modinfo.ini is one of them.
bool is_our_zip(const fs::path& zip, const std::string& mod_name) {
    const auto r = run_process(system_tar(), {L"-tf", zip.wstring()}, std::chrono::minutes(1));
    if (r.exit_code != 0) return false;
    std::istringstream lines(r.output);
    bool has_ini = false, any = false;
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        any = true;
        if (!line.starts_with(mod_name + "/")) return false;
        has_ini |= line == mod_name + "/modinfo.ini";
    }
    return any && has_ini;
}

}  // namespace

bool is_safe_relative(const fs::path& p) {
    if (p.empty() || p.has_root_name() || p.has_root_directory()) return false;
    return std::ranges::none_of(p, [](const fs::path& part) { return part == ".."; });
}

fs::path package_path(const Profile& profile, const std::string& mod_name, const fs::path& game_path) {
    check_mod_name(mod_name);
    if (!is_safe_relative(game_path))
        throw PackageError("game path '" + game_path.generic_string() + "' must be relative to the natives root, without '..'");

    // [guide] In the game, textures are named <name>.tex.<tex_suffix>. A plain ".tex" gets the profile's suffix
    // added, so users never have to type it; any other suffix is a different game's texture.
    fs::path in_game = game_path;
    const std::string file = lower(game_path.filename().string());
    if (file.ends_with(".tex")) {
        in_game += "." + profile.tex_suffix;
    } else if (file.find(".tex.") != std::string::npos && !file.ends_with(".tex." + profile.tex_suffix)) {
        throw PackageError("texture '" + game_path.generic_string() + "' has a different game's suffix; " +
                           profile.name + " textures end in .tex." + profile.tex_suffix);
    }
    return (fs::path(mod_name) / profile.natives_root / in_game).lexically_normal();
}

std::string write_modinfo(const ModInfo& info) {
    // [guide] Flat key=value lines. CRLF, as written by Notepad (most-tested case for Fluffy).
    std::string out;
    auto field = [&](const char* key, const std::string& value) {
        if (value.empty()) return;
        if (value.find_first_of("\r\n") != std::string::npos)
            throw PackageError(std::string("modinfo field '") + key + "' must be a single line");
        out += std::string(key) + "=" + value + "\r\n";
    };

    // Fluffy renders a literal "\n" in description as a line break [guide].
    std::string desc;
    for (size_t i = 0; i < info.description.size(); ++i) {
        const char c = info.description[i];
        if (c == '\r') continue;
        desc += c == '\n' ? std::string("\\n") : std::string(1, c);
    }

    field("name", info.name);
    field("version", info.version);
    field("description", desc);
    field("author", info.author);
    field("screenshot", info.screenshot);
    return out;
}

fs::path build_package(const Profile& profile, const PackageSpec& spec) {
    if (spec.files.empty()) throw PackageError("nothing to package: no files given");
    check_mod_name(spec.mod_name);

    const fs::path root = fs::absolute(spec.out_dir / spec.mod_name);
    const fs::path zip = fs::path(root) += ".zip";
    const bool old_root = fs::exists(root), old_zip = spec.zip && fs::exists(zip);
    if (old_root && !spec.replace)
        throw PackageError("output folder already exists: " + root.string() + " (turn on 'Replace existing' to overwrite)");
    if (old_zip && !spec.replace)
        throw PackageError("output zip already exists: " + zip.string() + " (turn on 'Replace existing' to overwrite)");
    if (old_root && !is_our_folder(root, spec.info.name))
        throw PackageError(root.string() + " exists but isn't a build of mod '" + spec.info.name +
                           "' (its modinfo.ini doesn't match); not replacing it");
    if (old_zip && !is_our_zip(zip, spec.mod_name))
        throw PackageError(zip.string() + " exists but isn't a build of mod '" + spec.mod_name + "'; not replacing it");

    // Validate everything before writing anything.
    std::vector<std::pair<fs::path, fs::path>> copies;  // source -> destination
    std::set<fs::path> seen;
    for (const auto& f : spec.files) {
        if (!fs::is_regular_file(f.source)) throw PackageError("input file not found: " + f.source.string());
        fs::path dest = spec.out_dir / package_path(profile, spec.mod_name, f.game_path);
        if (!seen.insert(dest).second)
            throw PackageError("game path listed twice: " + f.game_path.generic_string());
        copies.emplace_back(f.source, fs::absolute(dest));
    }

    ModInfo info = spec.info;
    if (!spec.screenshot.empty()) {
        if (!fs::is_regular_file(spec.screenshot))
            throw PackageError("screenshot not found: " + spec.screenshot.string());
        constexpr std::array kExts{".jpg", ".png", ".tga", ".bmp"};  // [guide]
        if (std::find(kExts.begin(), kExts.end(), lower(spec.screenshot.extension().string())) == kExts.end())
            throw PackageError("screenshot must be jpg, png, tga or bmp: " + spec.screenshot.string());
        info.screenshot = spec.screenshot.filename().string();
        copies.emplace_back(spec.screenshot, root / info.screenshot);
    }
    const std::string modinfo = write_modinfo(info);

    for (const auto& [_, dest] : copies)
        if (dest.native().size() > kMaxPath)
            throw PackageError("output path exceeds " + std::to_string(kMaxPath) +
                               " characters; use a shorter output folder: " + dest.string());

    if (old_root) fs::remove_all(root);  // checked above: a previous build of this mod
    if (old_zip) fs::remove(zip);
    try {
        for (const auto& [src, dest] : copies) {
            fs::create_directories(dest.parent_path());
            fs::copy_file(src, dest);
        }
        std::ofstream ini(root / "modinfo.ini", std::ios::binary);
        ini << modinfo;
        if (!ini.flush()) throw PackageError("failed to write modinfo.ini");
        ini.close();
        if (spec.zip) zip_folder(root, zip);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(root, ec);  // ours (new, or a replaced build); don't leave a half-built package
        fs::remove(zip, ec);
        throw;
    }
    return root;
}

}  // namespace remod
