#pragma once
// AI image generation (CLAUDE.md §10 M4): stable-diffusion.cpp's sd-cli run as a separate process, on the user's GPU
// (its Vulkan build runs on AMD and NVIDIA). remod never bundles it or a model: the user points to them, or lets remod
// download the tested sd.cpp build (the model is always theirs).
#include "image.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace remod {

struct ImageAiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Where the tool and model are (the app's settings; global like set_game_files_dir).
struct ImageAi {
    std::filesystem::path tool;   // sd-cli.exe, or the folder holding it
    std::filesystem::path model;  // a .safetensors / .gguf checkpoint (SDXL base 1.0 tested)
    std::filesystem::path vae;    // optional: a separate VAE (SDXL needs sdxl-vae-fp16-fix: its own gives NaN in fp16)
};
void set_image_ai(const ImageAi& ai);
const ImageAi& image_ai();

// sd-cli.exe given the tool setting (the exe, or its folder). Throws ImageAiError saying what to set if missing.
std::filesystem::path find_sd_cli(const std::filesystem::path& tool);

struct GenerateJob {
    Bgra image;                 // the picture to change (img2img: never text alone for a texture, CLAUDE.md §10 M4);
                                // no pixels: a new picture of its size from the prompt (a material swatch)
    bool tileable = false;      // its edges continue on the opposite side (sd-cli --circular), for repeating
    std::optional<Bgra> mask;   // white = may change (inpainting); any size, stretched to the image's
    std::string prompt, negative;
    float strength = 0.6f;      // 0..1: how far from the image it may go
    long long seed = 42;
    int steps = 25;
    float cfg = 6;
    unsigned work_side = 1024;  // the longer side the model works at (SDXL: 1024); the result is scaled back
};

// The size the model works at for a `width` x `height` picture: the longer side `work_side`, both multiples of 64.
std::pair<unsigned, unsigned> work_size(unsigned width, unsigned height, unsigned work_side);

// Runs sd-cli once in `temp_dir` (made; its files left for the caller to delete). The result is the image's size, its
// colour only (alpha 255): the caller puts it back through the mask and keeps the original's alpha. `log` gets
// sd-cli's last lines. Throws ImageAiError (no tool / model, the tool failed or timed out, no output).
Bgra generate_image(const ImageAi& ai, const GenerateJob& job, const std::filesystem::path& temp_dir,
                    std::chrono::seconds timeout, std::string* log = nullptr);

// The sd.cpp build remod was tested with (CLAUDE.md §10 M4 spike): its Windows Vulkan zip.
inline constexpr const char* kSdCppRelease = "master-964-1d0a58a";
std::string sd_cpp_download_url();

// Downloads that zip into `dir` with Windows' own curl.exe and unpacks it with tar.exe (both in System32 on Windows 10
// 1803+); returns sd-cli.exe's path. `progress` gets a line per stage. Throws ImageAiError.
std::filesystem::path download_sd_cpp(const std::filesystem::path& dir,
                                      const std::function<void(const std::string&)>& progress = {});

}  // namespace remod
