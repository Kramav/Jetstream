#pragma once
// Movies (CLAUDE.md §10 M3): the game's pre-rendered movies are MP4 files, named <id>.mov.1.x64 under
// natives/STM/streaming/_chainsaw/movie/. This reads what a replacement must match.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace remod {

struct MovieInfo {
    unsigned width = 0, height = 0;  // the video track's
    double seconds = 0;              // the video track's length
    double fps = 0;                  // its frames over its length
    std::string video;               // its codec, e.g. "avc1" (H.264); "" if there's no video track
    std::string audio;               // e.g. "mp4a" (AAC); "" if there's no audio track
    std::uint64_t bitrate = 0;       // the whole file's, bits per second (its size over its length)
    unsigned audio_rate = 0, audio_channels = 0;  // the audio track's sample rate (Hz) and channels; 0 if none
};

// An MP4's video and audio tracks, from its boxes (ISO base media file format; the index box may come after the
// data). Only box headers and the small boxes describing the tracks are read. Throws std::runtime_error if the file
// isn't an MP4.
MovieInfo read_mp4_info(const std::filesystem::path& file);

// Writes `out`: an MP4 with one H.264 (High profile) video track at `like`'s size and frame rate, at about its bit
// rate. The picture is `video`'s (any file Windows can play), scaled to fit (black bars if its shape differs), frames
// repeated or dropped to the frame rate, as long as `video`, or with `same_length` as long as `like` (cut, or its last
// frame held). With no `video`: a test card reading `title` and the seconds, as long as `like`.
// If `like` has sound in the file, so does `out`: AAC at its sample rate and channels (48 or 44.1 kHz; 1, 2 or 6),
// as long as the picture: `video`'s sound (cut, or silence after it), silence if it has none, a test card's a beep
// each second. Without, `out` has no sound track either (the game's story movies: their sound is in its sound bank).
// Windows' Media Foundation encodes it (a hardware encoder if there is one). Returns what the sound is ("" for no
// track). Throws std::runtime_error.
std::string encode_movie(const std::filesystem::path& video, const MovieInfo& like, const std::string& title,
                  const std::filesystem::path& out, bool same_length = false);

// `video`'s sound (its first sound track) as 16-bit samples at `rate`, `channels` interleaved in a WAV's order,
// exactly `frames` long: cut, or silence after it; 0: its own length (none: an error). Windows converts the rate and mixes to mono or stereo; with more
// channels the sound is in the first two (front left and right), the rest silent. A video without sound: silence.
// No `video`: a test card's beep (1 kHz, the first tenth of each second) in the first two. *what says which, as
// encode_movie's. Throws std::runtime_error.
std::vector<std::int16_t> read_sound(const std::filesystem::path& video, unsigned rate, unsigned channels,
                                     std::uint64_t frames, std::string* what = nullptr);

// ---- New movies (docs/re4r_movies.md): our own movie under a new id, made like the game's mva000 ----

// What's wrong with a new movie's name, or "" if nothing: 6 lowercase letters, digits or _ (it replaces "mva000" in
// the game's prefabs byte for byte, so it must be as long), not starting "mv" (the game's own names).
std::string new_movie_name_problem(const std::string& name);

// A prefab's bytes with each UTF-16 "mv/<from>/<from>" (its paths to its movie) made "mv/<to>/<to>"; the names must be
// the same length. Returns how many it changed.
size_t rename_movie_paths(std::string& prefab, const std::string& from, const std::string& to);

// Plays a video for a preview (any file Windows plays; game movies named .mov.1.x64 too): frames are decoded on a
// thread of its own (Windows' Media Foundation), scaled to fit `max_side` (keeping the shape), as 32-bit BGRA. It
// opens paused on the first frame. Front ends call take() each frame and draw what it gives.
class MoviePlayer {
public:
    struct Frame {
        unsigned width = 0, height = 0;
        std::vector<std::uint8_t> bgra;  // rows top to bottom, width * 4 bytes each, alpha 255
        double time = 0;                 // seconds
    };

    explicit MoviePlayer(const std::filesystem::path& video, unsigned max_side = 1280);
    ~MoviePlayer();
    MoviePlayer(const MoviePlayer&) = delete;
    MoviePlayer& operator=(const MoviePlayer&) = delete;

    bool take(Frame& frame);  // the newest frame, if one came since the last call
    void play();              // from the start again once it has ended
    void pause();
    void seek(double seconds);  // shows the frame there; keeps playing or paused
    bool playing() const;
    double position() const;    // the shown frame's time, seconds
    MovieInfo info() const;     // size, length and frame rate, once opened (zeros before)
    std::string error() const;  // why it can't play; "" while fine

    struct State;  // shared with the decoding thread (movie.cpp)

private:
    std::unique_ptr<State> state_;
    std::thread thread_;
};

}  // namespace remod
