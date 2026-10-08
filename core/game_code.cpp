#include "game_code.hpp"

#include "settings.hpp"

#include <lua.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace remod {

namespace fs = std::filesystem;

std::string GameMethod::prototype() const {
    std::string s = name + "(";
    for (size_t i = 0; i < params.size(); ++i) s += (i ? ", " : "") + params[i].first;
    return s + ")";
}

// The cache's lines (§ reading the dump: T / F / M, tab-separated), from the first T on, and an index of the T lines.
struct GameCode::Store {
    std::string text;
    std::vector<std::string_view> names;                      // sorted
    std::vector<std::pair<std::string_view, size_t>> index;   // name -> offset of its T line, sorted by name
    std::mutex mutex;
    std::unordered_map<size_t, std::unique_ptr<GameType>> parsed;  // by T-line offset; pointers stay valid
};

GameCode::GameCode() = default;
GameCode::~GameCode() = default;
GameCode::GameCode(GameCode&&) noexcept = default;
GameCode& GameCode::operator=(GameCode&&) noexcept = default;

size_t GameCode::size() const { return store ? store->index.size() : 0; }

const std::vector<std::string_view>& GameCode::names() const {
    static const std::vector<std::string_view> none;
    return store ? store->names : none;
}

namespace {

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> out;
    for (size_t at; (at = line.find('\t')) != std::string_view::npos; line.remove_prefix(at + 1)) out.push_back(line.substr(0, at));
    out.push_back(line);
    return out;
}

// One F or M line (its fields split) into `t`.
void add_member(GameType& t, const std::vector<std::string_view>& f) {
    if (f[0] == "F" && f.size() == 4) {
        t.fields.push_back({std::string(f[1]), std::string(f[2]), f[3] == "1"});
    } else if (f[0] == "M" && f.size() >= 4 && f.size() % 2 == 0) {
        GameMethod m{std::string(f[1]), std::string(f[2]), {}, f[3] == "1"};
        for (size_t i = 4; i + 1 < f.size(); i += 2) m.params.emplace_back(std::string(f[i]), std::string(f[i + 1]));
        t.methods.push_back(std::move(m));
    }
}

// The lines of the type whose T line starts at `at`, the T line itself left out.
std::string_view block(const std::string& text, size_t at) {
    const size_t from = text.find('\n', at) + 1;
    size_t end = text.find("\nT\t", from - 1);
    end = end == std::string::npos ? text.size() : end + 1;
    return std::string_view(text).substr(from, end - from);
}

template <class F>
void each_line(std::string_view s, F&& f) {
    while (!s.empty()) {
        const size_t nl = s.find('\n');
        f(s.substr(0, nl));
        if (nl == std::string_view::npos) break;
        s.remove_prefix(nl + 1);
    }
}

}  // namespace

const GameType* GameCode::find(std::string_view name) const {
    if (!store) return nullptr;
    Store& s = *store;
    const auto it = std::ranges::lower_bound(s.index, name, {}, &std::pair<std::string_view, size_t>::first);
    if (it == s.index.end() || it->first != name) return nullptr;
    std::lock_guard lock(s.mutex);
    auto& slot = s.parsed[it->second];
    if (!slot) {
        slot = std::make_unique<GameType>();
        const size_t tab = s.text.find('\t', it->second + 2);  // "T\t<name>\t<parent>"
        slot->parent = s.text.substr(tab + 1, s.text.find('\n', tab) - tab - 1);
        each_line(block(s.text, it->second), [&](std::string_view line) { add_member(*slot, split_tabs(line)); });
    }
    return slot.get();
}

namespace {

// The type and its parents, nearest first. ponytail: stops after 64 (a cycle in a broken dump).
std::vector<const GameType*> lineage(const GameCode& code, const std::string& type) {
    std::vector<const GameType*> out;
    for (const GameType* t = code.find(type); t && out.size() < 64; t = t->parent.empty() ? nullptr : code.find(t->parent))
        out.push_back(t);
    return out;
}

// REFramework skips generic method definitions (a "!" in a parameter or return type) when matching [official].
bool generic_definition(const GameMethod& m) {
    if (m.returns.find('!') != std::string::npos) return true;
    return std::ranges::any_of(m.params, [](const auto& p) { return p.first.find('!') != std::string::npos; });
}

}  // namespace

