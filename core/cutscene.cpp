#include "cutscene.hpp"

#include "image.hpp"
#include "texture_converter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <iterator>
#include <sstream>
#include <tuple>

namespace remod {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string num(double v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

bool is_number(const json& j) { return j.is_number(); }

// An array of `n` numbers.
bool numbers(const json& j, size_t n) {
    return j.is_array() && j.size() == n && std::ranges::all_of(j, is_number);
}

// [x, y, z, w] of length about 1.
bool unit_rotation(const json& j) {
    if (!numbers(j, 4)) return false;
    double len = 0;
    for (const auto& v : j) len += v.get<double>() * v.get<double>();
    return len >= 0.5 && len <= 1.5;
}

// Letters, digits and _: a name that's also a file name and a Lua key.
bool plain_name(const std::string& s) {
    return !s.empty() && std::ranges::all_of(s, [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; });
}

}  // namespace

fs::path runtime_dir() {
    const fs::path profiles = find_profiles_dir();
    return profiles.empty() ? fs::path() : profiles.parent_path() / "runtime";
}

PackageFile cutscene_runtime(const fs::path& runtime) {
    std::error_code ec;
    const fs::path script = runtime / "remod_cutscene.lua";
    if (!fs::is_regular_file(script, ec)) throw PackageError("remod's cutscene runtime is missing: " + script.string());
    return {script, "reframework/autorun/remod_cutscene.lua"};
}

std::vector<PackageFile> cutscene_files(const fs::path& cutscene, const fs::path& runtime) {
    std::error_code ec;
    std::string ext = cutscene.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".json" || !fs::is_regular_file(cutscene, ec))
        throw PackageError("not a cutscene file (.json): " + cutscene.string());
    std::vector<PackageFile> files{cutscene_runtime(runtime),
                                   {cutscene, "reframework/data/remod_cutscenes/" + cutscene.filename().string()}};
    // Its actors' definitions that remod ships (runtime/puppets: game file paths only). Others come from the mod that
    // adds the character (e.g. a new character's own definition).
    const json c = json::parse(read_text(cutscene), nullptr, false);
    if (c.is_object() && c.contains("actors") && c["actors"].is_array())
        for (const json& a : c["actors"]) {
            const std::string puppet = a.is_object() ? a.value("puppet", std::string()) : "";
            const fs::path def = runtime / "puppets" / (puppet + ".json");
            if (plain_name(puppet) && fs::is_regular_file(def, ec) &&
                std::ranges::find(files, def, &PackageFile::source) == files.end())
                files.push_back({def, "reframework/data/remod_puppets/" + puppet + ".json"});
        }
    return files;
}

