#include "sound.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace remod {

namespace {

namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error(why); }

std::uint32_t u32(const std::string& b, size_t at) {
    if (at > b.size() || b.size() - at < 4) fail("a sound file ends too soon");
    std::uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail("can't read " + file.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
bool is_bank(const std::string& name) { return lower(name).find(".sbnk.") != std::string::npos; }
bool is_package(const std::string& name) { return lower(name).find(".spck.") != std::string::npos; }

// A file's language: what follows ".x64." ("ch_x.sbnk.1.x64.en" -> "en"), "" for none.
std::string language_of(const std::string& name) {
    const std::string low = lower(name);
    const size_t at = low.rfind(".x64.");
    return at == std::string::npos ? "" : low.substr(at + 5);
}

// A package's header (its table), without its files' bytes.
Akpk read_package_header(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string head(8, '\0');
    if (!in.read(head.data(), 8)) fail("can't read " + file.string());
    head.resize(8 + size_t(u32(head, 4)));
    if (!in.read(head.data() + 8, std::streamsize(head.size() - 8))) fail(file.string() + " ends inside its header");
    return read_akpk(head);
}

// A bank's media ids and event data's sources, read past its media (DATA) without loading it.
struct BankHead {
    std::vector<std::uint32_t> media;
    std::vector<BankSource> sources;
};
BankHead read_bank_head(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail("can't read " + file.string());
    Bank events;
    BankHead head;
    for (std::uint64_t at = 0;;) {
        std::string h(8, '\0');
        in.seekg(std::streamoff(at));
        if (!in.read(h.data(), 8)) break;
        const std::uint32_t size = u32(h, 4);
        const std::string tag = h.substr(0, 4);
        if (tag == "DIDX" || tag == "HIRC") {
            std::string body(size, '\0');
            if (!in.read(body.data(), size)) fail(file.string() + " ends inside a chunk");
            if (tag == "DIDX")
                for (size_t i = 0; i + 12 <= body.size(); i += 12) head.media.push_back(u32(body, i));
            else
                events.chunks.emplace_back(tag, std::move(body));
        }
        at += 8 + std::uint64_t(size);
    }
    head.sources = bank_sources(events);
    return head;
}

// The copy of `file` under the natives root's streaming/ folder (a package outside it holds only its header).
fs::path streaming_copy(const fs::path& file) {
    std::error_code ec;
    for (fs::path dir = file.parent_path(); dir.has_relative_path(); dir = dir.parent_path()) {
        if (const fs::path copy = dir / "streaming" / file.lexically_relative(dir); fs::is_regular_file(copy, ec))
            return copy;
        if (dir == dir.parent_path()) break;
    }
    return {};
}

// A WEM cut short: a streamed sound's first part (its RIFF size says more follows).
bool cut_short(const std::string& wem) { return wem.size() >= 8 && std::uint64_t(u32(wem, 4)) + 8 > wem.size(); }

// Where each sound sits in a file, so the Browser reads a few headers, not a whole 837 MB music package.
struct Placed {
    std::uint32_t id = 0;
    std::uint64_t at = 0, size = 0;
};
std::string read_at(std::ifstream& in, std::uint64_t at, std::uint64_t size) {
    std::string b(size_t(size), '\0');
    in.seekg(std::streamoff(at));
    if (!in.read(b.data(), std::streamsize(size))) fail("a sound file ends too soon");
    return b;
}
// A package's sounds (its stream table; offset = start block x block size, as read_akpk).
std::vector<Placed> package_streams(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string head(8, '\0');
    if (!in.read(head.data(), 8) || head.substr(0, 4) != "AKPK") fail(file.string() + " isn't a sound package (AKPK)");
    head = read_at(in, 0, 8 + std::uint64_t(u32(head, 4)));
    std::vector<Placed> out;
    size_t q = 28 + size_t(u32(head, 12)) + u32(head, 16);  // past the language map and the bank table
    for (std::uint32_t i = 0, n = u32(head, q); i < n; ++i, q += 20)
        out.push_back({u32(head, q + 4), std::uint64_t(u32(head, q + 16)) * std::max(1u, u32(head, q + 8)),
                       u32(head, q + 12)});
    return out;
}
// A bank's media (DIDX: id, offset in DATA, size), read past the media without loading them.
std::vector<Placed> bank_media(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail("can't read " + file.string());
    std::string index;
    std::uint64_t data = 0;
    for (std::uint64_t at = 0;;) {
        std::string h(8, '\0');
        in.seekg(std::streamoff(at));
        if (!in.read(h.data(), 8)) break;
        if (at == 0 && h.substr(0, 4) != "BKHD") fail(file.string() + " isn't a sound bank");
        const std::uint32_t size = u32(h, 4);
        if (h.substr(0, 4) == "DIDX") index = read_at(in, at + 8, size);
        if (h.substr(0, 4) == "DATA") data = at + 8;
        at += 8 + std::uint64_t(size);
    }
    std::vector<Placed> out;
    for (size_t i = 0; i + 12 <= index.size(); i += 12) out.push_back({u32(index, i), data + u32(index, i + 4), u32(index, i + 8)});
    return out;
}
// The package holding the sounds' bytes: the file, or for a header-only copy its copy under streaming\.
fs::path package_with_data(const fs::path& file) {
    std::uint64_t end = 0;
    for (const Placed& p : package_streams(file)) end = std::max(end, p.at + p.size);
    if (std::error_code ec; fs::file_size(file, ec) >= end) return file;
    const fs::path copy = streaming_copy(file);
    if (copy.empty()) fail("its sounds are in its copy under streaming\\, which isn't there");
    return copy;
}
// One sound's bytes from a file, by its place (empty if the id isn't there).
std::string read_placed(const fs::path& file, const std::vector<Placed>& places, std::uint32_t id) {
    for (const Placed& p : places)
        if (p.id == id && p.size) {
            std::ifstream in(file, std::ios::binary);
            return read_at(in, p.at, p.size);
        }
    return {};
}

}  // namespace

SoundReplacement replace_sounds(const fs::path& natives, const std::string& sound_dir,
                                const std::set<std::uint32_t>& ids, const SoundSource& sound,
                                const fs::path& codebooks) {
    const fs::path base = natives / sound_dir, streamed = natives / "streaming" / sound_dir;
    const std::string dir = fs::path(sound_dir).generic_string() + "/";
    std::error_code ec;
    if (!fs::is_directory(base, ec)) fail("the game's sound folder isn't there: " + base.string());

    // Which files name an id: packages by their tables, banks by their media and event data.
    std::vector<std::string> banks, packages;
    std::set<std::pair<std::uint32_t, std::string>> in_package;  // (id, language)
    std::set<std::uint32_t> music, found;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        const std::string name = e.path().filename().string();
        if (!e.is_regular_file(ec)) continue;
        if (is_package(name)) {
            bool hit = false;
            for (const AkpkFile& f : read_package_header(e.path()).streams)
                if (ids.contains(std::uint32_t(f.id))) {
                    hit = true;
                    found.insert(std::uint32_t(f.id));
                    in_package.insert({std::uint32_t(f.id), language_of(name)});
                }
            if (hit) packages.push_back(name);
        } else if (is_bank(name)) {
            const BankHead h = read_bank_head(e.path());
            bool hit = false;
            for (const std::uint32_t id : h.media)
                if (ids.contains(id)) hit = true, found.insert(id);
            for (const BankSource& s : h.sources)
                if (ids.contains(s.media)) {
                    hit = true;
                    if (s.music) music.insert(s.media);
                }
            if (hit) banks.push_back(name);
        }
    }

    // The originals: a streamed sound's from its package, any other's whole from a bank.
    SoundReplacement out;
    std::map<std::pair<std::uint32_t, std::string>, std::string> originals;
    std::map<std::string, Akpk> package_data;
    std::map<std::string, Bank> bank_data;
    for (const std::string& name : packages) {
        Akpk p = read_akpk(read_file(streamed / name));
        for (const AkpkFile& f : p.streams)
            if (ids.contains(std::uint32_t(f.id))) originals[{std::uint32_t(f.id), language_of(name)}] = f.data;
        package_data[name] = std::move(p);
    }
    for (const std::string& name : banks) {
        Bank b = read_bank(read_file(base / name));
        for (const BankMedia& m : b.media)
            if (ids.contains(m.id) && !cut_short(m.data)) originals.emplace(std::pair{m.id, language_of(name)}, m.data);
        bank_data[name] = std::move(b);
    }
    auto original_for = [&](std::uint32_t id, const std::string& language) -> const std::string* {
        auto it = originals.find({id, language});
        if (it == originals.end()) it = originals.find({id, ""});
        if (it == originals.end())
            it = std::ranges::find_if(originals, [&](const auto& o) { return o.first.first == id; });
        return it == originals.end() ? nullptr : &it->second;
    };
    for (const std::uint32_t id : ids)
        if (!found.contains(id) || !original_for(id, "")) out.missing.insert(id);
    if (!out.missing.empty()) return out;
    auto streamed_in = [&](std::uint32_t id, const std::string& language) {
        return in_package.contains({id, language}) || in_package.contains({id, ""}) ||
               std::ranges::any_of(in_package, [&](const auto& p) { return p.first == id; });
    };

    std::map<const std::string*, std::string> made;  // each original's new sound, encoded once
    auto new_for = [&](std::uint32_t id, const std::string* original) -> const std::string& {
        if (const auto it = made.find(original); it != made.end()) return it->second;
        const WemInfo like = read_wem_info(*original);
        try {
            return made[original] = encode_wem(*original, sound(id, like, music.contains(id)), codebooks);
        } catch (const std::runtime_error& e) {
            fail("sound " + std::to_string(id) + ": " + e.what());
        }
    };
    for (auto& [name, p] : package_data) {
        for (AkpkFile& f : p.streams)
            if (ids.contains(std::uint32_t(f.id)))
                f.data = new_for(std::uint32_t(f.id), original_for(std::uint32_t(f.id), language_of(name)));
        out.files.push_back({"streaming/" + dir + name, write_akpk(p, true)});
        out.files.push_back({dir + name, write_akpk(p, false)});
    }
    for (auto& [name, b] : bank_data) {
        const std::string language = language_of(name);
        for (BankMedia& m : b.media) {
            if (!ids.contains(m.id)) continue;
            const std::string* original = original_for(m.id, language);
            const std::string& now = new_for(m.id, original);
            // A streamed sound's first part: its header and as many packets as the original's had.
            m.data = streamed_in(m.id, language) ? now.substr(0, wem_prefix(now, wem_packets_within(*original, m.data.size())))
                                                 : now;
        }
        for (const BankSource& s : bank_sources(b)) {
            if (!ids.contains(s.media)) continue;
            const std::string* original = original_for(s.media, language);
            const std::string& now = new_for(s.media, original);
            set_source_memory(b, s, std::uint32_t(s.stream == 0 ? now.size()
                                                                : wem_prefix(now, wem_packets_within(*original, s.memory))));
        }
        out.files.push_back({dir + name, write_bank(b)});
    }
    return out;
}

