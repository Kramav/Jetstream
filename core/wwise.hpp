#pragma once
// Wwise audio without Wwise (CLAUDE.md §10, "Story movies' sound"): the game's sound packages (.spck, Audiokinetic's
// AKPK file package) and the .wem sounds inside them, read and written by our own code. Layouts were worked out from
// reference files and the game's own (spikes/wwise/); Vorbis sounds are encoded by libvorbis, Opus by libopus.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace remod {

// A sound package: a language map, then three tables (banks, streamed sounds, externals) of files by id, then the
// files' bytes in table order. The game's copy outside streaming/ holds the header only; the streaming copy, the data.
struct AkpkFile {
    std::uint64_t id = 0;          // the WEM's id (as the sound bank names it)
    std::uint32_t block = 1;       // alignment of its offset (1 in every RE4R package)
    std::uint32_t language = 0;    // an id from the language map
    std::string data;              // empty when read from a header-only package
};
struct Akpk {
    std::uint32_t version = 1;
    std::string languages;  // the language map, as stored
    std::vector<AkpkFile> banks, streams, externals;
};
Akpk read_akpk(std::string_view bytes);  // throws std::runtime_error
// The package's bytes: its header, then (with_data) every file's data in table order, each at its block alignment.
// Every file needs its data either way: its size and offset go in the header.
std::string write_akpk(const Akpk& pack, bool with_data);

// A sound bank (.sbnk, Wwise's .bnk): chunks in file order. Its media index and data (DIDX, DATA) are kept as files
// by id, rebuilt on writing (16-byte aligned, zero padding, as Wwise writes them); every other chunk as it is.
struct BankMedia {
    std::uint32_t id = 0;
    std::string data;  // a whole WEM, or a streamed one's first part (its prefetch)
};
struct Bank {
    std::vector<std::pair<std::string, std::string>> chunks;  // tag and body; DIDX's and DATA's come from `media`
    std::vector<BankMedia> media;
};
Bank read_bank(std::string_view bytes);  // throws std::runtime_error
std::string write_bank(const Bank& bank);

// Where a bank's event data (HIRC: Sounds and music tracks) records a media file's in-memory size. Stream 0: the
// media is in a bank (size = its size); 1: streamed, its prefetch in a bank; 2: streamed only. For 1 and 2 the size
// is the WEM's header and first packets (Wwise's prefetch amount, wem_prefix).
struct BankSource {
    std::uint32_t media = 0;
    std::uint8_t stream = 0;
    std::uint32_t memory = 0;
    size_t at = 0;       // the size's offset in the HIRC chunk's body
    bool music = false;  // a music track's (it also records the sound's length)
};
std::vector<BankSource> bank_sources(const Bank& bank);
void set_source_memory(Bank& bank, const BankSource& source, std::uint32_t memory);

// A WEM's header and first `packets` packets: how many bytes, and how many packets end within its first `bytes`.
size_t wem_prefix(std::string_view wem, size_t packets);
size_t wem_packets_within(std::string_view wem, size_t bytes);

constexpr std::uint16_t kWemVorbis = 0xFFFF, kWemOpus = 0x3041;
struct WemInfo {
    std::uint16_t codec = 0;  // kWemVorbis, kWemOpus, ...
    unsigned channels = 0, rate = 0;
    std::uint32_t samples = 0;  // per channel
};
WemInfo read_wem_info(std::string_view wem);  // throws std::runtime_error

// A new WEM in `like`'s form (codec, channels, rate; a Vorbis one's setup, so the game's sound bank still fits)
// holding `pcm`: 16-bit samples, like's channels interleaved in a WAV's order, pcm.size() / channels long.
// Vorbis needs `codebooks` (find_codebooks) to check that libvorbis makes like's setup. Throws std::runtime_error.
std::string encode_wem(std::string_view like, const std::vector<std::int16_t>& pcm,
                       const std::filesystem::path& codebooks);
// A WEM's sound as 16-bit samples, channels interleaved in a WAV's order (Vorbis and Opus). Throws.
std::vector<std::int16_t> decode_wem(std::string_view wem, const std::filesystem::path& codebooks);

// Wwise's Vorbis codebook library (third_party/ww2ogg/packed_codebooks_aoTuV_603.bin): data\ beside the exe in the
// release zip, else the source tree's. Empty if neither is found.
std::filesystem::path find_codebooks();

}  // namespace remod
