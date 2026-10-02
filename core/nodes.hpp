#pragma once
// The node types users place in a graph (CLAUDE.md §4): what each one takes and gives (its NodeSpec) and what it does
// in a run (NodeSpec::run). Adding a node type = adding its spec and run function in nodes.cpp; the graph engine
// (graph.hpp) and the front ends work from the specs and need no change.
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace remod {

struct Graph;
class NodeRun;  // node_run.hpp: one node's view of a run (core only)

// What flows along a link, by kind (front ends colour links and pins by it): a texture file, an image file, plain
// text, a path to any file, a folder. Any: a pass-through (Split) carries whatever is linked into it
// (Graph::output_type works out what). Kinds planned for later milestones, with their colours reserved: script, AI
// call (CLAUDE.md §4).
enum class PortType { Tex, Image, Text, Path, Folder, Any };

// Which picker a front end should offer for a path field. OpenTexture: an RE Engine texture of any known game
// (front ends build the filter from the profiles' tex suffixes).
enum class PathKind { None, OpenFile, OpenTexture, SaveFile, Folder };

// How an input can be typed in. None = link-only. Choice: one of InputSpec::options. Number: from InputSpec::min to
// max, stored as text (front ends offer a drag field).
enum class Widget { None, Text, Path, Checkbox, Choice, Number };

// `name`s are the stable ids stored in graph files; `title`, `label` and `hint` are what users read.
// An output may carry a typed field for where the node writes it (e.g. Export's "Image file"): front ends show it
// on the output side, and it's stored in Node::params under `field`. Such a field is always required.
struct PortSpec {  // an output
    const char* name;
    PortType type;
    const char* label;
    const char* field = nullptr;
    const char* field_label = "";
    const char* hint = "";
    PathKind path = PathKind::None;
    const char* filter = nullptr;
    bool multiple = false;  // any number of links, one row each (a Split); every other output feeds one input
};

// An input. Every input has a pin. Editable inputs (widget != None) can instead be typed; a link wins over the
// typed value. `multiple` inputs are link-only and take any number of links, in link order.
struct InputSpec {
    const char* name;
    const char* label;
    PortType type;
    Widget widget = Widget::None;
    bool required = false;  // editable: typed or linked; link-only: at least one link
    bool multiple = false;
    const char* hint = "";
    PathKind path = PathKind::None;
    const char* filter = nullptr;  // extensions for typed file paths, e.g. "png,jpg"; nullptr = any
    const char* initial = "";      // a new node's value, e.g. "true" for a checkbox ticked by default
    std::vector<std::array<const char*, 2>> options = {};  // Choice: {stored value, what users read}
    // A destination (where the node writes: Copy's destination, Package's output folder) names the output it shares
    // a row with, on the output side. The row has one circle: on the right by default (the field is typed, the
    // circle passes the result on), on the left once flipped (Graph::flip: a link sets the field, e.g. from a Value,
    // and the result isn't passed on).
    const char* result = nullptr;
    // Number: its range, how front ends show it (printf, e.g. "%.0f%%"), and what they show for 0 (e.g. "auto";
    // nullptr: the number).
    float min = 0, max = 0;
    const char* format = "%.0f";
    const char* zero = nullptr;
    // A tuning field most uses leave alone (Replace photo's ageing, feather...): front ends show it only while it's in
    // use (changed from its initial value, or linked), folding the rest into one row that opens them (user,
    // 2026-10-02: big blocks crowd the graph).
    bool advanced = false;
    bool editable() const { return widget != Widget::None; }
};

// Whether a typed value is the input's initial one: a Number by value ("100" = "100.0"), a checkbox "" = "false".
bool at_initial(const InputSpec& in, const std::string& value);

// What kind of step a block is; front ends give each family its own outline (docs/design_handoff_node_graph):
// Source brings a file in, Transform converts, Manual is the user's own step, Flow passes a value on (Split), File
// changes files, Output builds the mod, Value computes or holds a value.
enum class Family { Source, Transform, Manual, Flow, File, Output, Value };

struct NodeSpec {
    const char* type;
    const char* title;
    const char* summary;
    std::vector<InputSpec> inputs;
    std::vector<PortSpec> outputs;
    std::vector<const char*> state = {};  // other params the node keeps (e.g. EditImage's "done"); no field shown
    bool manual = false;                  // a step the user does by hand (front ends mark it clearly)
    bool utility = false;  // a simple helper (Split, Text): front ends draw it small and list it after the main steps
    Family family = Family::Transform;
    // Values before a run (preview_values, graph.hpp): a pure type's run only computes (no files read or written), so
    // a preview runs it; a step gives its predictable outputs through `preview` (e.g. the path it will write), without
    // doing anything. Neither: its outputs are known only in a run.
    bool pure = false;
    bool thumbnail = false;  // an image block: front ends show a live thumbnail of its result (preview_image)
    const char* view_size = nullptr;  // its picture is the point (Preview): shown as large as this input says (px)
    void (*run)(NodeRun&) = nullptr;      // what it does in a run; throws to fail the run
    void (*preview)(NodeRun&) = nullptr;  // a step's outputs as far as known before running it; throws if unknown
};

// LoadTex, ExportImage, EditImage, ImportImage, SaveTex, PackageMod, CopyFile; utilities Value (a value kept in the
// graph, of whatever kind it feeds), Text, and Split (one value to several inputs: an output feeds one input, so
// using it in several places takes a Split).
const std::vector<NodeSpec>& node_specs();
const NodeSpec* find_spec(std::string_view type);
const InputSpec* find_input(const NodeSpec& spec, std::string_view name);

// The picker a typed value of `type` gets (a Value's field takes the kind it feeds); None for text.
PathKind picker_for(PortType type);

// Why `path` can't go into a field with this picker and filter (e.g. dropped there from the browser), or "" if it can.
// Folder fields take folders; file fields (OpenFile, OpenTexture, SaveFile) take files, with an extension in `filter`
// if one is given (e.g. "png,tga,jpg"); a text field (None) takes no path.
std::string path_fit(PathKind kind, const char* filter, const std::string& path, bool is_folder);

// Can an output of type `out` feed `in`? Same type; anything into a Text or Path input (as its text/path: a texture
// is a file too); Text into an editable input (a typed value), and a Path into a texture or image file field; anything
// into a pass-through, and a pass-through with nothing linked in yet into anything (checked once it has a type).
bool accepts(const InputSpec& in, PortType out);

// ---- Helpers belonging to particular node types ----

// Editor warnings that need no run: "Destination exists: <path>" on each file step (Copy file) whose typed
// destination is already there. Only checks for existence, but that's the file system: front ends call it when inputs
// change and every second or two, not every frame. A destination or source fed by a link is unknown until a run, so
// no warning then, unless the typed destination is a file path. The run checks again and decides.
std::map<int, std::string> destination_warnings(const Graph& graph, const std::filesystem::path& base_dir);

// "<natives root>/<rest>" in `file` (case-insensitive) -> "<rest>", else "". Lets LoadTex infer the game path
// when the .tex sits inside an extracted natives tree.
std::string game_path_from(const std::filesystem::path& file, const std::string& natives_root);

// Text: fills "{1}", "{2}", ... in `text` with `parts` (1-based). Throws GraphError if text uses a part that isn't
// given.
std::string fill_template(const std::string& text, const std::vector<std::string>& parts);

// Cut text: `text` cut at `marker`'s first (or last) occurrence, ignoring case and treating \ and / alike (it's mostly
// for paths); the kept part keeps its own characters. After / Before leave the marker out, From / UpTo keep it.
// nullopt if the marker isn't in the text (or is empty).
enum class CutKeep { After, Before, From, UpTo };
std::optional<std::string> cut_text(const std::string& text, const std::string& marker, CutKeep keep, bool last);

}  // namespace remod
