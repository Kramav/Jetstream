// Stands in for stable-diffusion.cpp's sd-cli.exe in tests: reads -i, writes -o with the colours inverted, at the
// -W x -H it was asked for. A prompt holding "fail" exits 3. Also writes args.txt (its arguments) beside the output.
#include "image.hpp"

#include <fstream>
#include <map>
#include <string>

int wmain(int argc, wchar_t** argv) {
    std::map<std::wstring, std::wstring> a;
    for (int i = 1; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    const std::filesystem::path out = a[L"-o"];
    {
        std::ofstream args(out.parent_path() / "args.txt");
        for (int i = 1; i < argc; ++i) args << std::filesystem::path(argv[i]).string() << "\n";
    }
    if (a[L"-p"].find(L"fail") != std::wstring::npos) return 3;
    remod::Bgra img = remod::load_image(a[L"-i"]);
    if (img.width != unsigned(std::stoi(a[L"-W"])) || img.height != unsigned(std::stoi(a[L"-H"]))) return 4;
    for (size_t i = 0; i < img.pixels.size(); i += 4)
        for (int c = 0; c < 3; ++c) img.pixels[i + c] = std::uint8_t(255 - img.pixels[i + c]);
    remod::save_png(out, img);
    return 0;
}
