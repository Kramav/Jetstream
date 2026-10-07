#include "cutscene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

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
    return {cutscene_runtime(runtime), {cutscene, "reframework/data/remod_cutscenes/" + cutscene.filename().string()}};
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
            if (!k.contains("rotation") || !numbers(k["rotation"], 4)) {
                p.push_back(where + ": rotation must be [x, y, z, w]");
            } else {
                double len = 0;
                for (const auto& v : k["rotation"]) len += v.get<double>() * v.get<double>();
                if (len < 0.5 || len > 1.5) p.push_back(where + ": rotation isn't a unit quaternion");
            }
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
    if (const json* motions = list("motions"))
        for (size_t i = 0; i < motions->size(); ++i) {
            const json& m = (*motions)[i];
            const std::string where = "motion " + std::to_string(i + 1);
            time_of(m, where, "t");
            for (const char* key : {"bank", "motion"})
                if (!m.contains(key) || !m[key].is_number_integer() || m[key].get<long long>() < 0)
                    p.push_back(where + ": " + key + " must be a whole number, 0 or more");
            if (m.contains("actor") && m["actor"] != "player") p.push_back(where + ": actor can only be \"player\" for now");
        }
    return p;
}

void use_recording(const fs::path& cutscene, const fs::path& recording) {
    std::error_code ec;
    if (!fs::is_regular_file(recording, ec))
        throw PackageError("no recording yet: in game, press F10 at each shot (" + recording.string() + ")");
    const json rec = json::parse(read_text(recording), nullptr, false);
    if (rec.is_discarded() || !rec.contains("camera") || !rec["camera"].is_array() || rec["camera"].empty())
        throw PackageError("the recording has no camera keys: " + recording.string());
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

}  // namespace remod
