// remod CLI. M1: the pipeline steps as separate commands (the graph runner comes later).
#include "package.hpp"
#include "profile.hpp"
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
    "  remod tex2png --profile <toml> --noesis <Noesis64.exe> --tex <file.tex.N> --out <file.png>\n"
    "  remod png2tex --profile <toml> --noesis <Noesis64.exe> --png <edited.png> --original <file.tex.N>\n"
    "                --out <new.tex.N>\n"
    "  remod package --profile <toml> --tex <file> --game-path <natives-relative path> --name <ModName>\n"
    "                --out <dir> [--version v] [--author a] [--description d] [--screenshot file]\n";

struct Command {
    std::vector<std::string> required;
    std::vector<std::string> optional;
};

const std::map<std::string, Command> kCommands{
    {"tex2png", {{"profile", "noesis", "tex", "out"}, {}}},
    {"png2tex", {{"profile", "noesis", "png", "original", "out"}, {}}},
    {"package", {{"profile", "tex", "game-path", "name", "out"}, {"version", "author", "description", "screenshot"}}},
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

    const remod::Profile profile = remod::load_profile(args["profile"]);

    if (cmd->first == "tex2png") {
        remod::NoesisConverter noesis(args["noesis"]);
        print_meta(noesis.load_tex(args["tex"], args["out"], profile));
        std::cout << "wrote: " << args["out"] << "\n";
        return 0;
    }
    if (cmd->first == "png2tex") {
        remod::NoesisConverter noesis(args["noesis"]);
        print_meta(noesis.save_tex(args["png"], args["original"], args["out"], profile));
        std::cout << "wrote: " << args["out"] << "\n";
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