std::vector<std::string> check_cutscene(const std::string& json_text) {
    std::vector<std::string> p;
    const json c = json::parse(json_text, nullptr, false);
    if (c.is_discarded()) return {"not readable JSON"};
    if (!c.is_object()) return {"not a cutscene: a JSON object is expected"};
    if (c.value("schema_version", -1) != 0) p.push_back("schema_version must be 0");
    const double length = c.contains("length") && c["length"].is_number() ? c["length"].get<double>() : -1;
    if (length <= 0) p.push_back("length (seconds) must be a number above 0");
    if (c.contains("start")) {
        static const std::vector<std::string> keys{"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F11", "F12"};
        const json& s = c["start"];
        if (!s.is_object() || !s.contains("key") || !s["key"].is_string() ||
            std::ranges::find(keys, s["key"].get<std::string>()) == keys.end())
            p.push_back("start.key must be one of F1-F9, F11, F12 (F10 records camera keys)");
    }
    if (c.contains("letterbox") && !(c["letterbox"].is_number() && c["letterbox"].get<double>() >= 0 &&
                                     c["letterbox"].get<double>() < 0.5))
        p.push_back("letterbox must be a number from 0 to below 0.5 (each bar's share of the screen's height)");
    // The cutscene starts by itself when every condition its trigger names becomes true.
    if (c.contains("trigger")) {
        const json& t = c["trigger"];
        if (!t.is_object()) {
            p.push_back("trigger must be an object (near, talk, flags, after, stage, area, location, chapter, delay, once)");
        } else {
            bool any = false;
            if (t.contains("near")) {
                any = true;
                const json& n = t["near"];
                if (!n.is_object() || !n.contains("position") || !numbers(n["position"], 3))
                    p.push_back("trigger.near.position must be [x, y, z] (from Make a trigger here, never typed)");
                if (!n.is_object() || !n.contains("radius") || !n["radius"].is_number() || n["radius"].get<double>() <= 0)
                    p.push_back("trigger.near.radius must be a number of metres above 0");
            }
            for (const char* key : {"stage", "area", "location", "chapter"})
                if (t.contains(key)) {
                    any = true;
                    if (!t[key].is_string() || t[key].get<std::string>().empty())
                        p.push_back(std::string("trigger.") + key + " must be the game's name for it (as the menu's Now line shows)");
                }
            // Talking to a character: near one of that kind, a prompt shows; its key starts the cutscene.
            if (t.contains("talk")) {
                any = true;
                const json& k = t["talk"];
                if (!k.is_object() || !k.contains("npc") || !plain_name(k["npc"].is_string() ? k["npc"].get<std::string>() : ""))
                    p.push_back("trigger.talk.npc must be the character's kind, as the menu's Characters near Leon shows "
                                "(e.g. \"ch3_a8z0\")");
                if (k.is_object() && k.contains("key")) {
                    const std::string key = k["key"].is_string() ? k["key"].get<std::string>() : "";
                    const bool letter = key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z';
                    const bool fkey = key.size() >= 2 && key.size() <= 3 && key[0] == 'F' &&
                                      std::ranges::all_of(key.substr(1), [](unsigned char ch) { return std::isdigit(ch); }) &&
                                      std::stoi(key.substr(1)) >= 1 && std::stoi(key.substr(1)) <= 12 && key != "F10";
                    if (!letter && !fkey) p.push_back("trigger.talk.key must be a capital letter A-Z or F1-F12 (not F10)");
                }
                if (k.is_object() && k.contains("prompt") && !(k["prompt"].is_string() && !k["prompt"].get<std::string>().empty()))
                    p.push_back("trigger.talk.prompt must be text, e.g. \"Talk\"");
                if (k.is_object() && k.contains("radius") && !(k["radius"].is_number() && k["radius"].get<double>() > 0))
                    p.push_back("trigger.talk.radius must be a number of metres above 0");
            }
            // Story flags: each must be on ("Name") or off ("!Name"), by the game's names (the menu's Story flags).
            if (t.contains("flags")) {
                any = true;
                if (!t["flags"].is_array() || t["flags"].empty() ||
                    !std::ranges::all_of(t["flags"], [](const json& f) {
                        return f.is_string() && f.get<std::string>().size() > (f.get<std::string>().starts_with('!') ? 1u : 0u);
                    }))
                    p.push_back("trigger.flags must be a list of story flag names, \"!\" before one that must be off");
            }
            // Right after one of the game's movies or cutscenes ends.
            if (t.contains("after")) {
                any = true;
                const json& a = t["after"];
                const bool one = a.is_object() && a.size() == 1 && (a.contains("movie") || a.contains("event"));
                if (!one || !plain_name(a.begin()->is_string() ? a.begin()->get<std::string>() : ""))
                    p.push_back("trigger.after must be {\"movie\": id} or {\"event\": id}, the game's names (e.g. "
                                "\"mva000\", \"csa012\")");
            }
            if (!any) p.push_back("trigger needs a condition: near, talk, flags, after, stage, area, location or chapter");
            if (t.contains("delay") && !(t["delay"].is_number() && t["delay"].get<double>() >= 0))
                p.push_back("trigger.delay must be seconds, 0 or more");
            if (t.contains("once") && !t["once"].is_boolean() && t["once"] != "session")
                p.push_back("trigger.once must be true (once per save), \"session\" (once each time the game runs) or false "
                            "(every time; a talk trigger's default)");
        }
    }

    // Each list: entries with a time `t` (and `until`, for spans) inside the cutscene.
    const auto list = [&](const char* name) -> const json* {
        if (!c.contains(name)) return nullptr;
        if (!c[name].is_array()) {
            p.push_back(std::string(name) + " must be a list");
            return nullptr;
        }
        return &c[name];
    };
    const auto time_of = [&](const json& e, const std::string& where, const char* key) -> double {
        if (!e.contains(key) || !e[key].is_number() || e[key].get<double>() < 0) {
            p.push_back(where + ": " + key + " must be a number of seconds, 0 or more");
            return -1;
        }
        const double t = e[key].get<double>();
        if (length > 0 && t > length) p.push_back(where + ": " + key + " " + num(t) + " is past the length (" + num(length) + ")");
        return t;
    };
    const auto span = [&](const json& e, const std::string& where) {
        const double t = time_of(e, where, "t"), until = time_of(e, where, "until");
        if (t >= 0 && until >= 0 && until <= t) p.push_back(where + ": until " + num(until) + " must be after t " + num(t));
    };

    if (const json* keys = list("camera")) {
        double before = -1;
        for (size_t i = 0; i < keys->size(); ++i) {
            const json& k = (*keys)[i];
            const std::string where = "camera key " + std::to_string(i + 1);
            if (!k.is_object()) {
                p.push_back(where + " must be an object");
                continue;
            }
            const double t = time_of(k, where, "t");
            if (t >= 0 && before >= 0 && t <= before)
                p.push_back(where + ": t " + num(t) + " isn't after the key before it (" + num(before) + ")");
            if (t >= 0) before = t;
            if (!k.contains("position") || !numbers(k["position"], 3)) p.push_back(where + ": position must be [x, y, z]");
            if (!k.contains("rotation") || !numbers(k["rotation"], 4))
                p.push_back(where + ": rotation must be [x, y, z, w]");
            else if (!unit_rotation(k["rotation"]))
                p.push_back(where + ": rotation isn't a unit quaternion");
            if (k.contains("fov") && !(k["fov"].is_number() && k["fov"].get<double>() > 1 && k["fov"].get<double>() < 179))
                p.push_back(where + ": fov must be degrees, above 1 and below 179");
            if (k.contains("ease")) {
                const std::string e = k["ease"].is_string() ? k["ease"].get<std::string>() : "";
                if (e != "smooth" && e != "linear" && e != "cut") p.push_back(where + ": ease must be smooth, linear or cut");
            }
        }
    }
    if (const json* subs = list("subtitles"))
        for (size_t i = 0; i < subs->size(); ++i) {
            const json& s = (*subs)[i];
            const std::string where = "subtitle " + std::to_string(i + 1);
            span(s, where);
            if (!s.contains("text") || !s["text"].is_string()) p.push_back(where + ": text must be text");
        }
    if (const json* fades = list("fades"))
        for (size_t i = 0; i < fades->size(); ++i) {
            const json& f = (*fades)[i];
            const std::string where = "fade " + std::to_string(i + 1);
            span(f, where);
            for (const char* key : {"from", "to"})
                if (!f.contains(key) || !f[key].is_number() || f[key].get<double>() < 0 || f[key].get<double>() > 1)
                    p.push_back(where + ": " + key + " must be from 0 (clear) to 1 (black)");
        }
    // Other characters, as puppets built from a definition in remod_puppets (spikes/character_probe.md): where each
    // stands, from a written-down spot or an offset from the player.
    std::vector<std::string> actor_names;
    if (const json* actors = list("actors")) {
        std::vector<std::string> puppets;
        for (size_t i = 0; i < actors->size(); ++i) {
            const json& a = (*actors)[i];
            const std::string where = "actor " + std::to_string(i + 1);
            if (!a.is_object()) {
                p.push_back(where + " must be an object");
                continue;
            }
            const std::string name = a.contains("name") && a["name"].is_string() ? a["name"].get<std::string>() : "";
            if (!plain_name(name) || name == "player")
                p.push_back(where + ": name must be letters, digits or _ (not \"player\"): motions name the actor by it");
            else if (std::ranges::find(actor_names, name) != actor_names.end())
                p.push_back(where + ": name " + name + " is already another actor's");
            else
                actor_names.push_back(name);
            const std::string puppet = a.contains("puppet") && a["puppet"].is_string() ? a["puppet"].get<std::string>() : "";
            if (!plain_name(puppet))
                p.push_back(where + ": puppet must be a definition's name in remod_puppets, e.g. \"luis\"");
            else if (std::ranges::find(puppets, puppet) != puppets.end())
                p.push_back(where + ": puppet " + puppet + " is already another actor (one actor per puppet for now)");
            else
                puppets.push_back(puppet);
            if (a.contains("position") || a.contains("rotation")) {
                if (!a.contains("position") || !numbers(a["position"], 3))
                    p.push_back(where + ": position must be [x, y, z] (from Write down Leon's spot, never typed)");
                if (!a.contains("rotation") || !unit_rotation(a["rotation"]))
                    p.push_back(where + ": rotation must be a unit quaternion [x, y, z, w] (from Write down Leon's spot)");
                if (a.contains("offset")) p.push_back(where + ": give a position or an offset, not both");
            }
            if (a.contains("offset") && !numbers(a["offset"], 3))
                p.push_back(where + ": offset must be [right, up, forward], metres from the player");
            if (a.contains("hides") && a["hides"] != "partner")
                p.push_back(where + ": hides can only be \"partner\" (the real partner, hidden while it plays)");
        }
    }
    // Animation files put on a character as a bank of our own number (spikes/event_animation_test.md).
    if (const json* files = list("animation_files")) {
        std::vector<std::pair<std::string, long long>> used;  // (actor, bank)
        for (size_t i = 0; i < files->size(); ++i) {
            const json& a = (*files)[i];
            const std::string where = "animation file " + std::to_string(i + 1);
            if (!a.is_object()) {
                p.push_back(where + " must be an object");
                continue;
            }
            const std::string actor = a.contains("actor") && a["actor"].is_string() ? a["actor"].get<std::string>() : "player";
            if (actor != "player" && std::ranges::find(actor_names, actor) == actor_names.end())
                p.push_back(where + ": actor must be \"player\" or one of the actors' names");
            std::string file = a.contains("file") && a["file"].is_string() ? a["file"].get<std::string>() : "";
            std::string lower = file;
            std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            std::ranges::replace(lower, '\\', '/');
            if (!lower.ends_with(".motlist") || lower.starts_with("natives/") || lower.find(':') != std::string::npos)
                p.push_back(where + ": file must be a game path ending in .motlist, without natives/STM and without the "
                                    "number after it, e.g. _Chainsaw/Event/cs/csa012/csa012_s00/chara/cha300_00/cha300_00.motlist");
            if (!a.contains("bank") || !a["bank"].is_number_integer() || a["bank"].get<long long>() < 0 ||
                a["bank"].get<long long>() > 0xFFFFFFFFll) {
                p.push_back(where + ": bank must be a whole number, 0 or more (e.g. 9000: one of your own, not the character's)");
            } else if (const std::pair key{actor, a["bank"].get<long long>()}; std::ranges::find(used, key) != used.end()) {
                p.push_back(where + ": bank " + std::to_string(key.second) + " is already another animation file's for " +
                            (actor == "player" ? std::string("the player") : actor));
            } else {
                used.push_back(key);
            }
        }
    }
    if (const json* motions = list("motions"))
        for (size_t i = 0; i < motions->size(); ++i) {
            const json& m = (*motions)[i];
            const std::string where = "motion " + std::to_string(i + 1);
            time_of(m, where, "t");
            for (const char* key : {"bank", "motion"})
                if (!m.contains(key) || !m[key].is_number_integer() || m[key].get<long long>() < 0)
                    p.push_back(where + ": " + key + " must be a whole number, 0 or more");
            if (m.contains("actor") && m["actor"] != "player" &&
                !(m["actor"].is_string() && std::ranges::find(actor_names, m["actor"].get<std::string>()) != actor_names.end()))
                p.push_back(where + ": actor must be \"player\" or one of the actors' names");
        }
    // A game movie by its id's name (RE4R: chainsaw.MovieDefine.ID, e.g. "mva000") or a New movie's; the timeline
    // waits while it plays. A New sound's name: it plays from then on, beside the rest.
    for (const auto& [key, what, example] : {std::tuple{"movies", "movie", "the movie's name, e.g. \"mva000\""},
                                             std::tuple{"sounds", "sound", "a New sound block's name"}})
        if (const json* entries = list(key))
            for (size_t i = 0; i < entries->size(); ++i) {
                const json& m = (*entries)[i];
                const std::string where = std::string(what) + " " + std::to_string(i + 1);
                time_of(m, where, "t");
                const std::string id = m.contains("id") && m["id"].is_string() ? m["id"].get<std::string>() : "";
                if (id.empty() || !std::ranges::all_of(id, [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; }))
                    p.push_back(where + ": id must be " + example);
            }
    return p;
}

