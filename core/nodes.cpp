#include "node_run.hpp"

#include "image.hpp"
#include "package.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>

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
    };
}

NodeSpec export_image() {
    return {
        .type = "ExportImage",
        .title = "Export image",
        .summary = "Converts the texture to an image file to edit: PNG, TGA or JPG, whichever the file name ends in. "
                   "If the file is already there (your edited version), it's kept, never overwritten.",
        .inputs = {{.name = "tex", .label = "texture", .type = Tex, .required = true}},
        .outputs = {{.name = "png", .type = Image, .label = "image", .field = "png", .field_label = "Image file",
                     .hint = "Where to write the image. Its ending picks the format: .png, .tga (e.g. for GIMP) or "
                             ".jpg. JPG loses some quality and all transparency.",
                     .path = PathKind::SaveFile, .filter = kEditImageFormats}},
        .run = [](NodeRun& r) {
            const Value tex = r.input("tex");
            const fs::path png = r.resolve(r.text("png"));
            if (!fs::exists(png)) {
                r.run.options.converter.load_tex(tex.path, png, r.profile());
                r.run.fresh_exports.insert(r.node.id);
                r.done("exported " + png.filename().string(), png);
            } else {
                r.done("kept your " + png.filename().string(), png);
            }
            r.output("png", file_value(png, tex.game_path));
        },
    };
}