const GameField* GameCode::field(const std::string& type, const std::string& name) const {
    for (const GameType* t : lineage(*this, type))
        for (const GameField& f : t->fields)
            if (f.name == name) return &f;
    return nullptr;
}

const GameMethod* GameCode::method(const std::string& type, const std::string& name) const {
    // As RETypeDefinition::get_method: the name first, then "name(T1, T2)", each from the type up through its parents.
    for (const GameType* t : lineage(*this, type))
        for (const GameMethod& m : t->methods)
            if (m.name == name && !generic_definition(m)) return &m;
    if (name.find('(') == std::string::npos) return nullptr;
    for (const GameType* t : lineage(*this, type))
        for (const GameMethod& m : t->methods)
            if (!generic_definition(m) && m.prototype() == name) return &m;
    return nullptr;
}

// ---- Reading the dump ----

namespace {

using nlohmann::json;

// The dump's layout [official, ObjectExplorer.cpp]: { "<type>": { "parent": "<type>", "fields": { "<name>": { "type",
// "flags" } }, "methods": { "<name><id>": { "id", "flags", "params": [ { "type", "name" } ], "returns": { "type" } } },
// ... } }. Everything else (addresses, RSZ, properties, reflection) is skipped as it streams past.
// Each type is written out as cache lines once the next begins, so only one is held at a time.
struct DumpReader : json::json_sax_t {
    std::string& out;
    explicit DumpReader(std::string& o) : out(o) {}

    int depth = 0;  // open objects and arrays
    std::string keys[8];
    std::string type_name;
    GameType current;
    GameType* type = nullptr;
    size_t types = 0;

    void flush() {
        if (!type) return;
        // ponytail: a tab or line break inside a name would break the line format; none seen, made a space.
        const auto clean = [](std::string& s) { std::ranges::replace_if(s, [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' '); };
        clean(type_name), clean(current.parent);
        for (GameField& f : current.fields) clean(f.name), clean(f.type);
        for (GameMethod& m : current.methods) {
            clean(m.name), clean(m.returns);
            for (auto& [ptype, pname] : m.params) clean(ptype), clean(pname);
        }
        out += "T\t" + type_name + "\t" + current.parent + "\n";
        for (const GameField& f : current.fields) out += "F\t" + f.name + "\t" + f.type + "\t" + (f.is_static ? "1" : "0") + "\n";
        for (const GameMethod& m : current.methods) {
            out += "M\t" + m.name + "\t" + m.returns + "\t" + (m.is_static ? "1" : "0");
            for (const auto& [ptype, pname] : m.params) out += "\t" + ptype + "\t" + pname;
            out += "\n";
        }
        ++types;
        current = {};
        type = nullptr;
    }
    GameMethod* method = nullptr;
    long long method_id = -1;
    std::string method_key, error;

    const std::string& at(int d) const {
        static const std::string none;
        return d < 8 ? keys[d] : none;
    }
    bool in(const char* section) const { return depth >= 3 && at(2) == section; }

    bool key(string_t& k) override {
        if (depth < 8) keys[depth] = k;
        if (depth == 1) {
            flush();
            type_name = k;
            type = &current;
            method = nullptr;
        } else if (depth == 3 && type && in("fields")) {
            type->fields.push_back({k, "", false});
        } else if (depth == 3 && type && in("methods")) {
            type->methods.push_back({});
            method = &type->methods.back();
            method_key = k;
            method_id = -1;
        }
        return true;
    }
    bool string(string_t& v) override {
        if (!type) return true;
        if (depth == 2 && at(2) == "parent") {
            type->parent = std::move(v);
        } else if (depth == 4 && in("fields") && !type->fields.empty()) {
            if (at(4) == "type") type->fields.back().type = std::move(v);
            if (at(4) == "flags") type->fields.back().is_static = v.find("Static") != std::string::npos;
        } else if (depth == 4 && in("methods") && method && at(4) == "flags") {
            method->is_static = v.find("Static") != std::string::npos;
        } else if (depth == 5 && in("methods") && method && at(4) == "returns" && at(5) == "type") {
            method->returns = std::move(v);
        } else if (depth == 6 && in("methods") && method && at(4) == "params" && !method->params.empty()) {
            if (at(6) == "type") method->params.back().first = std::move(v);
            if (at(6) == "name") method->params.back().second = std::move(v);
        }
        return true;
    }
    bool number_integer(number_integer_t v) override { return id(v); }
    bool number_unsigned(number_unsigned_t v) override { return id(static_cast<long long>(v)); }
    bool id(long long v) {
        if (depth == 4 && in("methods") && at(4) == "id") method_id = v;
        return true;
    }
    bool start_object(std::size_t) override {
        ++depth;
        if (depth == 6 && in("methods") && method && at(4) == "params") method->params.emplace_back();
        return true;
    }
    bool end_object() override {
        if (depth == 4 && in("methods") && method) {
            // Its key is the name and its id ("get_HP1234"); the id says where the name ends.
            std::string name = method_key;
            if (const std::string id = method_id >= 0 ? std::to_string(method_id) : ""; !id.empty() && name.ends_with(id))
                name.resize(name.size() - id.size());
            else
                while (!name.empty() && std::isdigit(static_cast<unsigned char>(name.back()))) name.pop_back();
            method->name = std::move(name);
        }
        --depth;
        return true;
    }
    bool start_array(std::size_t) override { return ++depth, true; }
    bool end_array() override { return --depth, true; }
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return true; }
    bool binary(binary_t&) override { return true; }
    bool parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception& e) override {
        error = "at byte " + std::to_string(position) + ": " + e.what();
        return false;
    }
};