namespace {

double num_of(const json& e, const char* key, double fallback) {
    return e.contains(key) && e[key].is_number() ? e[key].get<double>() : fallback;
}
std::string text_of(const json& e, const char* key) {
    return e.contains(key) && e[key].is_string() ? e[key].get<std::string>() : "";
}
template <size_t N>
std::array<double, N> nums_of(const json& e, const char* key, std::array<double, N> fallback) {
    if (!e.contains(key) || !numbers(e[key], N)) return fallback;
    for (size_t i = 0; i < N; ++i) fallback[i] = e[key][i].get<double>();
    return fallback;
}
// The entries of a list, objects only.
std::vector<json> entries(const json& c, const char* key) {
    std::vector<json> out;
    if (c.contains(key) && c[key].is_array())
        for (const json& e : c[key])
            if (e.is_object()) out.push_back(e);
    return out;
}
const std::vector<std::string> kEdited{"schema_version", "name",    "length",          "start",     "letterbox",
                                       "camera",         "actors",  "animation_files", "motions",   "subtitles",
                                       "fades",          "movies",  "sounds"};

CutsceneCameraKey camera_key_of(const json& k) {
    CutsceneCameraKey x{.t = num_of(k, "t", 0)};
    x.position = nums_of(k, "position", x.position);
    x.rotation = nums_of(k, "rotation", x.rotation);
    x.fov = num_of(k, "fov", 0);
    x.ease = text_of(k, "ease");
    return x;
}

json read_json_file(const fs::path& file) {
    const json j = json::parse(read_text(file), nullptr, false);
    if (j.is_discarded() || !j.is_object())
        throw PackageError(file.filename().string() + " isn't readable JSON: fix it first (Open in editor)");
    return j;
}

}  // namespace

