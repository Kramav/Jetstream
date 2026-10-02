#include "node_run.hpp"
#include "custom.hpp"

#include "image.hpp"
#include "package.hpp"
#include "process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <tuple>

namespace remod {

namespace fs = std::filesystem;

namespace {

using enum PortType;

// ---- Shared by several node types ----

// A path as typed or pasted: surrounding spaces and quotes (Explorer's "Copy as path") dropped, relative to
// `base_dir`, made absolute, separators made '\', "." and ".." resolved. Empty if `text` is.
fs::path clean_path(std::string text, const fs::path& base_dir) {
    const auto junk = [](char c) { return c == '"' || std::isspace(static_cast<unsigned char>(c)); };
    while (!text.empty() && junk(text.front())) text.erase(text.begin());
    while (!text.empty() && junk(text.back())) text.pop_back();
    if (text.empty()) return {};
    return fs::absolute(base_dir / fs::path(text)).lexically_normal();
}

// For file system calls on a clean_path: long paths get the \\?\ prefix that lifts Windows' 260-character limit (the
// app has no longPathAware manifest). From 248, the limit for a folder. Never shown to users.
fs::path long_path(const fs::path& p) {
    const std::wstring& s = p.native();
    if (s.size() < 248 || s.starts_with(LR"(\\?\)")) return p;
    if (s.starts_with(LR"(\\)")) return LR"(\\?\UNC\)" + s.substr(2);  // \\server\share\...
    return LR"(\\?\)" + s;
}

// The file a file step writes: `dest`, or `dest\<source's name>` if `dest` is a folder (an existing one, or typed
// with a trailing separator). Shared by the run and the editor's warning.
fs::path destination_for(const fs::path& source, const fs::path& dest) {
    if (!dest.has_filename() || fs::is_directory(long_path(dest))) return dest / source.filename();
    return dest;
}

// The folder holding <natives root> and the path below it, if `file` is inside one (case-insensitive).
std::optional<std::pair<fs::path, std::string>> split_natives(const fs::path& file, const std::string& natives_root) {
    auto lower = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const fs::path normal = file.lexically_normal();
    const std::vector<fs::path> parts(normal.begin(), normal.end());
    std::vector<std::string> root;
    for (const auto& p : fs::path(natives_root).lexically_normal()) root.push_back(lower(p.string()));
    if (root.empty()) return std::nullopt;
    for (size_t i = 0; i + root.size() < parts.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < root.size() && match; ++k) match = lower(parts[i + k].string()) == root[k];
        if (!match) continue;
        fs::path dir;
        for (size_t k = 0; k < i + root.size(); ++k) dir /= parts[k];
        std::string rest;
        for (size_t k = i + root.size(); k < parts.size(); ++k) rest += (rest.empty() ? "" : "/") + parts[k].string();
        return std::pair{dir, rest};
    }
    return std::nullopt;
}

// ---- The node types, one function each: its spec and what it does in a run ----

NodeSpec load_tex() {
    return {
        .type = "LoadTex",
        .title = "Original texture",
        .summary = "Picks the game's original .tex. Nothing is converted here: its size and format are read so the "
                   "later steps can match them.",
        .inputs = {{.name = "tex", .label = "Texture file", .type = Tex, .widget = Widget::Path, .required = true,
                    .hint = "The original texture, e.g. from your REtool folder. Named .tex or .tex.<version> - "
                            "either works; the game is detected from the file itself.",
                    .path = PathKind::OpenTexture},
                   {.name = "game_path", .label = "In-game path", .type = Text, .widget = Widget::Text,
                    .hint = "Where the texture lives in the game, under natives/STM. Leave empty if the file is "
                            "inside a natives\\STM\\... folder: it's worked out automatically. The .tex version "
                            "suffix is added for you."}},
        .outputs = {{"tex", Tex, "texture"}},
        .family = Family::Source,
        .run = [](NodeRun& r) {
            const fs::path tex = r.resolve(r.text("tex"));
            const TexMeta m = read_tex_meta(tex, r.profile());
            std::string game_path = r.text("game_path");
            const auto natives = split_natives(tex, r.profile().natives_root);
            if (game_path.empty() && natives) game_path = natives->second;
            r.output("tex", file_value(tex, game_path));
            r.done(tex.filename().string() + " " + std::to_string(m.width) + "x" + std::to_string(m.height) + " " +
                       m.format + ", " + std::to_string(m.mip_count) + " mips",
                   tex);
            if (natives && !natives->second.starts_with("streaming/") &&
                fs::exists(natives->first / "streaming" / natives->second))
                r.log("note: the game also has a high-resolution streaming/" + natives->second +
                      "; if your change doesn't show in game, that copy may need replacing too (CLAUDE.md §9)");
        },
        .preview = [](NodeRun& r) {
            r.output("tex", file_value(r.resolve(r.text("tex"))));
        },
    };
}

// Files in folder's patterns: "*" any run of characters, "?" one; ";" or "," between several; ignoring case. Empty:
// every texture (is_tex_name).
bool name_matches(const std::string& name, const std::string& patterns) {
    auto low = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string n = low(name);
    if (patterns.find_first_not_of(" ;,") == std::string::npos) return is_tex_name(n);
    std::function<bool(size_t, const std::string&, size_t)> match = [&](size_t i, const std::string& p, size_t j) {
        if (j == p.size()) return i == n.size();
        if (p[j] == '*') return match(i, p, j + 1) || (i < n.size() && match(i + 1, p, j));
        return i < n.size() && (p[j] == '?' || p[j] == n[i]) && match(i + 1, p, j + 1);
    };
    std::string list = low(patterns);
    std::ranges::replace(list, ';', ',');
    std::istringstream items(list);
    for (std::string pattern; std::getline(items, pattern, ',');) {
        pattern.erase(0, pattern.find_first_not_of(' '));
        pattern.erase(pattern.find_last_not_of(' ') + 1);
        if (!pattern.empty() && match(0, pattern, 0)) return true;
    }
    return false;
}

// An item's {name}: a texture's name before ".tex" (x.tex.143221013 -> x), else the name without its extension.
std::string item_name(const fs::path& file) {
    std::string name = file.filename().string(), low = name;
    std::ranges::transform(low, low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return is_tex_name(low) ? name.substr(0, low.rfind(".tex")) : file.stem().string();
}

NodeSpec files_in_folder() {
    return {
        .type = "FilesInFolder",
        .title = "Files in folder",
        .summary = "Every matching file in a folder, as a list: each block it feeds runs once per file. Put {name} "
                   "in their file names (e.g. edits\\{name}.png) so each file gets its own. A block taking many "
                   "(Package's textures) gets them all.",
        .inputs = {{.name = "folder", .label = "Folder", .type = Folder, .widget = Widget::Path, .required = true,
                    .hint = "Where the files are, e.g. a natives\\STM\\... folder of your REtool files.",
                    .path = PathKind::Folder},
                   {.name = "pattern", .label = "Files", .type = Text, .widget = Widget::Text,
                    .hint = "Which files: * stands for anything, ; between several, e.g. *_iam.tex* or *.png;*.tga. "
                            "Empty: every texture."},
                   {.name = "subfolders", .label = "Include subfolders", .type = Text, .widget = Widget::Checkbox},
                   {.name = "on_fail", .label = "If a file fails", .type = Text, .widget = Widget::Choice,
                    .required = true,
                    .hint = "Stop the run, or leave that file out and go on with the rest (it's listed as failed).",
                    .initial = "stop", .options = {{"stop", "Stop the run"}, {"skip", "Skip that file"}}}},
        .outputs = {{.name = "files", .type = Any, .label = "each file", .list = true}},
        .family = Family::Source,
        .pure = true,
        .run = [](NodeRun& r) {
            const fs::path folder = r.resolve(r.text("folder"));
            std::error_code ec;
            if (!fs::is_directory(folder, ec)) throw GraphError("no folder " + folder.string());
            std::vector<fs::path> files;
            auto take = [&](const fs::directory_entry& e) {
                if (e.is_regular_file(ec) && name_matches(e.path().filename().string(), r.text("pattern")))
                    files.push_back(e.path());
            };
            if (r.text("subfolders") == "true")
                for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
                     !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                    take(*it);
            else
                for (const auto& e : fs::directory_iterator(folder, ec)) take(e);
            if (files.empty()) throw GraphError("no files in " + folder.string() + " match");
            std::vector<std::pair<std::string, fs::path>> keyed;  // sorted by their path in the folder, ignoring case
            for (const auto& f : files) keyed.emplace_back(f.lexically_relative(folder).generic_string(), f);
            std::ranges::sort(keyed, [](const auto& a, const auto& b) {
                return std::lexicographical_compare(a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
                                                    [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
            });
            std::vector<Value> values;
            std::vector<ListItem> items;
            for (const auto& [key, f] : keyed) {
                const auto natives = split_natives(f, r.profile().natives_root);
                values.push_back(file_value(f, natives ? natives->second : ""));
                items.push_back({item_name(f), key});
            }
            r.output_list("files", std::move(values), std::move(items));
        },
    };
}

// Whether an Export image's file is for the user to edit: it goes (through Splits) to an Edit image step. Otherwise it's
// a working file (e.g. for Replace photo; user, 2026-10-02) that nobody edits: exported again every run.
// A file's last write time as text ("" if it isn't there), to notice a file changed outside the tool.
std::string write_time(const fs::path& file) {
    std::error_code ec;
    const auto t = fs::last_write_time(file, ec);
    return ec ? std::string() : std::to_string(t.time_since_epoch().count());
}

bool for_editing(const Graph& g, int id) {
    for (const Link& l : g.links) {
        if (l.from_node != id) continue;
        const Node* to = g.find(l.to_node);
        if (to && (to->type == "EditImage" || (to->type == "Split" && for_editing(g, to->id)))) return true;
    }
    return false;
}

NodeSpec export_image() {
    return {
        .type = "ExportImage",
        .title = "Export image",
        .summary = "Converts the texture to an image file: PNG, TGA or JPG, whichever the file name ends in. Going to "
                   "an Edit image step, a file already there (your edited version) is kept, unless the texture has "
                   "changed since; otherwise it's a working file, exported again every run.",
        .inputs = {{.name = "tex", .label = "texture", .type = Tex, .required = true}},
        .outputs = {{.name = "png", .type = Image, .label = "image", .field = "png", .field_label = "Image file",
                     .hint = "Where to write the image. Its ending picks the format: .png, .tga (e.g. for GIMP) or "
                             ".jpg. JPG loses some quality and all transparency.",
                     .path = PathKind::SaveFile, .filter = kEditImageFormats}},
        .state = {"exported_from", "exported_time"},  // the texture its file is from, the file's write time
        .family = Family::Transform,
        .run = [](NodeRun& r) {
            const Value tex = r.input("tex");
            const fs::path png = r.resolve(r.text("png"));
            r.claim(png);  // each item its own file, kept or not
            // A file for editing is kept, unless it's from another texture (user, 2026-10-02: overwrite it; the old
            // one would go into the mod). No record (older graphs, the CLI): it's taken to be from this texture.
            // A working file is kept only while it's the one exported last from this texture (user: no needless
            // writes): same texture, the file's write time unchanged.
            const std::string *from = r.state("exported_from"), *at = r.state("exported_time");
            const bool editing = for_editing(r.run.graph, r.node.id);
            const bool stale = from && *from != tex.path.string();
            const bool untouched = from && !stale && at && *at == write_time(png);
            if (!fs::exists(png) || (editing ? stale : !untouched)) r.change(ChangeKind::Write, png);
            if (fs::exists(png) && (editing ? stale : !untouched)) {
                if (editing) r.log(png.filename().string() + " is from another texture (" + *from + "): exporting again");
                fs::remove(png);
            }
            if (!fs::exists(png)) {
                r.run.options.converter.load_tex(tex.path, png, r.profile());
                r.run.fresh_exports.insert({r.node.id, r.run.item});
                r.done("exported " + png.filename().string(), png);
            } else {
                r.done((editing ? "kept your " : "kept ") + png.filename().string(), png);
            }
            r.set_state("exported_from", tex.path.string());
            r.set_state("exported_time", write_time(png));
            r.output("png", file_value(png, tex.game_path));
        },
        .preview = [](NodeRun& r) {
            r.input("tex");  // known only if its texture is
            r.change(ChangeKind::Write, r.resolve(r.text("png")));
            r.output("png", file_value(r.resolve(r.text("png"))));
        },
    };
}

NodeSpec edit_image() {
    return {
        .type = "EditImage",
        .title = "Edit image",
        .summary = "YOUR STEP: open the image in your image editor, change it, save it (same size and format), then "
                   "click Done editing. The run waits here until you do.",
        .inputs = {{.name = "png", .label = "image to edit", .type = Image, .required = true},
                   {.name = "editor", .label = "Open with", .type = Path, .widget = Widget::Path,
                    .hint = "The program Open in editor uses, e.g. C:\\Program Files\\GIMP 3\\bin\\gimp.exe. "
                            "Empty: Windows' editor for the image's file type. Link a Value to use one for every "
                            "Edit image block.",
                    .path = PathKind::OpenFile, .filter = "exe"}},
        .outputs = {{"image", Image, "edited image"}},
        .state = {"done"},
        .manual = true,
        .family = Family::Manual,
        .run = [](NodeRun& r) {
            const Value png = r.input("png");
            const Graph& g = r.run.graph;
            const bool re_exported =
                r.run.fresh_exports.contains({g.links[g.links_into(r.node.id, "png").at(0)].from_node, r.run.item});
            if (re_exported) {  // a new image: an earlier "done" doesn't count
                r.set_state("done", "");
                if (!r.item()) r.run.result.reset_edits.push_back(r.node.id);
            }
            const std::string* flag = r.state("done");
            if (r.run.options.edits_done || (!re_exported && flag && *flag == "true")) {
                r.output("image", png);
                r.done("edited " + png.path.filename().string(), png.path);
            } else {
                r.wait("edit " + png.path.filename().string() + ", then click Done editing", png.path);
                r.log("waiting for you to edit " + png.path.string());
            }
        },
        .preview = [](NodeRun& r) {
            r.output("image", r.input("png"));
        },
    };
}

NodeSpec import_image() {
    return {
        .type = "ImportImage",
        .title = "Use existing image",
        .summary = "Uses an image you've already edited (or any image, e.g. for the preview), instead of exporting one.",
        .inputs = {{.name = "png", .label = "Image file", .type = Image, .widget = Widget::Path, .required = true,
                    .hint = "A PNG, TGA or JPG. For a texture it must be the same size as the original.",
                    .path = PathKind::OpenFile, .filter = kEditImageFormats}},
        .outputs = {{"image", Image, "image"}},
        .family = Family::Source,
        .run = [](NodeRun& r) {
            const fs::path png = r.resolve(r.text("png"));
            if (!fs::is_regular_file(png)) throw GraphError("image not found: " + png.string());
            r.output("image", file_value(png));
            r.done("using " + png.filename().string(), png);
        },
        .preview = [](NodeRun& r) {
            r.output("image", file_value(r.resolve(r.text("png"))));
        },
    };
}

std::string file_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw GraphError("can't read " + file.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

// A run cache file's name: the bytes it was made from, hashed.
// ponytail: std::hash (MSVC: 64-bit FNV-1a); accidental collisions are negligible at a cache's size. A SHA-256
// (BCrypt) if a cache ever holds millions.
std::string hash_hex(const std::string& bytes) {
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(std::hash<std::string>{}(bytes)));
    return hex;
}

// The texture made from this image and original, by this converter (and its options for this game).
std::string cache_name(const fs::path& image, const fs::path& original, const Profile& p, const std::string& converter) {
    return hash_hex(file_bytes(image) + '\0' + file_bytes(original) + '\0' + converter) + ".tex." + p.tex_suffix;
}

// Oldest first (a reuse touches its file) until the cache is under its limit; `keep` (just written) stays.
void prune_cache(const fs::path& dir, const fs::path& keep) {
    constexpr std::uintmax_t limit = 512ull << 20;  // ponytail: fixed; a setting if someone needs more
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::uintmax_t total = 0;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) {
            files.emplace_back(e.last_write_time(ec), e.path());
            total += e.file_size(ec);
        }
    std::ranges::sort(files);
    for (const auto& [time, file] : files) {
        if (total <= limit) break;
        if (file == keep) continue;
        const std::uintmax_t size = fs::file_size(file, ec);
        if (fs::remove(file, ec)) total -= size;
    }
}

NodeSpec save_tex() {
    return {
        .type = "SaveTex",
        .title = "Convert image to texture",
        .summary = "Turns the edited image back into a game texture with the original's size, format and mipmaps.",
        .inputs = {{.name = "image", .label = "edited image", .type = Image, .required = true},
                   {.name = "original", .label = "original texture", .type = Tex, .required = true}},
        .outputs = {{"tex", Tex, "new texture"}},
        .family = Family::Transform,
        .run = [](NodeRun& r) {
            const Value original = r.input("original");
            const fs::path image = r.input("image").path, &cache = r.run.options.cache_dir;
            // A texture converted before from the same bytes is reused (user, 2026-10-02: an unchanged run writes
            // nothing; Noesis also takes seconds). Written as .part, then renamed, so a cut-off write is never reused.
            const fs::path out = cache.empty()
                                     ? r.temp_file(".tex." + r.profile().tex_suffix)
                                     : cache / cache_name(image, original.path, r.profile(),
                                                                  r.run.options.converter.id(r.profile()));
            std::error_code ec;
            TexMeta m;
            if (!cache.empty() && fs::is_regular_file(out, ec)) {
                m = read_tex_meta(out, r.profile());
                fs::last_write_time(out, fs::file_time_type::clock::now(), ec);  // recently used: pruned last
                r.output("tex", file_value(out, original.game_path));
                r.done("unchanged: reused the texture converted before (" + m.format + ", " +
                       std::to_string(m.mip_count) + " mips)");
            } else {
                fs::path target = out;
                if (!cache.empty()) {
                    fs::create_directories(cache);
                    target += ".part";
                    fs::remove(target, ec);
                }
                m = r.run.options.converter.save_tex(image, original.path, target, r.profile());
                if (target != out) {
                    fs::rename(target, out);
                    prune_cache(cache, out);
                }
                r.output("tex", file_value(out, original.game_path));
                r.done("encoded " + m.format + ", " + std::to_string(m.mip_count) + " mips");
            }
            // Waiting (CLAUDE.md §9): whether the game minds a different mip count is untested, so say so.
            const std::uint32_t original_mips = read_tex_meta(original.path, r.profile()).mip_count;
            if (m.mip_count != original_mips)
                r.warn(original.path.filename().string() + " has " + std::to_string(original_mips) +
                       " mip level(s), the new texture has " + std::to_string(m.mip_count) +
                       " (the Noesis converter writes them down to 8x8). It may work in game; if the texture looks wrong or "
                       "the game misbehaves, this is the likely cause.");
        },
    };
}

// ---- Image blocks: Adjust colour, Resize image, Overlay image. Each writes a PNG: to its Save to if set (then that's
// its output, kept), else to the run's temporary folder (gone after the run). ----

InputSpec image_input(const char* name, const char* label, const char* hint) {
    return {.name = name, .label = label, .type = Image, .widget = Widget::Path, .required = true, .hint = hint,
            .path = PathKind::OpenFile, .filter = kEditImageFormats};
}

InputSpec advanced(InputSpec in) {
    in.advanced = true;
    return in;
}

InputSpec number_input(const char* name, const char* label, const char* hint, const char* initial, float min,
                       float max, const char* format, const char* zero = nullptr) {
    return {.name = name, .label = label, .type = Text, .widget = Widget::Number, .hint = hint, .initial = initial,
            .min = min, .max = max, .format = format, .zero = zero};
}

InputSpec save_to_input() {
    return {.name = "save_to", .label = "Save to", .type = Path, .widget = Widget::Path,
            .hint = "Optional: keep the result as this PNG (written again each run); the block's output is then this "
                    "file. Empty: a temporary file, gone after the run.",
            .path = PathKind::SaveFile, .filter = "png", .result = "image"};
}

// Where an image block writes: its Save to (folders made), else the run's temporary folder.
fs::path image_out(NodeRun& r) {
    const std::string to = r.text("save_to");
    if (to.empty()) return r.temp_file(".png");
    const fs::path out = clean_path(to, r.run.options.base_dir);
    r.change(ChangeKind::Write, out);
    if (out.has_parent_path()) fs::create_directories(long_path(out.parent_path()));
    return out;
}

// A Number field's value: empty is 0; else a number in the field's range (InputSpec::min / max), or nullopt.
std::optional<float> parse_number(const InputSpec& in, std::string t) {
    t.erase(0, t.find_first_not_of(' '));
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.empty()) return 0.0f;
    try {
        size_t used = 0;
        const float v = std::stof(t, &used);
        if (used == t.size() && v >= in.min && v <= in.max) return v;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

float number(const NodeRun& r, const char* input) {
    const InputSpec& in = *find_input(*find_spec(r.node.type), input);
    const std::string t = r.text(input);
    if (const auto v = parse_number(in, t)) return *v;
    throw GraphError(std::string(in.label) + " must be a number from " + std::to_string(int(in.min)) + " to " +
                     std::to_string(int(in.max)) + ", not '" + t + "'");
}

// Replace photo's Zoom and Picture X / Y as a Framing (percentages; an empty Zoom, as in older graphs, is 100%).
}  // namespace

bool at_initial(const InputSpec& in, const std::string& value) {
    if (in.widget == Widget::Checkbox) return (value == "true") == (std::string_view(in.initial) == "true");
    if (in.widget == Widget::Number) {
        const auto v = parse_number(in, value), d = parse_number(in, in.initial);
        return v && d && *v == *d;
    }
    return value == in.initial;
}

namespace {

Framing framing_from(float zoom, float x, float y) { return {zoom > 0 ? zoom / 100 : 1, x / 100, y / 100}; }
Framing framing_of(const NodeRun& r) {
    return framing_from(number(r, "zoom"), number(r, "picture_x"), number(r, "picture_y"));
}

void write_image(NodeRun& r, const Bgra& image, const std::string& what) {
    const bool kept = !r.text("save_to").empty();
    const fs::path& cache = r.run.options.cache_dir;
    if (!kept && !cache.empty()) {
        // A temporary result goes to the run cache, named by its pixels: the same result is the file already there.
        const std::string name = hash_hex(std::string(reinterpret_cast<const char*>(image.pixels.data()),
                                                      image.pixels.size()) +
                                          std::to_string(image.width) + "x" + std::to_string(image.height));
        const fs::path out = cache / (name + ".png");
        if (std::error_code ec; fs::is_regular_file(out, ec)) {
            fs::last_write_time(out, fs::file_time_type::clock::now(), ec);
        } else {
            fs::create_directories(cache);
            const fs::path part = cache / (name + ".part.png");
            save_png(part, image);
            fs::rename(part, out);
            prune_cache(cache, out);
        }
        r.output("image", file_value(out));
        r.done(what, out);
        return;
    }
    const fs::path out = image_out(r);
    // Save to is written only when the result differs from what's there (user, 2026-10-02: no needless write cycles;
    // reading it back costs no wear).
    bool same = false;
    if (std::error_code ec; kept && fs::is_regular_file(out, ec)) try {
            const Bgra old = load_image(out);
            same = old.width == image.width && old.height == image.height && old.pixels == image.pixels;
        } catch (const std::exception&) {  // unreadable: written again
        }
    if (!same) save_png(out, image);
    r.output("image", file_value(out));
    r.done(what + (!kept ? "" : (same ? ", " : ", saved ") + out.filename().string() + (same ? " unchanged" : "")), out);
}

// Before a run: the result's path is known only when it's kept (Save to).
void image_preview(NodeRun& r) {
    const std::string to = r.text("save_to");
    if (to.empty()) throw GraphError("a temporary file, made in the run");
    r.change(ChangeKind::Write, clean_path(to, r.run.options.base_dir));
    r.output("image", file_value(clean_path(to, r.run.options.base_dir)));
}

NodeSpec adjust_colour_node() {
    return {
        .type = "AdjustColour",
        .title = "Adjust colour",
        .summary = "Shifts an image's hue, saturation, brightness and contrast. Only the colour changes: the alpha "
                   "channel (often other data in RE textures) is kept as it is.",
        .inputs = {image_input("image", "Image", "The image to change, e.g. Export image's."),
                   number_input("hue", "Hue", "Degrees to turn the colours, -180 to 180. 0: as is.", "0", -180, 180,
                                "%.0f deg"),
                   number_input("saturation", "Saturation", "-100 (grey) to 100. 0: as is.", "0", -100, 100, "%.0f%%"),
                   number_input("brightness", "Brightness", "-100 to 100. 0: as is.", "0", -100, 100, "%.0f%%"),
                   number_input("contrast", "Contrast", "-100 (flat) to 100. 0: as is.", "0", -100, 100, "%.0f%%"),
                   save_to_input()},
        .outputs = {{"image", Image, "image"}},
        .family = Family::Transform,
        .thumbnail = true,
        .run = [](NodeRun& r) {
            Bgra image = load_image(clean_path(r.text("image"), r.run.options.base_dir));
            adjust_colour(image, number(r, "hue"), number(r, "saturation") / 100, number(r, "brightness") / 100,
                          number(r, "contrast") / 100);
            write_image(r, image, "adjusted");
        },
        .preview = image_preview,
    };
}

NodeSpec resize_image_node() {
    return {
        .type = "ResizeImage",
        .title = "Resize image",
        .summary = "Scales an image to a size, or to the size of another image or texture (e.g. a new picture to the "
                   "original texture's size).",
        .inputs = {image_input("image", "Image", "The image to resize."),
                   number_input("width", "Width", "Pixels. 0 (auto): from Height, keeping the shape.", "0", 0, 16384,
                                "%.0f px", "auto"),
                   number_input("height", "Height", "Pixels. 0 (auto): from Width, keeping the shape.", "0", 0, 16384,
                                "%.0f px", "auto"),
                   {.name = "match", .label = "Match size of", .type = Path, .widget = Widget::Path,
                    .hint = "Optional: an image or texture whose size to use (wins over Width and Height). Link "
                            "Original texture to fit a picture to the texture it replaces.",
                    .path = PathKind::OpenFile},
                   {.name = "fit", .label = "Fit", .type = Text, .widget = Widget::Choice, .required = true,
                    .hint = "How the shape is kept when the sizes' proportions differ.", .initial = "fit",
                    .options = {{"fit", "Fit inside (bars)"}, {"fill", "Fill (crop)"}, {"stretch", "Stretch"}}},
                   save_to_input()},
        .outputs = {{"image", Image, "image"}},
        .family = Family::Transform,
        .thumbnail = true,
        .run = [](NodeRun& r) {
            const Bgra image = load_image(clean_path(r.text("image"), r.run.options.base_dir));
            unsigned w = 0, h = 0;
            if (const std::string match = r.text("match"); !match.empty()) {
                const fs::path file = clean_path(match, r.run.options.base_dir);
                std::string name = file.filename().string();
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (name.find(".tex") != std::string::npos) {
                    const TexMeta m = read_tex_meta(file, r.profile());
                    w = m.width;
                    h = m.height;
                } else {
                    const Bgra other = load_image(file);
                    w = other.width;
                    h = other.height;
                }
            } else {
                w = unsigned(number(r, "width"));
                h = unsigned(number(r, "height"));
                if (!w && !h) throw GraphError("set Width and/or Height, or Match size of");
                if (!w) w = std::max(1u, unsigned(std::lround(double(h) * image.width / image.height)));
                if (!h) h = std::max(1u, unsigned(std::lround(double(w) * image.height / image.width)));
            }
            const std::string fit = r.text("fit");
            write_image(r, resize_image(image, w, h, fit == "fill" ? Fit::Fill : fit == "stretch" ? Fit::Stretch : Fit::Fit),
                        "resized to " + std::to_string(w) + "x" + std::to_string(h));
        },
        .preview = image_preview,
    };
}

NodeSpec overlay_image_node() {
    return {
        .type = "OverlayImage",
        .title = "Overlay image",
        .summary = "Puts one image on top of another, e.g. a picture or logo on a UI plate. The base image's alpha "
                   "(transparency, or other data in RE textures) is kept.",
        .inputs = {image_input("base", "Base image", "The image underneath; the result has its size."),
                   image_input("top", "Top image", "The image put on top (its own transparency counts)."),
                   number_input("x", "X", "Pixels from the base's left edge to the top image's left edge.", "0",
                                -16384, 16384, "%.0f px"),
                   number_input("y", "Y", "Pixels from the base's top edge to the top image's top edge.", "0", -16384,
                                16384, "%.0f px"),
                   number_input("opacity", "Opacity", "0 to 100 percent.", "100", 0, 100, "%.0f%%"),
                   save_to_input()},
        .outputs = {{"image", Image, "image"}},
        .family = Family::Transform,
        .thumbnail = true,
        .run = [](NodeRun& r) {
            Bgra base = load_image(clean_path(r.text("base"), r.run.options.base_dir));
            const Bgra top = load_image(clean_path(r.text("top"), r.run.options.base_dir));
            overlay_image(base, top, int(number(r, "x")), int(number(r, "y")), number(r, "opacity") / 100);
            write_image(r, base, "overlaid");
        },
        .preview = image_preview,
    };
}

// A picture on the graph (user, 2026-10-02: "place the image preview output onto my graph directly"): whatever is
// linked in, an image or a texture (or a path to one), shown as large as Size. Changes nothing.
NodeSpec preview_node() {
    return {
        .type = "Preview",
        .title = "Preview",
        .summary = "Shows an image or texture on the graph, as large as you like; it follows the values live. Changes "
                   "nothing. Click the picture to enlarge it. To preview something that also goes on to a step, put a "
                   "Split on its link.",
        .inputs = {{.name = "in", .label = "picture", .type = Any},
                   advanced(number_input("size", "Size", "How large the picture shows on the graph, in pixels at "
                                         "100% zoom.", "320", 120, 1600, "%.0f px"))},
        .family = Family::Output,
        .pure = true,
        .thumbnail = true,
        .view_size = "size",
        .run = [](NodeRun& r) {
            r.done(r.run.graph.links_into(r.node.id, "in").empty() ? "nothing linked in" : "shown on the graph");
        },
    };
}

NodeSpec replace_photo_node() {
    return {
        .type = "ReplacePhoto",
        .title = "Replace photo",
        .summary = "Puts a picture in place of the photo in a frame (e.g. RE4R's framed photos): it finds the photo's "
                   "edge, fills it end to end and carries the old photo's ageing over: its tone, the frame's shadow, "
                   "its stains, and if wanted its scratches.",
        .inputs = {image_input("frame", "Frame image", "The framed photo, e.g. Export image of the frame's texture."),
                   image_input("picture", "Picture", "The new picture. It's cropped to fill the photo's area."),
                   advanced(number_input("frame_width", "Frame width",
                                "Where the old photo starts: its distance in from the frame's outer outline, in "
                                "pixels. Larger moves the edge inwards (a smaller picture). 0 (auto): found, the "
                                "innermost ring all the way round. To nudge the found edge, use Grow instead.",
                                "0", 0, 4096, "%.0f px", "auto")),
                   advanced(number_input("grow", "Grow", "Pixels to move the found edge outwards (the picture covers "
                                         "more of the frame); negative moves it in (shows more of the old photo).",
                                         "0", -40, 40, "%.0f px")),
                   advanced(number_input("feather", "Feather", "Softens the edge, in pixels.", "1", 0, 20, "%.0f px")),
                   advanced(number_input("zoom", "Zoom", "Zooms into the picture: 100 just fills the photo's area end "
                                         "to end, 200 shows half as much of it.", "100", 100, 400, "%.0f%%")),
                   advanced(number_input("picture_x", "Picture X", "Which part of the picture shows across: -100 its "
                                         "left edge, 100 its right edge, 0 the middle. Matters once it's wider than "
                                         "the area (or zoomed).", "0", -100, 100, "%.0f%%")),
                   advanced(number_input("picture_y", "Picture Y", "Which part of the picture shows up and down: -100 "
                                         "its top, 100 its bottom, 0 the middle (e.g. -40 keeps heads in).", "0",
                                         -100, 100, "%.0f%%")),
                   advanced(number_input("tone", "Match tone", "The old photo's brightness, contrast and colour cast.",
                                         "100", 0, 100, "%.0f%%")),
                   advanced(number_input("shading", "Shading", "The old photo's darkening towards its edges: the "
                                         "frame's shadow.", "100", 0, 100, "%.0f%%")),
                   advanced(number_input("stains", "Stains", "The old photo's stains: colour its toning doesn't "
                                         "explain, darkening the new picture.", "100", 0, 100, "%.0f%%")),
                   advanced(number_input("detail", "Scratches", "The old photo's scratches and specks (thin marks "
                                         "only, not its picture's outlines).", "0", 0, 100, "%.0f%%")),
                   {.name = "show_outline", .label = "Show edge", .type = Text, .widget = Widget::Checkbox,
                    .hint = "Draws the photo's edge as found on the preview (not in the result).", .advanced = true},
                   save_to_input()},
        .outputs = {{"image", Image, "image"}},
        .family = Family::Transform,
        .thumbnail = true,
        .run = [](NodeRun& r) {
            const Bgra frame = load_image(clean_path(r.text("frame"), r.run.options.base_dir));
            const Bgra picture = load_image(clean_path(r.text("picture"), r.run.options.base_dir));
            const PhotoArea area = photo_area(frame, number(r, "frame_width"), number(r, "grow"), number(r, "feather"));
            const Ageing ageing{number(r, "tone") / 100, number(r, "shading") / 100, number(r, "stains") / 100,
                                number(r, "detail") / 100};
            write_image(r, replace_photo(frame, picture, area.mask, ageing, 1, framing_of(r)),
                        "photo replaced (frame width " + std::to_string(int(area.frame_width)) + " px)");
        },
        .preview = image_preview,
    };
}

NodeSpec package_mod() {
    return {
        .type = "PackageMod",
        .title = "Package for Fluffy",
        .summary = "Builds the mod folder and a .zip to add in Fluffy Mod Manager. Connect as many textures as the mod "
                   "replaces; previews are combined into the one image Fluffy shows.",
        .inputs = {{.name = "tex", .label = "texture", .type = Tex, .required = true, .multiple = true,
                    .hint = "Each new texture in the mod. A new line appears as you connect one."},
                   {.name = "preview", .label = "preview", .type = Image, .multiple = true,
                    .hint = "Images for Fluffy's preview, e.g. the edited images. Several are tiled into one picture."},
                   {.name = "name", .label = "Mod name", .type = Text, .widget = Widget::Text, .required = true,
                    .hint = "Shown in Fluffy; also the folder and .zip name."},
                   {.name = "out", .label = "Output folder", .type = Folder, .widget = Widget::Path, .required = true,
                    .hint = "Where <Mod name>\\ and <Mod name>.zip are created.", .path = PathKind::Folder,
                    .result = "mod"},
                   {.name = "version", .label = "Version", .type = Text, .widget = Widget::Text,
                    .hint = "Shown in Fluffy."},
                   {.name = "author", .label = "Author", .type = Text, .widget = Widget::Text,
                    .hint = "Shown in Fluffy."},
                   {.name = "description", .label = "Description", .type = Text, .widget = Widget::Text,
                    .hint = "Shown in Fluffy."},
                   {.name = "replace", .label = "Replace existing", .type = Text, .widget = Widget::Checkbox,
                    .hint = "Overwrite this mod's previous folder and .zip in the output folder. Only output this tool "
                            "made for the same mod name is replaced."}},
        .outputs = {{"mod", Path, "built mod"}},
        .family = Family::Output,
        .run = [](NodeRun& r) {
            PackageSpec spec{.mod_name = r.text("name"),
                             .out_dir = r.resolve(r.text("out")),
                             .info = {.name = r.text("name"),
                                      .version = r.text("version"),
                                      .description = r.text("description"),
                                      .author = r.text("author")},
                             .replace = r.text("replace") == "true"};
            for (const auto& tex : r.values("tex")) {
                if (tex.game_path.empty())
                    throw GraphError("game path unknown for " + tex.path.filename().string() +
                                     ": fill in 'In-game path' on its Original texture node (the .tex isn't inside a " +
                                     r.profile().natives_root + " folder)");
                spec.files.push_back({tex.path, tex.game_path});
            }
            std::vector<fs::path> previews;
            for (const auto& v : r.values("preview")) previews.push_back(v.path);
            // One preview is used as it is, except a TGA: Fluffy's TGA support is unconfirmed ([guide]), so it goes
            // through tile_images like several previews do, which writes a PNG.
            if (previews.size() == 1 && !has_extension(previews[0].string(), "tga")) {
                spec.screenshot = previews[0];
            } else if (!previews.empty()) {
                // Its file name is the mod's screenshot name (a repeated Package writes it again for each item).
                spec.screenshot = r.run.work_dir / "preview.png";
                tile_images(previews, spec.screenshot);
                if (previews.size() > 1) r.log("combined " + std::to_string(previews.size()) + " previews");
            }
            r.change(ChangeKind::Write, spec.out_dir / spec.mod_name);  // the mod folder and its .zip
            r.change(ChangeKind::Write, (spec.out_dir / spec.mod_name) += ".zip");
            bool unchanged = false;
            const fs::path root = build_package(r.profile(), spec, &unchanged);
            r.output("mod", file_value(fs::path(root) += ".zip"));
            r.done(unchanged ? root.filename().string() + ".zip unchanged (the same build is there)"
                             : "packaged " + std::to_string(spec.files.size()) + " texture(s) into " +
                                   root.filename().string() + ".zip",
                   fs::path(root) += ".zip");
        },
        .preview = [](NodeRun& r) {
            const std::string name = r.text("name");
            if (name.empty()) throw GraphError("no mod name yet");
            r.change(ChangeKind::Write, r.resolve(r.text("out")) / name);
            r.change(ChangeKind::Write, (r.resolve(r.text("out")) / name) += ".zip");
            r.output("mod", file_value(fs::absolute(r.resolve(r.text("out")) / name) += ".zip"));  // as build_package
        },
    };
}

NodeSpec text_node() {
    return {
        .type = "Text",
        .title = "Text",
        .summary = "A piece of text to feed into any field, e.g. one mod name used in several places. {1}, {2}, ... "
                   "are replaced by whatever is connected to the numbered inputs.",
        .inputs = {{.name = "text", .label = "Text", .type = Text, .widget = Widget::Text,
                    .hint = "The text. Use {1}, {2}, ... to insert the connected inputs, e.g. \"{1} v2\"."},
                   {.name = "parts", .label = "part", .type = Text, .multiple = true,
                    .hint = "Values for {1}, {2}, ... in connection order. Files give their full path."}},
        .outputs = {{"text", Text, "text"}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            std::vector<std::string> parts;
            for (const auto& v : r.values("parts")) parts.push_back(v.text);
            const std::string out = fill_template(r.text("text"), parts);
            r.output("text", {out, {}, {}});
            r.done("\"" + out + "\"");
        },
    };
}

// ---- File steps: Copy, Move, Rename share their checks, Overwrite mode and the destination-exists warning ----

InputSpec if_exists_input() {
    return {.name = "if_exists", .label = "Overwrite mode", .type = Text, .widget = Widget::Choice, .required = true,
            .hint = "What to do when the destination file is already there. Skip keeps it and passes it on.",
            .initial = "fail",
            .options = {{"fail", "Fail if exists"}, {"overwrite", "Overwrite"}, {"skip", "Skip if exists"}}};
}

InputSpec create_dirs_input() {
    return {.name = "create_dirs", .label = "Create folders", .type = Text, .widget = Widget::Checkbox,
            .hint = "Create the destination's folder (and any above it) if it doesn't exist.", .initial = "true"};
}

enum class Transfer { Copy, Move, Rename };

// Puts `source` at `target`, copying or moving it, and passes `target` on as "path". The target is checked here, at
// run time, whatever the editor warned earlier (destination_warnings): Overwrite mode decides, and the log says what
// happened to an existing one.
void transfer(NodeRun& r, const fs::path& source, const fs::path& target, Transfer how) {
    const NodeSpec& spec = *find_spec(r.node.type);
    const std::string mode = r.text("if_exists");
    if (!is_option(*find_input(spec, "if_exists"), mode))  // a linked value can be anything
        throw GraphError("Overwrite mode can't be '" + mode + "'");
    if (!fs::exists(long_path(source)))
        throw GraphError("source file not found: " + source.string() +
                         (how == Transfer::Copy ? "" : " (moved by an earlier run?)"));
    if (fs::is_directory(long_path(source)))
        throw GraphError(source.string() + " is a folder; " + spec.title + " handles single files");
    std::error_code ec;
    if (fs::equivalent(long_path(source), long_path(target), ec))
        throw GraphError("source and destination are the same file: " + target.string());
    const bool exists = fs::exists(long_path(target));
    if (exists)
        r.log("warning: destination exists: " + target.string() + " (" +
              (mode == "overwrite" ? "overwritten" : mode == "skip" ? "skipped" : "failed") + ")");
    if (exists && mode == "fail")
        throw GraphError("destination exists: " + target.string() + " (choose Overwrite or Skip if exists to allow that)");
    if (exists && mode == "skip") {
        r.output("path", file_value(target));
        r.done("kept existing " + target.filename().string(), target);
        return;
    }
    r.change(ChangeKind::Write, target);
    if (how != Transfer::Copy) r.change(ChangeKind::Remove, source);
    try {
        if (find_input(spec, "create_dirs") && r.text("create_dirs") == "true")
            fs::create_directories(long_path(target.parent_path()));
        else if (!fs::is_directory(long_path(target.parent_path())))
            throw GraphError("destination folder doesn't exist: " + target.parent_path().string() +
                             (find_input(spec, "create_dirs") ? " (tick Create folders)" : ""));
        if (how != Transfer::Copy) fs::rename(long_path(source), long_path(target), ec);
        if (how == Transfer::Copy || ec)  // a move that couldn't rename (e.g. another drive): copy, then remove below
            fs::copy_file(long_path(source), long_path(target), fs::copy_options::overwrite_existing);
    } catch (const fs::filesystem_error& e) {
        throw GraphError("can't write " + target.string() + ": " + e.code().message());
    }
    if (how != Transfer::Copy && fs::exists(long_path(source)) && !fs::remove(long_path(source), ec))
        throw GraphError("copied to " + target.string() + " but couldn't remove the original: " + ec.message());
    r.output("path", file_value(target));
    const char* verb = how == Transfer::Copy ? "copied to " : how == Transfer::Move ? "moved to " : "renamed to ";
    r.done(verb + (how == Transfer::Rename ? target.filename().string() : target.string()), target);
}

// Copy's and Move's outputs and changes before a run (Rename has its own target).
void transfer_preview(NodeRun& r, Transfer how) {
    const fs::path source = clean_path(r.text("source"), r.run.options.base_dir);
    const fs::path target = destination_for(source, clean_path(r.text("dest"), r.run.options.base_dir));
    r.change(ChangeKind::Write, target);
    if (how != Transfer::Copy) r.change(ChangeKind::Remove, source);
    r.output("path", file_value(target));
}

NodeSpec copy_file() {
    return {
        .type = "CopyFile",
        .title = "Copy file",
        .summary = "Copies one file, e.g. into a mod folder or a backup. Gives the copy's full path to later steps.",
        .inputs = {{.name = "source", .label = "Source file", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "The file to copy (not a folder). A path pasted with quotes, from Explorer's \"Copy as "
                            "path\", is fine.",
                    .path = PathKind::OpenFile},
                   {.name = "dest", .label = "Destination", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "A file path, or a folder: the copy then keeps the source's name. End a folder that "
                            "doesn't exist yet with \\.",
                    .path = PathKind::SaveFile, .result = "path"},
                   if_exists_input(),
                   create_dirs_input()},
        .outputs = {{"path", Path, "copied path"}},
        .family = Family::File,
        .run = [](NodeRun& r) {
            const fs::path source = clean_path(r.text("source"), r.run.options.base_dir);
            transfer(r, source, destination_for(source, clean_path(r.text("dest"), r.run.options.base_dir)),
                     Transfer::Copy);
        },
        .preview = [](NodeRun& r) { transfer_preview(r, Transfer::Copy); },
    };
}

NodeSpec move_file() {
    return {
        .type = "MoveFile",
        .title = "Move file",
        .summary = "Moves one file to another folder or name. Once moved, a second run finds no source and stops, so "
                   "it suits one-off tidying more than a graph you run again and again.",
        .inputs = {{.name = "source", .label = "Source file", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "The file to move (not a folder).", .path = PathKind::OpenFile},
                   {.name = "dest", .label = "Destination", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "A file path, or a folder: the file then keeps its name. End a folder that doesn't exist "
                            "yet with \\.",
                    .path = PathKind::SaveFile, .result = "path"},
                   if_exists_input(),
                   create_dirs_input()},
        .outputs = {{"path", Path, "moved path"}},
        .family = Family::File,
        .run = [](NodeRun& r) {
            const fs::path source = clean_path(r.text("source"), r.run.options.base_dir);
            transfer(r, source, destination_for(source, clean_path(r.text("dest"), r.run.options.base_dir)),
                     Transfer::Move);
        },
        .preview = [](NodeRun& r) { transfer_preview(r, Transfer::Move); },
    };
}

NodeSpec rename_file() {
    return {
        .type = "RenameFile",
        .title = "Rename file",
        .summary = "Gives one file a new name in the same folder.",
        .inputs = {{.name = "source", .label = "File", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "The file to rename (not a folder).", .path = PathKind::OpenFile},
                   {.name = "name", .label = "New name", .type = Text, .widget = Widget::Text, .required = true,
                    .hint = "Just the name, with its extension, e.g. preview_old.png. No folders.",
                    .result = "path"},
                   if_exists_input()},
        .outputs = {{"path", Path, "renamed path"}},
        .family = Family::File,
        .run = [](NodeRun& r) {
            const fs::path source = clean_path(r.text("source"), r.run.options.base_dir);
            const fs::path name(r.text("name"));
            if (name.has_parent_path() || name.has_root_path() || name == "." || name == "..")
                throw GraphError("New name must be just a name, not a path: " + name.string());
            transfer(r, source, source.parent_path() / name, Transfer::Rename);
        },
        .preview = [](NodeRun& r) {
            const fs::path source = clean_path(r.text("source"), r.run.options.base_dir);
            r.change(ChangeKind::Write, source.parent_path() / r.text("name"));
            r.change(ChangeKind::Remove, source);
            r.output("path", file_value(source.parent_path() / r.text("name")));
        },
    };
}

// Into the Recycle Bin, without asking; Windows only asks if the drive has none (it would delete for good).
void recycle(const fs::path& file) {
    if (file.native().size() >= MAX_PATH)
        throw GraphError(file.string() + " is too long a path for the Recycle Bin; untick To Recycle Bin to delete it");
    std::wstring from = file.native();
    from.push_back(L'\0');  // the list ends with two nulls
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted)
        throw GraphError("couldn't move " + file.string() + " to the Recycle Bin");
}

NodeSpec delete_file() {
    return {
        .type = "DeleteFile",
        .title = "Delete file",
        .summary = "Deletes one file, into the Recycle Bin unless you untick it. Never a folder.",
        .inputs = {{.name = "source", .label = "File", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "The file to delete.", .path = PathKind::OpenFile},
                   {.name = "recycle", .label = "To Recycle Bin", .type = Text, .widget = Widget::Checkbox,
                    .hint = "Into the Recycle Bin, so it can be restored. Untick to delete it for good.",
                    .initial = "true"},
                   {.name = "missing_ok", .label = "Missing is fine", .type = Text, .widget = Widget::Checkbox,
                    .hint = "If the file isn't there, carry on (it's already gone). Untick to stop the run instead.",
                    .initial = "true"}},
        .outputs = {},
        .family = Family::File,
        .run = [](NodeRun& r) {
            const fs::path file = clean_path(r.text("source"), r.run.options.base_dir);
            if (!fs::exists(long_path(file))) {
                if (r.text("missing_ok") != "true") throw GraphError("file to delete not found: " + file.string());
                r.done("already gone: " + file.filename().string(), file);
                return;
            }
            if (fs::is_directory(long_path(file))) throw GraphError(file.string() + " is a folder; Delete file deletes files");
            r.change(ChangeKind::Remove, file);
            if (r.text("recycle") == "true") {
                recycle(file);
                r.done("moved to the Recycle Bin: " + file.filename().string(), file);
            } else {
                std::error_code ec;
                if (!fs::remove(long_path(file), ec)) throw GraphError("can't delete " + file.string() + ": " + ec.message());
                r.done("deleted " + file.filename().string(), file);
            }
        },
        .preview = [](NodeRun& r) { r.change(ChangeKind::Remove, clean_path(r.text("source"), r.run.options.base_dir)); },
    };
}

NodeSpec make_folder() {
    return {
        .type = "MakeFolder",
        .title = "Make folder",
        .summary = "Makes a folder (and any above it) if it isn't there yet, and passes it on.",
        .inputs = {{.name = "folder", .label = "Folder", .type = Folder, .widget = Widget::Path, .required = true,
                    .hint = "The folder to make. Already there is fine.",
                    .path = PathKind::Folder, .result = "folder"}},
        .outputs = {{"folder", Folder, "folder"}},
        .family = Family::File,
        .run = [](NodeRun& r) {
            const fs::path folder = clean_path(r.text("folder"), r.run.options.base_dir);
            const bool existed = fs::exists(long_path(folder));
            if (existed && !fs::is_directory(long_path(folder)))
                throw GraphError(folder.string() + " is a file, not a folder");
            if (!existed) r.change(ChangeKind::MakeFolder, folder);
            std::error_code ec;
            if (!existed && !fs::create_directories(long_path(folder), ec))
                throw GraphError("can't make " + folder.string() + ": " + ec.message());
            r.output("folder", file_value(folder));
            r.done((existed ? "already there: " : "made ") + folder.string(), folder);
        },
        .preview = [](NodeRun& r) {
            r.change(ChangeKind::MakeFolder, clean_path(r.text("folder"), r.run.options.base_dir));
            r.output("folder", file_value(clean_path(r.text("folder"), r.run.options.base_dir)));
        },
    };
}

// A value typed once and kept in the graph file. Any type: it's the kind of field it feeds (Graph::output_type), and
// front ends offer that kind's picker (picker_for).
NodeSpec value() {
    return {
        .type = "Value",
        .title = "Value",
        .summary = "A value you type once and keep in the graph, e.g. your output folder, the version or your name. "
                   "It becomes whatever it's connected to: a folder, a file, text. For several places, connect it "
                   "through a Split.",
        .inputs = {},
        .outputs = {{.name = "value", .type = Any, .label = "value", .field = "value", .field_label = "Value",
                     .hint = "Kept in the graph file. Its kind follows what it's connected to."}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            const std::string text = r.node.params.at("value");  // validate(): required
            const PortType kind = r.run.graph.output_type(r.node.id, "value");
            const bool file = kind == Tex || kind == Image || kind == Path || kind == Folder;
            r.output("value", {text, file ? r.resolve(text) : fs::path(), {}});
            r.done("\"" + text + "\"");
        },
    };
}

// Any type: what comes out is what goes in (a texture keeps its game path); Graph::output_type tells which.
NodeSpec split() {
    return {
        .type = "Split",
        .title = "Split",
        .summary = "Passes one value on to several steps. Connect each step to its own line; a new line appears as "
                   "you connect one.",
        .inputs = {{.name = "in", .label = "in", .type = Any, .required = true}},
        .outputs = {{.name = "out", .type = Any, .label = "out", .multiple = true}},
        .utility = true,
        .family = Family::Flow,
        .pure = true,
        .run = [](NodeRun& r) {
            r.output("out", r.input("in"));  // as is: a texture keeps its game path
            r.done("passed on to " + std::to_string(r.run.graph.links_from(r.node.id, "out").size()) + " step(s)");
        },
    };
}

// ---- Path utilities: work on the path only, never touch a file (Require file only looks) ----

NodeSpec join_path() {
    return {
        .type = "JoinPath",
        .title = "Join path",
        .summary = "A folder plus a relative path, with the separators put right: D:\\mods + MyMod\\natives gives "
                   "D:\\mods\\MyMod\\natives. Like Value, it becomes the kind it's connected to (a folder or a file).",
        .inputs = {{.name = "folder", .label = "Folder", .type = Folder, .widget = Widget::Path, .required = true,
                    .hint = "The folder to start from.", .path = PathKind::Folder},
                   {.name = "add", .label = "Add", .type = Text, .widget = Widget::Text, .required = true,
                    .hint = "A relative path to add, e.g. natives\\STM or MyMod.zip. Not a full path."}},
        .outputs = {{.name = "path", .type = Any, .label = "path"}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            const fs::path folder = clean_path(r.text("folder"), r.run.options.base_dir);
            const fs::path add(r.text("add"));
            if (add.has_root_name() || add.has_root_directory())  // D:\x or \x would replace the folder, not add to it
                throw GraphError("'Add' must be a relative path, not " + add.string());
            const fs::path joined = (folder / add).lexically_normal();
            r.output("path", file_value(joined));
            r.done(joined.string());
        },
    };
}

NodeSpec path_parts() {
    return {
        .type = "PathParts",
        .title = "Path parts",
        .summary = "Splits a path into its folder, file name, name without extension, and extension.",
        .inputs = {{.name = "path", .label = "Path", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "Any file path.", .path = PathKind::OpenFile}},
        .outputs = {{"folder", Folder, "folder"},
                    {"name", Text, "file name"},
                    {"stem", Text, "name without extension"},
                    {"extension", Text, "extension"}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            const fs::path path = clean_path(r.text("path"), r.run.options.base_dir);
            r.output("folder", file_value(path.parent_path()));
            r.output("name", {path.filename().string(), {}, {}});
            r.output("stem", {path.stem().string(), {}, {}});
            r.output("extension", {path.extension().string(), {}, {}});
            r.done(path.filename().string());
        },
    };
}

NodeSpec change_extension() {
    return {
        .type = "ChangeExtension",
        .title = "Change extension",
        .summary = "The same path with another extension: a.tga with png gives a.png.",
        .inputs = {{.name = "path", .label = "Path", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "Any file path.", .path = PathKind::OpenFile},
                   {.name = "ext", .label = "Ext", .type = Text, .widget = Widget::Text,
                    .hint = "The new extension, .png or png. Only the last one changes (a.tex.143221013 gives "
                            "a.tex.png). Empty removes it."}},
        .outputs = {{"path", Path, "path"}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            fs::path path = clean_path(r.text("path"), r.run.options.base_dir);
            std::string ext = r.text("ext");
            if (!ext.empty() && ext.front() != '.') ext.insert(0, ".");
            path.replace_extension(ext);
            r.output("path", file_value(path));
            r.done(path.filename().string());
        },
    };
}

NodeSpec cut_text_node() {
    return {
        .type = "CutText",
        .title = "Cut text",
        .summary = "Keeps part of a text or path, cut at a marker: a texture's path after natives/STM/ is its in-game "
                   "path. Ignores case and treats \\ and / alike.",
        .inputs = {{.name = "text", .label = "Text", .type = Text, .widget = Widget::Text, .required = true,
                    .hint = "The text to cut. Link a file or texture to cut its full path."},
                   {.name = "marker", .label = "Marker", .type = Text, .widget = Widget::Text, .required = true,
                    .hint = "Where to cut, e.g. natives/STM/. The run stops if the text doesn't contain it."},
                   {.name = "keep", .label = "Keep", .type = Text, .widget = Widget::Choice, .required = true,
                    .hint = "Which part to keep.", .initial = "after",
                    .options = {{"after", "After the marker"},
                                {"before", "Before the marker"},
                                {"from", "From the marker on"},
                                {"upto", "Up to the marker"}}},
                   {.name = "occurrence", .label = "Which", .type = Text, .widget = Widget::Choice, .required = true,
                    .hint = "If the marker appears more than once: cut at its first or its last.", .initial = "first",
                    .options = {{"first", "First one"}, {"last", "Last one"}}}},
        .outputs = {{"text", Text, "text"}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            const std::string text = r.text("text"), marker = r.text("marker"), keep = r.text("keep");
            const CutKeep how = keep == "before" ? CutKeep::Before
                                : keep == "from" ? CutKeep::From
                                : keep == "upto" ? CutKeep::UpTo
                                                 : CutKeep::After;
            const auto out = cut_text(text, marker, how, r.text("occurrence") == "last");
            if (!out) throw GraphError("'" + marker + "' isn't in '" + text + "'");
            r.output("text", {*out, {}, {}});
            r.done("\"" + *out + "\"");
        },
    };
}

NodeSpec require_file() {
    return {
        .type = "RequireFile",
        .title = "Require file",
        .summary = "Stops the run with a clear message if the file isn't there, before any later step uses it.",
        .inputs = {{.name = "file", .label = "File", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "The file that must exist.", .path = PathKind::OpenFile}},
        .outputs = {{"file", Path, "file"}},
        .utility = true,
        .family = Family::Value,
        .run = [](NodeRun& r) {
            const auto linked = r.values("file");
            const fs::path file = clean_path(r.text("file"), r.run.options.base_dir);
            if (!fs::is_regular_file(long_path(file))) throw GraphError("required file missing: " + file.string());
            r.output("file", file_value(file, linked.empty() ? std::string() : linked[0].game_path));
            r.done("found " + file.filename().string(), file);
        },
        .preview = [](NodeRun& r) {
            r.output("file", file_value(clean_path(r.text("file"), r.run.options.base_dir)));
        },
    };
}

// ---- Custom nodes' pins (custom.hpp): Input and Output blocks inside a custom node's graph. Expanding a custom
// block replaces them by what's linked to its pins; editing the custom node's own graph, an Input gives its default
// like a Value, so previews work in there too. ----

NodeSpec node_input() {
    return {
        .type = "NodeInput",
        .title = "Input",
        .summary = "One input pin of a custom node, named here. It becomes whatever it's connected to inside. A "
                   "default makes the pin a field on the custom block, typed or linked.",
        .inputs = {{.name = "name", .label = "Pin name", .type = Text, .widget = Widget::Text, .required = true,
                    .hint = "What the pin is called on the custom block."},
                   {.name = "default", .label = "Default", .type = Text, .widget = Widget::Text,
                    .hint = "Used when nothing is typed or linked on the custom block. Empty: the pin must be "
                            "linked or typed."}},
        .outputs = {{"value", Any, "value"}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            const std::string text = r.text("default");
            if (text.empty()) throw GraphError("Input '" + r.text("name") + "' has no default to try it with");
            const PortType kind = r.run.graph.output_type(r.node.id, "value");
            const bool file = kind == Tex || kind == Image || kind == Path || kind == Folder;
            r.output("value", {text, file ? r.resolve(text) : fs::path(), {}});
            r.done("\"" + text + "\"");
        },
    };
}

NodeSpec node_output() {
    return {
        .type = "NodeOutput",
        .title = "Output",
        .summary = "One output pin of a custom node, named here: what's linked in goes out of the custom block.",
        .inputs = {{.name = "name", .label = "Pin name", .type = Text, .widget = Widget::Text, .required = true,
                    .hint = "What the pin is called on the custom block."},
                   {.name = "value", .label = "value", .type = Any, .required = true}},
        .outputs = {},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) { r.done("\"" + r.input("value").text + "\""); },
    };
}

// ---- Run program (user, 2026-10-02: "claude -p, scripts, a possible ComfyUI bridge") ----

fs::path system_program(const wchar_t* relative) {  // from System32, never from PATH
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    return n > 0 && n < MAX_PATH ? fs::path(dir) / relative : fs::path();
}

fs::path search_path(const std::wstring& name, const wchar_t* extension) {
    wchar_t found[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, name.c_str(), extension, MAX_PATH, found, nullptr);
    return n > 0 && n < MAX_PATH ? fs::path(found) : fs::path();
}

// The program a Run program field names: a path (relative to the graph's folder), else a name looked up there, then
// on PATH, trying .exe, .cmd, .bat, .ps1 and .py (claude -> claude.exe or claude.cmd). Throws if there's none.
fs::path find_program(const std::string& text, const fs::path& base_dir) {
    const fs::path given = clean_path(text, base_dir);
    if (given.empty()) throw GraphError("no program given");
    std::error_code ec;
    if (fs::is_regular_file(given, ec)) return given;
    const fs::path name = fs::path(text).filename();
    if (fs::path(text).has_parent_path() || name.empty()) throw GraphError("no program " + given.string());
    for (const wchar_t* ext : {L"", L".exe", L".cmd", L".bat", L".ps1", L".py"}) {
        if (fs::is_regular_file(base_dir / (name.wstring() + ext), ec)) return base_dir / (name.wstring() + ext);
        if (const fs::path p = search_path(name.wstring() + ext, nullptr); !p.empty()) return p;
    }
    throw GraphError("no program called " + text + " here or on PATH");
}

// The arguments as typed, split the way a command line is (quotes keep spaces together).
// ponytail: the text goes through the ANSI code page like the tool's other paths; switch the tool to wide strings if
// non-ASCII arguments ever matter.
std::vector<std::wstring> split_arguments(const std::string& text) {
    std::vector<std::wstring> args;
    if (text.find_first_not_of(" \t") == std::string::npos) return args;
    int count = 0;
    // A dummy program name first: CommandLineToArgvW reads the first word by other rules.
    LPWSTR* argv = CommandLineToArgvW((L"x " + fs::path(text).wstring()).c_str(), &count);
    if (!argv) throw GraphError("can't read the arguments: " + text);
    for (int i = 1; i < count; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    return args;
}

// The last part of what a program printed, for an error or the log.
std::string tail_of(const std::string& output, size_t keep = 1500) {
    std::string t = output.size() > keep ? "..." + output.substr(output.size() - keep) : output;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    return t;
}

NodeSpec run_program() {
    return {
        .type = "RunProgram",
        .title = "Run program",
        .summary = "Runs a program or script (.exe, .bat / .cmd, .ps1, .py), e.g. claude -p or your own Python, "
                   "with your arguments, and passes on what it printed and the file it wrote. It runs as you: "
                   "nothing checks what it changes.",
        .inputs = {{.name = "program", .label = "Program", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "A program or script, or just its name to look it up on PATH: claude, python, "
                            "C:\\tools\\upscale.exe, my_script.py (next to the graph).",
                    .path = PathKind::OpenFile},  // no filter: a bare name (claude) is fine
                   {.name = "arguments", .label = "Arguments", .type = Text, .widget = Widget::Text,
                    .hint = "As on a command line; quotes keep spaces together. {in}: what's linked into the input, "
                            "{out}: the output file, {name}: the file's name when repeated for a list. Link a Text "
                            "block to build them from several values."},
                   {.name = "in", .label = "input", .type = Any},
                   {.name = "out_file", .label = "Output file", .type = Path, .widget = Widget::Path,
                    .hint = "Where the program writes its result, if it writes one: put {out} in the arguments. "
                            "After the run it must be there; it's then passed on.",
                    .path = PathKind::SaveFile, .result = "file"},
                   {.name = "timeout", .label = "Time limit", .type = Text, .widget = Widget::Number,
                    .hint = "Seconds before the program is stopped.", .initial = "600", .min = 1, .max = 86400,
                    .format = "%.0f s", .advanced = true}},
        .outputs = {{"text", Text, "what it printed"}, {"file", Path, "output file"}},
        .family = Family::Transform,
        .run = [](NodeRun& r) {
            const fs::path& base = r.run.options.base_dir;
            const fs::path program = find_program(r.text("program"), base);
            r.change(ChangeKind::Run, program);
            const fs::path out = clean_path(r.text("out_file"), base);
            if (!out.empty()) {
                r.change(ChangeKind::Write, out);
                if (out.has_parent_path()) fs::create_directories(long_path(out.parent_path()));
            }
            const std::vector<Value> in = r.values("in");
            std::vector<std::wstring> args;
            for (std::wstring arg : split_arguments(r.text("arguments"))) {
                for (const auto& [mark, value, missing] :
                     {std::tuple{L"{in}", in.empty() ? std::wstring() : fs::path(in[0].text).wstring(),
                                 "nothing is linked into its input"},
                      std::tuple{L"{out}", out.wstring(), "Output file is empty"}})
                    for (size_t at; (at = arg.find(mark)) != std::wstring::npos;) {
                        if (value.empty()) throw GraphError(std::string("the arguments use ") +
                                                            fs::path(mark).string() + " but " + missing);
                        arg.replace(at, std::wcslen(mark), value);
                    }
                args.push_back(std::move(arg));
            }

            // Scripts go through their interpreter. cmd.exe reparses its line (& | < > ^ run commands, %x% expands),
            // so a batch file's arguments may not hold those characters (or quotes); there's no safe escaping.
            std::string ext = program.extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto timeout = std::chrono::seconds(static_cast<long long>(number(r, "timeout")));
            const auto start = std::chrono::steady_clock::now();
            ProcessResult result;
            try {
                if (ext == ".bat" || ext == ".cmd") {
                    std::wstring line = L"\"" + program.wstring() + L"\"";
                    for (const std::wstring& a : args) {
                        if (a.find_first_of(L"&|<>^%!\"\r\n") != std::wstring::npos)
                            throw GraphError(program.filename().string() + " is a batch file: its arguments can't "
                                             "hold & | < > ^ % ! or quotes (Windows would run them as commands). "
                                             "Use the program's .exe, or a .ps1 / .py script.");
                        line += a.find_first_of(L" \t") == std::wstring::npos ? L" " + a : L" \"" + a + L"\"";
                    }
                    const fs::path cmd = system_program(L"cmd.exe");
                    result = run_process_line(cmd, L"cmd.exe /d /s /c \"" + line + L"\"", timeout, true, {}, base);
                } else if (ext == ".ps1") {
                    args.insert(args.begin(), {L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-File", program.wstring()});
                    result = run_process(system_program(L"WindowsPowerShell\\v1.0\\powershell.exe"), args, timeout,
                                         true, {}, base);
                } else if (ext == ".py") {
                    fs::path python = search_path(L"py.exe", nullptr);
                    if (python.empty()) python = search_path(L"python.exe", nullptr);
                    if (python.empty()) throw GraphError("no Python found (py.exe or python.exe on PATH)");
                    args.insert(args.begin(), program.wstring());
                    result = run_process(python, args, timeout, true, {}, base);
                } else {
                    result = run_process(program, args, timeout, true, {}, base);
                }
            } catch (const ProcessError& e) {
                throw GraphError(e.what());
            }
            if (result.exit_code != 0)
                throw GraphError(program.filename().string() + " failed (exit code " + std::to_string(result.exit_code) +
                                 ")" + (result.output.empty() ? "" : ":\n" + tail_of(result.output)));
            std::error_code ec;
            if (!out.empty() && !fs::is_regular_file(long_path(out), ec))
                throw GraphError(program.filename().string() + " finished but didn't write " + out.string());

            std::string text = result.output;  // what it printed (its errors too: one pipe)
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
            if (!text.empty()) r.log("printed: " + tail_of(text, 500));
            r.output("text", {text, {}, {}});
            if (!out.empty()) r.output("file", file_value(out, in.empty() ? "" : in[0].game_path));
            const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            char seconds[32];
            std::snprintf(seconds, sizeof seconds, "%.1f s", took);
            r.done("ran " + program.filename().string() + " (" + seconds + ")" +
                       (out.empty() ? "" : ", wrote " + out.filename().string()),
                   out);
        },
        .preview = [](NodeRun& r) {  // what it would run and write; what it prints is known only in a run
            r.change(ChangeKind::Run, find_program(r.text("program"), r.run.options.base_dir));
            if (const fs::path out = clean_path(r.text("out_file"), r.run.options.base_dir); !out.empty()) {
                r.change(ChangeKind::Write, out);
                r.output("file", file_value(out));
            }
        },
    };
}

}  // namespace

const std::vector<NodeSpec>& node_specs() {
    static const std::vector<NodeSpec> specs{
        // The main steps: the texture pipeline, then file steps.
        load_tex(), files_in_folder(), export_image(), edit_image(), import_image(), save_tex(), adjust_colour_node(),
        resize_image_node(), overlay_image_node(), replace_photo_node(), preview_node(), package_mod(),
        copy_file(), move_file(), rename_file(), delete_file(), make_folder(), run_program(),
        // Utilities.
        value(), text_node(), split(), join_path(), path_parts(), change_extension(), cut_text_node(), require_file(),
        node_input(), node_output(),
    };
    return specs;
}

const NodeSpec* find_spec(std::string_view type) {
    for (const auto& s : node_specs())
        if (type == s.type) return &s;
    return find_custom_spec(type);  // a custom node's (custom.hpp)
}

std::vector<const NodeSpec*> all_specs() {
    std::vector<const NodeSpec*> out;
    for (const auto& s : node_specs()) out.push_back(&s);
    for (const NodeSpec* s : custom_specs()) out.push_back(s);
    return out;
}

const InputSpec* find_input(const NodeSpec& spec, std::string_view name) {
    for (const auto& in : spec.inputs)
        if (name == in.name) return &in;
    return nullptr;
}

PathKind picker_for(PortType type) {
    switch (type) {
    case Tex: return PathKind::OpenTexture;
    case Image:
    case Path: return PathKind::OpenFile;
    case Folder: return PathKind::Folder;
    default: return PathKind::None;
    }
}

bool accepts(const InputSpec& in, PortType out) {
    if (in.type == out || in.type == Text || in.type == Path || in.type == Any || out == Any) return true;
    if (in.type == Folder) return out == Text;  // a typed folder; a file path isn't one
    return in.editable() && (out == Text || out == Path);  // a texture or image file field: a typed path
}

std::map<int, std::string> destination_warnings(const Graph& g, const fs::path& base_dir) {
    std::map<int, std::string> warnings;
    auto param = [](const Node& n, const char* name) { return n.params.contains(name) ? n.params.at(name) : ""; };
    auto linked = [&](const Node& n, const char* input) { return g.is_connected(n.id, input, false); };
    for (const Node& n : g.nodes) {
        const bool copy_or_move = n.type == "CopyFile" || n.type == "MoveFile";
        if (!copy_or_move && n.type != "RenameFile") continue;
        try {
            fs::path target;
            if (copy_or_move) {
                if (linked(n, "dest")) continue;
                const fs::path dest = clean_path(param(n, "dest"), base_dir);
                if (dest.empty()) continue;
                const bool folder = !dest.has_filename() || fs::is_directory(long_path(dest));
                if (folder && (linked(n, "source") || param(n, "source").empty())) continue;  // no name yet
                target = destination_for(clean_path(param(n, "source"), base_dir), dest);
            } else {  // Rename: the new name, next to the file
                if (linked(n, "source") || linked(n, "name") || param(n, "source").empty() || param(n, "name").empty())
                    continue;
                target = clean_path(param(n, "source"), base_dir).parent_path() / param(n, "name");
            }
            if (fs::exists(long_path(target))) warnings[n.id] = "Destination exists: " + target.string();
        } catch (const fs::filesystem_error&) {  // e.g. a half-typed path Windows rejects: nothing to warn about yet
        }
    }
    return warnings;
}

std::string game_path_from(const fs::path& file, const std::string& natives_root) {
    const auto split = split_natives(file, natives_root);
    return split ? split->second : "";
}

std::string fill_template(const std::string& text, const std::vector<std::string>& parts) {
    static const std::regex placeholder(R"(\{(\d+)\})");
    std::string out;
    auto last = text.cbegin();
    for (std::sregex_iterator it(text.begin(), text.end(), placeholder), end; it != end; ++it) {
        const size_t n = std::stoul((*it)[1].str());
        if (n == 0 || n > parts.size())
            throw GraphError("the text uses {" + std::to_string(n) + "} but " + std::to_string(parts.size()) +
                             " part(s) are connected");
        out.append(last, (*it)[0].first);
        out += parts[n - 1];
        last = (*it)[0].second;
    }
    out.append(last, text.cend());
    return out;
}

std::optional<ImagePreview> preview_image(const Graph& g, const RunValues& preview, int id, const fs::path& base_dir,
                                          unsigned max_side, const ImageLoader& load, const Profile* profile,
                                          std::string* why) {
    const Node* n = g.find(id);
    const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
    if (!spec) return std::nullopt;
    auto source = [&](const char* input) -> const Link* {  // the link into `input`, if any
        const auto links = g.links_into(id, input);
        return links.empty() ? nullptr : &g.links[links[0]];
    };
    if (n->type == "Split") {  // passes on what comes in
        const Link* l = source("in");
        return l ? preview_image(g, preview, l->from_node, base_dir, max_side, load, profile, why) : std::nullopt;
    }
    auto text = [&](const char* input) -> std::optional<std::string> {  // typed, or what the link holds
        if (const Link* l = source(input)) {
            const auto it = preview.find({l->from_node, l->from_port});
            return it == preview.end() ? std::nullopt : std::optional<std::string>(it->second);
        }
        const auto it = n->params.find(input);
        return it == n->params.end() ? std::string() : it->second;
    };
    auto number_of = [&](const char* input) -> std::optional<float> {
        const auto t = text(input);
        return t ? parse_number(*find_input(*spec, input), *t) : std::nullopt;
    };
    auto image = [&](const char* input) -> std::optional<ImagePreview> {  // an image block's result, else the file
        if (const Link* l = source(input))
            if (auto from_block = preview_image(g, preview, l->from_node, base_dir, max_side, load, profile, why))
                return from_block;
        const auto t = text(input);
        if (!t || t->empty()) return std::nullopt;
        return load(clean_path(*t, base_dir));
    };
    try {
        // Steps that hand an image on, so a preview can start at the texture itself, before anything is exported.
        if (n->type == "ExportImage") {  // its image file once it's there (it may hold an edit), else the texture
            std::error_code ec;
            // Only a file for editing (a run replaces a working file, and one from another texture).
            const auto from = n->params.find("exported_from");
            const bool stale = from != n->params.end() && from->second != text("tex").value_or(from->second);
            if (const auto png = text("png"); for_editing(g, id) && !stale && png && !png->empty() &&
                                              fs::is_regular_file(clean_path(*png, base_dir), ec))
                return load(clean_path(*png, base_dir));
            return image("tex");
        }
        if (n->type == "EditImage" || n->type == "ImportImage") return image("png");
        if (n->type == "Preview") return image("in");  // what comes in, as it is
        if (n->type == "AdjustColour") {
            auto img = image("image");
            const auto hue = number_of("hue"), sat = number_of("saturation"), bri = number_of("brightness"),
                       con = number_of("contrast");
            if (!img || !hue || !sat || !bri || !con) return std::nullopt;
            adjust_colour(img->image, *hue, *sat / 100, *bri / 100, *con / 100);
            return img;
        }
        if (n->type == "ResizeImage") {
            const auto img = image("image");
            if (!img || img->scale <= 0) return std::nullopt;
            const double real_w = img->image.width / img->scale, real_h = img->image.height / img->scale;
            double w = 0, h = 0;  // the real result's size, as the run works it out
            if (const auto match = text("match"); match && !match->empty()) {
                const fs::path file = clean_path(*match, base_dir);
                std::string name = file.filename().string();
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (name.find(".tex") != std::string::npos) {
                    if (!profile) return std::nullopt;
                    const TexMeta m = read_tex_meta(file, *profile);
                    w = m.width;
                    h = m.height;
                } else {
                    const auto other = load(file);
                    if (!other || other->scale <= 0) return std::nullopt;
                    w = other->image.width / other->scale;
                    h = other->image.height / other->scale;
                }
            } else {
                const auto wn = number_of("width"), hn = number_of("height");
                if (!wn || !hn || (*wn == 0 && *hn == 0)) return std::nullopt;
                w = *wn ? *wn : *hn * real_w / real_h;
                h = *hn ? *hn : *wn * real_h / real_w;
            }
            const double k = std::min(1.0, double(max_side) / std::max(w, h));
            const auto fit = text("fit");
            const Fit how = fit == "fill" ? Fit::Fill : fit == "stretch" ? Fit::Stretch : Fit::Fit;
            return ImagePreview{resize_image(img->image, std::max(1u, unsigned(std::lround(w * k))),
                                             std::max(1u, unsigned(std::lround(h * k))), how),
                                float(k)};
        }
        if (n->type == "ReplacePhoto") {
            const auto frame = image("frame"), picture = image("picture");
            const auto fw = number_of("frame_width"), grow = number_of("grow"), feather = number_of("feather"),
                       tone = number_of("tone"), shading = number_of("shading"), stains = number_of("stains"),
                       detail = number_of("detail"), zoom = number_of("zoom"), pic_x = number_of("picture_x"),
                       pic_y = number_of("picture_y");
            if (!frame || !picture || !fw || !grow || !feather || !tone || !shading || !stains || !detail || !zoom ||
                !pic_x || !pic_y)
                return std::nullopt;
            const float k = frame->scale;  // sizes on the thumbnail
            const PhotoArea area = photo_area(frame->image, *fw > 0 ? *fw * k : 0, *grow * k, *feather * k, k);
            ImagePreview out{replace_photo(frame->image, picture->image, area.mask,
                                           {*tone / 100, *shading / 100, *stains / 100, *detail / 100}, k,
                                           framing_from(*zoom, *pic_x, *pic_y)),
                             k};
            if (*fw <= 0) out.found["frame_width"] = area.frame_width / k;  // auto: the edge found
            if (text("show_outline") == "true")  // the edge as found, in cyan
                for (const auto& p : area.outline) {
                    const long x = std::lround(p[0]), y = std::lround(p[1]);
                    if (x < 0 || y < 0 || x >= long(out.image.width) || y >= long(out.image.height)) continue;
                    std::uint8_t* px = &out.image.pixels[(size_t(y) * out.image.width + size_t(x)) * 4];
                    px[0] = 255, px[1] = 255, px[2] = 0;
                }
            return out;
        }
        if (n->type == "OverlayImage") {
            auto base = image("base");
            auto top = image("top");
            const auto x = number_of("x"), y = number_of("y"), opacity = number_of("opacity");
            if (!base || !top || !x || !y || !opacity || top->scale <= 0) return std::nullopt;
            const float k = base->scale;  // the top image goes on at the base's scale
            if (std::abs(top->scale - k) > 1e-4f) {
                const auto tw = unsigned(std::lround(top->image.width / top->scale * k));
                const auto th = unsigned(std::lround(top->image.height / top->scale * k));
                if (tw == 0 || th == 0) return base;  // too small to see at this size
                top->image = resize_image(top->image, tw, th, Fit::Stretch);
            }
            overlay_image(base->image, top->image, int(std::lround(*x * k)), int(std::lround(*y * k)), *opacity / 100);
            return base;
        }
    } catch (const std::exception& e) {
        if (why) *why = e.what();  // a file that can't be read, a size that can't be: no thumbnail
    }
    return std::nullopt;
}

std::optional<std::string> cut_text(const std::string& text, const std::string& marker, CutKeep keep, bool last) {
    auto fold = [](std::string s) {  // same length: positions map back to the original
        for (char& c : s) c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    if (marker.empty()) return std::nullopt;
    const std::string t = fold(text), m = fold(marker);
    const size_t at = last ? t.rfind(m) : t.find(m);
    if (at == std::string::npos) return std::nullopt;
    switch (keep) {
    case CutKeep::After: return text.substr(at + m.size());
    case CutKeep::Before: return text.substr(0, at);
    case CutKeep::From: return text.substr(at);
    case CutKeep::UpTo: return text.substr(0, at + m.size());
    }
    return std::nullopt;
}

}  // namespace remod