NodeSpec edit_image() {
    return {
        .type = "EditImage",
        .title = "Edit image",
        .summary = "YOUR STEP: open the image in any image editor, change it, save it (same size and format), then "
                   "click Done editing. The run waits here until you do.",
        .inputs = {{.name = "png", .label = "image to edit", .type = Image, .required = true}},
        .outputs = {{"image", Image, "edited image"}},
        .state = {"done"},
        .manual = true,
        .run = [](NodeRun& r) {
            const Value png = r.input("png");
            const Graph& g = r.run.graph;
            const bool re_exported = r.run.fresh_exports.contains(g.links[g.links_into(r.node.id, "png").at(0)].from_node);
            if (re_exported) r.run.result.reset_edits.push_back(r.node.id);  // a new image: an earlier "done" doesn't count
            const auto flag = r.node.params.find("done");
            if (r.run.options.edits_done || (!re_exported && flag != r.node.params.end() && flag->second == "true")) {
                r.output("image", png);
                r.done("edited " + png.path.filename().string(), png.path);
            } else {
                r.wait("edit " + png.path.filename().string() + ", then click Done editing", png.path);
                r.log("waiting for you to edit " + png.path.string());
            }
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
        .run = [](NodeRun& r) {
            const fs::path png = r.resolve(r.text("png"));
            if (!fs::is_regular_file(png)) throw GraphError("image not found: " + png.string());
            r.output("image", file_value(png));
            r.done("using " + png.filename().string(), png);
        },
    };
}

NodeSpec save_tex() {
    return {
        .type = "SaveTex",
        .title = "Convert image to texture",
        .summary = "Turns the edited image back into a game texture with the original's size, format and mipmaps.",
        .inputs = {{.name = "image", .label = "edited image", .type = Image, .required = true},
                   {.name = "original", .label = "original texture", .type = Tex, .required = true}},
        .outputs = {{"tex", Tex, "new texture"}},
        .run = [](NodeRun& r) {
            const Value original = r.input("original");
            const fs::path out = r.run.work_dir / (std::to_string(r.node.id) + ".tex." + r.profile().tex_suffix);
            const TexMeta m = r.run.options.converter.save_tex(r.input("image").path, original.path, out, r.profile());
            r.output("tex", file_value(out, original.game_path));
            r.done("encoded " + m.format + ", " + std::to_string(m.mip_count) + " mips");
            // Waiting (CLAUDE.md §9): whether the game minds a different mip count is untested, so say so.
            const std::uint32_t original_mips = read_tex_meta(original.path, r.profile()).mip_count;
            if (m.mip_count != original_mips)
                r.warn(original.path.filename().string() + " has " + std::to_string(original_mips) +
                       " mip level(s), the new texture has " + std::to_string(m.mip_count) +
                       " (Noesis always writes them down to 8x8). It may work in game; if the texture looks wrong or "
                       "the game misbehaves, this is the likely cause.");
        },
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
                    .hint = "Where <Mod name>\\ and <Mod name>.zip are created.", .path = PathKind::Folder},
                   {.name = "version", .label = "Version", .type = Text, .widget = Widget::Text,
                    .hint = "Shown in Fluffy."},
                   {.name = "author", .label = "Author", .type = Text, .widget = Widget::Text,
                    .hint = "Shown in Fluffy."},
                   {.name = "description", .label = "Description", .type = Text, .widget = Widget::Text,
                    .hint = "Shown in Fluffy."},
                   {.name = "replace", .label = "Replace existing", .type = Text, .widget = Widget::Checkbox,
                    .hint = "Overwrite this mod's previous folder and .zip in the output folder. Only output this tool "
                            "made for the same mod name is replaced."}},
        .outputs = {},
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
                spec.screenshot = r.run.work_dir / "preview.png";
                tile_images(previews, spec.screenshot);
                if (previews.size() > 1) r.log("combined " + std::to_string(previews.size()) + " previews");
            }
            const fs::path root = build_package(r.profile(), spec);
            r.done("packaged " + std::to_string(spec.files.size()) + " texture(s) into " + root.filename().string() +
                       ".zip",
                   fs::path(root) += ".zip");
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
        .run = [](NodeRun& r) {
            std::vector<std::string> parts;
            for (const auto& v : r.values("parts")) parts.push_back(v.text);
            const std::string out = fill_template(r.text("text"), parts);
            r.output("text", {out, {}, {}});
            r.done("\"" + out + "\"");
        },
    };
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
                    .path = PathKind::SaveFile},
                   {.name = "if_exists", .label = "Overwrite mode", .type = Text, .widget = Widget::Choice,
                    .required = true,
                    .hint = "What to do when the destination file is already there. Skip keeps it and passes it on.",
                    .initial = "fail",
                    .options = {{"fail", "Fail if exists"}, {"overwrite", "Overwrite"}, {"skip", "Skip if exists"}}},
                   {.name = "create_dirs", .label = "Create folders", .type = Text, .widget = Widget::Checkbox,
                    .hint = "Create the destination's folder (and any above it) if it doesn't exist.",
                    .initial = "true"}},
        .outputs = {{"path", Path, "copied path"}},
        .run = [](NodeRun& r) {
            const fs::path source = clean_path(r.text("source"), r.run.options.base_dir);
            const std::string mode = r.text("if_exists");
            if (!is_option(*find_input(*find_spec("CopyFile"), "if_exists"), mode))  // a linked value can be anything
                throw GraphError("Overwrite mode can't be '" + mode + "'");
            if (!fs::exists(long_path(source))) throw GraphError("source file not found: " + source.string());
            if (fs::is_directory(long_path(source)))
                throw GraphError(source.string() + " is a folder; Copy file copies single files");
            const fs::path target = destination_for(source, clean_path(r.text("dest"), r.run.options.base_dir));
            std::error_code ec;
            if (fs::equivalent(long_path(source), long_path(target), ec))
                throw GraphError("source and destination are the same file: " + target.string());
            const bool exists = fs::exists(long_path(target));
            if (exists)  // the editor may have warned earlier; this is what actually happens
                r.log("warning: destination exists: " + target.string() + " (" +
                      (mode == "overwrite" ? "overwritten" : mode == "skip" ? "skipped" : "failed") + ")");
            if (exists && mode == "fail")
                throw GraphError("destination exists: " + target.string() +
                                 " (choose Overwrite or Skip if exists to allow that)");
            if (exists && mode == "skip") {
                r.done("kept existing " + target.filename().string(), target);
            } else {
                try {
                    if (r.text("create_dirs") == "true")
                        fs::create_directories(long_path(target.parent_path()));
                    else if (!fs::is_directory(long_path(target.parent_path())))
                        throw GraphError("destination folder doesn't exist: " + target.parent_path().string() +
                                         " (tick Create folders)");
                    fs::copy_file(long_path(source), long_path(target), fs::copy_options::overwrite_existing);
                } catch (const fs::filesystem_error& e) {
                    throw GraphError("can't write " + target.string() + ": " + e.code().message());
                }
                r.done("copied to " + target.string(), target);
            }
            r.output("path", file_value(target));
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
        .run = [](NodeRun& r) {
            r.output("out", r.input("in"));  // as is: a texture keeps its game path
            r.done("passed on to " + std::to_string(r.run.graph.links_from(r.node.id, "out").size()) + " step(s)");
        },
    };
}

}  // namespace

const std::vector<NodeSpec>& node_specs() {
    static const std::vector<NodeSpec> specs{
        load_tex(),   export_image(), edit_image(),
        import_image(), save_tex(),   package_mod(),
        copy_file(),  value(),        text_node(), split(),
    };
    return specs;
}

const NodeSpec* find_spec(std::string_view type) {
    for (const auto& s : node_specs())
        if (type == s.type) return &s;
    return nullptr;
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
    for (const Node& n : g.nodes) {
        if (n.type != "CopyFile" || g.is_connected(n.id, "dest", false)) continue;
        try {
            const fs::path dest = clean_path(param(n, "dest"), base_dir);
            if (dest.empty()) continue;
            const bool folder = !dest.has_filename() || fs::is_directory(long_path(dest));
            if (folder && (g.is_connected(n.id, "source", false) || param(n, "source").empty())) continue;  // no name yet
            const fs::path target = destination_for(clean_path(param(n, "source"), base_dir), dest);
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

}  // namespace remod
