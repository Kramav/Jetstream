#include "package.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>

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

}  // namespace

bool is_safe_relative(const fs::path& p) {
    if (p.empty() || p.has_root_name() || p.has_root_directory()) return false;
    return std::ranges::none_of(p, [](const fs::path& part) { return part == ".."; });
}

fs::path package_path(const Profile& profile, const std::string& mod_name, const fs::path& game_path) {
    check_mod_name(mod_name);
    if (!is_safe_relative(game_path))
        throw PackageError("game path '" + game_path.generic_string() + "' must be relative to the natives root, without '..'");

    // [guide] RE4R textures are named <name>.tex.<tex_suffix>.
    const std::string file = lower(game_path.filename().string());
    const bool is_tex = file.ends_with(".tex") || file.find(".tex.") != std::string::npos;
    if (is_tex && !file.ends_with(".tex." + profile.tex_suffix))
        throw PackageError("texture '" + game_path.generic_string() + "' must end in .tex." + profile.tex_suffix +
                           " for profile " + profile.id);

    return (fs::path(mod_name) / profile.natives_root / game_path).lexically_normal();
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
    if (fs::exists(root)) throw PackageError("output folder already exists, refusing to overwrite: " + root.string());

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

    try {
        for (const auto& [src, dest] : copies) {
            fs::create_directories(dest.parent_path());
            fs::copy_file(src, dest);
        }
        std::ofstream ini(root / "modinfo.ini", std::ios::binary);
        ini << modinfo;
        if (!ini.flush()) throw PackageError("failed to write modinfo.ini");
    } catch (...) {
        std::error_code ec;
        fs::remove_all(root, ec);  // root did not exist before; don't leave a half-built package
        throw;
    }
    return root;
}

}  // namespace remod