constexpr const char* kCacheMagic = "remod-game-code\t1";

std::string stamp(const fs::path& dump) {
    return dump.string() + "\t" + std::to_string(fs::file_size(dump)) + "\t" +
           std::to_string(fs::last_write_time(dump).time_since_epoch().count());
}

// Cache lines into a GameCode: indexes the T lines; nullopt if a line isn't ours (cut short, or another file).
std::optional<GameCode> from_lines(std::string text) {
    auto s = std::make_unique<GameCode::Store>();
    s->text = std::move(text);
    bool ok = true, any = false;
    size_t at = 0;
    each_line(s->text, [&](std::string_view line) {
        if (line.starts_with("T\t")) {
            const size_t tab = line.find('\t', 2);
            if (tab == std::string_view::npos) ok = false;
            else s->index.emplace_back(line.substr(2, tab - 2), at), any = true;
        } else if (!any || !(line.starts_with("F\t") || line.starts_with("M\t"))) {
            ok = false;
        }
        at += line.size() + 1;
    });
    if (!ok || (!s->text.empty() && s->text.back() != '\n')) return std::nullopt;
    std::ranges::stable_sort(s->index, {}, &std::pair<std::string_view, size_t>::first);
    // A name twice (a broken dump): the first kept.
    const auto dup = std::ranges::unique(s->index, {}, &std::pair<std::string_view, size_t>::first);
    s->index.erase(dup.begin(), dup.end());
    s->names.reserve(s->index.size());
    for (const auto& [name, _] : s->index) s->names.push_back(name);
    GameCode code;
    code.store = std::move(s);
    return code;
}

std::optional<std::string> read_whole(const fs::path& file) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || !f) return std::nullopt;
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> in(f, &std::fclose);
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) return std::nullopt;
    std::string text(size, '\0');
    if (std::fread(text.data(), 1, size, f) != size) return std::nullopt;
    return text;
}

