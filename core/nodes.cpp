#include "node_run.hpp"
#include "custom.hpp"

#include "browse.hpp"
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
#include <set>
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
                      ": add a Streaming copy block to replace both (CLAUDE.md §9)");
        },
        .preview = [](NodeRun& r) {
            r.output("tex", file_value(r.resolve(r.text("tex"))));
        },
    };
}

// Files in folder's patterns: "*" any run of characters, "?" one; ";" or "," between several; ignoring case. Empty:
// every texture (is_tex_name).
bool any_pattern(const std::string& name, const std::string& patterns);

bool name_matches(const std::string& name, const std::string& patterns) {
    auto low = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string n = low(name);
    if (patterns.find_first_not_of(" ;,") == std::string::npos) return is_tex_name(n);
    return any_pattern(n, patterns);
}

// `name` matches one of `patterns` ("*" any run of characters, "?" one; ";" or "," between several; ignoring case).
bool any_pattern(const std::string& name, const std::string& patterns) {
    auto low = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string n = low(name);
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
        .state = {"show"},  // the item previews show (its key); front ends set it
        .family = Family::Source,
        .pure = true,
        .run = [](NodeRun& r) {
            const fs::path folder = r.resolve(r.text("folder"));
            std::error_code ec;
            if (!fs::is_directory(folder, ec)) throw GraphError("no folder " + folder.string());
            std::vector<fs::path> files;
            // Into texture inputs, only textures: a pattern such as *.tex* also matches a tool's leftover x.texout.tga.
            const bool textures = r.run.graph.output_type(r.node.id, "files") == Tex;
            auto take = [&](const fs::directory_entry& e) {
                std::string name = e.path().filename().string(), low = name;
                std::ranges::transform(low, low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (e.is_regular_file(ec) && name_matches(name, r.text("pattern")) && (!textures || is_tex_name(low)))
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

std::string file_bytes(const fs::path& file);
std::string hash_hex(const std::string& bytes);
void prune_cache(const fs::path& dir, const fs::path& keep);

NodeSpec export_image() {
    return {
        .type = "ExportImage",
        .title = "Export image",
        .summary = "Converts the texture to an image file: PNG, TGA or JPG, whichever the file name ends in. Going to "
                   "an Edit image step, a file already there (your edited version) is kept, unless the texture has "
                   "changed since; otherwise it's a working file, exported again when the texture changes. Leave the "
                   "file empty when no one edits it by hand: a working copy is kept for you.",
        .inputs = {{.name = "tex", .label = "texture", .type = Tex, .required = true}},
        .outputs = {{.name = "png", .type = Image, .label = "image", .field = "png", .field_label = "Image file",
                     .hint = "Where to write the image. Its ending picks the format: .png, .tga (e.g. for GIMP) or "
                             ".jpg. JPG loses some quality and all transparency. Empty (not for an Edit image step): "
                             "a working copy the tool keeps.",
                     .path = PathKind::SaveFile, .filter = kEditImageFormats, .field_optional = true}},
        .state = {"exported_from", "exported_time"},  // the texture its file is from, the file's write time
        .family = Family::Transform,
        .run = [](NodeRun& r) {
            const Value tex = r.input("tex");
            if (r.text("png").empty()) {  // a working copy: in the run cache by the texture's bytes, else temporary
                if (for_editing(r.run.graph, r.node.id))
                    throw GraphError("Image file is required: it's the file you edit in the Edit image step");
                const fs::path& cache = r.run.options.cache_dir;
                const fs::path out = cache.empty() ? r.temp_file(".png")
                                                   : cache / (hash_hex(file_bytes(tex.path) + '\0' +
                                                                       r.run.options.converter.id(r.profile())) + ".png");
                if (std::error_code ec; !cache.empty() && fs::is_regular_file(out, ec)) {
                    fs::last_write_time(out, fs::file_time_type::clock::now(), ec);  // recently used: pruned last
                    r.done("unchanged: reused the image exported before", out);
                } else {
                    fs::path target = out;
                    if (!cache.empty()) {
                        fs::create_directories(cache);
                        (target = out) += ".part.png";
                    }
                    r.run.options.converter.load_tex(tex.path, target, r.profile());
                    if (target != out) {
                        fs::rename(target, out);
                        prune_cache(cache, out);
                    }
                    r.done("exported a working copy", out);
                }
                r.output("png", file_value(out, tex.game_path));
                return;
            }
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
                if (png.has_parent_path()) fs::create_directories(long_path(png.parent_path()));  // e.g. edits/
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
            if (r.text("png").empty()) throw GraphError("a working copy, made in the run");
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

// ---- Channel tools (CLAUDE.md §9, §10: RE textures pack data in channels; a colour edit or an AI result must leave
// those alone) ----

constexpr std::array<std::array<const char*, 2>, 5> kChannels{
    {{"red", "Red"}, {"green", "Green"}, {"blue", "Blue"}, {"alpha", "Alpha"}, {"colour", "Colour (no alpha)"}}};

Channel channel_of(const std::string& value) {
    for (size_t i = 0; i < kChannels.size(); ++i)
        if (value == kChannels[i][0]) return Channel(i);
    throw GraphError("no channel '" + value + "'");
}

NodeSpec pick_channel_node() {
    return {
        .type = "PickChannel",
        .title = "Pick channel",
        .summary = "One channel of an image as a grey picture, to see or edit it alone (e.g. the alpha of an albd "
                   "texture, which is data, not transparency). Colour: the colour with the alpha made opaque.",
        .inputs = {image_input("image", "Image", "The image to take the channel from."),
                   {.name = "channel", .label = "Channel", .type = Text, .widget = Widget::Choice, .required = true,
                    .hint = "Which channel. RE textures often keep data in one (CLAUDE.md: nrrc's normal is in green "
                            "and alpha).",
                    .initial = "alpha", .options = {kChannels.begin(), kChannels.end()}},
                   save_to_input()},
        .outputs = {{"image", Image, "image"}},
        .family = Family::Transform,
        .thumbnail = true,
        .run = [](NodeRun& r) {
            const Bgra image = load_image(clean_path(r.text("image"), r.run.options.base_dir));
            write_image(r, channel_image(image, channel_of(r.text("channel"))), r.text("channel") + " channel");
        },
        .preview = image_preview,
    };
}

// The optional images Merge channels takes: colour, then one per channel.
constexpr const char* kMergeInputs[] = {"colour", "red", "green", "blue", "alpha"};

NodeSpec merge_channels_node() {
    auto optional_image = [](const char* name, const char* label, const char* hint) {
        InputSpec in = image_input(name, label, hint);
        in.required = false;
        return in;
    };
    return {
        .type = "MergeChannels",
        .title = "Merge channels",
        .summary = "Puts channels back into an image: its colour from one image, any single channel from a grey one. "
                   "E.g. an edited or AI-made picture as the colour of the original, keeping the original's alpha "
                   "(data in RE textures). Every image must be the base's size.",
        .inputs = {image_input("base", "Base image", "The image whose channels are kept unless replaced."),
                   optional_image("colour", "Colour from", "Its red, green and blue replace the base's; alpha kept."),
                   optional_image("red", "Red from", "A grey image: its brightness becomes the red channel."),
                   optional_image("green", "Green from", "A grey image: its brightness becomes the green channel."),
                   optional_image("blue", "Blue from", "A grey image: its brightness becomes the blue channel."),
                   optional_image("alpha", "Alpha from", "A grey image: its brightness becomes the alpha channel."),
                   save_to_input()},
        .outputs = {{"image", Image, "image"}},
        .family = Family::Transform,
        .thumbnail = true,
        .run = [](NodeRun& r) {
            Bgra base = load_image(clean_path(r.text("base"), r.run.options.base_dir));
            std::array<std::optional<Bgra>, 5> parts;
            std::string merged;
            for (size_t i = 0; i < parts.size(); ++i)
                if (const std::string t = r.text(kMergeInputs[i]); !t.empty()) {
                    parts[i] = load_image(clean_path(t, r.run.options.base_dir));
                    merged += std::string(merged.empty() ? "" : ", ") + kMergeInputs[i];
                }
            if (merged.empty()) throw GraphError("nothing to merge in: link a colour or a channel image");
            try {
                merge_channels(base, parts[0] ? &*parts[0] : nullptr,
                               {parts[1] ? &*parts[1] : nullptr, parts[2] ? &*parts[2] : nullptr,
                                parts[3] ? &*parts[3] : nullptr, parts[4] ? &*parts[4] : nullptr});
            } catch (const std::runtime_error& e) {
                throw GraphError(e.what());
            }
            write_image(r, base, "merged " + merged);
        },
        .preview = image_preview,
    };
}

// ---- Masks (CLAUDE.md §10: "change only the jacket") ----

// The mesh's parts whose material matches `patterns` (all if empty). Throws, naming the materials there are, if none.
std::vector<const MeshPart*> pick_parts(const MeshModel& mesh, const std::string& patterns) {
    std::vector<const MeshPart*> out;
    std::set<std::string> names;
    for (const MeshPart& p : mesh.parts) {
        names.insert(p.material);
        if (patterns.find_first_not_of(" ;,") == std::string::npos || any_pattern(p.material, patterns)) out.push_back(&p);
    }
    if (out.empty()) {
        std::string list;
        for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
        throw GraphError("no material matches '" + patterns + "'; this mesh has: " + list);
    }
    return out;
}

// A texture's or image's size: a texture by its header (for this game), an image by its own.
std::pair<std::uint32_t, std::uint32_t> picture_size(const fs::path& file, const Profile& profile) {
    if (read_tex_version(file)) {
        const TexMeta m = read_tex_meta(file, profile);
        return {m.width, m.height};
    }
    return image_size(file);
}

NodeSpec mesh_mask_node() {
    return {
        .type = "MeshMask",
        .title = "Mesh mask",
        .summary = "A mask of where some of a mesh's parts sit on their texture: white where their materials' UV "
                   "triangles fall, black elsewhere. E.g. only the jacket of a character, for Blend in mask.",
        .inputs = {{.name = "mesh", .label = "Mesh", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "The game's .mesh file (RE4R), e.g. from your REtool folder.", .path = PathKind::OpenFile},
                   {.name = "size_of", .label = "Size of", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "The texture (or an image of it) the mask is for: the mask gets its size.",
                    .path = PathKind::OpenTexture},
                   {.name = "materials", .label = "Materials", .type = Text, .widget = Widget::Text,
                    .hint = "Which parts, by material name: * stands for anything, ; between several (e.g. "
                            "*jacket*). Empty: every part. A run that finds none lists the mesh's materials."},
                   advanced(number_input("grow", "Grow", "Pixels the mask reaches past the parts' edges, to cover "
                                         "the seams.", "2", 0, 64, "%.0f px")),
                   save_to_input()},
        .outputs = {{"image", Image, "mask"}},
        .family = Family::Transform,
        .thumbnail = true,
        .run = [](NodeRun& r) {
            const fs::path& base = r.run.options.base_dir;
            const MeshModel mesh = [&] {
                try {
                    return read_mesh(clean_path(r.text("mesh"), base));
                } catch (const std::runtime_error& e) {
                    throw GraphError(e.what());
                }
            }();
            const auto [w, h] = picture_size(clean_path(r.text("size_of"), base), r.profile());
            const auto parts = pick_parts(mesh, r.text("materials"));
            const Bgra mask{w, h, uv_mask(parts, w, h, unsigned(number(r, "grow")))};
            write_image(r, mask, "mask of " + std::to_string(parts.size()) + " part(s)");
        },
        .preview = image_preview,
    };
}

// Which texture some of a mesh's parts use, from its material file (.mdf2, found as the Browser's 3D view finds it):
// their colour texture, or the one of theirs whose file name matches. Only that material's textures are looked for
// on disk (no index of the game files, which takes seconds).
NodeSpec part_texture_node() {
    return {
        .type = "PartTexture",
        .title = "Part texture",
        .summary = "The texture some of a mesh's parts use, found from its material file: their colour texture, or "
                   "another of theirs by name (e.g. the normal map). Pick the parts by material name, as in Mesh mask. "
                   "The mesh must be in your REtool folder, where its material and textures are.",
        .inputs = {{.name = "mesh", .label = "Mesh", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "The game's .mesh file, in your REtool folder.", .path = PathKind::OpenFile},
                   {.name = "materials", .label = "Materials", .type = Text, .widget = Widget::Text,
                    .hint = "Which parts, by material name: * stands for anything, ; between several (e.g. *Pants*). "
                            "Empty: every part (they must share one texture). A run that finds none lists them."},
                   advanced({.name = "texture", .label = "Texture name", .type = Text, .widget = Widget::Text,
                             .hint = "Optional: another of the parts' textures, by file name (* for anything), e.g. "
                                     "*_nrmr* for the normal map. Empty: their colour texture."}),
                   advanced({.name = "material", .label = "Material file", .type = Path, .widget = Widget::Path,
                             .hint = "Optional: another .mdf2 than the mesh's own, e.g. a costume variant "
                                     "(cha000_00b.mdf2.32 next to cha000_00's mesh). Empty: the mesh's own.",
                             .path = PathKind::OpenFile})},
        .outputs = {{"tex", Tex, "texture"}},
        .family = Family::Source,
        .pure = true,  // only reads files, like Streaming copy
        .run = [](NodeRun& r) {
            const Profile& profile = r.profile();
            const fs::path mesh = clean_path(r.text("mesh"), r.run.options.base_dir);
            if (std::error_code ec; !fs::is_regular_file(long_path(mesh), ec))
                throw GraphError("mesh not found: " + mesh.string());
            const auto natives = split_natives(mesh, profile.natives_root);
            if (!natives)
                throw GraphError(mesh.filename().string() + " isn't inside a " + profile.natives_root +
                                 " folder: use the one in your REtool folder, where its material and textures are");
            MeshTextures m;
            try {  // the material's textures that are on disk, then matched as the Browser matches its index
                std::vector<std::string> present;
                const std::string chosen = r.text("material");
                const fs::path material = chosen.empty() ? fs::path() : clean_path(chosen, r.run.options.base_dir);
                for (const std::string& t : mesh_textures(natives->first, natives->second, {}, material).textures)
                    if (std::error_code ec; fs::is_regular_file(long_path(natives->first / (t + "." + profile.tex_suffix)), ec))
                        present.push_back(t + "." + profile.tex_suffix);
                std::ranges::sort(present, [](const std::string& a, const std::string& b) {
                    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
                        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
                    });
                });
                m = mesh_textures(natives->first, natives->second, present, material);
            } catch (const std::runtime_error& e) {
                throw GraphError(e.what());
            }
            const std::string patterns = r.text("materials"), want = r.text("texture");
            const bool every = patterns.find_first_not_of(" ;,") == std::string::npos;
            std::string names;
            std::vector<std::string> without;
            std::map<std::string, std::vector<std::string>> by_texture;  // texture -> the parts using it
            for (const MeshMaterial& mat : m.materials) {
                names += (names.empty() ? "" : ", ") + mat.name;
                if (every ? mat.hidden : !any_pattern(mat.name, patterns)) continue;
                int pick = want.empty() ? mat.albedo : -1;
                for (const size_t t : mat.textures)
                    if (pick < 0 && !want.empty() && m.found[t] && any_pattern(file_name(m.textures[t]), want)) pick = int(t);
                if (pick < 0)
                    without.push_back(mat.name);
                else
                    by_texture[m.textures[size_t(pick)]].push_back(mat.name);
            }
            auto list = [](const std::vector<std::string>& v) {
                std::string s;
                for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
                return s;
            };
            if (by_texture.empty() && without.empty())
                throw GraphError("no material matches '" + patterns + "'; " + file_name(m.material) + " has: " + names);
            if (by_texture.empty())
                throw GraphError(list(without) + ": no " +
                                 (want.empty() ? std::string("colour texture") : "texture matching '" + want + "'") +
                                 " in your game files");
            if (by_texture.size() > 1) {
                std::string uses;
                for (const auto& [tex, mats] : by_texture) uses += (uses.empty() ? "" : "; ") + list(mats) + ": " + file_name(tex);
                throw GraphError("these parts use different textures (" + uses + "): narrow Materials to one");
            }
            const auto& [rel, mats] = *by_texture.begin();
            if (!without.empty()) r.log("note: " + list(without) + " have no such texture and are left out");
            r.output("tex", file_value(natives->first / rel, rel));
            r.done(file_name(rel) + " (" + list(mats) + ")", natives->first / rel);
        },
    };
}

NodeSpec mask_blend_node() {
    return {
        .type = "MaskBlend",
        .title = "Blend in mask",
        .summary = "The edited image where the mask is white, the original where it's black, mixed in between: an "
                   "edit or an AI picture on one area only (e.g. a Mesh mask's jacket). The original's alpha is kept.",
        .inputs = {image_input("base", "Original", "The image to change in places; the result has its size."),
                   image_input("edited", "Edited", "The changed image (same size): what shows where the mask is white."),
                   image_input("mask", "Mask", "A grey image: white takes the edit, black keeps the original (e.g. a "
                                               "Mesh mask, or Pick channel of a painted mask)."),
                   number_input("feather", "Feather", "Pixels the mask's edge is softened over. 0: a hard edge.", "0",
                                0, 256, "%.0f px"),
                   advanced({.name = "invert", .label = "Invert", .type = Text, .widget = Widget::Checkbox,
                             .hint = "Change where the mask is black instead."}),
                   save_to_input()},
        .outputs = {{"image", Image, "image"}},
        .family = Family::Transform,
        .thumbnail = true,
        .run = [](NodeRun& r) {
            const fs::path& base_dir = r.run.options.base_dir;
            Bgra base = load_image(clean_path(r.text("base"), base_dir));
            const Bgra edited = load_image(clean_path(r.text("edited"), base_dir));
            const Bgra mask = load_image(clean_path(r.text("mask"), base_dir));
            try {
                masked_blend(base, edited, mask, number(r, "feather"), r.text("invert") == "true");
            } catch (const std::runtime_error& e) {
                throw GraphError(e.what());
            }
            write_image(r, base, "blended in the mask");
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

// ---- Conditions (user, 2026-10-03): a branch runs only when it should. An If passes nothing on when its condition
// is no; the blocks that nothing reaches don't run (Not needed, run_graph). Put it before the steps to skip, so a
// branch not taken never writes a file. Conditions are their own kind (Bool: "true" / "false"). ----

Value yes_no(bool yes) { return {yes ? "true" : "false"}; }

NodeSpec if_node() {
    return {
        .type = "If",
        .title = "If",
        .summary = "Passes its value on only when the condition is yes. When it's no, it passes nothing on: the steps "
                   "it feeds don't run (they show Not needed) and write nothing. Put it before the steps that should "
                   "only sometimes run.",
        .inputs = {{.name = "value", .label = "value", .type = Any, .required = true},
                   {.name = "condition", .label = "Condition", .type = Bool, .widget = Widget::Checkbox,
                    .hint = "Link a condition (File exists, Text matches, Not). Unlinked, this box turns the branch on "
                            "or off by hand.",
                    .initial = "true"}},
        .outputs = {{"value", Any, "value"}},
        .utility = true,
        .family = Family::Flow,
        .pure = true,
        .run = [](NodeRun& r) {
            if (r.condition("condition")) {
                r.output("value", r.input("value"));
                r.done("yes: passed on");
            } else {
                r.nothing("value", block_title(r.node) + " said no");
                r.done("no: passed nothing on");
            }
        },
    };
}

NodeSpec first_of_node() {
    return {
        .type = "FirstOf",
        .title = "First of",
        .summary = "Passes on the first of its two inputs that has a value: with an If on each, one branch or the "
                   "other (if / else). Both must be of one kind.",
        .inputs = {{.name = "first", .label = "first", .type = Any},
                   {.name = "second", .label = "else", .type = Any}},
        .outputs = {{"value", Any, "value"}},
        .utility = true,
        .family = Family::Flow,
        .pure = true,
        .run = [](NodeRun& r) {
            for (const char* in : {"first", "second"})
                if (auto v = r.values(in); !v.empty()) {
                    r.output("value", v.front());
                    r.done(std::string("passed on its ") + (in == std::string("first") ? "first" : "else") + " input");
                    return;
                }
            r.nothing("value", "neither input of " + block_title(r.node) + " has a value");
            r.done("neither input has a value: passed nothing on");
        },
    };
}

NodeSpec file_exists_node() {
    return {
        .type = "FileExists",
        .title = "File exists",
        .summary = "Yes if a file or folder is there, else no. Checked before the run's steps change anything.",
        .inputs = {{.name = "path", .label = "Path", .type = Path, .widget = Widget::Path, .required = true,
                    .hint = "The file or folder to look for.", .path = PathKind::OpenFile}},
        .outputs = {{"yes", Bool, "exists"}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            const fs::path p = clean_path(r.text("path"), r.run.options.base_dir);
            std::error_code ec;
            const bool there = fs::exists(long_path(p), ec);
            r.output("yes", yes_no(there));
            r.done(there ? "yes: it's there" : "no: " + p.filename().string() + " isn't there");
        },
    };
}

NodeSpec text_matches_node() {
    return {
        .type = "TextMatches",
        .title = "Text matches",
        .summary = "Yes if a text or path matches a pattern: * stands for anything, ? for one character, ; between "
                   "several. Ignores case; \\ and / count as the same. E.g. *_albd* for colour textures.",
        .inputs = {{.name = "text", .label = "Text", .type = Text, .widget = Widget::Text, .required = true,
                    .hint = "The text to check, e.g. a texture's path linked in."},
                   {.name = "pattern", .label = "Pattern", .type = Text, .widget = Widget::Text, .required = true,
                    .hint = "* stands for anything, ? for one character, ; between several (any one matching is "
                            "enough), e.g. *_albd*;*_alba*. The whole text must match: start and end with * to find it "
                            "anywhere."}},
        .outputs = {{"yes", Bool, "matches"}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            auto slashes = [](std::string s) {
                std::ranges::replace(s, '\\', '/');
                return s;
            };
            const bool match = any_pattern(slashes(r.text("text")), slashes(r.text("pattern")));
            r.output("yes", yes_no(match));
            r.done(match ? "yes: matches" : "no: doesn't match");
        },
    };
}

NodeSpec not_node() {
    return {
        .type = "Not",
        .title = "Not",
        .summary = "Yes becomes no and no becomes yes, e.g. to run a branch when a file isn't there.",
        .inputs = {{.name = "in", .label = "condition", .type = Bool, .required = true}},
        .outputs = {{"yes", Bool, "not"}},
        .utility = true,
        .family = Family::Value,
        .pure = true,
        .run = [](NodeRun& r) {
            const bool yes = !r.condition("in");
            r.output("yes", yes_no(yes));
            r.done(yes ? "yes" : "no");
        },
    };
}

// The rule (user, 2026-10-03; CLAUDE.md §9): a texture with a streaming copy is replaced together with it.
NodeSpec streaming_copy_node() {
    return {
        .type = "StreamingCopy",
        .title = "Streaming copy",
        .summary = "Finds the high-resolution copy the game keeps of a texture under natives\\STM\\streaming. A "
                   "texture with one must be replaced together with it. Edit 'full size' (the copy, or the texture "
                   "itself when there's none); 'streaming copy (if any)' gives nothing when there's none, so the steps "
                   "it feeds don't run. Convert with streaming copy does both conversions.",
        .inputs = {{.name = "tex", .label = "texture", .type = Tex, .required = true,
                    .hint = "The texture as the mod replaces it, from your REtool folder (not its streaming copy)."}},
        .outputs = {{"full", Tex, "full size"}, {"streaming", Tex, "streaming copy (if any)"}},
        .family = Family::Source,
        .pure = true,  // only looks at the disk, like Files in folder
        .run = [](NodeRun& r) {
            const Value tex = r.input("tex");
            const auto natives = split_natives(tex.path, r.profile().natives_root);
            if (!natives)
                throw GraphError(tex.path.filename().string() + " isn't inside a " + r.profile().natives_root +
                                 " folder, so its streaming copy can't be looked for: use the one in your REtool "
                                 "folder");
            std::string low = natives->second;
            std::ranges::transform(low, low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (low.starts_with("streaming/"))
                throw GraphError(tex.path.filename().string() + " is a streaming copy itself: use the texture at the "
                                 "same path without streaming\\, and this block finds this one");
            const fs::path copy = natives->first / "streaming" / natives->second;
            if (std::error_code ec; fs::is_regular_file(long_path(copy), ec)) {
                const Value streaming = file_value(copy, "streaming/" + natives->second);
                r.output("full", streaming);
                r.output("streaming", streaming);
                r.done("found streaming/" + natives->second, copy);
            } else {
                r.output("full", tex);
                r.nothing("streaming", "no streaming copy of " + tex.path.filename().string());
                r.done("no streaming copy: the texture alone is replaced");
            }
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
                    .format = "%.0f s", .advanced = true},
                   {.name = "always", .label = "Always run", .type = Text, .widget = Widget::Checkbox,
                    .hint = "Run it every time. Off, it runs again only when the program, the arguments, the input "
                            "(a file by its contents) or the output file changed; otherwise it passes on its last "
                            "result. Tick it when the result depends on anything else: a file only the arguments "
                            "name, the web, the time.",
                    .advanced = true}},
        .outputs = {{"text", Text, "what it printed"}, {"file", Path, "output file"}},
        .state = {"ran_key", "ran_time", "ran_text"},  // what decided its last result, and that result (below)
        .family = Family::Transform,
        .run = [](NodeRun& r) {
            const fs::path& base = r.run.options.base_dir;
            const fs::path program = find_program(r.text("program"), base);
            const fs::path out = clean_path(r.text("out_file"), base);
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

            // Ran before with the same program, arguments, input and output, and the output is as it left it: its
            // last result again, nothing run (user, 2026-10-02: no needless runs; claude -p costs time and money).
            // The input counts by its bytes when it's a file. A file only the arguments name isn't watched: Always
            // run is for that, and for programs that give something new each time (the web, the time, a seed).
            std::error_code ec;
            std::string key = program.string() + '\0' + write_time(program) + '\0' +
                              std::to_string(fs::file_size(program, ec)) + '\0' + base.string() + '\0' + out.string();
            for (const std::wstring& a : args) key += '\0' + fs::path(a).string();
            if (!in.empty()) {  // a file by its bytes (also text naming one: the input takes any kind)
                const fs::path file = in[0].path.empty() ? r.resolve(in[0].text) : in[0].path;
                key += '\0' + (fs::is_regular_file(file, ec) ? file_bytes(file) : in[0].text);
            }
            key = hash_hex(key);
            const std::string *ran_key = r.state("ran_key"), *ran_time = r.state("ran_time"),
                              *ran_text = r.state("ran_text");
            if (r.text("always") != "true" && ran_key && *ran_key == key && ran_text &&
                (out.empty() || (ran_time && *ran_time == write_time(out)))) {
                if (!out.empty()) r.claim(out);  // each item its own file
                r.output("text", {*ran_text, {}, {}});
                if (!out.empty()) r.output("file", file_value(out, in.empty() ? "" : in[0].game_path));
                r.done("unchanged: kept the last result (tick Always run to run it anyway)", out);
                return;
            }
            r.change(ChangeKind::Run, program);
            if (!out.empty()) {
                r.change(ChangeKind::Write, out);
                if (out.has_parent_path()) fs::create_directories(long_path(out.parent_path()));
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
            if (!out.empty() && !fs::is_regular_file(long_path(out), ec))
                throw GraphError(program.filename().string() + " finished but didn't write " + out.string());

            std::string text = result.output;  // what it printed (its errors too: one pipe)
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
            if (!text.empty()) r.log("printed: " + tail_of(text, 500));
            // Remembered in the graph for the next run (above). ponytail: up to 64 KB of printed text; more, and it
            // simply runs again next time.
            const bool keep = text.size() <= 64 * 1024;
            r.set_state("ran_key", keep ? key : "");
            r.set_state("ran_time", keep && !out.empty() ? write_time(out) : "");
            r.set_state("ran_text", keep ? text : "");
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
        load_tex(), files_in_folder(), streaming_copy_node(), export_image(), edit_image(), import_image(), save_tex(), adjust_colour_node(),
        resize_image_node(), overlay_image_node(), pick_channel_node(), merge_channels_node(), part_texture_node(),
        mesh_mask_node(),
        mask_blend_node(), replace_photo_node(),
        preview_node(), package_mod(),
        copy_file(), move_file(), rename_file(), delete_file(), make_folder(), run_program(),
        // Utilities.
        value(), text_node(), split(), if_node(), first_of_node(), file_exists_node(), text_matches_node(), not_node(),
        join_path(), path_parts(), change_extension(), cut_text_node(), require_file(), node_input(), node_output(),
    };
    return specs;
}

const char* ai_note(std::string_view type) {
    // For an AI building graphs (CLAUDE.md §10 M4, item 5): when to use a block, when not, an example. The tooltip
    // text (summary) says what it does; this says how it fits in.
    static const std::map<std::string_view, const char*> notes{
        {"LoadTex", "Start of most graphs: the game's original texture. Pick a file inside an extracted natives/STM "
                    "folder so its in-game path is known; otherwise fill In-game path. For many textures use Files in "
                    "folder instead and link it into Texture file."},
        {"FilesInFolder", "Repeats everything it feeds once per matching file (fan-out). Put {name} in every file "
                          "name those blocks write (e.g. edits/{name}.png). Package collects all items. Use 'show' "
                          "(set) to pick which file previews and images show."},
        {"ExportImage", "Texture -> editable image file. Needed before any image block or Edit image. Its Image file "
                        "is required; a file going to Edit image is kept between runs (the user's edit)."},
        {"EditImage", "The user's manual step: a run pauses here. Never mark it done yourself: tell the user which "
                      "file to edit and call edit_done only after they say they've finished."},
        {"ImportImage", "Bring in a picture that already exists (a photo, a logo, an AI result written by Run "
                        "program). Not for game textures: use Original texture."},
        {"SaveTex", "Image -> game texture with the original's size, format and mips. Link the original texture "
                    "(through a Split if it also feeds Export image). The image must be the original's size."},
        {"AdjustColour", "Hue, saturation, brightness, contrast on the colour; alpha kept. Not for data textures "
                         "(nrrc, nrmr, atos, occ, msk: their channels are data, not colour)."},
        {"ResizeImage", "To a size or to match another image or texture (Match size of). Use before Save as texture "
                        "or Merge channels when sizes differ."},
        {"OverlayImage", "One image on top of another at x, y (logos, stamps). The base's alpha is kept."},
        {"PickChannel", "One channel as a grey image, to look at or edit data (e.g. an albd texture's alpha)."},
        {"PartTexture", "Start here to change part of a character or prop: the mesh and its parts' material names "
                        "(e.g. *Pants*) give the texture they use, with its in-game path. A wrong name fails listing "
                        "the materials. Follow with Streaming copy (characters have one) and Mesh mask for the area."},
        {"MeshMask", "A mask of some of a mesh's parts on their texture, by material name (e.g. *jacket*). Use it to "
                     "change one area only: feed Blend in mask. A wrong name fails the run listing the materials."},
        {"MaskBlend", "Puts an edited or AI-made image into the original only where a mask is white (a Mesh mask, a "
                      "painted mask); feather the edge a few pixels. Then Merge channels if the texture packs data, "
                      "and Convert image to texture."},
        {"MergeChannels", "Put an edited or AI-made picture into an original's colour while keeping its alpha (data): "
                          "base = the original image, Colour from = the new picture. Required after any AI image "
                          "result that will replace a texture with packed channels."},
        {"ReplacePhoto", "Only for RE4R UI frames holding an old photo (cs_ui3210_file_*): puts a new picture in the "
                         "photo's place, keeping the frame and the photo's ageing. Not for general overlays."},
        {"Preview", "Shows an image on the graph; changes nothing. Use image on it to see an intermediate result."},
        {"PackageMod", "End of a mod graph: textures (any number, or every item of a list) into a Fluffy Mod Manager "
                       ".zip. Needs a mod name and output folder; Replace existing to rebuild."},
        {"CopyFile", "Copy a file (backups, staging). Writing outside the graph's folder needs the user's approval."},
        {"MoveFile", "Move a file; removing the original needs the user's approval."},
        {"RenameFile", "Rename a file in place; needs the user's approval (it removes the old name)."},
        {"DeleteFile", "Delete a file (to the Recycle Bin). Always needs the user's approval; avoid unless asked."},
        {"MakeFolder", "Make a folder before steps write into it (they also make folders themselves)."},
        {"RunProgram", "Run a program or script (claude -p, python, an upscaler) with {in}/{out}/{name} in its "
                       "arguments; passes on what it printed and the file it wrote. Starting it always needs the "
                       "user's approval. Reruns only when its inputs change (Always run to force)."},
        {"Value", "A value kept in the graph (output folder, version, author). Feed several places through a Split."},
        {"Text", "Builds text from parts: {1}, {2}... (mod names, file names, arguments for Run program)."},
        {"Split", "One output to several inputs (an output feeds one input). Use whenever a value is needed twice."},
        {"If", "Runs a branch only when a condition (File exists, Text matches, Not) is yes: put it at the branch's "
               "start, before any step that writes. No passes nothing on; the steps after it show Not needed."},
        {"FirstOf", "If / else: two Ifs (one on Not of the condition) into First of, which passes on whichever ran."},
        {"FileExists", "A condition: is a file there. Feed If."},
        {"TextMatches", "A condition on a name or path by pattern (*_albd*). Feed If, e.g. to treat colour and data "
                        "textures of a Files in folder list differently."},
        {"Not", "Flips a condition."},
        {"StreamingCopy", "Required for any texture the game also keeps under streaming/ (most character and prop "
                          "textures): the mod must replace both. Edit 'full size'; Convert the edit with the texture "
                          "as original, and again with 'streaming copy' as original; package both. Resize the edit to "
                          "the base texture's size (Match size of) for the first Convert."},
        {"JoinPath", "Folder + relative path -> a path (Add must be relative)."},
        {"PathParts", "A path's folder, name or extension."},
        {"ChangeExtension", "Same path with another extension (e.g. .png for a texture's edit file)."},
        {"CutText", "Keep the part of a text or path after / before a marker (e.g. after natives/STM/ for an "
                    "in-game path)."},
        {"RequireFile", "Fail early with a clear message when a needed file isn't there."},
        {"NodeInput", "Only inside a custom node's own graph: one of its input pins."},
        {"NodeOutput", "Only inside a custom node's own graph: one of its output pins."},
    };
    const auto it = notes.find(type);
    if (it != notes.end()) return it->second;
    const NodeSpec* spec = find_spec(type);  // a custom node: its own summary
    return spec ? spec->summary : "";
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
    if (in.type == out || in.type == Any || out == Any) return true;
    if (out == Bool) return in.type == Text;  // as "true" / "false": a checkbox field, or text
    if (in.type == Bool) return out == Text;  // a typed yes / no
    if (in.type == Text || in.type == Path) return true;
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
        if (it == n->params.end()) return std::string();
        try {
            return fill_game(it->second);
        } catch (const GraphError&) {
            return std::nullopt;  // {game} with no folder set: unknown
        }
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
        if (n->type == "MeshMask") {  // drawn straight at the thumbnail's size
            const auto mesh_file = text("mesh"), size_file = text("size_of");
            const auto grow = number_of("grow");
            if (!mesh_file || mesh_file->empty() || !size_file || size_file->empty() || !grow || !profile)
                return std::nullopt;
            const MeshModel mesh = read_mesh(clean_path(*mesh_file, base_dir));
            const auto [w, h] = picture_size(clean_path(*size_file, base_dir), *profile);
            const float scale = std::min(1.0f, float(max_side) / float(std::max(w, h)));
            const unsigned sw = std::max(1u, unsigned(w * scale)), sh = std::max(1u, unsigned(h * scale));
            const auto parts = pick_parts(mesh, text("materials").value_or(""));
            return ImagePreview{{sw, sh, uv_mask(parts, sw, sh, unsigned(std::lround(*grow * scale)))}, scale};
        }
        if (n->type == "MaskBlend") {
            auto base = image("base");
            auto edited = image("edited");
            auto mask = image("mask");
            const auto feather = number_of("feather");
            if (!base || !edited || !mask || !feather) return std::nullopt;
            for (ImagePreview* p : {&*edited, &*mask})
                if (p->image.width != base->image.width || p->image.height != base->image.height)
                    p->image = resize_image(p->image, base->image.width, base->image.height, Fit::Stretch);
            masked_blend(base->image, edited->image, mask->image, *feather * base->scale,
                         text("invert").value_or("") == "true");
            return base;
        }
        if (n->type == "PickChannel") {
            auto img = image("image");
            if (!img) return std::nullopt;
            img->image = channel_image(img->image, channel_of(text("channel").value_or("alpha")));
            return img;
        }
        if (n->type == "MergeChannels") {
            auto base = image("base");
            if (!base) return std::nullopt;
            std::array<std::optional<ImagePreview>, 5> parts;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (!source(kMergeInputs[i]) && text(kMergeInputs[i]).value_or("").empty()) continue;  // not given
                parts[i] = image(kMergeInputs[i]);
                if (!parts[i]) return std::nullopt;  // given, not known yet
                if (parts[i]->image.width != base->image.width || parts[i]->image.height != base->image.height)
                    parts[i]->image = resize_image(parts[i]->image, base->image.width, base->image.height, Fit::Stretch);
            }
            merge_channels(base->image, parts[0] ? &parts[0]->image : nullptr,
                           {parts[1] ? &parts[1]->image : nullptr, parts[2] ? &parts[2]->image : nullptr,
                            parts[3] ? &parts[3]->image : nullptr, parts[4] ? &parts[4]->image : nullptr});
            return base;
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
