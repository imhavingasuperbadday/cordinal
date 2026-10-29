// Reads a pack (packs.h) into plain data: the stored-ZIP container of a .cordpack, a small ini reader,
// and one reader per kind of file. Each reader takes what it understands and records the rest as a
// warning, so a pack written for a later cordinal still gives this one everything it can use.
#include "packs.h"

#include "autoplaylist.h"
#include "inflate.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <iterator>

namespace {

uint32_t Le16(const std::string& s, size_t at) { return (uint8_t)s[at] | (uint8_t)s[at + 1] << 8; }
uint32_t Le32(const std::string& s, size_t at) { return Le16(s, at) | Le16(s, at + 2) << 16; }

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n"), e = s.find_last_not_of(L" \t\r\n");
    return b == std::wstring::npos ? L"" : s.substr(b, e - b + 1);
}

std::wstring Utf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

int ToInt(const std::wstring* v, int def, int lo, int hi) {
    if (!v || v->empty()) return def;
    return std::clamp((int)wcstol(v->c_str(), nullptr, 10), lo, hi);
}

bool ToBool(const std::wstring* v, bool def) {
    if (!v || v->empty()) return def;
    std::wstring s = Lower(*v);
    return !(s == L"0" || s == L"no" || s == L"off" || s == L"false");
}