std::optional<GameCode> read_cache(const fs::path& cache, const std::string& want) {
    auto text = read_whole(cache);
    if (!text) return std::nullopt;
    const std::string head = std::string(kCacheMagic) + "\t" + want + "\n";
    if (!text->starts_with(head)) return std::nullopt;
    text->erase(0, head.size());
    return from_lines(std::move(*text));
}

void write_cache(const std::string& lines, const fs::path& cache, const std::string& stamp_line) {
    std::error_code ec;
    fs::create_directories(cache.parent_path(), ec);
    const fs::path part = fs::path(cache) += ".part";
    {
        std::ofstream out(part, std::ios::binary);
        out << kCacheMagic << "\t" << stamp_line << "\n" << lines;
        if (!out.flush()) return;  // a cache is only a speed-up
    }
    fs::rename(part, cache, ec);
}

}  // namespace

GameCode load_game_code(const fs::path& dump, const fs::path& cache) {
    std::error_code ec;
    if (!fs::is_regular_file(dump, ec)) throw std::runtime_error("SDK dump not found: " + dump.string());
    const std::string want = stamp(dump);
    if (!cache.empty())
        if (auto cached = read_cache(cache, want)) return std::move(*cached);
    std::FILE* opened = nullptr;
    if (_wfopen_s(&opened, dump.c_str(), L"rb") != 0) opened = nullptr;
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> in(opened, &std::fclose);
    if (!in) throw std::runtime_error("can't open " + dump.string());
    std::string lines;
    DumpReader reader(lines);
    if (!json::sax_parse(in.get(), &reader))
        throw std::runtime_error(dump.filename().string() + " isn't a readable SDK dump (" + reader.error + ")");
    reader.flush();
    if (reader.types == 0) throw std::runtime_error(dump.filename().string() + " holds no types: not an SDK dump?");
    if (!cache.empty()) write_cache(lines, cache, want);
    auto code = from_lines(std::move(lines));
    if (!code) throw std::runtime_error(dump.filename().string() + " couldn't be indexed");  // not expected: lines cleaned
    return std::move(*code);
}

namespace {
std::mutex g_code_mutex;
fs::path g_dump, g_cache_dir;
std::string g_loaded;  // stamp of the dump g_code was read from
std::unique_ptr<GameCode> g_code;
}  // namespace

void set_sdk_dump(const fs::path& dump, const fs::path& cache_dir) {
    std::lock_guard lock(g_code_mutex);
    g_dump = dump;
    g_cache_dir = cache_dir;
}

const GameCode* game_code() {
    std::lock_guard lock(g_code_mutex);
    if (g_dump.empty()) return nullptr;
    std::error_code ec;
    if (!fs::is_regular_file(g_dump, ec)) throw std::runtime_error("SDK dump not found: " + g_dump.string());
    const std::string now = stamp(g_dump);
    if (!g_code || g_loaded != now) {
        fs::path dir = g_cache_dir;
        if (dir.empty())
            if (const fs::path run = default_cache_dir(); !run.empty())  // %LOCALAPPDATA%\remod\run_cache: ours beside it
                dir = run.parent_path() / "game_code";
        const fs::path cache =
            dir.empty() ? fs::path() : dir / (std::to_string(std::hash<std::string>{}(g_dump.string())) + ".txt");
        g_code = std::make_unique<GameCode>(load_game_code(g_dump, cache));
        g_loaded = now;
    }
    return g_code.get();
}

// ---- Search ----

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string field_detail(const GameField& f) { return (f.is_static ? "static " : "") + f.type; }

std::string method_detail(const GameMethod& m) {
    std::string s = m.is_static ? "static " : "";
    s += (m.returns.empty() ? "void" : m.returns) + " " + m.name + "(";
    for (size_t i = 0; i < m.params.size(); ++i)
        s += (i ? ", " : "") + m.params[i].first + (m.params[i].second.empty() ? "" : " " + m.params[i].second);
    return s + ")";
}

}  // namespace