std::vector<SoundEntry> list_sounds(const fs::path& file) {
    const std::string name = file.filename().string();
    auto entry = [](std::uint32_t id, const std::string& wem, const char* where) {
        SoundEntry e{id, {}, where};
        try {
            e.info = read_wem_info(wem);
        } catch (const std::runtime_error&) {
            e.where = "not a sound";
        }
        return e;
    };
    // Only each sound's start is read: enough for its header (read_wem_info takes a cut-off data chunk). An Opus
    // sound's packet table comes before its data and can be longer (6 KB for the intro's dialogue): then that sound
    // is read whole. ponytail: one sound at most, never the whole file; reading chunk by chunk if that's ever slow.
    auto head_of = [](std::ifstream& in, const Placed& p) {
        std::string head = read_at(in, p.at, std::min<std::uint64_t>(p.size, 4096));
        if (head.size() < p.size) try {
                read_wem_info(head);
            } catch (const std::runtime_error&) {
                head = read_at(in, p.at, p.size);
            }
        return head;
    };
    std::vector<SoundEntry> out;
    if (is_package(name)) {
        const fs::path f = package_with_data(file);
        std::ifstream in(f, std::ios::binary);
        for (const Placed& p : package_streams(f)) out.push_back(entry(p.id, head_of(in, p), "in this package"));
    } else if (is_bank(name)) {
        std::ifstream in(file, std::ios::binary);
        for (const Placed& p : bank_media(file)) {
            const std::string head = head_of(in, p);
            const bool first_part = head.size() >= 8 && std::uint64_t(u32(head, 4)) + 8 > p.size;  // as cut_short
            out.push_back(entry(p.id, head, first_part ? "streamed: its first part" : "in this bank"));
        }
    } else {
        fail(name + " isn't a sound bank (.sbnk) or package (.spck)");
    }
    return out;
}

