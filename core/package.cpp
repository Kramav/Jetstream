#include "package.hpp"

#include "process.hpp"
#include "texture_converter.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string_view>

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

bool is_game_root_path(const fs::path& game_path) {
    return !game_path.empty() && lower(game_path.begin()->string()) == "reframework";
}

fs::path package_path(const Profile& profile, const std::string& mod_name, const fs::path& game_path) {
    check_mod_name(mod_name);
    if (!is_safe_relative(game_path))
        throw PackageError("game path '" + game_path.generic_string() + "' must be relative to the natives root, without '..'");
    if (is_game_root_path(game_path)) return (fs::path(mod_name) / game_path).lexically_normal();

    // [guide] In the game, textures are named <name>.tex.<tex_suffix>. Any other ending (a plain ".tex", or a tool's
    // ".tex.re2remake") becomes the profile's suffix, so users never have to type it. Which game a texture is for
    // is its header's business: build_package checks that.
    fs::path in_game = game_path;
    const std::string file = game_path.filename().string(), low = lower(file);
    if (is_tex_name(low)) in_game.replace_filename(file.substr(0, low.rfind(".tex") + 4) + "." + profile.tex_suffix);
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

namespace {

std::string read_all(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Whether `root` already holds exactly these files (same bytes) and this modinfo.ini, nothing else.
bool same_build(const fs::path& root, const std::vector<std::pair<fs::path, fs::path>>& copies, const std::string& modinfo) {
    std::error_code ec;
    std::set<fs::path> expected{(root / "modinfo.ini").lexically_normal()};
    for (const auto& [src, dest] : copies) {
        expected.insert(dest.lexically_normal());
        if (fs::file_size(src, ec) != fs::file_size(dest, ec) || ec || read_all(src) != read_all(dest)) return false;
    }
    if (read_all(root / "modinfo.ini") != modinfo) return false;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec))
        if (entry.is_regular_file() && !expected.contains(entry.path().lexically_normal())) return false;
    return !ec;
}

}  // namespace

fs::path build_package(const Profile& profile, const PackageSpec& spec, bool* unchanged) {
    if (unchanged) *unchanged = false;
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
        if (const auto version = read_tex_version(f.source); version && std::to_string(*version) != profile.tex_suffix)
            throw PackageError(f.source.string() + " is a texture of version " + std::to_string(*version) + ", not " +
                               profile.name + "'s (" + profile.tex_suffix + ")");
        fs::path dest = spec.out_dir / package_path(profile, spec.mod_name, f.game_path);
        if (!seen.insert(dest).second)
            throw PackageError("game path listed twice: " + f.game_path.generic_string());
        copies.emplace_back(f.source, fs::absolute(dest));
    }

    ModInfo info = spec.info;
    if (std::ranges::any_of(spec.files, [](const PackageFile& f) { return is_game_root_path(f.game_path); }))
        info.description += (info.description.empty() ? "" : "\n") + std::string("Needs REFramework.");
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

    if (old_root && (old_zip || !spec.zip) && same_build(root, copies, modinfo)) {
        if (unchanged) *unchanged = true;  // the same build: nothing to write
        return root;
    }
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

std::vector<PackageFile> script_files(const fs::path& lua) {
    std::error_code ec;
    if (lower(lua.extension().string()) != ".lua" || !fs::is_regular_file(lua, ec))
        throw PackageError("not a .lua file: " + lua.string());
    const std::string stem = lua.stem().string();
    std::vector<PackageFile> files{{lua, "reframework/autorun/" + stem + ".lua"}};
    const fs::path modules = lua.parent_path() / stem;
    std::vector<PackageFile> more;
    if (fs::is_directory(modules, ec))
        for (const auto& e : fs::recursive_directory_iterator(modules, ec))
            if (e.is_regular_file())
                more.push_back({e.path(), "reframework/autorun/" + stem + "/" +
                                              e.path().lexically_relative(modules).generic_string()});
    std::ranges::sort(more, {}, &PackageFile::game_path);
    files.insert(files.end(), more.begin(), more.end());
    return files;
}