std::vector<CodeHit> search_game_code(const GameCode& code, const std::string& query, size_t limit) {
    std::vector<CodeHit> hits;
    std::string q = query;
    q.erase(0, q.find_first_not_of(' '));
    q.erase(q.find_last_not_of(' ') + 1);
    if (const GameType* t = code.find(q)) {  // a type by name: all its own members
        hits.push_back({q, "", t->parent.empty() ? "" : "parent " + t->parent});
        for (const GameField& f : t->fields) hits.push_back({q, f.name, field_detail(f)});
        for (const GameMethod& m : t->methods) hits.push_back({q, m.name, method_detail(m)});
        return hits;
    }
    std::vector<std::string> words;
    std::istringstream in(lower(q));
    for (std::string w; in >> w;) words.push_back(w);
    if (words.empty()) return hits;
    const auto all = [&](const std::string& text) {
        return std::ranges::all_of(words, [&](const std::string& w) { return text.find(w) != std::string::npos; });
    };
    std::string low;  // reused: no allocation per name
    const auto lowered = [&](std::string_view type, std::string_view member) -> const std::string& {
        low.assign(type);
        if (!member.empty()) low.append(".").append(member);
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return low;
    };
    for (std::string_view name : code.names())
        if (hits.size() < limit && all(lowered(name, {}))) {
            const GameType& t = *code.find(name);
            hits.push_back({std::string(name), "", t.parent.empty() ? "" : "parent " + t.parent});
        }
    // Members straight from the cache's lines, so a search doesn't read every type in.
    if (!code.store) return hits;
    for (const auto& [name, at] : code.store->index) {
        if (hits.size() >= limit) break;
        each_line(block(code.store->text, at), [&](std::string_view line) {
            const std::string_view member = line.substr(2, line.find('\t', 2) - 2);
            if (hits.size() >= limit || !all(lowered(name, member))) return;
            GameType one;
            add_member(one, split_tabs(line));
            if (!one.fields.empty()) hits.push_back({std::string(name), one.fields[0].name, field_detail(one.fields[0])});
            if (!one.methods.empty()) hits.push_back({std::string(name), one.methods[0].name, method_detail(one.methods[0])});
        });
    }
    return hits;
}

// ---- Checking a script ----

namespace {

struct Token {
    enum Kind { Name, String, Symbol } kind;
    std::string text;
    int line;
};

// Lua's tokens, enough to find names and strings [Lua 5.4 manual §3.1]: comments and long brackets skipped or read
// whole, numbers as one symbol. Syntax errors are Lua's own parser's business (check_lua), so this never fails.
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    int line = 1;
    size_t i = 0;
    // A long bracket "[==[" at i: its level (number of '='), else -1.
    const auto long_open = [&](size_t at) {
        if (at >= s.size() || s[at] != '[') return -1;
        size_t j = at + 1;
        while (j < s.size() && s[j] == '=') ++j;
        return j < s.size() && s[j] == '[' ? int(j - at - 1) : -1;
    };
    // Reads a long bracket's contents (i at its "["), leaving i past its close.
    const auto long_read = [&](int level) {
        i += size_t(level) + 2;
        const std::string close = "]" + std::string(size_t(level), '=') + "]";
        const size_t end = s.find(close, i);
        const size_t stop = end == std::string::npos ? s.size() : end;
        std::string text = s.substr(i, stop - i);
        line += int(std::ranges::count(text, '\n'));
        i = end == std::string::npos ? s.size() : end + close.size();
        return text;
    };
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') {
            ++line, ++i;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            i += 2;
            if (const int level = long_open(i); level >= 0) long_read(level);
            else while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '"' || c == '\'') {
            const int at = line;
            std::string text;
            for (++i; i < s.size() && s[i] != c && s[i] != '\n'; ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    ++i;
                    if (s[i] == '\n') ++line;
                    text += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
                } else {
                    text += s[i];
                }
            }
            ++i;
            out.push_back({Token::String, text, at});
        } else if (const int level = long_open(i); level >= 0) {
            const int at = line;
            std::string text = long_read(level);
            if (!text.empty() && text[0] == '\n') text.erase(0, 1);  // Lua drops a first newline
            out.push_back({Token::String, text, at});
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const size_t from = i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            out.push_back({Token::Name, s.substr(from, i - from), line});
        } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                   (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            for (++i; i < s.size(); ++i) {
                const char d = s[i];
                const bool sign = (d == '+' || d == '-') && std::string_view("eEpP").find(s[i - 1]) != std::string_view::npos;
                if (!std::isalnum(static_cast<unsigned char>(d)) && d != '.' && !sign) break;
            }
            out.push_back({Token::Symbol, "0", line});
        } else {
            out.push_back({Token::Symbol, std::string(1, c), line});
            ++i;
        }
    }
    return out;
}