Cutscene read_cutscene(const fs::path& file) {
    std::error_code ec;
    Cutscene c;
    if (!fs::is_regular_file(file, ec)) {
        c.name = file.stem().string();
        c.kept = "{}";
        return c;
    }
    const json j = read_json_file(file);
    c.name = text_of(j, "name");
    c.length = num_of(j, "length", 5);
    if (j.contains("start") && j["start"].is_object()) c.start_key = text_of(j["start"], "key");
    c.letterbox = num_of(j, "letterbox", 0);
    for (const json& k : entries(j, "camera")) c.camera.push_back(camera_key_of(k));
    for (const json& a : entries(j, "actors")) {
        CutsceneActor x{.name = text_of(a, "name"), .puppet = text_of(a, "puppet")};
        x.at_spot = a.contains("position");
        x.position = nums_of(a, "position", x.position);
        x.rotation = nums_of(a, "rotation", x.rotation);
        x.offset = nums_of(a, "offset", x.offset);
        x.hides_partner = a.contains("hides") && a["hides"] == "partner";
        c.actors.push_back(x);
    }
    for (const json& m : entries(j, "motions")) {
        CutsceneMotion x{.t = num_of(m, "t", 0)};
        if (m.contains("actor") && m["actor"].is_string()) x.actor = m["actor"].get<std::string>();
        x.bank = static_cast<long long>(num_of(m, "bank", 0));
        x.motion = static_cast<long long>(num_of(m, "motion", 0));
        x.frame = num_of(m, "frame", 0);
        x.blend = num_of(m, "blend", 10);
        c.motions.push_back(x);
    }
    for (const json& a : entries(j, "animation_files")) {
        CutsceneAnimationFile x{.file = text_of(a, "file"), .bank = static_cast<long long>(num_of(a, "bank", 9000))};
        if (a.contains("actor") && a["actor"].is_string()) x.actor = a["actor"].get<std::string>();
        c.animation_files.push_back(x);
    }
    for (const json& s : entries(j, "subtitles"))
        c.subtitles.push_back({num_of(s, "t", 0), num_of(s, "until", 0), text_of(s, "text")});
    for (const json& f : entries(j, "fades"))
        c.fades.push_back({num_of(f, "t", 0), num_of(f, "until", 0), num_of(f, "from", 0), num_of(f, "to", 0)});
    for (const json& m : entries(j, "movies")) c.movies.push_back({num_of(m, "t", 0), text_of(m, "id")});
    for (const json& s : entries(j, "sounds")) c.sounds.push_back({num_of(s, "t", 0), text_of(s, "id")});
    c.trigger = j.contains("trigger");
    json kept = j;
    for (const auto& key : kEdited) kept.erase(key);
    c.kept = kept.dump();
    return c;
}

std::string cutscene_text(const Cutscene& c) {
    json kept = json::parse(c.kept.empty() ? "{}" : c.kept, nullptr, false);
    if (!kept.is_object()) kept = json::object();
    json j = {{"schema_version", 0}, {"name", c.name}, {"length", c.length}};
    if (!c.start_key.empty()) j["start"] = {{"key", c.start_key}};
    if (c.letterbox > 0) j["letterbox"] = c.letterbox;
    if (!c.camera.empty()) {
        j["camera"] = json::array();
        for (const auto& k : c.camera) {
            json x = {{"t", k.t}, {"position", k.position}, {"rotation", k.rotation}};
            if (k.fov > 0) x["fov"] = k.fov;
            if (!k.ease.empty()) x["ease"] = k.ease;
            j["camera"].push_back(x);
        }
    }
    if (!c.actors.empty()) {
        j["actors"] = json::array();
        for (const auto& a : c.actors) {
            json x = {{"name", a.name}, {"puppet", a.puppet}};
            if (a.at_spot) {
                x["position"] = a.position;
                x["rotation"] = a.rotation;
            } else {
                x["offset"] = a.offset;
            }
            if (a.hides_partner) x["hides"] = "partner";
            j["actors"].push_back(x);
        }
    }
    j["subtitles"] = json::array();
    for (const auto& s : c.subtitles) j["subtitles"].push_back({{"t", s.t}, {"until", s.until}, {"text", s.text}});
    j["fades"] = json::array();
    for (const auto& f : c.fades) j["fades"].push_back({{"t", f.t}, {"until", f.until}, {"from", f.from}, {"to", f.to}});
    if (!c.animation_files.empty()) {
        j["animation_files"] = json::array();
        for (const auto& a : c.animation_files) {
            json x = {{"file", a.file}, {"bank", a.bank}};
            if (a.actor != "player") x["actor"] = a.actor;
            j["animation_files"].push_back(x);
        }
    }
    j["motions"] = json::array();
    for (const auto& m : c.motions) {
        json x = {{"t", m.t}, {"bank", m.bank}, {"motion", m.motion}, {"frame", m.frame}, {"blend", m.blend}};
        if (m.actor != "player") x["actor"] = m.actor;
        j["motions"].push_back(x);
    }
    for (const auto& [key, cues] : {std::pair{"movies", &c.movies}, std::pair{"sounds", &c.sounds}})
        if (!cues->empty()) {
            j[key] = json::array();
            for (const auto& q : *cues) j[key].push_back({{"t", q.t}, {"id", q.id}});
        }
    for (auto it = kept.begin(); it != kept.end(); ++it) j[it.key()] = it.value();  // trigger, the rest
    return j.dump(2) + "\n";
}