std::string sound_wem(const fs::path& file, std::uint32_t id) {
    const std::string name = file.filename().string();
    if (is_package(name)) {
        const fs::path f = package_with_data(file);
        if (std::string wem = read_placed(f, package_streams(f), id); !wem.empty()) return wem;
    } else {
        if (std::string wem = read_placed(file, bank_media(file), id); !wem.empty() && !cut_short(wem)) return wem;
        // A streamed sound's first part: the whole one is in a package beside the bank (its streaming copy).
        std::error_code ec;
        const fs::path packages = streaming_copy(file).empty() ? fs::path() : streaming_copy(file).parent_path();
        for (const fs::path& dir : {file.parent_path(), packages}) {
            if (dir.empty()) continue;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (!is_package(e.path().filename().string())) continue;
                const Akpk head = read_package_header(e.path());
                if (std::ranges::none_of(head.streams, [&](const AkpkFile& s) { return s.id == id; })) continue;
                const fs::path f = package_with_data(e.path());
                if (std::string wem = read_placed(f, package_streams(f), id); !wem.empty()) return wem;
            }
        }
    }
    fail("sound " + std::to_string(id) + " wasn't found");
}

std::string wav_bytes(const std::vector<std::int16_t>& pcm, unsigned channels, unsigned rate) {
    std::string out = "RIFF";
    auto put = [&](std::uint32_t v, int bytes) { out.append(reinterpret_cast<const char*>(&v), size_t(bytes)); };
    const std::uint32_t data = std::uint32_t(pcm.size() * 2);
    put(36 + data, 4);
    out += "WAVEfmt ";
    put(16, 4), put(1, 2), put(channels, 2), put(rate, 4), put(rate * channels * 2, 4), put(channels * 2, 2), put(16, 2);
    out += "data";
    put(data, 4);
    out.append(reinterpret_cast<const char*>(pcm.data()), data);
    return out;
}

