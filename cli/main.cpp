// remod CLI: runs a saved graph headlessly (`run`), plus the pipeline steps as single commands.
#include "api.hpp"
#include "graph.hpp"
#include "package.hpp"
#include "profile.hpp"
#include "settings.hpp"
#include "texture_converter.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// ponytail: narrow argv, so non-ASCII paths outside the ANSI codepage fail; switch to wmain if that bites.
namespace {

constexpr const char* kUsage =
    "usage:\n"
    "  remod run --graph <file.json> [--profiles <dir>] [--edited true] [--noesis <Noesis64.exe>]\n"
    "            --edited true: treat Edit image steps as done (you've edited the images)\n"
    "  remod tex2png --profile <toml> --tex <file.tex.N> --out <file.png|.tga|.jpg> [--noesis <Noesis64.exe>]\n"
    "  remod png2tex --profile <toml> --png <edited.png|.tga|.jpg> --original <file.tex.N> --out <new.tex.N>\n"
    "                [--noesis <Noesis64.exe>]\n"
    "  --noesis: convert textures with Noesis (optional) instead of the built-in converter\n"
    "  remod package --profile <toml> --tex <file> --game-path <natives-relative path> --name <ModName>\n"
    "                --out <dir> [--version v] [--author a] [--description d] [--screenshot file]\n"
    "                [--replace true]\n"
    "  remod api     graph editing for programs: one JSON request per line on stdin, one JSON reply per line\n"
    "                (requests: core/api.hpp)\n";

struct Command {
    std::vector<std::string> required;
    std::vector<std::string> optional;
};

const std::map<std::string, Command> kCommands{
    {"run", {{"graph"}, {"profiles", "edited", "noesis"}}},
    {"api", {{}, {}}},
    {"tex2png", {{"profile", "tex", "out"}, {"noesis"}}},
    {"png2tex", {{"profile", "png", "original", "out"}, {"noesis"}}},
    {"package", {{"profile", "tex", "game-path", "name", "out"}, {"version", "author", "description", "screenshot", "replace"}}},
};

void print_meta(const remod::TexMeta& m) {
    std::cout << m.width << "x" << m.height << " " << m.format << ", " << m.mip_count << " mips, " << m.array_count
              << " image(s)\n";
}

int run(int argc, char** argv) {
    const auto cmd = argc >= 2 ? kCommands.find(argv[1]) : kCommands.end();
    if (cmd == kCommands.end()) {
        std::cerr << kUsage;
        return 2;
    }

    std::map<std::string, std::string> args;
    for (int i = 2; i < argc; i += 2) {
        const std::string key = argv[i];
        if (!key.starts_with("--") || i + 1 >= argc) {
            std::cerr << "error: expected --option value, got '" << key << "'\n" << kUsage;
            return 2;
        }
        args[key.substr(2)] = argv[i + 1];
    }
    const auto& [required, optional] = cmd->second;
    for (const auto& req : required) {
        if (!args.contains(req)) {
            std::cerr << "error: missing --" << req << "\n" << kUsage;
            return 2;
        }
    }
    for (const auto& [key, _] : args) {
        if (std::ranges::count(required, key) == 0 && std::ranges::count(optional, key) == 0) {
            std::cerr << "error: unknown option --" << key << " for " << cmd->first << "\n" << kUsage;
            return 2;
        }
    }

    if (cmd->first == "api") {
        remod::ApiSession session;
        for (std::string line; std::getline(std::cin, line);)
            if (!line.empty()) std::cout << session.call(line) << std::endl;  // flushed: the caller waits for each reply
        return 0;
    }

    if (cmd->first == "run") {
        const std::filesystem::path graph_file = args["graph"];
        const remod::Graph graph = remod::load_graph(graph_file);
        const std::filesystem::path profiles = args.contains("profiles") ? args["profiles"] : remod::find_profiles_dir();
        if (profiles.empty()) throw std::runtime_error("no profiles folder found; pass --profiles <dir>");
        const remod::Profile profile = remod::load_profile_by_id(profiles, graph.profile);
        const auto converter = remod::make_converter(args["noesis"]);
        const auto result = remod::run_graph(
            graph, {.profile = profile,
                    .converter = *converter,
                    .base_dir = std::filesystem::absolute(graph_file).parent_path(),
                    .log = [](const std::string& line) { std::cout << line << "\n"; },
                    .edits_done = args["edited"] == "true",
                    .cache_dir = remod::default_cache_dir()});
        for (const auto& w : result.warnings) std::cout << "WARNING: " << w << "\n";
        std::cout << result.message << "\n";
        return 0;
    }

    const remod::Profile profile = remod::load_profile(args["profile"]);

    if (cmd->first == "tex2png") {
        print_meta(remod::make_converter(args["noesis"])->load_tex(args["tex"], args["out"], profile));
        std::cout << "wrote: " << args["out"] << "\n";
        return 0;
    }
    if (cmd->first == "png2tex") {
        const remod::TexMeta made = remod::make_converter(args["noesis"])->save_tex(args["png"], args["original"], args["out"], profile);
        print_meta(made);
        std::cout << "wrote: " << args["out"] << "\n";
        if (const auto mips = remod::read_tex_meta(args["original"], profile).mip_count; made.mip_count != mips)
            std::cout << "WARNING: the original has " << mips << " mip level(s), the new texture " << made.mip_count
                      << ". It may work in game; if the texture looks wrong, this is the likely cause.\n";
        return 0;
    }

    remod::PackageSpec spec;
    spec.mod_name = args["name"];
    spec.out_dir = args["out"];
    spec.info = {.name = args["name"],
                 .version = args["version"],
                 .description = args["description"],
                 .author = args["author"]};
    spec.files.push_back({args["tex"], args["game-path"]});
    spec.screenshot = args["screenshot"];
    spec.replace = args["replace"] == "true";

    const auto root = remod::build_package(profile, spec);
    std::cout << "packaged: " << root.string() << ".zip\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