bool EndsWith(const std::wstring& s, const wchar_t* tail) {
    size_t n = wcslen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// The file name without its folder and extension: "themes/sea.ini" -> "sea".
std::wstring Stem(const std::wstring& path) {
    size_t slash = path.rfind(L'/'), dot = path.rfind(L'.');
    size_t from = slash == std::wstring::npos ? 0 : slash + 1;
    return path.substr(from, dot == std::wstring::npos || dot < from ? std::wstring::npos : dot - from);
}

const IniSection* Find(const std::vector<IniSection>& ini, const wchar_t* name) {
    for (const auto& s : ini)
        if (s.name == name) return &s;
    return nullptr;
}

const struct Role {
    const wchar_t* key;
    Rgb Palette::*field;
} kRoles[] = {
    {L"bg", &Palette::bg},         {L"surface", &Palette::surface},   {L"surface2", &Palette::surface2},
    {L"border", &Palette::border}, {L"text", &Palette::text},         {L"sub", &Palette::sub},
    {L"faint", &Palette::faint},   {L"accent", &Palette::accent},     {L"accent2", &Palette::accent2},
    {L"hover", &Palette::hover},   {L"select", &Palette::select},     {L"selecttext", &Palette::selectText},
    {L"playing", &Palette::playing}, {L"error", &Palette::error},     {L"amber", &Palette::amber},
    {L"ok", &Palette::ok},
};

const wchar_t* const kSoundNames[] = {L"rate", L"unrate", L"queue_add", L"play_next", L"queue_clear", L"tags_saved",
                                      L"organized", L"unorganized", L"oops", L"new_files", L"dice", L"drop", L"sparkle",
                                      L"milestone"};
static_assert(std::size(kSoundNames) == (size_t)Sfx::CardinalSong, "every sound effect but the cardinal's song");

const PackEntry* FindFile(const std::vector<PackEntry>& files, const std::wstring& path) {
    for (const auto& f : files)
        if (f.path == path) return &f;
    return nullptr;
}

// A theme's sounds: what its [sounds] section names, else sounds/<name>.wav or .flac when the pack
// has one.
void ReadSounds(Pack& pack, PackTheme& t, const std::wstring& file, const IniSection* named,
                const std::vector<PackEntry>& files) {
    if (named)
        for (const auto& [key, value] : named->values)
            if (std::find(std::begin(kSoundNames), std::end(kSoundNames), key) == std::end(kSoundNames))
                pack.warnings.push_back(file + L": there is no sound effect called " + key);
    for (size_t i = 0; i < std::size(kSoundNames); ++i) {
        const std::wstring* given = named ? named->Get(kSoundNames[i]) : nullptr;
        std::wstring path = given ? Lower(*given) : L"";
        std::replace(path.begin(), path.end(), L'\\', L'/');
        const PackEntry* f = given ? FindFile(files, path) : nullptr;
        if (given && !f) pack.warnings.push_back(file + L": the sound " + *given + L" isn't in the pack");
        if (!given && !(f = FindFile(files, L"sounds/" + std::wstring(kSoundNames[i]) + L".wav")))
            f = FindFile(files, L"sounds/" + std::wstring(kSoundNames[i]) + L".flac");
        if (f) t.sounds.push_back({(Sfx)i, f->bytes});
    }
}

void ReadTheme(Pack& pack, const std::wstring& file, const std::vector<IniSection>& ini, const std::vector<PackEntry>& files) {
    const IniSection* theme = Find(ini, L"theme");
    const IniSection* colors = Find(ini, L"palette");
    if (!theme && !colors) {
        pack.warnings.push_back(file + L" has no [theme] or [palette] section");
        return;
    }
    static const IniSection none;
    if (!theme) theme = &none;
    PackTheme t;
    const std::wstring* name = theme->Get(L"name");
    t.name = name && !name->empty() ? *name : pack.name;
    // Colours the pack leaves out come from the built-in palette it names as its base.
    const std::wstring* base = theme->Get(L"base");
    t.palette = base && Lower(*base) == L"dark" ? DarkPalette() : LightPalette();
    if (const std::wstring* dark = theme->Get(L"dark")) t.palette.dark = ToBool(dark, t.palette.dark);
    if (colors)
        for (const auto& [key, value] : colors->values) {
            std::wstring k = key;
            std::erase(k, L'_');
            const Role* role = std::find_if(std::begin(kRoles), std::end(kRoles), [&](const Role& r) { return k == r.key; });
            Rgb rgb;
            if (role == std::end(kRoles)) pack.warnings.push_back(file + L": no palette colour is called " + key);
            else if (!ParseRgb(value, rgb)) pack.warnings.push_back(file + L": " + key + L" is not a #RRGGBB colour");
            else t.palette.*role->field = rgb;
        }
    if (const std::wstring* font = theme->Get(L"font")) t.font = font->substr(0, LF_FACESIZE - 1);
    t.fontPx = ToInt(theme->Get(L"size"), 0, 0, 48);
    const std::wstring* mark = theme->Get(L"playing_mark");
    t.mark = mark && !mark->empty() ? mark->substr(0, 8) : L"\x25B6";
    if (const std::wstring* prefix = theme->Get(L"message_prefix")) t.prefix = prefix->substr(0, 16);
    if (const std::wstring* rating = theme->Get(L"rating_mark"); rating && !rating->empty())
        t.ratingMark = rating->substr(0, rating->size() >= 2 && IS_HIGH_SURROGATE((*rating)[0]) ? 2 : 1);
    t.legibility = PaletteLegibility(t.palette);
    // Waveform colours are marks on the player's surface: the same 3:1 the accent needs.
    if (const IniSection* wave = Find(ini, L"waveform"))
        for (auto [key, field] : {std::pair{L"played", &PackTheme::wavePlayed}, std::pair{L"unplayed", &PackTheme::waveRest}})
            if (const std::wstring* v = wave->Get(key)) {
                if (!ParseRgb(*v, t.*field)) pack.warnings.push_back(file + L": waveform " + key + L" is not a #RRGGBB colour");
                else if (double c = Contrast(t.*field, t.palette.surface); c < 3 && t.legibility.empty()) {
                    wchar_t text[96];
                    swprintf_s(text, L"waveform %ls on surface is %.1f:1, needs 3:1", key, std::floor(c * 10) / 10);
                    t.legibility = text;
                }
            }
    ReadSounds(pack, t, file, Find(ini, L"sounds"), files);
    pack.themes.push_back(std::move(t));
}

// autoplaylists/*.txt: an optional "name ..." line, then the rules as cordinal saves them.
void ReadAutoPlaylist(Pack& pack, const PackEntry& f) {
    std::string text = f.bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? f.bytes.substr(3) : f.bytes;
    PackAutoPlaylist a{Stem(f.path), {}};
    if (text.compare(0, 5, "name ") == 0) {
        size_t end = text.find('\n');
        a.name = Trim(Utf8(text.substr(5, end == std::string::npos ? std::string::npos : end - 5)));
        text = end == std::string::npos ? "" : text.substr(end + 1);
    }
    AutoPlaylist parsed;
    if (a.name.empty() || !ParseAutoPlaylist(text, parsed)) {
        pack.warnings.push_back(f.path + L" is not an auto-playlist (it starts with cordinal-auto 1)");
        return;
    }
    a.rules = SerializeAutoPlaylist(parsed);  // what this version understood, so nothing unknown reaches the library
    if (pack.autoPlaylists.size() < 64) pack.autoPlaylists.push_back(std::move(a));
}

// Each section is a column, except [set.<name>]: a column set whose `show` lists column names.
void ReadColumns(Pack& pack, const std::vector<IniSection>& ini) {
    for (const auto& s : ini) {
        if (s.name == L"pack") continue;
        if (s.name.starts_with(L"set.")) {
            PackColumnSet set;
            const std::wstring *name = s.Get(L"name"), *show = s.Get(L"show");
            set.name = name && !name->empty() ? *name : s.name.substr(4);
            for (size_t at = 0; show && at <= show->size();) {
                size_t comma = std::min(show->find(L',', at), show->size());
                if (std::wstring item = Trim(show->substr(at, comma - at)); !item.empty()) set.show.push_back(item);
                at = comma + 1;
            }
            if (set.show.empty()) pack.warnings.push_back(L"columns.ini: [" + s.name + L"] shows no columns");
            else if (pack.columnSets.size() < 32) pack.columnSets.push_back(std::move(set));
            continue;
        }
        PackColumn c;
        c.key = s.name;
        const std::wstring* format = s.Get(L"format");
        if (!format || format->empty()) {
            pack.warnings.push_back(L"columns.ini: [" + s.name + L"] has no format");
            continue;
        }
        const std::wstring *title = s.Get(L"title"), *sort = s.Get(L"sort"), *align = s.Get(L"align");
        c.title = title && !title->empty() ? *title : s.name;
        c.display = *format;
        c.sort = sort && !sort->empty() ? *sort : *format;
        c.right = align && Lower(*align) == L"right";
        c.width = ToInt(s.Get(L"width"), 120, 16, 1000);
        if (pack.columns.size() < 64) pack.columns.push_back(std::move(c));
    }
}

void ReadGroups(Pack& pack, const std::vector<IniSection>& ini) {
    const IniSection* s = Find(ini, L"groups");
    if (!s) {
        pack.warnings.push_back(L"groups.ini has no [groups] section");
        return;
    }
    PackGroups& g = pack.groups;
    const struct { const wchar_t* key; std::wstring PackGroups::*field; } texts[] = {
        {L"by", &PackGroups::by}, {L"title", &PackGroups::title}, {L"subtitle", &PackGroups::subtitle},
        {L"right", &PackGroups::right}};
    for (const auto& t : texts)
        if (const std::wstring* v = s->Get(t.key)) g.*t.field = *v;
    g.cover = ToBool(s->Get(L"cover"), true);
    g.any = !s->values.empty();  // a [groups] with every line commented out changes nothing
}

void ReadFormats(Pack& pack, const std::vector<IniSection>& ini) {
    const IniSection* s = Find(ini, L"formats");
    const wchar_t* keys[kPackFormatCount] = {L"window_title", L"status_bar", L"now_playing", L"now_playing_2", L"tray_tip"};
    for (int i = 0; s && i < kPackFormatCount; ++i)
        if (const std::wstring* v = s->Get(keys[i])) pack.formats[i] = *v;
    if (!s) pack.warnings.push_back(L"formats.ini has no [formats] section");
}

void ReadPresets(Pack& pack, const std::wstring& file, const std::vector<IniSection>& ini) {
    for (const auto& s : ini) {
        const std::wstring* bands = s.Get(L"bands");
        if (!bands) continue;
        PackPreset p;
        const std::wstring* name = s.Get(L"name");
        p.name = name && !name->empty() ? *name : s.name;
        const wchar_t* c = bands->c_str();
        for (float& gain : p.gains) {
            wchar_t* end = nullptr;
            double v = wcstod(c, &end);
            if (end == c) break;
            gain = (float)std::clamp(v == v ? v : 0.0, -12.0, 12.0);
            c = *end == L',' ? end + 1 : end;
        }
        if (pack.presets.size() < 32) pack.presets.push_back(std::move(p));
    }
    if (pack.presets.empty()) pack.warnings.push_back(file + L" has no preset with bands");
}

}  // namespace