void write_cutscene(const fs::path& file, const Cutscene& c) {
    std::error_code ec;
    const std::string text = cutscene_text(c);
    if (fs::is_regular_file(file, ec)) fs::copy_file(file, fs::path(file) += ".bak", fs::copy_options::overwrite_existing);
    if (file.has_parent_path()) fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
    if (!out.flush()) throw PackageError("couldn't write " + file.string());
}

Cutscene new_cutscene(const fs::path& file) {
    Cutscene c{.name = file.stem().string(), .length = 5, .start_key = "F5", .letterbox = 0.12};
    c.fades = {{.t = 0, .until = 1, .from = 1, .to = 0}, {.t = 4, .until = 5, .from = 0, .to = 1}};
    c.kept = "{}";
    return c;
}

bool is_cutscene_file(const fs::path& file) {
    std::error_code ec;
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (ext != ".json" || !fs::is_regular_file(file, ec) || fs::file_size(file, ec) > (1u << 20) || ec) return false;
    const json j = json::parse(read_text(file), nullptr, false);
    return !j.is_discarded() && j.is_object() && j.contains("schema_version") && j.contains("length") &&
           !j.contains("nodes");
}

namespace {

constexpr double kMinSpan = 0.05;

// The start and end of a ref's item, if it has one.
double* time_of(Cutscene& c, CutsceneItemRef r, double** until = nullptr) {
    const auto at = [&](auto& list) -> decltype(&list[0]) {
        return r.index >= 0 && r.index < int(list.size()) ? &list[r.index] : nullptr;
    };
    if (until) *until = nullptr;
    switch (r.lane) {
    case CutsceneLane::Camera:
        if (auto* k = at(c.camera)) return &k->t;
        break;
    case CutsceneLane::Fade:
        if (auto* f = at(c.fades)) {
            if (until) *until = &f->until;
            return &f->t;
        }
        break;
    case CutsceneLane::Subtitle:
        if (auto* s = at(c.subtitles)) {
            if (until) *until = &s->until;
            return &s->t;
        }
        break;
    case CutsceneLane::Motion:
        if (auto* m = at(c.motions)) return &m->t;
        break;
    case CutsceneLane::Movie:
        if (auto* q = at(c.movies)) return &q->t;
        break;
    case CutsceneLane::Sound:
        if (auto* q = at(c.sounds)) return &q->t;
        break;
    }
    return nullptr;
}

}  // namespace

std::vector<CutsceneTimelineLane> cutscene_lanes(const Cutscene& c) {
    std::vector<CutsceneTimelineLane> lanes;
    auto& cam = lanes.emplace_back(CutsceneTimelineLane{CutsceneLane::Camera, "", "camera", {}});
    for (int i = 0; i < int(c.camera.size()); ++i)
        cam.items.push_back({{CutsceneLane::Camera, i}, c.camera[i].t, std::nullopt,
                             "key " + std::to_string(i + 1) + (c.camera[i].ease.empty() ? "" : ", " + c.camera[i].ease)});
    auto& fades = lanes.emplace_back(CutsceneTimelineLane{CutsceneLane::Fade, "", "fades", {}});
    for (int i = 0; i < int(c.fades.size()); ++i)
        fades.items.push_back({{CutsceneLane::Fade, i}, c.fades[i].t, c.fades[i].until,
                               c.fades[i].to > c.fades[i].from ? "fade out" : "fade in"});
    auto& subs = lanes.emplace_back(CutsceneTimelineLane{CutsceneLane::Subtitle, "", "subtitles", {}});
    for (int i = 0; i < int(c.subtitles.size()); ++i)
        subs.items.push_back({{CutsceneLane::Subtitle, i}, c.subtitles[i].t, c.subtitles[i].until, c.subtitles[i].text});
    std::vector<std::string> who{"player"};
    for (const auto& a : c.actors) who.push_back(a.name);
    for (const auto& name : who) {
        auto& lane = lanes.emplace_back(CutsceneTimelineLane{CutsceneLane::Motion, name, name == "player" ? "Leon" : name, {}});
        for (int i = 0; i < int(c.motions.size()); ++i)
            if (c.motions[i].actor == name)
                lane.items.push_back({{CutsceneLane::Motion, i}, c.motions[i].t, std::nullopt,
                                      std::to_string(c.motions[i].bank) + " / " + std::to_string(c.motions[i].motion)});
    }
    for (const auto& [lane_kind, title, cues] : {std::tuple{CutsceneLane::Movie, "movies", &c.movies},
                                                std::tuple{CutsceneLane::Sound, "sounds", &c.sounds}}) {
        auto& lane = lanes.emplace_back(CutsceneTimelineLane{lane_kind, "", title, {}});
        for (int i = 0; i < int(cues->size()); ++i)
            lane.items.push_back({{lane_kind, i}, (*cues)[i].t, std::nullopt, (*cues)[i].id});
    }
    return lanes;
}

void set_item_time(Cutscene& c, CutsceneItemRef& ref, double t, std::optional<double> until) {
    double* end = nullptr;
    double* start = time_of(c, ref, &end);
    if (!start) return;
    const double length = std::max(c.length, kMinSpan);
    if (end) {
        double u = until.value_or(*end);
        u = std::clamp(u, kMinSpan, length);
        t = std::clamp(t, 0.0, u - kMinSpan);
        *start = t;
        *end = u;
    } else {
        *start = std::clamp(t, 0.0, length);
    }
    if (ref.lane == CutsceneLane::Camera) {  // keep the keys in time order; the ref follows its key
        const CutsceneCameraKey moved = c.camera[ref.index];
        std::ranges::stable_sort(c.camera, {}, &CutsceneCameraKey::t);
        ref.index = int(std::ranges::find(c.camera, moved) - c.camera.begin());
    }
}

