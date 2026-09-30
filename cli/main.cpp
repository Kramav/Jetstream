// remod CLI. M1: package an already-prepared .tex (no conversion yet).
#include "package.hpp"
#include "profile.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>

// ponytail: narrow argv, so non-ASCII paths outside the ANSI codepage fail; switch to wmain if that bites.
namespace {

constexpr const char* kUsage =
    "usage: remod package --profile <toml> --tex <file> --game-path <natives-relative path>\n"
    "                     --name <ModName> --out <dir>\n"
    "                     [--version v] [--author a] [--description d] [--screenshot file]\n";

int run(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "package") {
        std::cerr << kUsage;
        return 2;
    }

    std::map<std::string, std::string> args;
    for (int i = 2; i < argc; i += 2) {
        std::string key = argv[i];
        if (!key.starts_with("--") || i + 1 >= argc) {
            std::cerr << "error: expected --option value, got '" << key << "'\n" << kUsage;
            return 2;
        }
        args[key.substr(2)] = argv[i + 1];
    }
    for (const char* req : {"profile", "tex", "game-path", "name", "out"}) {
        if (!args.contains(req)) {
            std::cerr << "error: missing --" << req << "\n" << kUsage;
            return 2;
        }
    }
    for (const auto& [key, _] : args) {
        static const char* known[] = {"profile", "tex",     "game-path",   "name",      "out",
                                      "version", "author",  "description", "screenshot"};
        if (std::find(std::begin(known), std::end(known), key) == std::end(known)) {
            std::cerr << "error: unknown option --" << key << "\n" << kUsage;
            return 2;
        }
    }

    const remod::Profile profile = remod::load_profile(args["profile"]);
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