const std::wstring* IniSection::Get(const wchar_t* key) const {
    for (const auto& [k, v] : values)
        if (k == key) return &v;
    return nullptr;
}

std::wstring DecodePackText(const std::string& bytes) {
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xFE)
        return std::wstring((const wchar_t*)(bytes.data() + 2), (bytes.size() - 2) / 2);
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) return Utf8(bytes.substr(3));
    return Utf8(bytes);
}

std::vector<IniSection> ParseIni(const std::string& bytes, bool unquote) {
    std::vector<IniSection> out;
    std::wstring text = DecodePackText(bytes);
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find(L'\n', at);
        std::wstring line = Trim(text.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at));
        at = end == std::wstring::npos ? text.size() : end + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[' && line.back() == L']') {
            out.push_back({Lower(Trim(line.substr(1, line.size() - 2))), {}});
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (out.empty()) out.push_back({});
        std::wstring key = Lower(Trim(line.substr(0, eq))), value = Trim(line.substr(eq + 1));
        std::replace(key.begin(), key.end(), L' ', L'_');
        if (unquote && value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
        out.back().values.emplace_back(std::move(key), std::move(value));
    }
    return out;
}

bool ParseRgb(const std::wstring& text, Rgb& out) {
    std::wstring s = Trim(text);
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6 || !std::all_of(s.begin(), s.end(), [](wchar_t c) { return iswxdigit(c) != 0; })) return false;
    out = (Rgb)wcstoul(s.c_str(), nullptr, 16);
    return true;
}

