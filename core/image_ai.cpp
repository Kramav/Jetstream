#include "image_ai.hpp"

#include "process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace remod {

namespace fs = std::filesystem;

namespace {

ImageAi g_ai;

fs::path system32(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) throw ImageAiError("can't find Windows' System32 folder");
    return fs::path(dir) / name;
}

std::string tail(const std::string& s, size_t keep = 1500) {
    std::string t = s.size() > keep ? "..." + s.substr(s.size() - keep) : s;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    return t;
}

std::wstring number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return fs::path(buf).wstring();
}

}  // namespace

void set_image_ai(const ImageAi& ai) { g_ai = ai; }
const ImageAi& image_ai() { return g_ai; }

fs::path find_sd_cli(const fs::path& tool) {
    std::error_code ec;
    if (tool.empty())
        throw ImageAiError("the AI tool isn't set: in the Pipeline panel's AI section, Download it or pick sd-cli.exe");
    if (fs::is_regular_file(tool, ec)) return tool;
    if (fs::is_regular_file(tool / "sd-cli.exe", ec)) return tool / "sd-cli.exe";
    throw ImageAiError("no sd-cli.exe at " + tool.string() + ": Download it again, or pick sd-cli.exe");
}

std::pair<unsigned, unsigned> work_size(unsigned width, unsigned height, unsigned work_side) {
    const double k = double(std::max(64u, work_side)) / std::max(1u, std::max(width, height));
    auto to64 = [](double v) { return std::max(64u, unsigned(std::lround(v / 64)) * 64); };
    return {to64(width * k), to64(height * k)};
}

Bgra generate_image(const ImageAi& ai, const GenerateJob& job, const fs::path& temp_dir, std::chrono::seconds timeout,
                    std::string* log) {
    const fs::path exe = find_sd_cli(ai.tool);
    std::error_code ec;
    if (ai.model.empty()) throw ImageAiError("no AI model set: pick one in the Pipeline panel's AI section");
    if (!fs::is_regular_file(ai.model, ec)) throw ImageAiError("AI model not found: " + ai.model.string());
    if (!ai.vae.empty() && !fs::is_regular_file(ai.vae, ec)) throw ImageAiError("AI VAE not found: " + ai.vae.string());
    if (job.image.width == 0 || job.image.height == 0) throw ImageAiError("the image is empty");
    if (job.prompt.find_first_not_of(" \t\r\n") == std::string::npos) throw ImageAiError("the prompt is empty");

    // The model works at its own size (multiples of 64); the result is scaled back to the picture's.
    const auto [w, h] = work_size(job.image.width, job.image.height, job.work_side);
    fs::create_directories(temp_dir);
    const fs::path in = temp_dir / "init.png", mask = temp_dir / "mask.png", out = temp_dir / "out.png";
    std::vector<std::wstring> args{L"-m", ai.model.wstring(), L"-o", out.wstring(),
                                   L"-p", fs::path(job.prompt).wstring(), L"-W", std::to_wstring(w),
                                   L"-H", std::to_wstring(h), L"--seed", std::to_wstring(job.seed),
                                   L"--steps", std::to_wstring(job.steps), L"--cfg-scale", number(job.cfg),
                                   L"--vae-tiling"};
    if (!job.image.pixels.empty()) {
        Bgra init = resize_image(job.image, w, h, Fit::Stretch);
        for (size_t i = 3; i < init.pixels.size(); i += 4) init.pixels[i] = 255;  // colour only: alpha is often data
        save_png(in, init);
        args.insert(args.end(), {L"-i", in.wstring(), L"--strength", number(std::clamp(job.strength, 0.f, 1.f))});
    }
    if (job.tileable) args.push_back(L"--circular");
    if (!job.negative.empty()) args.insert(args.end(), {L"-n", fs::path(job.negative).wstring()});
    if (!ai.vae.empty()) args.insert(args.end(), {L"--vae", ai.vae.wstring()});
    if (job.mask && !job.image.pixels.empty()) {
        save_png(mask, resize_image(*job.mask, w, h, Fit::Stretch));
        args.insert(args.end(), {L"--mask", mask.wstring()});
    }
    fs::remove(out, ec);
    ProcessResult r;
    try {
        r = run_process(exe, args, timeout, true, {}, temp_dir);
    } catch (const ProcessError& e) {
        throw ImageAiError(std::string("sd-cli: ") + e.what());
    }
    if (log) *log = tail(r.output, 600);
    if (r.exit_code != 0 || !fs::is_regular_file(out, ec))
        throw ImageAiError("sd-cli failed (exit code " + std::to_string(r.exit_code) + ")" +
                           (r.output.empty() ? "" : ":\n" + tail(r.output)));
    Bgra result = resize_image(load_image(out), job.image.width, job.image.height, Fit::Stretch);
    for (size_t i = 3; i < result.pixels.size(); i += 4) result.pixels[i] = 255;
    return result;
}

std::string sd_cpp_download_url() {
    // master-964-1d0a58a's asset names carry the commit: sd-<tag minus its number>-bin-win-vulkan-x64.zip.
    const std::string tag = kSdCppRelease, commit = tag.substr(tag.rfind('-') + 1);
    return "https://github.com/leejet/stable-diffusion.cpp/releases/download/" + tag + "/sd-master-" + commit +
           "-bin-win-vulkan-x64.zip";
}

fs::path download_sd_cpp(const fs::path& dir, const std::function<void(const std::string&)>& progress) {
    auto say = [&](const std::string& s) {
        if (progress) progress(s);
    };
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw ImageAiError("can't make " + dir.string() + ": " + ec.message());
    const fs::path zip = dir / "sd-cpp-vulkan.zip";
    say("downloading stable-diffusion.cpp " + std::string(kSdCppRelease) + " (about 30 MB)...");
    try {
        const ProcessResult get = run_process(system32(L"curl.exe"),
                                              {L"-sSfL", L"--retry", L"2", L"-o", zip.wstring(),
                                               fs::path(sd_cpp_download_url()).wstring()},
                                              std::chrono::minutes(15));
        if (get.exit_code != 0) throw ImageAiError("the download failed: " + tail(get.output, 400));
        say("unpacking...");
        const ProcessResult unpack =
            run_process(system32(L"tar.exe"), {L"-xf", zip.wstring(), L"-C", dir.wstring()}, std::chrono::minutes(2));
        if (unpack.exit_code != 0) throw ImageAiError("unpacking failed: " + tail(unpack.output, 400));
    } catch (const ProcessError& e) {
        throw ImageAiError(e.what());
    }
    fs::remove(zip, ec);
    const fs::path exe = dir / "sd-cli.exe";
    if (!fs::is_regular_file(exe, ec)) throw ImageAiError("the download has no sd-cli.exe");
    say("stable-diffusion.cpp is in " + dir.string());
    return exe;
}

}  // namespace remod