std::string game_sound_text(const std::string& file, std::uint32_t id) { return file + "#" + std::to_string(id); }

bool parse_game_sound(const std::string& text, std::string& file, std::uint32_t& id) {
    const size_t hash = text.rfind('#');
    if (hash == std::string::npos || hash == 0 || hash + 1 >= text.size() || text.size() - hash - 1 > 10) return false;
    std::uint64_t n = 0;
    for (size_t i = hash + 1; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
        n = n * 10 + std::uint64_t(text[i] - '0');
    }
    if (n == 0 || n > 0xFFFFFFFFull) return false;
    file = text.substr(0, hash);
    id = std::uint32_t(n);
    return true;
}

std::uint32_t sound_id_in(const std::string& name) {
    const std::string stem = fs::path(name).stem().string();
    std::uint64_t id = 0;
    for (size_t i = 0; i < stem.size();) {
        if (!std::isdigit(static_cast<unsigned char>(stem[i]))) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < stem.size() && std::isdigit(static_cast<unsigned char>(stem[j]))) ++j;
        if (j - i >= 4 && j - i <= 10) id = std::stoull(stem.substr(i, j - i));
        i = j;
    }
    return id <= 0xFFFFFFFFull ? std::uint32_t(id) : 0;
}

}  // namespace remod