// The end-of-central-directory record locates the central directory, whose entries point at each
// file's local header; the data follows that header. Stored and deflated entries are read; ZIP64
// and other methods are refused.
bool ReadPackZip(const std::string& zip, std::vector<PackEntry>& out, std::wstring& error) {
    error = L"it is not a ZIP file";
    if (zip.size() < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t at = zip.size() - 22, stop = zip.size() > 65557 ? zip.size() - 65557 : 0;; --at) {
        if (Le32(zip, at) == 0x06054B50) { eocd = at; break; }
        if (at == stop) break;
    }
    if (eocd == std::string::npos) return false;
    error = L"its file list is damaged";
    size_t count = Le16(zip, eocd + 10), dir = Le32(zip, eocd + 16);
    if (count > 1024) return false;
    size_t total = 0;
    for (size_t i = 0, at = dir; i < count; ++i) {
        if (at + 46 > zip.size() || Le32(zip, at) != 0x02014B50) return false;
        uint32_t method = Le16(zip, at + 10), crc = Le32(zip, at + 16), packed = Le32(zip, at + 20), size = Le32(zip, at + 24);
        size_t nameLen = Le16(zip, at + 28), next = at + 46 + nameLen + Le16(zip, at + 30) + Le16(zip, at + 32);
        size_t local = Le32(zip, at + 42);
        if (at + 46 + nameLen > zip.size()) return false;
        std::wstring name = Lower(Utf8(zip.substr(at + 46, nameLen)));
        std::replace(name.begin(), name.end(), L'\\', L'/');
        at = next;
        if (name.empty() || name.back() == L'/') continue;
        if ((method != 0 && method != 8) || (method == 0 && packed != size)) {
            error = name + L" is compressed in a way packs can't read (store or deflate it)";
            return false;
        }
        if (local + 30 > zip.size() || Le32(zip, local) != 0x04034B50) return false;
        size_t data = local + 30 + Le16(zip, local + 26) + Le16(zip, local + 28);
        if (data > zip.size() || packed > zip.size() - data || (total += size) > (64u << 20)) return false;
        std::string bytes;
        if (method == 0) bytes = zip.substr(data, size);
        else if (!Inflate((const unsigned char*)zip.data() + data, packed, size, bytes) || bytes.size() != size) {
            error = name + L" can't be unpacked";
            return false;
        }
        if (Crc32((const unsigned char*)bytes.data(), bytes.size()) != crc) {
            error = name + L" fails its checksum";
            return false;
        }
        out.push_back({std::move(name), std::move(bytes)});
    }
    error.clear();
    return true;
}

