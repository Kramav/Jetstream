#include "profile.hpp"

#include "package.hpp"

#include <toml++/toml.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace remod {

namespace {

constexpr std::array kStringKeys{"id",         "name",          "tex_suffix", "natives_root",
                                 "pak_script", "noesis_export", "file_list"};
constexpr std::array kPackagingValues{"loose_archive", "pak"};

Profile from_table(const toml::table& root) {
    std::vector<std::string> errors;
    Profile p;

    const toml::table* game = root["game"].as_table();
    if (!game) throw ProfileError("profile: missing [game] table");

    std::string* fields[] = {&p.id,         &p.name,          &p.tex_suffix, &p.natives_root,
                             &p.pak_script, &p.noesis_export, &p.file_list};
    for (size_t i = 0; i < kStringKeys.size(); ++i) {
        auto v = (*game)[kStringKeys[i]].value<std::string>();
        if (!v || v->empty())
            errors.push_back(std::string("'") + kStringKeys[i] + "' must be a non-empty string");
        else
            *fields[i] = *v;
    }

    if (const toml::array* arr = (*game)["packaging"].as_array(); !arr || arr->empty()) {
        errors.push_back("'packaging' must be a non-empty array");
    } else {
        for (const auto& el : *arr) {
            auto v = el.value<std::string>();
            if (!v || std::find(kPackagingValues.begin(), kPackagingValues.end(), *v) == kPackagingValues.end())
                errors.push_back("'packaging' values must be one of: loose_archive, pak");
            else
                p.packaging.push_back(*v);
        }
    }

    if (!p.natives_root.empty() && !is_safe_relative(p.natives_root))
        errors.push_back("'natives_root' must be a relative path without '..'");

    for (const auto& [key, _] : *game) {
        const bool known = key.str() == "packaging" ||
                           std::find(kStringKeys.begin(), kStringKeys.end(), key.str()) != kStringKeys.end();
        if (!known) errors.push_back("unknown key '" + std::string(key.str()) + "'");
    }

    if (!errors.empty()) {
        std::string msg = "profile invalid:";
        for (const auto& e : errors) msg += "\n  " + e;
        throw ProfileError(msg);
    }
    return p;
}

}  // namespace

std::vector<std::string> Profile::unresolved() const {
    std::vector<std::string> out;
    const std::string* fields[] = {&id,         &name,          &tex_suffix, &natives_root,
                                   &pak_script, &noesis_export, &file_list};
    for (size_t i = 0; i < kStringKeys.size(); ++i)
        if (*fields[i] == "TBD") out.emplace_back(kStringKeys[i]);
    return out;
}

Profile parse_profile(std::string_view toml_text) {
    try {
        return from_table(toml::parse(toml_text));
    } catch (const toml::parse_error& e) {
        throw ProfileError(std::string("profile: TOML parse error: ") + std::string(e.description()));
    }
}

Profile load_profile(const std::filesystem::path& file) {
    try {
        return from_table(toml::parse_file(file.string()));
    } catch (const toml::parse_error& e) {
        throw ProfileError(file.string() + ": " + std::string(e.description()));
    }
}

std::vector<Profile> load_profiles(const std::filesystem::path& profiles_dir, std::vector<std::string>* errors) {
    std::vector<Profile> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(profiles_dir, ec)) {
        if (e.path().extension() != ".toml") continue;
        try {
            out.push_back(load_profile(e.path()));
        } catch (const ProfileError& err) {
            if (errors) errors->push_back(err.what());
        }
    }
    if (ec && errors) errors->push_back("can't read profiles folder " + profiles_dir.string() + ": " + ec.message());
    std::ranges::sort(out, {}, &Profile::name);
    return out;
}

Profile load_profile_by_id(const std::filesystem::path& profiles_dir, const std::string& id) {
    const bool plain = !id.empty() && std::ranges::all_of(id, [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
    if (!plain) throw ProfileError("invalid profile id '" + id + "'");
    Profile p = load_profile(profiles_dir / (id + ".toml"));
    if (p.id != id) throw ProfileError(id + ".toml declares id '" + p.id + "'");
    return p;
}

std::filesystem::path find_profiles_dir() {
    namespace fs = std::filesystem;
    if (fs::is_directory("profiles")) return fs::absolute("profiles");
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    for (fs::path dir = fs::path(exe).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_directory(dir / "profiles")) return dir / "profiles";
        if (dir == dir.parent_path()) break;
    }
    return {};
}

}  // namespace remod