size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = up;
        }
    }
    return row[b.size()];
}

// Up to three of `names` closest to `name` (ignoring case), as " (did you mean a, b?)", else "".
template <class Names>
std::string nearest(const std::string& name, const Names& names) {
    const std::string want = lower(name);
    const size_t limit = std::max<size_t>(2, want.size() / 4);
    std::vector<std::pair<size_t, std::string>> close;
    for (const auto& n : names) {
        if (n.size() > want.size() + limit || want.size() > n.size() + limit) continue;  // can't be close
        const std::string low = lower(std::string(n));
        if (const size_t d = edit_distance(want, low); d <= limit) close.emplace_back(d, std::string(n));
    }
    std::ranges::sort(close);
    std::string out;
    for (size_t i = 0; i < close.size() && i < 3; ++i) out += (i ? ", " : "") + close[i].second;
    return out.empty() ? "" : " (did you mean " + out + "?)";
}

// What a variable (or an expression) stands for: a type definition, or an object of a type.
struct Ref {
    enum Kind { Type, Object } kind;
    std::string type;
};

class Checker {
public:
    Checker(const std::vector<Token>& t, const GameCode& code, std::vector<ScriptProblem>& problems)
        : t_(t), code_(code), problems_(problems) {}

    void run() {
        std::map<std::string, Ref> vars;
        for (size_t i = 0; i < t_.size(); ++i) {
            if (is(i, Token::Name) && is_sym(i + 1, "=") && !is_sym(i + 2, "=") && !is_sym(i - 1, ".") &&
                !is_sym(i - 1, ":") && !is_sym(i - 1, ",") && !is_sym(i - 1, "=") && !is_sym(i - 1, "~") &&
                !is_sym(i - 1, "<") && !is_sym(i - 1, ">"))
                vars.erase(t_[i].text);  // assigned again: whatever it held before no longer counts
            size_t end = 0;
            std::optional<Ref> ref = producer(i, end);
            if (!ref && is(i, Token::Name) && !is_sym(i - 1, ".") && !is_sym(i - 1, ":"))
                if (const auto v = vars.find(t_[i].text); v != vars.end()) ref = v->second, end = i + 1;
            if (!ref) continue;
            const bool plain = !is_sym(end, ":") && !is_sym(end, ".") && !is_sym(end, "(") && !is_sym(end, "[");
            if (producer_at_(i) && plain && is_sym(i - 1, "=") && is(i - 2, Token::Name) && !is_sym(i - 3, ".") &&
                !is_sym(i - 3, ":") && !is_sym(i - 3, ","))
                vars[t_[i - 2].text] = *ref;
            member(*ref, end);
            if (end > i + 1) i = end - 1;
        }
    }

private:
    const std::vector<Token>& t_;
    const GameCode& code_;
    std::vector<ScriptProblem>& problems_;

    bool is(size_t i, Token::Kind k) const { return i < t_.size() && t_[i].kind == k; }
    bool is_sym(size_t i, const char* s) const { return is(i, Token::Symbol) && t_[i].text == s; }
    bool is_name(size_t i, const char* s) const { return is(i, Token::Name) && t_[i].text == s; }

    bool producer_at_(size_t i) const { return is_name(i, "sdk") && is_sym(i + 1, "."); }