CutsceneItemRef add_item(Cutscene& c, CutsceneLane lane, const std::string& actor, double t) {
    t = std::clamp(t, 0.0, std::max(c.length, 0.0));
    const double end = std::max(c.length, kMinSpan);
    switch (lane) {
    case CutsceneLane::Camera:
        return {};
    case CutsceneLane::Fade: {
        const bool fade_in = t < c.length / 2;
        const double from = std::min(t, end - kMinSpan);
        c.fades.push_back({.t = from, .until = std::min(from + 1, end), .from = fade_in ? 1.0 : 0.0, .to = fade_in ? 0.0 : 1.0});
        return {lane, int(c.fades.size()) - 1};
    }
    case CutsceneLane::Subtitle: {
        const double from = std::min(t, end - kMinSpan);
        c.subtitles.push_back({.t = from, .until = std::min(from + 2, end), .text = "..."});
        return {lane, int(c.subtitles.size()) - 1};
    }
    case CutsceneLane::Motion:
        c.motions.push_back({.t = t, .actor = actor.empty() ? "player" : actor, .bank = 1000, .motion = 160});
        return {lane, int(c.motions.size()) - 1};
    case CutsceneLane::Movie:
        c.movies.push_back({.t = t});
        return {lane, int(c.movies.size()) - 1};
    case CutsceneLane::Sound:
        c.sounds.push_back({.t = t});
        return {lane, int(c.sounds.size()) - 1};
    }
    return {};
}

void remove_item(Cutscene& c, CutsceneItemRef ref) {
    const auto erase = [&](auto& list) {
        if (ref.index >= 0 && ref.index < int(list.size())) list.erase(list.begin() + ref.index);
    };
    switch (ref.lane) {
    case CutsceneLane::Camera: erase(c.camera); break;
    case CutsceneLane::Fade: erase(c.fades); break;
    case CutsceneLane::Subtitle: erase(c.subtitles); break;
    case CutsceneLane::Motion: erase(c.motions); break;
    case CutsceneLane::Movie: erase(c.movies); break;
    case CutsceneLane::Sound: erase(c.sounds); break;
    }
}

CutsceneFrame cutscene_frame(const Cutscene& c, double t) {
    CutsceneFrame f;
    // As draw_overlays: every subtitle and fade whose span holds t, in file order (the last one drawn shows on top).
    for (const auto& s : c.subtitles)
        if (t >= s.t && t < s.until) f.subtitle = s.text;
    double clear = 1;
    for (const auto& x : c.fades)
        if (t >= x.t && t < x.until) {
            const double u = (t - x.t) / std::max(x.until - x.t, 1e-6);
            clear *= 1 - std::clamp(x.from + (x.to - x.from) * u, 0.0, 1.0);  // black layers over each other
        }
    f.black = 1 - clear;
    // As camera_at: the key a camera arrives at says how (smooth, linear, cut).
    const auto& keys = c.camera;
    if (!keys.empty()) {
        f.camera_from = f.camera_to = int(keys.size()) - 1;
        if (t <= keys.front().t) {
            f.camera_from = f.camera_to = 0;
        } else {
            for (size_t i = 0; i + 1 < keys.size(); ++i)
                if (t <= keys[i + 1].t) {
                    f.camera_from = int(i);
                    f.camera_to = int(i + 1);
                    double u = (t - keys[i].t) / std::max(keys[i + 1].t - keys[i].t, 1e-6);
                    const std::string& ease = keys[i + 1].ease;
                    if (ease.empty() || ease == "smooth") u = u * u * (3 - 2 * u);
                    else if (ease == "cut") u = 0;
                    f.camera_progress = u;
                    break;
                }
        }
    }
    std::vector<std::string> who{"player"};
    for (const auto& a : c.actors) who.push_back(a.name);
    for (const auto& name : who) {
        CutsceneFrame::Playing p{.actor = name};
        for (int i = 0; i < int(c.motions.size()); ++i) {
            const auto& m = c.motions[i];
            if (m.actor == name && m.t <= t && (p.motion < 0 || m.t >= c.motions[p.motion].t)) {
                p.motion = i;
                p.since = t - m.t;
            }
        }
        f.playing.push_back(p);
    }
    return f;
}

std::optional<Spot> read_spot(const fs::path& game_dir) {
    const json j = json::parse(read_text(game_dir / "reframework/data/remod_cutscenes/spot.json"), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("position") || !numbers(j["position"], 3) ||
        !j.contains("rotation") || !unit_rotation(j["rotation"]))
        return std::nullopt;
    return Spot{nums_of(j, "position", std::array<double, 3>{}), nums_of(j, "rotation", std::array<double, 4>{})};
}

std::optional<PickedAnimation> read_picked_animation(const fs::path& game_dir) {
    const json j = json::parse(read_text(game_dir / "reframework/data/remod_cutscenes/animation.json"), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("bank") || !j["bank"].is_number_integer() ||
        !j.contains("motion") || !j["motion"].is_number_integer())
        return std::nullopt;
    PickedAnimation p{.actor = text_of(j, "actor"), .bank = j["bank"].get<long long>(),
                      .motion = j["motion"].get<long long>(), .name = text_of(j, "name"), .frame = num_of(j, "frame", 0),
                      .file = text_of(j, "file")};
    if (p.actor.empty()) p.actor = "player";
    return p;
}

std::vector<std::string> puppet_names(const fs::path& game_dir, const fs::path& runtime) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const fs::path& dir : {runtime.empty() ? fs::path() : runtime / "puppets",
                                game_dir.empty() ? fs::path() : game_dir / "reframework/data/remod_puppets"}) {
        if (dir.empty() || !fs::is_directory(dir, ec)) continue;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".json" && plain_name(it->path().stem().string()))
                out.push_back(it->path().stem().string());
    }
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