namespace {

fs::path in_game(const PackageFile& f, const fs::path& game_dir) {
    if (!is_game_root_path(f.game_path) || !is_safe_relative(f.game_path))
        throw PackageError(f.game_path.generic_string() + " isn't a path in REFramework's folder");
    return game_dir / f.game_path;
}

}  // namespace

void install_in_game(const std::vector<PackageFile>& files, const fs::path& game_dir) {
    if (!fs::is_regular_file(game_dir / "dinput8.dll"))
        throw PackageError("REFramework isn't installed in " + game_dir.string() +
                           " (no dinput8.dll): set the game's folder, or install REFramework");
    for (const auto& f : files) in_game(f, game_dir);  // all checked before anything is copied
    for (const auto& f : files) {
        const fs::path dest = in_game(f, game_dir);
        fs::create_directories(dest.parent_path());
        fs::copy_file(f.source, dest, fs::copy_options::overwrite_existing);
    }
}

fs::path framework_log(const fs::path& game_dir) { return game_dir / "re2_framework_log.txt"; }

bool lua_errors_logged(const fs::path& game_dir) {
    // utility::Config's "key=value" lines; a bool is "true" / "false" [official, REFramework's utility/Config].
    std::ifstream in(game_dir / "re2_fw_config.txt");
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "ScriptRunner_LogToDisk=true") return true;
    }
    return false;
}

std::vector<GameError> script_errors_in_log(const fs::path& log, std::uintmax_t from, const fs::path& script) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(log, ec);
    if (ec) return {};
    std::ifstream in(log, std::ios::binary);
    in.seekg(std::streamoff(from <= size ? from : 0));
    // spdlog's lines: "[<date> <time>] [REFramework] [error] <message>"; a message's further lines (a Lua stack
    // traceback) don't start with "[".
    std::vector<std::string> entries;
    bool error = false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("[")) {
            const size_t at = line.find("] [error] ");
            error = at != std::string::npos;
            if (error) entries.push_back(line.substr(at + 10));
        } else if (error && !entries.empty()) {
            entries.back() += "\n" + line;
        }
    }
    const std::string stem = lower(script.stem().string()), file = stem + ".lua";
    // Where `name` starts a word in `text` (after a path separator, a space, a quote or the start), else npos.
    const auto word = [](const std::string& text, const std::string& name, size_t from = 0) {
        for (size_t at = text.find(name, from); at != std::string::npos; at = text.find(name, at + 1))
            if (at == 0 || std::string_view("/ \"'").find(text[at - 1]) != std::string_view::npos) return at;
        return std::string::npos;
    };
    std::vector<GameError> out;
    for (const std::string& e : entries) {
        std::string low = lower(e);
        std::ranges::replace(low, '\\', '/');
        if (word(low, file) == std::string::npos && word(low, stem + "/") == std::string::npos) continue;
        if (const auto same = std::ranges::find(out, e, &GameError::message); same != out.end()) {
            ++same->count;
            continue;
        }
        GameError g{.message = e};
        for (size_t at = word(low, file + ":"); at != std::string::npos && !g.line; at = word(low, file + ":", at + 1)) {
            const size_t digits = at + file.size() + 1;
            size_t end = digits;
            while (end < low.size() && std::isdigit(static_cast<unsigned char>(low[end]))) ++end;
            if (end > digits) g.line = std::stoi(low.substr(digits, end - digits));
        }
        out.push_back(std::move(g));
    }
    return out;
}

void remove_from_game(const std::vector<PackageFile>& files, const fs::path& game_dir) {
    const fs::path autorun = (game_dir / "reframework" / "autorun").lexically_normal();
    for (const auto& f : files) {
        fs::path p = in_game(f, game_dir).lexically_normal();
        std::error_code ec;
        fs::remove(p, ec);
        for (p = p.parent_path(); p != autorun && p.native().size() > autorun.native().size() && fs::is_empty(p, ec) && !ec;
             p = p.parent_path())
            fs::remove(p, ec);
    }
}

}  // namespace remod