bool Pack::Empty() const {
    return themes.empty() && columns.empty() && columnSets.empty() && !groups.any && keys.empty() && globalKeys.empty() &&
           fields.empty() && presets.empty() && layouts.empty() && autoPlaylists.empty() && fonts.empty() && functions.empty() && std::all_of(std::begin(formats), std::end(formats), [](auto& f) { return f.empty(); });
}

Pack ParsePack(const std::wstring& id, const std::vector<PackEntry>& files) {
    Pack pack;
    pack.id = pack.name = id;
    auto file = [&](const wchar_t* path) -> const PackEntry* {
        for (const auto& f : files)
            if (f.path == path) return &f;
        return nullptr;
    };
    // Every ini file may say which pack format it was written for; a later one is refused whole.
    auto read = [&](const PackEntry& f, std::vector<IniSection>& ini) {
        ini = ParseIni(f.bytes);
        const IniSection* head = Find(ini, L"pack");
        int format = ToInt(head ? head->Get(L"format") : nullptr, 1, 0, 1 << 20);
        if (format <= kPackFormat) return true;
        pack.warnings.push_back(f.path + L" needs a later cordinal (pack format " + std::to_wstring(format) + L")");
        return false;
    };
    std::vector<IniSection> ini;
    if (const PackEntry* f = file(L"pack.ini"); f && !read(*f, ini)) {
        pack.error = L"it needs a later version of cordinal";
        return pack;
    } else if (f)
        if (const IniSection* head = Find(ini, L"pack")) {
            const struct { const wchar_t* key; std::wstring Pack::*field; } fields[] = {
                {L"name", &Pack::name}, {L"author", &Pack::author}, {L"version", &Pack::version},
                {L"description", &Pack::description}};
            for (const auto& fd : fields)
                if (const std::wstring* v = head->Get(fd.key); v && !v->empty()) pack.*fd.field = *v;
        }
    for (const auto& f : files) {
        const std::wstring& p = f.path;
        bool isIni = EndsWith(p, L".ini");
        if (p == L"pack.ini") continue;
        if (isIni && !read(f, ini)) continue;
        if (p == L"theme.ini" || (p.starts_with(L"themes/") && isIni)) ReadTheme(pack, p, ini, files);
        else if (p == L"columns.ini") ReadColumns(pack, ini);
        else if (p == L"groups.ini") ReadGroups(pack, ini);
        else if (p == L"formats.ini") ReadFormats(pack, ini);
        else if (p == L"eq.ini" || (p.starts_with(L"eq/") && isIni)) ReadPresets(pack, p, ini);
        else if (p == L"keys.ini") {
            const IniSection *s = Find(ini, L"keys"), *global = Find(ini, L"global keys");
            if (s)
                for (const auto& [k, v] : s->values) pack.keys.push_back({k, v});
            if (global)
                for (const auto& [k, v] : global->values) pack.globalKeys.push_back({k, v});
            if (!s && !global) pack.warnings.push_back(L"keys.ini has no [keys] or [global keys] section");
        } else if (p == L"fields.ini") {
            if (const IniSection* s = Find(ini, L"fields")) {
                for (const auto& [k, v] : s->values)
                    if (!k.empty() && !v.empty() && pack.fields.size() < 128) pack.fields.push_back({k, v});
            } else pack.warnings.push_back(L"fields.ini has no [fields] section");
        } else if (p == L"layout.ini" || (p.starts_with(L"layouts/") && isIni)) {
            if (const IniSection* s = Find(ini, L"layout")) {
                const std::wstring* name = s->Get(L"name");
                pack.layouts.push_back({name && !name->empty() ? *name : Stem(p), *s});
            } else pack.warnings.push_back(p + L" has no [layout] section");
        } else if (p.starts_with(L"autoplaylists/") && EndsWith(p, L".txt")) {
            ReadAutoPlaylist(pack, f);
        } else if (p == L"functions.tf") {
            pack.functions = DecodePackText(f.bytes);
        } else if (p.starts_with(L"fonts/") && (EndsWith(p, L".ttf") || EndsWith(p, L".otf"))) {
            if (f.bytes.size() <= (8u << 20)) pack.fonts.push_back({p, f.bytes});
            else pack.warnings.push_back(p + L" is larger than 8 MB");
        } else if (isIni) {
            pack.warnings.push_back(p + L" is not a file packs use");
        }
    }
    if (pack.Empty() && pack.error.empty()) pack.error = L"it holds nothing cordinal can use";
    return pack;
}

