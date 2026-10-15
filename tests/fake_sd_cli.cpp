// Stands in for stable-diffusion.cpp's sd-cli.exe in tests: reads -i, writes -o with the colours inverted, at the
// -W x -H it was asked for. Without -i (text to image) a flat picture: orange with --circular, else grey. A prompt
// holding "fail" exits 3. Also writes args.txt (its arguments) beside the output.
#include "image.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <string>

int run(int argc, wchar_t** argv) {
    std::map<std::wstring, std::wstring> a;
    for (int i = 1; i < argc; ++i) {  // a flag alone (--vae-tiling) or with its value
        const std::wstring key = argv[i];
        const bool valued = i + 1 < argc && argv[i + 1][0] != L'-';
        a[key] = valued ? argv[++i] : L"1";
    }
    const std::filesystem::path out = a[L"-o"];
    {
        std::ofstream args(out.parent_path() / "args.txt");
        for (int i = 1; i < argc; ++i) args << std::filesystem::path(argv[i]).string() << "\n";
    }
    if (a[L"-p"].find(L"fail") != std::wstring::npos) return 3;
    if (!a.contains(L"-i")) {
        const unsigned w = unsigned(std::stoi(a[L"-W"])), h = unsigned(std::stoi(a[L"-H"]));
        remod::Bgra made{w, h, std::vector<std::uint8_t>(size_t(w) * h * 4, 255)};
        const bool circular = a.contains(L"--circular");
        for (size_t i = 0; i < made.pixels.size(); i += 4)
            made.pixels[i] = circular ? 0 : 128, made.pixels[i + 1] = 128, made.pixels[i + 2] = circular ? 255 : 128;
        remod::save_png(out, made);
        return 0;
    }
    remod::Bgra img = remod::load_image(a[L"-i"]);
    if (img.width != unsigned(std::stoi(a[L"-W"])) || img.height != unsigned(std::stoi(a[L"-H"]))) return 4;
    for (size_t i = 0; i < img.pixels.size(); i += 4)
        for (int c = 0; c < 3; ++c) img.pixels[i + c] = std::uint8_t(255 - img.pixels[i + c]);
    remod::save_png(out, img);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {  // said, never an abort() dialog
        std::cerr << "fake_sd_cli: " << e.what() << '\n';
        return 5;
    } catch (...) {
        std::cerr << "fake_sd_cli: failed\n";
        return 5;
    }
}
