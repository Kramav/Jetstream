#include "gui.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>

// Layout [REE-Lib GuiFile.cs, MIT; RE4R's files read 2026-10-10]. Offsets are absolute, little endian.
//   header: u32 version (version % 100 = 34), u32 "GUIR", i64 offsetsStartOffset, ...; at offsetsStartOffset: i64
//           offsetsStart, i64 viewOffset, then i64 container count and an i64 offset per container.
//   container info: 16 ID, i64 name (UTF-16), i64 class (ASCII), i64 elements (-> i64 count, i64 offset each), ...
//   element (112 bytes): 16 ID, 16 container ID, 16 guid, i64 name (UTF-16), i64 class, i64 attributes (-> i64 count,
//           32 bytes each), i64 reorders, i64 extra attributes, i64 state refs, i64 element data, u64 flags.
//   attribute (32 bytes): u8 type, 3 zero, u32, i64 name (ASCII), 8-byte value (inline, or an offset), u64 name hash.
namespace remod {
namespace {

constexpr std::uint8_t kU16 = 0x05, kStr16 = 0x0D, kVec3 = 0x16;
constexpr size_t kElementSize = 112, kAttributeSize = 32;

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("not a GUI layout RE4R reads: " + why); }

template <class T> T get(const std::string& b, std::uint64_t at) {
    if (at > b.size() || b.size() - at < sizeof(T)) bad("an offset points past the end");
    T v;
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}
template <class T> void put(std::string& b, std::uint64_t at, T v) { std::memcpy(b.data() + at, &v, sizeof v); }

std::string ascii(const std::string& b, std::uint64_t at) {
    std::string s;
    for (char c; (c = get<char>(b, at + s.size())) != 0;) s += c;
    return s;
}
std::string utf16(const std::string& b, std::uint64_t at) {  // names are ASCII; others come out as '?'
    std::string s;
    for (std::uint16_t c; (c = get<std::uint16_t>(b, at + 2 * s.size())) != 0;) s += c < 0x80 ? char(c) : '?';
    return s;
}

struct List {
    std::uint64_t field;                  // where the container keeps the list's offset
    std::vector<std::uint64_t> elements;  // each element's offset
    size_t index;                         // the named one's
};

List find_list(const std::string& b, const std::string& entry) {
    if (b.size() < 64 || get<std::uint32_t>(b, 4) != 0x52495547) bad("no GUIR header");
    if (get<std::uint32_t>(b, 0) % 100 != 34) bad("version " + std::to_string(get<std::uint32_t>(b, 0)) + ", not RE4R's (34)");
    const auto starts = get<std::uint64_t>(b, 8);
    const auto containers = get<std::uint64_t>(b, starts + 16);
    if (containers > 100000) bad("container count");
    for (std::uint64_t c = 0; c < containers; ++c) {
        const auto info = get<std::uint64_t>(b, starts + 24 + 8 * c);
        const auto list = get<std::uint64_t>(b, info + 32);
        if (list == 0) continue;
        const auto count = get<std::uint64_t>(b, list);
        if (count > 100000) bad("element count");
        List l{info + 32, {}, 0};
        bool found = false;
        for (std::uint64_t i = 0; i < count; ++i) {
            l.elements.push_back(get<std::uint64_t>(b, list + 8 + 8 * i));
            if (!found && utf16(b, get<std::uint64_t>(b, l.elements.back() + 48)) == entry) found = true, l.index = i;
        }
        if (found) return l;
    }
    throw std::runtime_error("no element named " + entry + " in the GUI layout");
}

// An element's attribute named `name`: its offset, or 0.
std::uint64_t attribute(const std::string& b, std::uint64_t element, const char* name) {
    const auto block = get<std::uint64_t>(b, element + 64);
    const auto count = get<std::uint64_t>(b, block);
    for (std::uint64_t i = 0; i < count && i < 1000; ++i) {
        const std::uint64_t a = block + 8 + kAttributeSize * i;
        if (ascii(b, get<std::uint64_t>(b, a + 8)) == name) return a;
    }
    return 0;
}

GuiEntry entry_at(const std::string& b, std::uint64_t element) {
    GuiEntry e{utf16(b, get<std::uint64_t>(b, element + 48))};
    if (const auto p = attribute(b, element, "Position"); p && get<std::uint8_t>(b, p) == kVec3) {
        const auto v = get<std::uint64_t>(b, p + 16);
        e.x = get<float>(b, v), e.y = get<float>(b, v + 4), e.z = get<float>(b, v + 8);
    }
    if (const auto p = attribute(b, element, "Priority"); p && get<std::uint8_t>(b, p) == kU16)
        e.priority = get<std::uint16_t>(b, p + 16);
    return e;
}

std::uint64_t append(std::string& b, const void* data, size_t size) {
    b.resize((b.size() + 15) & ~size_t(15), '\0');
    const std::uint64_t at = b.size();
    b.append(static_cast<const char*>(data), size);
    return at;
}

std::uint64_t fnv(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

}  // namespace

std::vector<GuiEntry> gui_list(const std::string& gui, const std::string& entry) {
    std::vector<GuiEntry> out;
    for (const auto e : find_list(gui, entry).elements) out.push_back(entry_at(gui, e));
    return out;
}

std::string add_gui_entries(const std::string& gui, const std::string& copy, const std::vector<std::string>& names) {
    const List l = find_list(gui, copy);
    for (const auto& n : names)
        for (const auto e : l.elements)
            if (utf16(gui, get<std::uint64_t>(gui, e + 48)) == n) throw std::runtime_error(n + " is already in the GUI layout");
    const std::uint64_t src = l.elements[l.index];
    const GuiEntry from = entry_at(gui, src);
    GuiEntry step{};
    if (l.index > 0) {
        const GuiEntry prev = entry_at(gui, l.elements[l.index - 1]);
        step.x = from.x - prev.x, step.y = from.y - prev.y, step.z = from.z - prev.z;
    }
    const auto name_attr = attribute(gui, src, "Name");
    if (name_attr && get<std::uint8_t>(gui, name_attr) != kStr16) bad(copy + "'s Name isn't text");
    const auto pos_attr = attribute(gui, src, "Position");
    if (pos_attr && get<std::uint8_t>(gui, pos_attr) != kVec3) bad(copy + "'s Position isn't a Vec3");
    const auto attrs = get<std::uint64_t>(gui, src + 64);
    const auto attr_count = get<std::uint64_t>(gui, attrs);

    std::string b = gui;
    std::vector<std::uint64_t> added;
    for (size_t i = 0; i < names.size(); ++i) {
        const float k = float(i + 1);
        std::u16string wide(names[i].begin(), names[i].end());
        const auto name_at = append(b, wide.c_str(), (wide.size() + 1) * 2);
        const float pos[3]{from.x + k * step.x, from.y + k * step.y, from.z + k * step.z};
        const auto pos_at = append(b, pos, sizeof pos);
        const std::string block = gui.substr(attrs, 8 + kAttributeSize * attr_count);
        const auto block_at = append(b, block.data(), block.size());
        for (std::uint64_t a = 0; a < attr_count; ++a) {
            const std::uint64_t at = block_at + 8 + kAttributeSize * a;
            const std::string what = ascii(b, get<std::uint64_t>(b, at + 8));
            if (what == "Name") put(b, at + 16, name_at);
            if (what == "Position" && pos_attr) put(b, at + 16, pos_at);
            if (what == "Priority" && get<std::uint8_t>(b, at) == kU16)
                put(b, at + 16, std::uint16_t(from.priority + int(i) + 1));
        }
        std::string element = gui.substr(src, kElementSize);
        const std::uint64_t id[2]{fnv(copy + '/' + names[i]), fnv(names[i] + '/' + copy)};  // the same names, the same IDs
        std::memcpy(element.data(), id, 16);
        put(element, 48, name_at);
        put(element, 64, block_at);
        added.push_back(append(b, element.data(), element.size()));
    }
    std::vector<std::uint64_t> list{l.elements.size() + added.size()};
    list.insert(list.end(), l.elements.begin(), l.elements.begin() + l.index + 1);
    list.insert(list.end(), added.begin(), added.end());
    list.insert(list.end(), l.elements.begin() + l.index + 1, l.elements.end());
    put(b, l.field, append(b, list.data(), list.size() * 8));
    return b;
}

}  // namespace remod