    // sdk.<call>("<type>") at i: checks the type's name; gives what it returns, if tracked, with `end` past ")".
    std::optional<Ref> producer(size_t i, size_t& end) {
        if (!producer_at_(i) || !is(i + 2, Token::Name) || !is_sym(i + 3, "(") || !is(i + 4, Token::String) ||
            !is_sym(i + 5, ")"))
            return std::nullopt;
        const std::string& call = t_[i + 2].text;
        const bool type_def = call == "find_type_definition";
        const bool object = call == "get_managed_singleton" || call == "create_instance";
        if (!type_def && !object && call != "typeof") return std::nullopt;
        end = i + 6;
        const std::string& name = t_[i + 4].text;
        if (!known_type(name)) {
            problem(t_[i + 4].line, "no type \"" + name + "\" in the game" + nearest(name, code_.names()));
            return std::nullopt;
        }
        if (!type_def && !object) return std::nullopt;  // typeof gives a System.Type: not followed
        return Ref{type_def ? Ref::Type : Ref::Object, name};
    }

    // An array of a known type ("X[]") is known too: the dump lists only the arrays the game itself uses.
    bool known_type(std::string name) const {
        while (name.ends_with("[]")) name.resize(name.size() - 2);
        return code_.find(name) != nullptr;
    }

    // ":<call>("<name>"" at i on `ref`: checks a method or field name.
    void member(const Ref& ref, size_t i) {
        if (!is_sym(i, ":") || !is(i + 1, Token::Name) || !is_sym(i + 2, "(") || !is(i + 3, Token::String)) return;
        const std::string& call = t_[i + 1].text;
        const Token& name = t_[i + 3];
        const bool method = ref.kind == Ref::Type ? call == "get_method" : call == "call";
        const bool field = call == "get_field" || (ref.kind == Ref::Object && call == "set_field");
        if (method && !code_.method(ref.type, name.text)) {
            std::vector<std::string> names;
            std::string same;  // the name's own prototypes, when a prototype didn't match
            const std::string bare = name.text.substr(0, name.text.find('('));
            for (const GameType* t : lineage(code_, ref.type))
                for (const GameMethod& m : t->methods) {
                    names.push_back(m.name);
                    if (m.name == bare) same += (same.empty() ? "" : ", ") + m.prototype();
                }
            problem(name.line, ref.type + " has no method \"" + name.text + "\"" +
                                   (!same.empty() && bare != name.text ? " (it has " + same + ")" : nearest(bare, names)));
        } else if (field && !code_.field(ref.type, name.text)) {
            std::vector<std::string> names;
            for (const GameType* t : lineage(code_, ref.type))
                for (const GameField& f : t->fields) names.push_back(f.name);
            problem(name.line, ref.type + " has no field \"" + name.text + "\"" + nearest(name.text, names));
        }
    }

    void problem(int line, std::string message) { problems_.push_back({line, std::move(message)}); }
};

}  // namespace

std::vector<ScriptProblem> check_lua(const std::string& source, const std::string& name, const GameCode* code) {
    std::vector<ScriptProblem> problems;
    // Syntax: Lua's own parser (nothing runs). Its message is "<name>:<line>: <what>".
    lua_State* L = luaL_newstate();
    if (!L) throw std::runtime_error("out of memory starting Lua");
    if (luaL_loadbufferx(L, source.data(), source.size(), ("=" + name).c_str(), "t") != LUA_OK) {
        std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
        int line = 0;
        if (message.starts_with(name + ":")) {
            size_t at = name.size() + 1, end = at;
            while (end < message.size() && std::isdigit(static_cast<unsigned char>(message[end]))) ++end;
            if (end > at && end < message.size() && message[end] == ':') {
                line = std::stoi(message.substr(at, end - at));
                message = message.substr(end + 1);
                message.erase(0, message.find_first_not_of(' '));
            }
        }
        problems.push_back({line, "syntax: " + message});
    }
    lua_close(L);
    if (code) Checker(tokenize(source), *code, problems).run();
    std::ranges::stable_sort(problems, {}, &ScriptProblem::line);
    return problems;
}

}  // namespace remod
