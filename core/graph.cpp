#include "graph.hpp"

#include "image.hpp"
#include "package.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

namespace remod {

namespace fs = std::filesystem;
using json = nlohmann::json;

const std::vector<NodeSpec>& node_specs() {
    using enum PortType;
    static const std::vector<NodeSpec> specs{
        {"LoadTex",
         "Original texture",
         "Picks the game's original .tex. Nothing is converted here: its size and format are read so the later "
         "steps can match them.",
         {{.name = "tex", .label = "Texture file", .type = Tex, .widget = Widget::Path, .required = true,
           .hint = "The original texture, e.g. from your REtool folder. Named .tex or .tex.<version> - either "
                   "works; the game is detected from the file itself.",
           .path = PathKind::OpenTexture},
          {.name = "game_path", .label = "In-game path", .type = Text, .widget = Widget::Text,
           .hint = "Where the texture lives in the game, under natives/STM. Leave empty if the file is inside a "
                   "natives\\STM\\... folder: it's worked out automatically. The .tex version suffix is added for "
                   "you."}},
         {{"tex", Tex, "texture"}}},
        {"ExportImage",
         "Export image",
         "Converts the texture to an image file to edit: PNG, TGA or JPG, whichever the file name ends in. If the "
         "file is already there (your edited version), it's kept, never overwritten.",
         {{.name = "tex", .label = "texture", .type = Tex, .required = true}},
         {{.name = "png", .type = Image, .label = "image", .field = "png", .field_label = "Image file",
           .hint = "Where to write the image. Its ending picks the format: .png, .tga (e.g. for GIMP) or .jpg. "
                   "JPG loses some quality and all transparency.",
           .path = PathKind::SaveFile, .filter = kEditImageFormats}}},
        {"EditImage",
         "Edit image",
         "YOUR STEP: open the image in any image editor, change it, save it (same size and format), then click "
         "Done editing. The run waits here until you do.",
         {{.name = "png", .label = "image to edit", .type = Image, .required = true}},
         {{"image", Image, "edited image"}},
         {"done"},
         true},
        {"ImportImage",
         "Use existing image",
         "Uses an image you've already edited (or any image, e.g. for the preview), instead of exporting one.",
         {{.name = "png", .label = "Image file", .type = Image, .widget = Widget::Path, .required = true,
           .hint = "A PNG, TGA or JPG. For a texture it must be the same size as the original.",
           .path = PathKind::OpenFile, .filter = kEditImageFormats}},
         {{"image", Image, "image"}}},
        {"SaveTex",
         "Convert image to texture",
         "Turns the edited image back into a game texture with the original's size, format and mipmaps.",
         {{.name = "image", .label = "edited image", .type = Image, .required = true},
          {.name = "original", .label = "original texture", .type = Tex, .required = true}},
         {{"tex", Tex, "new texture"}}},
        {"PackageMod",
         "Package for Fluffy",
         "Builds the mod folder and a .zip to add in Fluffy Mod Manager. Connect as many textures as the mod "
         "replaces; previews are combined into the one image Fluffy shows.",
         {{.name = "tex", .label = "texture", .type = Tex, .required = true, .multiple = true,
           .hint = "Each new texture in the mod. A new line appears as you connect one."},
          {.name = "preview", .label = "preview", .type = Image, .multiple = true,
           .hint = "Images for Fluffy's preview, e.g. the edited images. Several are tiled into one picture."},
          {.name = "name", .label = "Mod name", .type = Text, .widget = Widget::Text, .required = true,
           .hint = "Shown in Fluffy; also the folder and .zip name."},
          {.name = "out", .label = "Output folder", .type = Text, .widget = Widget::Path, .required = true,
           .hint = "Where <Mod name>\\ and <Mod name>.zip are created.", .path = PathKind::Folder},
          {.name = "version", .label = "Version", .type = Text, .widget = Widget::Text, .hint = "Shown in Fluffy."},
          {.name = "author", .label = "Author", .type = Text, .widget = Widget::Text, .hint = "Shown in Fluffy."},
          {.name = "description", .label = "Description", .type = Text, .widget = Widget::Text,
           .hint = "Shown in Fluffy."},
          {.name = "replace", .label = "Replace existing", .type = Text, .widget = Widget::Checkbox,
           .hint = "Overwrite this mod's previous folder and .zip in the output folder. Only output this tool made "
                   "for the same mod name is replaced."}},
         {}},
        {"Text",
         "Text",
         "A piece of text to feed into any field, e.g. one mod name used in several places. {1}, {2}, ... are "
         "replaced by whatever is connected to the numbered inputs.",
         {{.name = "text", .label = "Text", .type = Text, .widget = Widget::Text,
           .hint = "The text. Use {1}, {2}, ... to insert the connected inputs, e.g. \"{1} v2\"."},
          {.name = "parts", .label = "part", .type = Text, .multiple = true,
           .hint = "Values for {1}, {2}, ... in connection order. Files give their full path."}},
         {{"text", Text, "text"}}},
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

bool accepts(const InputSpec& in, PortType out) {
    return in.type == out || in.type == PortType::Text || (out == PortType::Text && in.editable());
}

namespace {

const PortSpec* find_output(const NodeSpec& spec, const std::string& name) {
    for (const auto& p : spec.outputs)
        if (name == p.name) return &p;
    return nullptr;
}

const char* type_name(PortType t) {
    switch (t) {
    case PortType::Tex: return "a texture";
    case PortType::Image: return "an image";
    case PortType::Text: return "text";
    }
    return "?";
}

// "Export image for editing (node 2)": the user-facing name first, the id to find it by.
std::string node_label(const Node& n) {
    const NodeSpec* spec = find_spec(n.type);
    return std::string(spec ? spec->title : n.type.c_str()) + " (node " + std::to_string(n.id) + ")";
}

// True if `path` ends in one of the comma-separated extensions in `filter` ("png,jpg"), ignoring case.
bool has_extension(const std::string& path, const char* filter) {
    std::string ext = fs::path(path).extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext.size() < 2) return false;
    std::istringstream list(filter);
    for (std::string item; std::getline(list, item, ',');)
        if (ext.substr(1) == item) return true;
    return false;
}

// "png,tga" -> ".png, .tga"
std::string extension_list(const char* filter) {
    std::string exts = ".";
    for (const char* c = filter; *c; ++c) exts += *c == ',' ? std::string(", .") : std::string(1, *c);
    return exts;
}

// Kahn's algorithm; ties keep file order so runs are deterministic. Returns fewer nodes than exist on a cycle.
std::vector<const Node*> topo_order(const Graph& g) {
    std::map<int, int> indegree;
    for (const auto& n : g.nodes) indegree[n.id] = 0;
    for (const auto& l : g.links)
        if (indegree.contains(l.from_node) && indegree.contains(l.to_node)) ++indegree[l.to_node];
    std::vector<const Node*> order;
    std::set<int> done;
    for (bool progress = true; progress;) {
        progress = false;
        for (const auto& n : g.nodes) {
            if (done.contains(n.id) || indegree[n.id] != 0) continue;
            done.insert(n.id);
            order.push_back(&n);
            for (const auto& l : g.links)
                if (l.from_node == n.id && indegree.contains(l.to_node)) --indegree[l.to_node];
            progress = true;
        }
    }
    return order;
}

// Checks one link against the node specs; "" if fine.
std::string check_link(const Graph& g, const Link& l) {
    const Node* from = g.find(l.from_node);
    const Node* to = g.find(l.to_node);
    if (!from || !to) return "link refers to a missing node";
    if (from == to) return "a node can't link to itself";
    const NodeSpec* from_spec = find_spec(from->type);
    const NodeSpec* to_spec = find_spec(to->type);
    if (!from_spec || !to_spec) return "link touches a node of unknown type";
    const PortSpec* out = find_output(*from_spec, l.from_port);
    const InputSpec* in = find_input(*to_spec, l.to_port);
    if (!out) return node_label(*from) + " has no output '" + l.from_port + "'";
    if (!in) return node_label(*to) + " has no input '" + l.to_port + "'";
    if (!accepts(*in, out->type))
        return std::string("'") + out->label + "' can't go into '" + in->label + "': that input needs " +
               type_name(in->type);
    return "";
}

struct Value {
    std::string text;       // what a text input sees: the text itself, or a file's full path
    fs::path path;          // files only
    std::string game_path;  // textures only; "" if unknown
};

Value file_value(const fs::path& p, std::string game_path = {}) { return {p.string(), p, std::move(game_path)}; }

// Scratch folder for intermediate files of one run, removed afterwards.
struct TempDir {
    fs::path path = fs::temp_directory_path() / ("remod_run_" + std::to_string(std::random_device{}()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

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

}  // namespace

Node& Graph::add_node(const std::string& type) {
    const NodeSpec* spec = find_spec(type);
    if (!spec) throw GraphError("unknown node type '" + type + "'");
    int id = 1;
    for (const auto& n : nodes) id = std::max(id, n.id + 1);
    Node n{.id = id, .type = type};
    for (const auto& in : spec->inputs)
        if (in.editable()) n.params[in.name] = "";
    for (const auto& out : spec->outputs)
        if (out.field) n.params[out.field] = "";
    return nodes.emplace_back(std::move(n));
}

void Graph::remove_node(int id) {
    std::erase_if(links, [id](const Link& l) { return l.from_node == id || l.to_node == id; });
    std::erase_if(nodes, [id](const Node& n) { return n.id == id; });
}

std::string Graph::can_connect(const Link& link) const {
    if (auto err = check_link(*this, link); !err.empty()) return err;
    const InputSpec* in = find_input(*find_spec(find(link.to_node)->type), link.to_port);
    if (!in->multiple && !links_into(link.to_node, link.to_port).empty())
        return std::string("'") + in->label + "' already has a link; delete that one first";
    Graph trial = *this;  // ponytail: copies the graph per check; fine for hand-built graphs of a few nodes
    trial.links.push_back(link);
    if (topo_order(trial).size() != trial.nodes.size()) return "that link would create a loop";
    return "";
}

std::string Graph::connect(const Link& link) {
    auto err = can_connect(link);
    if (err.empty()) links.push_back(link);
    return err;
}

void Graph::disconnect(size_t link_index) {
    if (link_index < links.size()) links.erase(links.begin() + static_cast<std::ptrdiff_t>(link_index));
}

bool Graph::is_connected(int node, const std::string& port, bool output) const {
    return std::ranges::any_of(links, [&](const Link& l) {
        return output ? l.from_node == node && l.from_port == port : l.to_node == node && l.to_port == port;
    });
}

std::vector<size_t> Graph::links_into(int node, const std::string& input) const {
    std::vector<size_t> out;
    for (size_t i = 0; i < links.size(); ++i)
        if (links[i].to_node == node && links[i].to_port == input) out.push_back(i);
    return out;
}

const Node* Graph::find(int id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

Node* Graph::find(int id) { return const_cast<Node*>(std::as_const(*this).find(id)); }

namespace {

// The input of `spec` that best takes an output of type `out`: exact type first, then (for text) an editable
// field or a multiple Text input. nullptr if none.
const InputSpec* best_input(const NodeSpec& spec, PortType out) {
    for (const auto& in : spec.inputs)
        if (in.type == out) return &in;
    for (const auto& in : spec.inputs)
        if (accepts(in, out) && (in.multiple || out == PortType::Text)) return &in;
    return nullptr;
}

// The output of `spec` that `in` best takes: exact type first. nullptr if none.
const PortSpec* best_output(const NodeSpec& spec, const InputSpec& in) {
    for (const auto& out : spec.outputs)
        if (out.type == in.type) return &out;
    for (const auto& out : spec.outputs)
        if (accepts(in, out.type)) return &out;
    return nullptr;
}

const InputSpec* input_of(const Graph& g, int node, const std::string& port) {
    const Node* n = g.find(node);
    const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
    return spec ? find_input(*spec, port) : nullptr;
}

const PortSpec* output_of(const Graph& g, int node, const std::string& port) {
    const Node* n = g.find(node);
    const NodeSpec* spec = n ? find_spec(n->type) : nullptr;
    return spec ? find_output(*spec, port) : nullptr;
}

}  // namespace

std::vector<Graph::Choice> Graph::choices_for_pin(int node, const std::string& port, bool output) const {
    std::vector<Choice> out;
    if (output) {
        const PortSpec* p = output_of(*this, node, port);
        if (!p) return out;
        for (const auto& spec : node_specs())
            if (const InputSpec* in = best_input(spec, p->type)) out.push_back({&spec, in->name});
    } else {
        const InputSpec* in = input_of(*this, node, port);
        if (!in) return out;
        for (const auto& spec : node_specs())
            if (const PortSpec* p = best_output(spec, *in)) out.push_back({&spec, p->name});
    }
    return out;
}

int Graph::add_connected(const Choice& choice, int node, const std::string& port, bool output) {
    const int id = add_node(choice.spec->type).id;
    if (!output) {  // the new node feeds this input: a single input's old link makes way
        const InputSpec* in = input_of(*this, node, port);
        if (in && !in->multiple) {
            const auto old = links_into(node, port);
            for (auto it = old.rbegin(); it != old.rend(); ++it) disconnect(*it);
        }
    }
    const Link link = output ? Link{node, port, id, choice.port} : Link{id, choice.port, node, port};
    if (auto err = connect(link); !err.empty()) {
        remove_node(id);
        throw GraphError(err);
    }
    return id;
}

std::vector<const NodeSpec*> Graph::choices_for_link(size_t link) const {
    std::vector<const NodeSpec*> out;
    if (link >= links.size()) return out;
    const PortSpec* src = output_of(*this, links[link].from_node, links[link].from_port);
    const InputSpec* dst = input_of(*this, links[link].to_node, links[link].to_port);
    if (!src || !dst) return out;
    for (const auto& spec : node_specs())
        if (best_input(spec, src->type) && best_output(spec, *dst)) out.push_back(&spec);
    return out;
}

int Graph::insert_node(size_t link, const std::string& type) {
    if (link >= links.size()) throw GraphError("no such link");
    const NodeSpec* spec = find_spec(type);
    if (!spec) throw GraphError("unknown node type '" + type + "'");
    const Link old = links[link];
    const PortSpec* src = output_of(*this, old.from_node, old.from_port);
    const InputSpec* dst = input_of(*this, old.to_node, old.to_port);
    const InputSpec* in = src ? best_input(*spec, src->type) : nullptr;
    const PortSpec* out = dst ? best_output(*spec, *dst) : nullptr;
    if (!in || !out) throw GraphError(std::string(spec->title) + " can't go on that link");

    const Graph before = *this;
    disconnect(link);
    const int id = add_node(type).id;
    const std::string err1 = connect({old.from_node, old.from_port, id, in->name});
    const std::string err2 = err1.empty() ? connect({id, out->name, old.to_node, old.to_port}) : "";
    if (!err1.empty() || !err2.empty()) {
        *this = before;
        throw GraphError(err1.empty() ? err2 : err1);
    }
    return id;
}

int Graph::duplicate_node(int id) {
    const Node* n = find(id);
    if (!n) throw GraphError("no such node");
    const NodeSpec* spec = find_spec(n->type);
    Node copy = *n;
    if (spec)
        for (const char* state : spec->state) copy.params.erase(state);
    int new_id = 1;
    for (const auto& other : nodes) new_id = std::max(new_id, other.id + 1);
    copy.id = new_id;
    copy.x += 40;
    copy.y += 40;
    nodes.push_back(std::move(copy));
    return new_id;
}

void Graph::disconnect_node(int id) {
    std::erase_if(links, [id](const Link& l) { return l.from_node == id || l.to_node == id; });
}

std::vector<std::string> Graph::validate() const {
    std::vector<std::string> errors;
    if (profile.empty()) errors.push_back("graph has no profile");
    if (nodes.empty()) errors.push_back("graph has no nodes");

    std::set<int> ids;
    for (const auto& n : nodes) {
        if (!ids.insert(n.id).second) errors.push_back("duplicate node id " + std::to_string(n.id));
        const NodeSpec* spec = find_spec(n.type);
        if (!spec) {
            errors.push_back(node_label(n) + ": unknown node type");
            continue;
        }
        for (const auto& in : spec->inputs) {
            const size_t linked = links_into(n.id, in.name).size();
            if (linked > 1 && !in.multiple)
                errors.push_back(node_label(n) + ": '" + in.label + "' is connected more than once");
            if (in.editable() && linked == 0) {  // the typed value counts
                const auto it = n.params.find(in.name);
                const bool empty = it == n.params.end() || it->second.empty();
                if (in.required && empty) errors.push_back(node_label(n) + ": " + in.label + " is required");
                if (!empty && in.filter && !has_extension(it->second, in.filter))
                    errors.push_back(node_label(n) + ": " + in.label + " must end in " + extension_list(in.filter));
            } else if (!in.editable() && in.required && linked == 0) {
                errors.push_back(node_label(n) + ": input '" + in.label + "' is not connected");
            }
        }
        for (const auto& out : spec->outputs) {  // where the node writes an output: always required
            if (!out.field) continue;
            const auto it = n.params.find(out.field);
            if (it == n.params.end() || it->second.empty())
                errors.push_back(node_label(n) + ": " + out.field_label + " is required");
            else if (out.filter && !has_extension(it->second, out.filter))
                errors.push_back(node_label(n) + ": " + out.field_label + " must end in " + extension_list(out.filter));
        }
        for (const auto& [key, _] : n.params) {
            const InputSpec* in = find_input(*spec, key);
            const bool known = (in && in->editable()) ||
                               std::ranges::any_of(spec->outputs, [&](const PortSpec& o) { return o.field && key == o.field; }) ||
                               std::ranges::any_of(spec->state, [&](const char* s) { return key == s; });
            if (!known) errors.push_back(node_label(n) + ": unknown parameter '" + key + "'");
        }
    }
    for (const auto& l : links)
        if (auto err = check_link(*this, l); !err.empty()) errors.push_back(err);
    if (errors.empty() && topo_order(*this).size() != nodes.size()) errors.push_back("graph contains a loop");
    return errors;
}

namespace {

// Graph files from before inputs were unified: PackageMod had a typed `screenshot` path. An empty one is
// dropped; a set one becomes an ImportImage node feeding the new `preview` input.
void migrate(Graph& g) {
    std::vector<std::pair<int, std::string>> screenshots;
    for (auto& n : g.nodes) {
        if (n.type != "PackageMod") continue;
        if (const auto it = n.params.find("screenshot"); it != n.params.end()) {
            if (!it->second.empty()) screenshots.emplace_back(n.id, it->second);
            n.params.erase(it);
        }
        n.params.try_emplace("replace", "");
    }
    for (const auto& [package, path] : screenshots) {
        const Node* p = g.find(package);
        const float x = p->x - 450, y = p->y + 350;
        Node& img = g.add_node("ImportImage");
        img.params["png"] = path;
        img.x = x;
        img.y = y;
        g.links.push_back({img.id, "image", package, "preview"});
    }

    // Export image used to pause for editing itself, with an output named "image". Editing is now its own
    // Edit image step: insert one after each such export and route the old links through it.
    std::vector<int> exports;
    for (const auto& n : g.nodes)
        if (n.type == "ExportImage") exports.push_back(n.id);
    for (int id : exports) {
        const bool old_links = std::ranges::any_of(g.links, [&](const Link& l) { return l.from_node == id && l.from_port == "image"; });
        if (!old_links) continue;
        const Node* e = g.find(id);
        const float x = e->x, y = e->y + 300;
        Node& edit = g.add_node("EditImage");
        edit.x = x;
        edit.y = y;
        for (auto& l : g.links)
            if (l.from_node == id && l.from_port == "image") l = {edit.id, "image", l.to_node, l.to_port};
        g.links.push_back({id, "png", edit.id, "png"});
    }
}

}  // namespace

Graph load_graph(const fs::path& file) {
    std::ifstream in(file);
    if (!in) throw GraphError("cannot open graph file " + file.string());
    try {
        const json j = json::parse(in);
        if (j.at("schema_version").get<int>() != 0)
            throw GraphError(file.string() + ": unsupported schema_version (expected 0)");
        Graph g;
        g.profile = j.at("profile").get<std::string>();
        for (const auto& jn : j.at("nodes")) {
            Node n{.id = jn.at("id").get<int>(), .type = jn.at("type").get<std::string>()};
            if (jn.contains("params")) n.params = jn["params"].get<std::map<std::string, std::string>>();
            if (jn.contains("pos")) {
                n.x = jn["pos"].at(0).get<float>();
                n.y = jn["pos"].at(1).get<float>();
            }
            g.nodes.push_back(std::move(n));
        }
        for (const auto& jl : j.value("links", json::array()))
            g.links.push_back({jl.at("from").at(0).get<int>(), jl.at("from").at(1).get<std::string>(),
                               jl.at("to").at(0).get<int>(), jl.at("to").at(1).get<std::string>()});
        migrate(g);
        return g;
    } catch (const json::exception& e) {
        throw GraphError(file.string() + ": invalid graph file: " + e.what());
    }
}

void save_graph(const Graph& g, const fs::path& file) {
    json j{{"schema_version", 0}, {"profile", g.profile}, {"nodes", json::array()}, {"links", json::array()}};
    for (const auto& n : g.nodes)
        j["nodes"].push_back({{"id", n.id}, {"type", n.type}, {"params", n.params}, {"pos", {n.x, n.y}}});
    for (const auto& l : g.links)
        j["links"].push_back({{"from", {l.from_node, l.from_port}}, {"to", {l.to_node, l.to_port}}});
    std::ofstream out(file);
    out << j.dump(2) << "\n";
    if (!out.flush()) throw GraphError("failed to write " + file.string());
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

RunResult run_graph(const Graph& g, const RunOptions& opt) {
    if (const auto errors = g.validate(); !errors.empty()) {
        std::string msg = "graph can't run:";
        for (const auto& e : errors) msg += "\n  " + e;
        throw GraphError(msg);
    }
    if (g.profile != opt.profile.id)
        throw GraphError("graph is for profile '" + g.profile + "' but '" + opt.profile.id + "' was loaded");

    auto log = [&](const std::string& s) {
        if (opt.log) opt.log(s);
    };
    auto resolve = [&](const std::string& p) -> fs::path {
        if (p.empty()) return {};
        const fs::path path(p);
        return (path.is_absolute() ? path : opt.base_dir / path).lexically_normal();
    };

    TempDir work;
    std::map<std::pair<int, std::string>, Value> outputs;
    RunResult result;
    // Edit image steps still waiting for the user, and everything downstream of them: skipped this run. Other
    // branches still run, so one run exports every image that needs editing.
    std::set<int> waiting;
    std::set<int> fresh_exports;  // Export image steps that wrote a new image in this run
    std::vector<fs::path> to_edit;

    for (const Node* node : topo_order(g)) {
        const Node& n = *node;
        if (std::ranges::any_of(g.links, [&](const Link& l) { return l.to_node == n.id && waiting.contains(l.from_node); })) {
            waiting.insert(n.id);
            result.nodes[n.id] = {NodeState::NotReached, "waits for an earlier step", {}};
            continue;
        }
        // Every value linked into `input`, in link order.
        auto values = [&](const char* input) {
            std::vector<Value> v;
            for (size_t i : g.links_into(n.id, input)) v.push_back(outputs.at({g.links[i].from_node, g.links[i].from_port}));
            return v;
        };
        // An editable input: its link if connected, else the typed value.
        auto text = [&](const char* input) {
            if (auto v = values(input); !v.empty()) return v.front().text;
            const auto it = n.params.find(input);
            return it == n.params.end() ? std::string() : it->second;
        };
        auto input = [&](const char* name) { return values(name).at(0); };  // required link-only inputs
        auto done = [&](const std::string& message, const fs::path& file = {}) {
            result.nodes[n.id] = {NodeState::Done, message, file};
            log(node_label(n) + ": " + message);
        };

        try {
            if (n.type == "LoadTex") {
                const fs::path tex = resolve(text("tex"));
                const TexMeta m = read_tex_meta(tex, opt.profile);
                std::string game_path = text("game_path");
                const auto split = split_natives(tex, opt.profile.natives_root);
                if (game_path.empty() && split) game_path = split->second;
                outputs[{n.id, "tex"}] = file_value(tex, game_path);
                done(tex.filename().string() + " " + std::to_string(m.width) + "x" + std::to_string(m.height) + " " +
                         m.format + ", " + std::to_string(m.mip_count) + " mips",
                     tex);
                if (split && !split->second.starts_with("streaming/") &&
                    fs::exists(split->first / "streaming" / split->second))
                    log(node_label(n) + ": note: the game also has a high-resolution streaming/" + split->second +
                        "; if your change doesn't show in game, that copy may need replacing too (CLAUDE.md §9)");
            } else if (n.type == "ExportImage") {
                const Value tex = input("tex");
                const fs::path png = resolve(text("png"));
                if (!fs::exists(png)) {
                    opt.converter.load_tex(tex.path, png, opt.profile);
                    fresh_exports.insert(n.id);
                    done("exported " + png.filename().string(), png);
                } else {
                    done("kept your " + png.filename().string(), png);
                }
                outputs[{n.id, "png"}] = file_value(png, tex.game_path);
            } else if (n.type == "EditImage") {
                const Value png = input("png");
                const size_t in = g.links_into(n.id, "png").at(0);
                const bool re_exported = fresh_exports.contains(g.links[in].from_node);
                if (re_exported) result.reset_edits.push_back(n.id);  // a new image: an earlier "done" doesn't count
                const auto flag = n.params.find("done");
                if (opt.edits_done || (!re_exported && flag != n.params.end() && flag->second == "true")) {
                    outputs[{n.id, "image"}] = png;
                    done("edited " + png.path.filename().string(), png.path);
                } else {
                    waiting.insert(n.id);
                    to_edit.push_back(png.path);
                    result.nodes[n.id] = {NodeState::Waiting,
                                          "edit " + png.path.filename().string() + ", then click Done editing", png.path};
                    log(node_label(n) + ": waiting for you to edit " + png.path.string());
                }
            } else if (n.type == "ImportImage") {
                const fs::path png = resolve(text("png"));
                if (!fs::is_regular_file(png)) throw GraphError("image not found: " + png.string());
                outputs[{n.id, "image"}] = file_value(png);
                done("using " + png.filename().string(), png);
            } else if (n.type == "SaveTex") {
                const Value original = input("original");
                const fs::path out = work.path / (std::to_string(n.id) + ".tex." + opt.profile.tex_suffix);
                const TexMeta m = opt.converter.save_tex(input("image").path, original.path, out, opt.profile);
                outputs[{n.id, "tex"}] = file_value(out, original.game_path);
                done("encoded " + m.format + ", " + std::to_string(m.mip_count) + " mips");
            } else if (n.type == "Text") {
                std::vector<std::string> parts;
                for (const auto& v : values("parts")) parts.push_back(v.text);
                const std::string text_out = fill_template(text("text"), parts);
                outputs[{n.id, "text"}] = {text_out, {}, {}};
                done("\"" + text_out + "\"");
            } else if (n.type == "PackageMod") {
                PackageSpec spec{.mod_name = text("name"),
                                 .out_dir = resolve(text("out")),
                                 .info = {.name = text("name"),
                                          .version = text("version"),
                                          .description = text("description"),
                                          .author = text("author")},
                                 .replace = text("replace") == "true"};
                for (const auto& tex : values("tex")) {
                    if (tex.game_path.empty())
                        throw GraphError("game path unknown for " + tex.path.filename().string() +
                                         ": fill in 'In-game path' on its Original texture node (the .tex isn't "
                                         "inside a " + opt.profile.natives_root + " folder)");
                    spec.files.push_back({tex.path, tex.game_path});
                }
                std::vector<fs::path> previews;
                for (const auto& v : values("preview")) previews.push_back(v.path);
                // One preview is used as it is, except a TGA: Fluffy's TGA support is unconfirmed ([guide]), so it
                // goes through tile_images like several previews do, which writes a PNG.
                if (previews.size() == 1 && !has_extension(previews[0].string(), "tga")) {
                    spec.screenshot = previews[0];
                } else if (!previews.empty()) {
                    spec.screenshot = work.path / "preview.png";
                    tile_images(previews, spec.screenshot);
                    if (previews.size() > 1)
                        log(node_label(n) + ": combined " + std::to_string(previews.size()) + " previews");
                }
                const fs::path root = build_package(opt.profile, spec);
                done("packaged " + std::to_string(spec.files.size()) + " texture(s) into " + root.filename().string() +
                         ".zip",
                     fs::path(root) += ".zip");
            }
        } catch (const std::exception& e) {
            result.nodes[n.id] = {NodeState::Failed, e.what(), {}};
            for (const auto& other : g.nodes) result.nodes.try_emplace(other.id);  // the rest: not reached
            throw RunError(node_label(n) + ": " + e.what(), std::move(result.nodes));
        }
    }

    if (!to_edit.empty()) {
        result.paused = true;
        result.message = "Waiting for you: edit ";
        result.message += to_edit.size() == 1 ? to_edit[0].string() : std::to_string(to_edit.size()) + " images:";
        if (to_edit.size() > 1)
            for (const auto& p : to_edit) result.message += "\n  " + p.string();
        result.message += std::string("\nthen click Done editing on the Edit image step") + (to_edit.size() > 1 ? "s" : "") +
                          " and Run again (from the CLI: run again with --edited true).";
    } else {
        result.message = "Done.";
    }
    return result;
}

void apply_run(Graph& g, const RunResult& result) {
    for (int id : result.reset_edits) set_edit_done(g, id, false);
}

void set_edit_done(Graph& g, int node, bool done) {
    Node* n = g.find(node);
    if (!n || n->type != "EditImage") return;
    if (done)
        n->params["done"] = "true";
    else
        n->params.erase("done");
}

}  // namespace remod
