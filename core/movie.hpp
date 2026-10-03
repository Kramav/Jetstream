#pragma once
// Movies (CLAUDE.md §10 M3): the game's pre-rendered movies are MP4 files, named <id>.mov.1.x64 under
// natives/STM/streaming/_chainsaw/movie/. This reads what a replacement must match.
#include <filesystem>
#include <string>

namespace remod {

struct MovieInfo {
    unsigned width = 0, height = 0;  // the video track's
    double seconds = 0;              // the video track's length
    double fps = 0;                  // its frames over its length
    std::string video;               // its codec, e.g. "avc1" (H.264); "" if there's no video track
    std::string audio;               // e.g. "mp4a" (AAC); "" if there's no audio track
};

// An MP4's video and audio tracks, from its boxes (ISO base media file format; the index box may come after the
// data). Only box headers and the small boxes describing the tracks are read. Throws std::runtime_error if the file
// isn't an MP4.
MovieInfo read_mp4_info(const std::filesystem::path& file);

}  // namespace remod
