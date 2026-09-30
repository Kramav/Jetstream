#include "graph.hpp"

#include "package.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <utility>

namespace remod {

namespace fs = std::filesystem;
using json = nlohmann::json;

const std::vector<NodeSpec>& node_specs() {
    static const std::vector<NodeSpec> specs{
        {"LoadTex",
         "Original texture",
         "Picks the game's original .tex. Nothing is converted here: its size and format are read so the later "
         "steps can match them.",
         {},
         {{"tex", PortType::Tex, "texture"}},
         {{"tex", true, "Texture file",
           "The original texture, e.g. from your extracted game files. Named .tex or .tex.<version> - either "
           "works; the game is detected from the file itself.",
           PathKind::OpenTexture},
          {"game_path", false, "In-game path",
           "Where the texture lives in the game, under natives/STM. Leave empty if the file is inside a "
           "natives\\STM\\... folder: it's worked out automatically. The .tex version suffix is added for you."}}},
        {"ExportImage",
         "Export PNG for editing",
         "Converts the texture to a PNG, then stops the run so you can edit it. On the next run your edited "
         "PNG is kept and passed on.",
         {{"tex", PortType::Tex, "texture"}},
         {{"image", PortType::Image, "edited PNG"}},
         {{"png", true, "PNG file", "Where to write the PNG you'll edit (must end in .png).", PathKind::SaveFile,
           "png"}}},
        {"ImportImage",
         "Use existing PNG",
         "Uses a PNG you've already edited, instead of exporting one.",
         {},
         {{"image", PortType::Image, "PNG"}},
         {{"png", true, "PNG file", "The edited PNG. It must be the same size as the original texture.",
           PathKind::OpenFile, "png"}}},
        {"SaveTex",
         "Convert PNG to texture",
         "Turns the edited PNG back into a game texture with the original's size, format and mipmaps.",
         {{"image", PortType::Image, "edited PNG"}, {"original", PortType::Tex, "original texture"}},
         {{"tex", PortType::Tex, "new texture"}},
         {}},
        {"PackageMod",
         "Package for Fluffy",
         "Builds the mod folder and a .zip to add in Fluffy Mod Manager.",
         {{"tex", PortType::Tex, "new texture"}},
         {},
         {{"name", true, "Mod name", "Shown in Fluffy; also the folder and .zip name."},
          {"out", true, "Output folder", "Where <Mod name>\\ and <Mod name>.zip are created.", PathKind::Folder},
          {"version", false, "Version", "Shown in Fluffy."},
          {"author", false, "Author", "Shown in Fluffy."},
          {"description", false, "Description", "Shown in Fluffy."},
          {"screenshot", false, "Screenshot", "Preview image shown in Fluffy (png, jpg, tga or bmp).",
           PathKind::OpenFile, "png,jpg,tga,bmp"}}},
    };
    return specs;
}

const NodeSpec* find_spec(std::string_view type) {
    for (const auto& s : node_specs())
        if (type == s.type) return &s;
    return nullptr;
}

namespace {

const PortSpec* find_port(const std::vector<PortSpec>& ports, const std::string& name) {
    for (const auto& p : ports)
        if (name == p.name) return &p;
    return nullptr;
}

// "Export PNG for editing (node 2)": the user-facing name first, the id to find it by.
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
    const PortSpec* out = find_port(from_spec->outputs, l.from_port);
    const PortSpec* in = find_port(to_spec->inputs, l.to_port);
    if (!out) return node_label(*from) + " has no output '" + l.from_port + "'";
    if (!in) return node_label(*to) + " has no input '" + l.to_port + "'";
    if (out->type != in->type)
        return std::string("'") + out->label + "' can't go into '" + in->label + "': that input needs " +
               (in->type == PortType::Tex ? "a texture" : "a PNG");
    return "";
}

struct Value {
    fs::path path;
    std::string game_path;  // "" if unknown
};

// Scratch folder for intermediate files of one run, removed afterwards.
struct TempDir {
    fs::path path = fs::temp_directory_path() / ("remod_run_" + std::to_string(std::random_device{}()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

Node& Graph::add_node(const std::string& type) {
    const NodeSpec* spec = find_spec(type);
    if (!spec) throw GraphError("unknown node type '" + type + "'");
    int id = 1;
    for (const auto& n : nodes) id = std::max(id, n.id + 1);
    Node n{.id = id, .type = type};
    for (const auto& p : spec->params) n.params[p.name] = "";
    return nodes.emplace_back(std::move(n));
}

void Graph::remove_node(int id) {
    std::erase_if(links, [id](const Link& l) { return l.from_node == id || l.to_node == id; });
    std::erase_if(nodes, [id](const Node& n) { return n.id == id; });
}

std::string Graph::can_connect(const Link& link) const {
    if (auto err = check_link(*this, link); !err.empty()) return err;
    for (const auto& l : links)
        if (l.to_node == link.to_node && l.to_port == link.to_port)
            return std::string("'") + find_port(find_spec(find(link.to_node)->type)->inputs, link.to_port)->label +
                   "' already has a link; delete that one first";
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

const Node* Graph::find(int id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

Node* Graph::find(int id) { return const_cast<Node*>(std::as_const(*this).find(id)); }

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
        for (const auto& p : spec->params) {
            const auto it = n.params.find(p.name);
            const bool empty = it == n.params.end() || it->second.empty();
            if (p.required && empty) errors.push_back(node_label(n) + ": " + p.label + " is required");
            if (!empty && p.filter && !has_extension(it->second, p.filter)) {
                std::string exts = ".";
                for (const char* c = p.filter; *c; ++c) exts += *c == ',' ? std::string(", .") : std::string(1, *c);
                errors.push_back(node_label(n) + ": " + p.label + " must end in " + exts);
            }
        }
        for (const auto& [key, _] : n.params)
            if (std::ranges::none_of(spec->params, [&](const ParamSpec& p) { return key == p.name; }))
                errors.push_back(node_label(n) + ": unknown parameter '" + key + "'");
        for (const auto& in : spec->inputs) {
            const auto count = std::ranges::count_if(
                links, [&](const Link& l) { return l.to_node == n.id && l.to_port == in.name; });
            if (count == 0) errors.push_back(node_label(n) + ": input '" + in.label + "' is not connected");
            if (count > 1) errors.push_back(node_label(n) + ": input '" + in.label + "' is connected more than once");
        }
    }
    for (const auto& l : links)
        if (auto err = check_link(*this, l); !err.empty()) errors.push_back(err);
    if (errors.empty() && topo_order(*this).size() != nodes.size()) errors.push_back("graph contains a loop");
    return errors;
}

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
    auto lower = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::vector<std::string> parts, root;
    for (const auto& p : file.lexically_normal()) parts.push_back(p.string());
    for (const auto& p : fs::path(natives_root).lexically_normal()) root.push_back(lower(p.string()));
    if (root.empty()) return "";
    for (size_t i = 0; i + root.size() < parts.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < root.size() && match; ++k) match = lower(parts[i + k]) == root[k];
        if (!match) continue;
        std::string rest;
        for (size_t k = i + root.size(); k < parts.size(); ++k) rest += (rest.empty() ? "" : "/") + parts[k];
        return rest;
    }
    return "";
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

    for (const Node* node : topo_order(g)) {
        const Node& n = *node;
        auto param = [&](const char* name) {
            const auto it = n.params.find(name);
            return it == n.params.end() ? std::string() : it->second;
        };
        auto input = [&](const char* port) -> const Value& {
            for (const auto& l : g.links)
                if (l.to_node == n.id && l.to_port == port) return outputs.at({l.from_node, l.from_port});
            throw GraphError("input not connected");  // unreachable after validate()
        };

        try {
            if (n.type == "LoadTex") {
                const fs::path tex = resolve(param("tex"));
                const TexMeta m = read_tex_meta(tex, opt.profile);
                std::string game_path = param("game_path");
                if (game_path.empty()) game_path = game_path_from(tex, opt.profile.natives_root);
                outputs[{n.id, "tex"}] = {tex, game_path};
                log(node_label(n) + ": " + tex.filename().string() + " " + std::to_string(m.width) + "x" +
                    std::to_string(m.height) + " " + m.format + ", " + std::to_string(m.mip_count) + " mips");
            } else if (n.type == "ExportImage") {
                const Value& tex = input("tex");
                const fs::path png = resolve(param("png"));
                if (!fs::exists(png)) {
                    opt.converter.load_tex(tex.path, png, opt.profile);
                    log(node_label(n) + ": exported " + png.string());
                    return {true, "Exported " + png.string() + ". Edit it, then run the graph again."};
                }
                outputs[{n.id, "image"}] = {png, tex.game_path};
                log(node_label(n) + ": using existing " + png.string());
            } else if (n.type == "ImportImage") {
                const fs::path png = resolve(param("png"));
                if (!fs::is_regular_file(png)) throw GraphError("PNG not found: " + png.string());
                outputs[{n.id, "image"}] = {png, ""};
                log(node_label(n) + ": " + png.string());
            } else if (n.type == "SaveTex") {
                const Value& original = input("original");
                const fs::path out = work.path / (std::to_string(n.id) + ".tex." + opt.profile.tex_suffix);
                const TexMeta m = opt.converter.save_tex(input("image").path, original.path, out, opt.profile);
                outputs[{n.id, "tex"}] = {out, original.game_path};
                log(node_label(n) + ": encoded " + m.format + ", " + std::to_string(m.mip_count) + " mips");
            } else if (n.type == "PackageMod") {
                const Value& tex = input("tex");
                if (tex.game_path.empty())
                    throw GraphError("game path unknown: fill in 'In-game path' on the Original texture node (the "
                                     ".tex isn't inside a " + opt.profile.natives_root + " folder)");
                PackageSpec spec{.mod_name = param("name"),
                                 .out_dir = resolve(param("out")),
                                 .info = {.name = param("name"),
                                          .version = param("version"),
                                          .description = param("description"),
                                          .author = param("author")},
                                 .files = {{tex.path, tex.game_path}},
                                 .screenshot = resolve(param("screenshot"))};
                const fs::path root = build_package(opt.profile, spec);
                log(node_label(n) + ": packaged " + root.string() + ".zip");
            }
        } catch (const std::exception& e) {
            throw GraphError(node_label(n) + ": " + e.what());
        }
    }
    return {false, "Done."};
}

}  // namespace remod