const wchar_t* PackSoundName(Sfx sound) { return (size_t)sound < std::size(kSoundNames) ? kSoundNames[(int)sound] : nullptr; }

// RIFF/WAVE with a "fmt " chunk of PCM (8, 16, 24 or 32 bits) or 32-bit float, mono or stereo, at
// most ten seconds: a sound effect, not a song.
bool DecodePackWav(const std::string& b, unsigned& rate, std::vector<float>& stereo) {
    if (b.size() < 12 || b.compare(0, 4, "RIFF") != 0 || b.compare(8, 4, "WAVE") != 0) return false;
    uint32_t format = 0, channels = 0, bits = 0;
    rate = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        uint32_t size = Le32(b, at + 4);
        size_t body = at + 8, n = std::min<size_t>(size, b.size() - body);
        if (b.compare(at, 4, "fmt ") == 0 && n >= 16) {
            format = Le16(b, body), channels = Le16(b, body + 2), rate = Le32(b, body + 4), bits = Le16(b, body + 14);
            if (format == 0xFFFE && n >= 26) format = Le16(b, body + 24);  // WAVE_FORMAT_EXTENSIBLE: its sub-format
        } else if (b.compare(at, 4, "data") == 0) {
            bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32), flt = format == 3 && bits == 32;
            if ((!pcm && !flt) || channels < 1 || channels > 2 || rate < 8000 || rate > 192000) return false;
            size_t bytes = bits / 8, frames = std::min<size_t>(n / (bytes * channels), (size_t)rate * 10);
            stereo.resize(frames * 2);
            for (size_t f = 0; f < frames; ++f)
                for (uint32_t c = 0; c < 2; ++c) {
                    size_t s = body + (f * channels + std::min(c, channels - 1)) * bytes;
                    float v;
                    if (flt) {
                        uint32_t u = Le32(b, s);
                        memcpy(&v, &u, 4);
                    } else if (bits == 8) {
                        v = ((uint8_t)b[s] - 128) / 128.0f;
                    } else {
                        int32_t x = 0;
                        for (size_t k = 0; k < bytes; ++k) x |= (int32_t)(uint8_t)b[s + k] << (8 * k + 32 - bits);
                        v = x / 2147483648.0f;
                    }
                    stereo[f * 2 + c] = v == v ? std::clamp(v, -1.0f, 1.0f) : 0;
                }
            return frames > 0;
        }
        at = body + size + (size & 1);
        if (size > b.size()) break;
    }
    return false;
}

std::wstring PackSummary(const Pack& p) {
    std::vector<std::wstring> parts;
    auto count = [&](size_t n, const wchar_t* one, const wchar_t* many) {
        if (n) parts.push_back(n == 1 ? one : std::to_wstring(n) + L" " + many);
    };
    size_t sounds = 0;
    for (const auto& t : p.themes) sounds += t.sounds.size();
    count(p.themes.size(), L"a theme", L"themes");
    count(sounds, L"a sound", L"sounds");
    count(p.fonts.size(), L"a font", L"fonts");
    count(p.columns.size(), L"a column", L"columns");
    count(p.columnSets.size(), L"a column set", L"column sets");
    count(p.fields.size(), L"a field", L"fields");
    if (p.groups.any) parts.push_back(L"group headers");
    const wchar_t* formats[kPackFormatCount] = {L"the window title", L"the status bar", L"the now playing line",
                                                L"the now playing second line", L"the tray tip"};
    for (int i = 0; i < kPackFormatCount; ++i)
        if (!p.formats[i].empty()) parts.push_back(formats[i]);
    count(p.keys.size(), L"a key", L"keys");
    count(p.globalKeys.size(), L"a global key", L"global keys");
    count(p.presets.size(), L"an EQ preset", L"EQ presets");
    count(p.layouts.size(), L"a layout", L"layouts");
    count(p.autoPlaylists.size(), L"an auto-playlist", L"auto-playlists");
    if (!p.functions.empty()) parts.push_back(L"title-format functions");
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i)
        out += (i == 0 ? L"" : i + 1 == parts.size() ? L" and " : L", ") + parts[i];
    return out.empty() ? L"nothing cordinal can use" : out;
}