namespace {

std::string lower_text(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

// The extracted file for a game path without its suffix (_Chainsaw/.../x.mesh -> natives\..\x.mesh.221108797),
// ignoring case; empty if there's none.
fs::path extracted(const fs::path& natives, const std::string& game_path) {
    const fs::path p = natives / fs::path(game_path).make_preferred();
    const std::string want = lower_text(p.filename().string()) + ".";
    std::error_code ec;
    for (auto it = fs::directory_iterator(p.parent_path(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string n = lower_text(it->path().filename().string());
        if (n.starts_with(want) && n.find('.', want.size()) == std::string::npos) return it->path();
    }
    return {};
}

// `path` with its folder `from` (a whole segment, any case) as `to`.
std::string with_folder(const std::string& path, const std::string& from, const std::string& to) {
    const std::string low = lower_text(path), seg = "/" + lower_text(from) + "/";
    const size_t at = low.find(seg);
    return at == std::string::npos ? path : path.substr(0, at + 1) + to + path.substr(at + 1 + from.size());
}

// A texture recoloured, written over a copy of the original (the built-in converter keeps its format and mips).
void recolour_texture(const fs::path& from, const fs::path& to, const NewCharacter& spec, const Profile& profile,
                      ITextureConverter& converter) {
    Bgra image = decode_tex(from, 1u << 30);
    colourize(image, spec.hue, spec.saturation);
    if (spec.brightness != 0 || spec.contrast != 0) adjust_colour(image, 0, 0, spec.brightness, spec.contrast);
    fs::create_directories(to.parent_path());
    const fs::path png = fs::path(to) += ".png";
    save_png(png, image);
    std::error_code ec;
    fs::remove(to, ec);
    try {
        converter.save_tex(png, from, to, profile);
    } catch (...) {
        fs::remove(png, ec);
        throw;
    }
    fs::remove(png, ec);
}

}  // namespace

std::vector<PackageFile> make_new_character(const NewCharacter& spec, const fs::path& natives, const Profile& profile,
                                            ITextureConverter& converter, const fs::path& out, std::string* report) {
    if (!plain_name(spec.name)) throw PackageError("the name must be letters and digits, e.g. rmc001");
    const json def = read_json_file(spec.definition);
    json made = def;
    json* part = nullptr;
    std::string parts;
    if (made.contains("parts") && made["parts"].is_array())
        for (json& p : made["parts"]) {
            parts += (parts.empty() ? "" : ", ") + text_of(p, "name");
            if (text_of(p, "name") == spec.part) part = &p;
        }
    if (!part) throw PackageError("the character has no part \"" + spec.part + "\" (its parts: " + parts + ")");
    const std::string mesh = text_of(*part, "mesh");
    std::string material = text_of(*part, "material");
    const std::string folder = fs::path(mesh).parent_path().parent_path().filename().string();
    if (folder.empty() || folder.size() != spec.name.size())
        throw PackageError("the name must be " + std::to_string(folder.size()) + " letters or digits, as long as " +
                           folder + " (the folder of " + mesh + " it takes the place of)");
    std::string notes;
    const fs::path mesh_file = extracted(natives, mesh);
    if (mesh_file.empty()) throw PackageError(mesh + " isn't in the game files (" + natives.string() + ")");
    fs::path mdf2 = extracted(natives, material);
    if (mdf2.empty()) {  // e.g. a costume variant from a later pak, not extracted: the mesh's own material
        const fs::path own = fs::path(mesh).replace_extension(".mdf2");
        notes += "; " + fs::path(material).filename().string() + " isn't in the game files, so " +
                 own.filename().string() + " is used";
        material = own.generic_string();
        mdf2 = extracted(natives, material);
        if (mdf2.empty()) throw PackageError(material + " isn't in the game files (" + natives.string() + ")");
    }

    // The material's texture paths (UTF-16, 2-byte aligned) under the folder, colour ones (_alb...) only.
    std::string bytes = read_text(mdf2);
    std::u16string text(bytes.size() / 2, u'\0');
    std::memcpy(text.data(), bytes.data(), text.size() * 2);
    const auto path_char = [](char16_t ch) {
        return ch < 128 && (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '/' || ch == '.' || ch == '-');
    };
    std::map<std::string, std::vector<size_t>> found;  // texture path -> where it starts, each time
    const std::string seg = "/" + lower_text(folder) + "/";
    for (size_t i = 0; i < text.size(); ++i) {
        if (!path_char(text[i]) || (i > 0 && path_char(text[i - 1]))) continue;
        size_t end = i;
        while (end < text.size() && path_char(text[end])) ++end;
        std::string path(end - i, ' ');
        for (size_t k = i; k < end; ++k) path[k - i] = static_cast<char>(text[k]);
        const std::string low = lower_text(path);
        const std::string file = fs::path(path).filename().string();
        if (low.ends_with(".tex") && low.find(seg) != std::string::npos && lower_text(file).find("_alb") != std::string::npos)
            found[path].push_back(i);
        i = end;
    }
    size_t recoloured = 0, streaming = 0;
    std::string skipped;
    std::vector<PackageFile> files;
    for (const auto& [path, starts] : found) {
        const std::string file = fs::path(path).filename().string();
        const fs::path base = extracted(natives, path);
        if ((spec.skip && spec.skip(file)) || base.empty()) {
            skipped += (skipped.empty() ? "" : ", ") + file + (base.empty() ? " (not in the game files)" : "");
            continue;
        }
        const std::string game = with_folder(path, folder, spec.name) + base.extension().string();
        recolour_texture(base, out / game, spec, profile, converter);
        files.push_back({out / game, game});
        if (const fs::path big = extracted(natives / "streaming", path); !big.empty()) {
            recolour_texture(big, out / "streaming" / game, spec, profile, converter);
            files.push_back({out / "streaming" / game, "streaming/" + game});
            ++streaming;
        }
        ++recoloured;
        for (const size_t at : starts) {  // repointed: the folder segment, in place
            const size_t f = lower_text(path).find(seg) + 1;
            for (size_t k = 0; k < spec.name.size(); ++k) {
                const char16_t ch = static_cast<unsigned char>(spec.name[k]);
                std::memcpy(bytes.data() + 2 * (at + f + k), &ch, 2);
            }
        }
    }
    if (recoloured == 0)
        throw PackageError("no colour texture (_alb...) of " + spec.part + " under " + folder + " to recolour" +
                           (skipped.empty() ? "" : " (left as they are: " + skipped + ")"));

    const std::string new_mesh = with_folder(mesh, folder, spec.name), new_material = with_folder(material, folder, spec.name);
    const std::string mesh_game = new_mesh + mesh_file.extension().string(), mdf2_game = new_material + mdf2.extension().string();
    fs::create_directories((out / mesh_game).parent_path());
    fs::copy_file(mesh_file, out / mesh_game, fs::copy_options::overwrite_existing);
    {
        std::ofstream o(out / mdf2_game, std::ios::binary);
        o << bytes;
        if (!o.flush()) throw PackageError("couldn't write " + (out / mdf2_game).string());
    }
    files.push_back({out / mesh_game, mesh_game});
    files.push_back({out / mdf2_game, mdf2_game});

    (*part)["mesh"] = new_mesh;
    (*part)["material"] = new_material;
    made["name"] = spec.name;
    const std::string def_game = "reframework/data/remod_puppets/" + spec.name + ".json";
    fs::create_directories((out / def_game).parent_path());
    {
        std::ofstream o(out / def_game, std::ios::binary);
        o << made.dump(2) << "\n";
        if (!o.flush()) throw PackageError("couldn't write " + (out / def_game).string());
    }
    files.push_back({out / def_game, def_game});
    if (report)
        *report = spec.name + ": " + spec.part + " (" + fs::path(mesh).filename().string() + ") copied, " +
                  std::to_string(recoloured) + " colour texture(s) recoloured (" + std::to_string(streaming) +
                  " with their streaming copies)" + (skipped.empty() ? "" : "; left as they are: " + skipped) + notes;
    return files;
}

namespace {

// A recording's camera keys (F10), or why not.
json recorded_keys(const fs::path& recording) {
    std::error_code ec;
    if (!fs::is_regular_file(recording, ec))
        throw PackageError("no recording yet: in game, press F10 at each shot (" + recording.string() + ")");
    const json rec = json::parse(read_text(recording), nullptr, false);
    if (rec.is_discarded() || !rec.contains("camera") || !rec["camera"].is_array() || rec["camera"].empty())
        throw PackageError("the recording has no camera keys: " + recording.string());
    return rec["camera"];
}

// A trigger the runtime made (Make a trigger here), or why not.
json made_trigger(const fs::path& trigger) {
    std::error_code ec;
    if (!fs::is_regular_file(trigger, ec))
        throw PackageError("no trigger yet: in game, stand where it should start and click Make a trigger here in "
                           "REFramework's menu (remod cutscenes) (" + trigger.string() + ")");
    const json t = json::parse(read_text(trigger), nullptr, false);
    if (t.is_discarded() || !t.is_object())
        throw PackageError("not a trigger the runtime made: " + trigger.string());
    if (t.empty())
        throw PackageError("the trigger has no conditions yet: in game, add some in REFramework's menu (remod cutscenes): "
                           "Make a trigger here, Talk trigger, a story flag, Start after it");
    return t;
}

}  // namespace

void apply_recording(Cutscene& c, const fs::path& recording) {
    c.camera.clear();
    for (const json& k : recorded_keys(recording))
        if (k.is_object()) c.camera.push_back(camera_key_of(k));
    if (!c.camera.empty()) c.length = std::max(c.length, c.camera.back().t + 1.0);  // a second on the last shot
}

void apply_trigger(Cutscene& c, const fs::path& trigger) {
    json kept = json::parse(c.kept.empty() ? "{}" : c.kept, nullptr, false);
    if (!kept.is_object()) kept = json::object();
    kept["trigger"] = made_trigger(trigger);
    c.kept = kept.dump();
    c.trigger = true;
}

void use_recording(const fs::path& cutscene, const fs::path& recording) {
    std::error_code ec;
    const json keys = recorded_keys(recording);
    const json rec = {{"camera", keys}};
    const double last = rec["camera"].back().value("t", 0.0);
    json c;
    if (fs::is_regular_file(cutscene, ec)) {
        c = json::parse(read_text(cutscene), nullptr, false);
        if (c.is_discarded() || !c.is_object())
            throw PackageError(cutscene.filename().string() + " isn't readable JSON: fix it first (Open in editor)");
        fs::copy_file(cutscene, fs::path(cutscene) += ".bak", fs::copy_options::overwrite_existing);
    } else {
        c = {{"schema_version", 0}, {"name", cutscene.stem().string()}, {"subtitles", json::array()},
             {"fades", json::array()}, {"motions", json::array()}};
    }
    c["camera"] = rec["camera"];
    c["length"] = std::max(c.value("length", 0.0), last + 1.0);  // a second on the last shot
    if (cutscene.has_parent_path()) fs::create_directories(cutscene.parent_path());
    std::ofstream out(cutscene, std::ios::binary);
    out << c.dump(2) << "\n";
    if (!out.flush()) throw PackageError("couldn't write " + cutscene.string());
}

void use_trigger(const fs::path& cutscene, const fs::path& trigger) {
    std::error_code ec;
    const json t = made_trigger(trigger);
    json c;
    if (fs::is_regular_file(cutscene, ec)) {
        c = json::parse(read_text(cutscene), nullptr, false);
        if (c.is_discarded() || !c.is_object())
            throw PackageError(cutscene.filename().string() + " isn't readable JSON: fix it first (Open in editor)");
        fs::copy_file(cutscene, fs::path(cutscene) += ".bak", fs::copy_options::overwrite_existing);
    } else {
        c = {{"schema_version", 0}, {"name", cutscene.stem().string()}, {"length", 5.0}, {"subtitles", json::array()},
             {"fades", json::array()}, {"motions", json::array()}};
    }
    c["trigger"] = t;
    if (cutscene.has_parent_path()) fs::create_directories(cutscene.parent_path());
    std::ofstream out(cutscene, std::ios::binary);
    out << c.dump(2) << "\n";
    if (!out.flush()) throw PackageError("couldn't write " + cutscene.string());
}

}  // namespace remod
