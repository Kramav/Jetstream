#pragma once
// Movies (CLAUDE.md §10 M3): the game's pre-rendered movies are MP4 files, named <id>.mov.1.x64 under
// natives/STM/streaming/_chainsaw/movie/. This reads what a replacement must match.
#include <cstdint>
#include <filesystem>
#include <string>

namespace remod {

struct MovieInfo {
    unsigned width = 0, height = 0;  // the video track's
    double seconds = 0;              // the video track's length
    double fps = 0;                  // its frames over its length
    std::string video;               // its codec, e.g. "avc1" (H.264); "" if there's no video track
    std::string audio;               // e.g. "mp4a" (AAC); "" if there's no audio track
    std::uint64_t bitrate = 0;       // the whole file's, bits per second (its size over its length)
};

// An MP4's video and audio tracks, from its boxes (ISO base media file format; the index box may come after the
// data). Only box headers and the small boxes describing the tracks are read. Throws std::runtime_error if the file
// isn't an MP4.
MovieInfo read_mp4_info(const std::filesystem::path& file);

// Writes `out`: an MP4 with one H.264 (High profile) video track at `like`'s size and frame rate, at about its bit
// rate. The picture is `video`'s (any file Windows can play), scaled to fit (black bars if its shape differs), frames
// repeated or dropped to the frame rate, as long as `video` (its sound is left out), or with `same_length` as long as
// `like` (cut, or its last frame held). With no `video`: a test card reading `title` and the seconds, as long as
// `like`. Windows' Media Foundation encodes it (a hardware encoder if there is one). Throws std::runtime_error.
void encode_movie(const std::filesystem::path& video, const MovieInfo& like, const std::string& title,
                  const std::filesystem::path& out, bool same_length = false);

}  // namespace remod
