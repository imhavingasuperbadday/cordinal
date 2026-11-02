// SDL counterparts of the private foobar2000 and Apollo windows and waveform appearance.
// Theme columns and filters stay separate from the normal view; playback uses the shared engine.
#include "alternate_ui.h"
#include "ui_internal.h"
#include "apollo_images.inc"
#include <charconv>
#include <set>
#include <fstream>
#include <condition_variable>
#include <cstring>

namespace lx {
namespace {
constexpr int kAltAction = 11000;
enum { kStyle, kMotion, kGameColors, kGlow, kBlack, kColor, kSkin, kOpacity, kAnalyzer, kScope };
WaveAppearance appearance;
struct Columns {
    std::vector<int> types;
    std::vector<int> widths = std::vector<int>(ui::kColumnCount);
    std::string pending;
    std::vector<std::string> catalog;
};
Columns fbColumns{{0, ui::kColCover, ui::kColTrackNo, ui::kColTitleArtist}};
Columns apColumns{{0, ui::kColApolloTitle, 6, ui::kColApolloBitrate, ui::kColApolloRate,
                   ui::kColApolloBits, ui::kColApolloSize, ui::kColApolloTag}};
using Node = AlbumListNode;
struct TreeRow { Node* node; int depth; };
struct Alternate {
    Rect side, sideTabs[2], view, filter, tree, statusBox, labels[2], tools[15];
    int panel = 0, viewMode = 0, focus = -1, selected = -1;
    float scroll = 0;
    std::wstring filterText, builtFilter;
    bool filterFocused = false;
    std::vector<Node> nodes;
    std::vector<TreeRow> rows;
    std::set<std::wstring> expanded;
    std::vector<TrackId> selection;
    uint32_t revision = UINT32_MAX;
    int builtView = -1;
    bool analyzer = true, scope = true;
    int skin = 0, opacity = 40;
    SDL_Texture* skinTexture = nullptr;
    SDL_Texture* toolbarTexture = nullptr;
    int skinFor = -1;
    float toolbarW = 0, toolbarH = 0;
} a;
struct ProbeJob {
    TrackId id;
    std::wstring path;
    Codec codec;
    uint64_t time, size;
};
struct ProbeResult { ProbeJob job; ApolloFileInfo info; };
struct CachedProbe {
    std::wstring path;
    uint64_t time = 0, size = 0;
    bool pending = false, ready = false;
    ApolloFileInfo info;
};
std::unordered_map<TrackId, CachedProbe> probes;
std::mutex probeMutex;
std::condition_variable probeReady;
std::deque<ProbeJob> probeJobs;
std::vector<ProbeResult> probeResults;
std::thread probeWorker;
bool probeQuit = false;
void StartProbeWorker() {
    if (probeWorker.joinable()) return;
    probeQuit = false;
    probeWorker = std::thread([] {
        for (;;) {
            ProbeJob job;
            {
                std::unique_lock lock(probeMutex);
                probeReady.wait(lock, [] { return probeQuit || !probeJobs.empty(); });
                if (probeQuit) return;
                job = std::move(probeJobs.front()); probeJobs.pop_front();
            }
            auto info = ProbeApolloFile(job.path, job.codec);
            std::lock_guard lock(probeMutex);
            probeResults.push_back({std::move(job), info});
        }
    });
}
struct ColorDialog {
    bool open = false;
    int dock = 0;
    bool rest = false, selectAll = true;
    std::string text;
    Rect panel, field, apply, reset, cancel;
    int focus = 0;
} color;
int Mode() {
    if (!kExtraThemes || g.settings.size != ViewSize::Normal || InternetRadio().active) return 0;
    return g.settings.theme == ThemeMode::Foobar ? 1 : g.settings.theme == ThemeMode::Apollo ? 2 : 0;
}
std::wstring Lower(std::wstring text) {
    for (auto& ch : text) ch = (wchar_t)towlower(ch);
    return text;
}
int Integer(const std::map<std::string, std::string>& values, const char* key, int def, int low, int high) {
    auto it = values.find(key);
    if (it == values.end()) return def;
    int value = def;
    auto parsed = std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == it->second.data() + it->second.size() ? std::clamp(value, low, high) : def;
}
std::string Hex(Rgb value) {
    if (value == kNoRgb) return "";
    char text[7]; snprintf(text, sizeof(text), "%06X", value & 0xffffff);
    return text;
}
Rgb ParseColor(const std::string& text) {
    Rgb out;
    return text.size() == 6 && ParseRgb(FromUtf8(text), out) ? out : kNoRgb;
}
void ResolveColumns(Columns& c) {
    if (!c.pending.empty()) {
        std::vector<int> types{0};
        c.widths.assign(ColumnTotal(), 0);
        for (size_t at = 0; at < c.pending.size();) {
            size_t end = std::min(c.pending.find(',', at), c.pending.size());
            std::string entry = c.pending.substr(at, end - at);
            size_t colon = entry.find(':');
            std::string token = entry.substr(0, colon);
            int type = token.starts_with("p") ? PackColumnType(token) : atoi(token.c_str());
            if (type >= 0 && type < ColumnTotal()) {
                if (type && std::find(types.begin(), types.end(), type) == types.end()) types.push_back(type);
                if (colon != std::string::npos) c.widths[type] = std::clamp(atoi(entry.c_str() + colon + 1), 0, 2000);
            }
            at = end + 1;
        }
        if (types.size() > 1) c.types = std::move(types);
        c.pending.clear();
    }
    c.widths.resize(ColumnTotal());
}
std::string ColumnText(Columns& c) {
    if (!c.pending.empty()) return c.pending;
    ResolveColumns(c);
    std::string text;
    for (int type : c.types) {
        if (type < 0 || type >= ColumnTotal()) continue;
        std::string key = type < ui::kColumnCount ? std::to_string(type) : PackColumnKey(type);
        if (key.empty()) continue;
        text += (text.empty() ? "" : ",") + key + ":" + std::to_string(c.widths[type]);
    }
    return text;
}
void ListGeometry(Rect list) {
    L.list = list;
    L.header = {list.x, list.y, list.w, S(24)};
    L.rowH = S(ui::kRow);
    L.rows = {list.x, L.header.B(), list.w, std::max(0.f, list.B() - L.header.B())};
    bool scroll = g_lines.size() * L.rowH > L.rows.h;
    float sb = S(17);
    if (scroll) L.rows.w = std::max(0.f, L.rows.w - sb);
    L.scrollbar = {list.R() - sb, L.rows.y, scroll ? sb : 0, L.rows.h};
}
void ClearSpecialGeometry() {
    L.cover = L.title = L.subtitle = L.chip = L.lyrics = L.upNext = {};
    L.grip = L.elapsed = L.remaining = L.speaker = L.browser = L.splitter = {};
    L.viewRow = {};
    for (auto& b : L.buttons) b = {};
    for (auto& v : L.views) v = {};
}
void TreeRows(Node& node, int depth) {
    a.rows.push_back({&node, depth});
    if (a.expanded.contains(node.key)) for (auto& child : node.children) TreeRows(child, depth + 1);
}
void RebuildTreeRows() {
    a.rows.clear();
    for (auto& node : a.nodes) TreeRows(node, 0);
    a.selected = std::clamp(a.selected, -1, (int)a.rows.size() - 1);
}
void BuildTree() {
    if (a.revision == g_lib.Revision() && a.builtFilter == a.filterText && a.builtView == a.viewMode) return;
    a.revision = g_lib.Revision(); a.builtFilter = a.filterText; a.builtView = a.viewMode;
    a.nodes = BuildAlbumList(g_lib, a.viewMode, a.filterText);
    a.scroll = 0; RebuildTreeRows();
}
void PickTree(int row, bool play) {
    if (row < 0 || row >= (int)a.rows.size()) return;
    a.selected = row; a.selection = a.rows[row].node->tracks;
    if (play) { PlayTracks(a.selection, 0); return; }
    for (size_t i = 0; i < g.playlists.size(); ++i)
        if (g.playlists[i].kind == PlaylistKind::Library) { SetActiveTab((int)i); break; }
    RebuildRows();
}
void SelectTreeRow(int row) {
    PickTree(std::clamp(row, 0, std::max(0, (int)a.rows.size() - 1)), false);
    float top = a.selected * S(ui::kRow);
    if (top < a.scroll) a.scroll = top;
    if (top + S(ui::kRow) > a.scroll + a.tree.h) a.scroll = top + S(ui::kRow) - a.tree.h;
}
void DrawTree(const Palette& p) {
    BuildTree();
    Fill(a.side, p.surface); Frame(a.side, p.border);
    const char* tabs[] = {"Album List", "Selection Properties"};
    for (int i = 0; i < 2; ++i) {
        DrawText(Font::Normal, tabs[i], a.sideTabs[i].x + S(6), TextY(a.sideTabs[i]), p.text, a.sideTabs[i].w - S(12));
        if (a.panel == i) Fill({a.sideTabs[i].x, a.sideTabs[i].B() - S(2), a.sideTabs[i].w, S(2)}, p.accent);
    }
    if (a.panel == 1) {
        auto ids = SelectedTracks();
        float y = a.sideTabs[0].B() + S(12);
        if (ids.empty()) { DrawText(Font::Normal, "No items selected.", a.side.x + S(10), y, p.sub, a.side.w - S(20)); return; }
        const wchar_t* names[] = {L"title", L"artist", L"album", L"date", L"genre", L"codec", L"bitrate", L"samplerate", L"path"};
        for (const auto* name : names) {
            std::wstring value; LookupField(ids[0], name, value);
            for (TrackId id : ids) {
                std::wstring other; LookupField(id, name, other);
                if (other != value) { value = L"(multiple values)"; break; }
            }
            DrawText(Font::Bold, Utf8(name), a.side.x + S(10), y, p.sub, a.side.w - S(20)); y += S(17);
            DrawText(Font::Normal, Utf8(value), a.side.x + S(10), y, p.text, a.side.w - S(20)); y += S(25);
            if (y + S(40) > a.side.B()) break;
        }
        return;
    }
    const char* views[] = {"by folder structure", "by artist/album", "by album", "by genre"};
    DrawButton(a.view, views[a.viewMode], p);
    Fill(a.filter, p.bg); Frame(a.filter, a.filterFocused ? p.accent : p.border);
    DrawText(Font::Normal, a.filterText.empty() ? "Filter" : Utf8(a.filterText), a.filter.x + S(6), TextY(a.filter),
        a.filterText.empty() ? p.faint : p.text, a.filter.w - S(12));
    Fill(a.tree, p.bg);
    SDL_Rect clip{(int)a.tree.x, (int)a.tree.y, (int)a.tree.w, (int)a.tree.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    int first = std::max(0, (int)(a.scroll / S(ui::kRow)));
    int last = std::min((int)a.rows.size(), first + (int)(a.tree.h / S(ui::kRow)) + 2);
    for (int row = first; row < last; ++row) {
        auto [node, depth] = a.rows[row];
        Rect r{a.tree.x, a.tree.y + row * S(ui::kRow) - a.scroll, a.tree.w, S(ui::kRow)};
        if (row == a.selected) Fill(r, p.select);
        float x = r.x + S(6 + 14 * depth);
        if (!node->children.empty()) { DrawText(Font::Normal, a.expanded.contains(node->key) ? "-" : "+", x, TextY(r), p.sub, S(12)); x += S(14); }
        std::string text = Utf8(node->label) + (node->tracks.size() > 1 ? " (" + std::to_string(node->tracks.size()) + ")" : "");
        DrawText(Font::Normal, text, x, TextY(r), row == a.selected ? p.selectText : p.text, r.R() - x - S(6));
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}
void Texture(SDL_Texture*& target, std::string_view bytes, float* w = nullptr, float* h = nullptr) {
    CoverImage image{};
    if (!DecodeCover(std::string(bytes), 2048, image)) return;
    target = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, image.width, image.height);
    if (!target) return;
    SDL_UpdateTexture(target, nullptr, image.rgba.data(), image.width * 4);
    SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(target, SDL_SCALEMODE_NEAREST);
    if (w) *w = image.width;
    if (h) *h = image.height;
}
void Sunken(const Rect& r, const Palette& p) {
    Fill(r, p.surface);
    Fill({r.x, r.y, r.w, S(1)}, p.border); Fill({r.x, r.y, S(1), r.h}, p.border);
    Fill({r.x, r.B() - S(1), r.w, S(1)}, p.bg); Fill({r.R() - S(1), r.y, S(1), r.h}, p.bg);
}
void ToolbarAction(int tool, float x, float y) {
    switch (tool) {
        case 0: ChooseFiles(); break;
        case 1: OpenPopup(x, y, BarMenu(3)); break;
        case 2: {
            std::vector<MenuItem> menu;
            for (int m : BarMenus()) menu.push_back(Submenu(Plain(ui::kMenuBar[m]), BarMenu(m)));
            OpenPopup(x, y, std::move(menu)); break;
        }
        case 3: Previous(); break;
        case 4: HoldPendingOpens(); Stop(); break;
        case 5: if (!IsPaused()) TogglePause(); break;
        case 6: if (g.playing == kNone || IsPaused()) TogglePause(); break;
        case 7: Next(); break;
        case 8: CycleRepeat(); break;
        case 9: ToggleShuffle(); break;
        case 10: a.analyzer = !a.analyzer; SaveSettings(); break;
        case 11: a.scope = !a.scope; SaveSettings(); break;
        case 12: OpenEqualizer(); break;
        case 13: ToggleMini(); break;
        case 14: SDL_HideWindow(g.window); break;
    }
    g.redraw = true;
}
void DrawApollo(const Palette& p) {
    Fill(L.player, p.surface);
    if (a.skinFor != a.skin) {
        if (a.skinTexture) SDL_DestroyTexture(a.skinTexture);
        a.skinTexture = nullptr; a.skinFor = a.skin;
        const std::string_view skins[] = {
            {(const char*)kApollo_rebar_capacala, sizeof(kApollo_rebar_capacala)},
            {(const char*)kApollo_rebar_apollo, sizeof(kApollo_rebar_apollo)},
            {(const char*)kApollo_rebar_letters, sizeof(kApollo_rebar_letters)},
            {(const char*)kApollo_rebar_notes, sizeof(kApollo_rebar_notes)}
        };
        if (a.skin < 4) Texture(a.skinTexture, skins[a.skin]);
    }
    if (a.skinTexture) {
        SDL_SetTextureAlphaMod(a.skinTexture, (uint8_t)(a.opacity * 255 / 100));
        SDL_FRect dest{L.player.x, L.player.y, L.player.w, L.player.h};
        SDL_RenderTexture(g.renderer, a.skinTexture, nullptr, &dest);
    }
    if (!a.toolbarTexture) Texture(a.toolbarTexture, {(const char*)kApollo_toolbar, sizeof(kApollo_toolbar)}, &a.toolbarW, &a.toolbarH);
    const int glyphs[] = {0, 11, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 13, 12, 14};
    const char* labels[] = {"Open", "Lists", "Options", "Previous", "Stop", "Pause", "Play", "Next", "Loop", "Shuffle", "Analyzer", "Scope", "EQ", "Mini", "Tray"};
    for (int i = 0; i < 15; ++i) {
        const Rect& r = a.tools[i];
        if (r.In(g_mouseX, g_mouseY) || a.focus == i) { Fill(r, p.hover); Frame(r, p.border); }
        float glyphW = a.toolbarW / 15;
        if (a.toolbarTexture && glyphW > 0) {
            SDL_FRect src{glyphs[i] * glyphW, 0, glyphW, a.toolbarH}, dst{r.x + (r.w - S(16)) / 2, r.y + S(3), S(16), S(16)};
            SDL_RenderTexture(g.renderer, a.toolbarTexture, &src, &dst);
        } else DrawText(Font::Small, labels[i], r.x, TextY(r, Font::Small), p.text, r.w, 1);
    }
    for (int i = 0; i < 2; ++i) DrawText(Font::Normal, i ? "Volume" : "Position", a.labels[i].x, TextY(a.labels[i]), p.text, a.labels[i].w);
    double duration = TrackDuration(), position = g.playing == kNone ? 0 : g.engine.Position();
    DrawSlider(L.seek, duration > 0 ? position / duration : 0, duration > 0, L.seek.In(g_mouseX, g_mouseY), p);
    DrawSlider(L.volume, g.settings.volume, true, L.volume.In(g_mouseX, g_mouseY), p);
    Sunken(a.statusBox, p);
    std::string status = g.playing == kNone ? "Playback stopped." : std::to_string(g.queuePos + 1) + ". " +
        Utf8(FormatForTrack(L"[%artist% - ]%title%", g.playing, false)) + " " + Utf8(FormatDuration(position)) + " (" + Utf8(FormatDuration(duration)) + ")";
    DrawText(Font::Normal, status, a.statusBox.x + S(4), TextY(a.statusBox), p.text, a.statusBox.w - S(8));
}
void CloseColor() { color.open = false; SDL_StopTextInput(g.window); g.redraw = true; }
void ApplyColor(bool reset) {
    Rgb value = reset ? kNoRgb : ParseColor(color.text);
    if (!reset && value == kNoRgb) { ShowMessage(L"Enter six hexadecimal digits, such as 80B8E8."); return; }
    (color.rest ? appearance.rest : appearance.played)[color.dock] = value;
    SaveSettings(); CloseColor();
}
}  // namespace

ApolloFileInfo ProbeApolloFile(const std::wstring& path, Codec codec) {
    ApolloFileInfo info;
    switch (codec) {
        case Codec::MP3: case Codec::MP2: case Codec::AAC: case Codec::WMA:
        case Codec::Vorbis: case Codec::Opus: case Codec::AC3: info.bits = 16; break;
        default: break;
    }
    std::ifstream stream(ToUtf8(path), std::ios::binary);
    if (!stream) return info;
    stream.seekg(0, std::ios::end);
    auto length = stream.tellg();
    if (length < 0) return info;
    info.size = (uint64_t)length;
    auto read = [&](uint64_t at, uint8_t* bytes, size_t size) {
        if (at > info.size || size > info.size - at) return size_t(0);
        stream.clear(); stream.seekg((std::streamoff)at);
        stream.read((char*)bytes, size);
        return (size_t)stream.gcount();
    };
    uint8_t head[64]{};
    size_t count = read(0, head, std::min<uint64_t>(sizeof(head), info.size));
    if (count >= 10 && !memcmp(head, "ID3", 3) && head[3] >= 2 && head[3] <= 4) {
        info.id3 = head[3]; info.id3rev = head[4];
        uint64_t audio = 10 + ((head[6] & 0x7f) << 21 | (head[7] & 0x7f) << 14 | (head[8] & 0x7f) << 7 | (head[9] & 0x7f));
        count = read(audio, head, audio <= info.size ? std::min<uint64_t>(sizeof(head), info.size - audio) : 0);
    }
    if (codec == Codec::FLAC && count >= 22 && !memcmp(head, "fLaC", 4))
        info.bits = (uint8_t)((((head[20] & 1) << 4) | (head[21] >> 4)) + 1);
    else if (codec == Codec::PCM && count >= 36 && !memcmp(head, "RIFF", 4) &&
             !memcmp(head + 8, "WAVE", 4) && !memcmp(head + 12, "fmt ", 4)) info.bits = head[34];
    uint8_t tail[128]{};
    if (info.size >= sizeof(tail) && read(info.size - sizeof(tail), tail, sizeof(tail)) == sizeof(tail) &&
        !memcmp(tail, "TAG", 3)) info.id3v1 = tail[125] == 0 && tail[126] != 0 ? 11 : 10;
    return info;
}
void TickAlternate() {
    std::vector<ProbeResult> results;
    { std::lock_guard lock(probeMutex); results.swap(probeResults); }
    bool changed = false;
    for (auto& result : results) {
        auto it = probes.find(result.job.id);
        if (it == probes.end()) continue;
        auto& cached = it->second;
        if (cached.path != result.job.path || cached.time != result.job.time || cached.size != result.job.size) continue;
        cached.info = result.info; cached.ready = true; cached.pending = false; changed = true;
    }
    if (changed) {
        InvalidateCellCache();
        if (!g.playlists.empty() && Active().sortColumn >= ui::kColApolloBits &&
            Active().sortColumn <= ui::kColApolloTag) RebuildRows();
        g.redraw = true;
    }
}
void StopAlternate() {
    { std::lock_guard lock(probeMutex); probeQuit = true; probeJobs.clear(); }
    probeReady.notify_all();
    if (probeWorker.joinable()) probeWorker.join();
    { std::lock_guard lock(probeMutex); probeResults.clear(); }
    probes.clear();
}
bool AlternateField(TrackId id, const std::wstring& name, std::wstring& out) {
    if (!g_lib.Contains(id) || (name != L"apollo_size" && name != L"apollo_bits" && name != L"apollo_tag")) return false;
    const auto& track = g_lib[id];
    if (name == L"apollo_size" && track.fileSize) {
        uint64_t size = track.fileSize;
        out = size < (10ull << 20) ? std::to_wstring((size + 512) >> 10) + L" KB" :
            std::to_wstring((size + (1 << 19)) >> 20) + L" MB";
        return true;
    }
    std::wstring path = g_lib.Path(id);
    if (size_t bar = path.find(L'|'); bar != std::wstring::npos) path.resize(bar);
    if (probes.size() > 20000) probes.clear();
    auto& cached = probes[id];
    if (cached.path != path || cached.time != track.fileTime || cached.size != track.fileSize)
        cached = CachedProbe{path, track.fileTime, track.fileSize};
    if (!cached.ready && !cached.pending) {
        StartProbeWorker();
        std::lock_guard lock(probeMutex);
        if (probeJobs.size() < 512) {
            cached.pending = true;
            probeJobs.push_back({id, path, track.codec, track.fileTime, track.fileSize});
            probeReady.notify_one();
        }
    }
    if (name == L"apollo_size") {
        uint64_t size = track.fileSize ? track.fileSize : cached.info.size;
        if (!size) return false;
        out = size < (10ull << 20) ? std::to_wstring((size + 512) >> 10) + L" KB" :
            std::to_wstring((size + (1 << 19)) >> 20) + L" MB";
    } else if (name == L"apollo_bits") {
        int bits = cached.info.bits;
        if (!bits) switch (track.codec) {
            case Codec::MP3: case Codec::MP2: case Codec::AAC: case Codec::WMA:
            case Codec::Vorbis: case Codec::Opus: case Codec::AC3: bits = 16; break;
            default: break;
        }
        std::wstring channels = track.channels == 1 ? L"mono" : track.channels == 2 ? L"stereo" :
            track.channels ? std::to_wstring(track.channels) + L" channels" : L"";
        out = bits ? std::to_wstring(bits) + L" bit" + (channels.empty() ? L"" : L" " + channels) : channels;
    } else {
        auto& info = cached.info;
        if (info.id3) out = L"v2." + std::to_wstring(info.id3) + L"." + std::to_wstring(info.id3rev);
        else if (info.id3v1) out = L"v1." + std::to_wstring(info.id3v1 - 10);
        else return false;
    }
    return !out.empty();
}

std::vector<AlbumListNode> BuildAlbumList(const Library& library, int view, const std::wstring& text) {
    // Build by stable indices so adding a group never invalidates another group's children.
    struct Builder { AlbumListNode node; std::vector<size_t> children; };
    std::vector<Builder> nodes(1);
    std::unordered_map<std::wstring, size_t> groups;
    const auto filter = Lower(text);
    auto group = [&](size_t parent, const std::wstring& name) {
        std::wstring key = std::to_wstring(parent) + L"|" + name;
        auto [it, fresh] = groups.emplace(key, nodes.size());
        if (fresh) {
            size_t index = nodes.size();
            nodes.push_back({{name.empty() ? L"(unknown)" : name, key, {}, {}}, {}});
            nodes[parent].children.push_back(index);
        }
        return it->second;
    };
    for (TrackId id = 0; id < library.Count(); ++id) {
        const auto& track = library[id];
        if (!track.InLibrary()) continue;
        std::wstring title = library.Str(track.title), artist = library.Str(track.albumArtist ? track.albumArtist : track.artist);
        const std::wstring album = library.Str(track.album), dir = library.Str(track.dir);
        if (title.empty()) title = library.Str(track.file);
        if (!filter.empty() && Lower(title + L" " + artist + L" " + album + L" " + library.Path(id)).find(filter) == std::wstring::npos) continue;
        size_t parent = 0;
        if (view == 0) {
            int depth = 0;
            for (size_t at = 0; at < dir.size();) {
                size_t end = ++depth < 32 ? std::min(dir.find_first_of(L"/\\", at), dir.size()) : dir.size();
                std::wstring part = dir.substr(at, end - at);
                at = end + 1;
                if (part.empty()) continue;
                parent = group(parent, part);
                nodes[parent].node.tracks.push_back(id);
            }
            if (!parent) { parent = group(0, L"(unknown folder)"); nodes[parent].node.tracks.push_back(id); }
        } else {
            parent = group(0, view == 1 ? artist : view == 2 ? album : library.Str(track.genre));
            nodes[parent].node.tracks.push_back(id);
            if (view == 1) { parent = group(parent, album); nodes[parent].node.tracks.push_back(id); }
        }
        size_t leaf = nodes.size();
        nodes.push_back({{title, std::to_wstring(parent) + L"/" + std::to_wstring(id), {id}, {}}, {}});
        nodes[parent].children.push_back(leaf);
    }
    auto finish = [&](auto&& self, size_t index) -> AlbumListNode {
        auto children = std::move(nodes[index].children);
        std::sort(children.begin(), children.end(), [&](size_t x, size_t y) {
            return CompareNatural(nodes[x].node.label, nodes[y].node.label) < 0;
        });
        auto node = std::move(nodes[index].node);
        node.children.reserve(children.size());
        for (size_t child : children) node.children.push_back(self(self, child));
        return node;
    };
    return finish(finish, 0).children;
}

WaveAppearance& FountainAppearance() { return appearance; }
bool FountainStyleShown() {
    return kExtraThemes && g.settings.theme == ThemeMode::Deltarune && g.settings.size == ViewSize::Normal &&
        g.settings.waveform && appearance.style != 0 && (g.settings.liveFountain || !LiveWaveformShown());
}
Rgb WavePlayedColor(const Palette& p) {
    if (g.playing != kNone && (g_lib[g.playing].Failed() || g.stopAfterCurrent)) return p.error;
    if (IsPaused()) return p.amber;
    int dock = std::clamp(g.settings.layout.dock, 0, 3);
    Rgb own = appearance.played[dock], theme = ThemeInfo(g.settings.theme).wavePlayed;
    return own != kNoRgb ? own : theme != kNoRgb ? theme : p.accent;
}
Rgb WaveRestColor(const Palette& p, bool live) {
    if (live && g.settings.fullLiveWaveform) return WavePlayedColor(p);
    int dock = std::clamp(g.settings.layout.dock, 0, 3);
    Rgb own = appearance.rest[dock], theme = ThemeInfo(g.settings.theme).waveRest;
    return own != kNoRgb ? own : theme != kNoRgb ? theme : p.dark ? p.faint : p.border;
}
void LoadAlternateSettings(const std::map<std::string, std::string>& v) {
    appearance.style = Integer(v, "wave_style", 2, 0, 2);
    appearance.motion = Integer(v, "wave_motion", 1, 0, 1);
    appearance.drColors = Integer(v, "wave_deltarune_colors", 1, 0, 1);
    appearance.glow = Integer(v, "wave_glow", 1, 0, 1);
    appearance.black = Integer(v, "wave_black", 1, 0, 1);
    for (int i = 0; i < 4; ++i) for (bool rest : {false, true}) {
        auto it = v.find("wave_" + std::to_string(i) + (rest ? "_rest" : "_played"));
        (rest ? appearance.rest : appearance.played)[i] = it == v.end() ? kNoRgb : ParseColor(it->second);
    }
    a.skin = Integer(v, "apollo_skin", 0, 0, 4);
    a.opacity = Integer(v, "apollo_opacity", 40, 20, 100);
    a.analyzer = Integer(v, "apollo_analyzer", 1, 0, 1);
    a.scope = Integer(v, "apollo_scope", 1, 0, 1);
    a.viewMode = Integer(v, "foobar_view", 0, 0, 3);
    for (auto [key, target] : {std::pair{"foobar_columns", &fbColumns}, {"apollo_columns", &apColumns}})
        if (auto it = v.find(key); it != v.end()) target->pending = it->second;
}
std::string AlternateSettingsText() {
    std::string text = "wave_style=" + std::to_string(appearance.style) + "\n";
    for (auto [key, value] : {std::pair{"wave_motion", appearance.motion}, {"wave_deltarune_colors", appearance.drColors},
                            {"wave_glow", appearance.glow}, {"wave_black", appearance.black},
                            {"apollo_analyzer", a.analyzer}, {"apollo_scope", a.scope}})
        text += std::string(key) + "=" + std::to_string(value) + "\n";
    for (int i = 0; i < 4; ++i) text += "wave_" + std::to_string(i) + "_played=" + Hex(appearance.played[i]) +
        "\nwave_" + std::to_string(i) + "_rest=" + Hex(appearance.rest[i]) + "\n";
    text += "apollo_skin=" + std::to_string(a.skin) + "\napollo_opacity=" + std::to_string(a.opacity) + "\nfoobar_view=" + std::to_string(a.viewMode) + "\n";
    text += "foobar_columns=" + ColumnText(fbColumns) + "\napollo_columns=" + ColumnText(apColumns) + "\n";
    return text;
}
bool AlternateMode() { return Mode() != 0; }
const wchar_t* ActiveColumnHeading(int column) {
    if (Mode() == 2 && column == 0) return L"No";
    if (Mode() == 2 && column == ui::kColApolloTitle) return L"Title";
    if (Mode() == 1 && column == 0) return L"Playing";
    return ColumnAt(column).name;
}
std::vector<int>& ActiveColumnTypes() {
    int mode = Mode();
    if (!mode) return g.settings.columns;
    auto& c = mode == 1 ? fbColumns : apColumns;
    ResolveColumns(c);
    std::erase_if(c.types, [](int type) { return type < 0 || type >= ColumnTotal(); });
    return c.types;
}
std::vector<int>& ActiveColumnWidths() {
    int mode = Mode();
    if (!mode) return g.settings.widths;
    auto& c = mode == 1 ? fbColumns : apColumns; ResolveColumns(c); return c.widths;
}
void AlternateThemeChanged() {
    if (!Mode()) { a.filterFocused = false; a.selection.clear(); }
    a.revision = UINT32_MAX;
}

void AlternateCatalogChanged() {
    for (Columns* c : {&fbColumns, &apColumns}) {
        std::vector<std::string> next;
        for (int type = 0; type < ColumnTotal(); ++type)
            next.push_back(type < ui::kColumnCount ? std::to_string(type) : PackColumnKey(type));
        if (!c->catalog.empty()) {
            const auto oldWidths = c->widths;
            std::vector<int> mapped;
            c->widths.assign(ColumnTotal(), 0);
            for (int type : c->types) {
                if (type < 0 || type >= (int)c->catalog.size()) continue;
                auto it = std::find(next.begin(), next.end(), c->catalog[type]);
                if (it == next.end()) continue;
                int now = (int)(it - next.begin());
                mapped.push_back(now);
                c->widths[now] = type < (int)oldWidths.size() ? oldWidths[type] : 0;
            }
            c->types = std::move(mapped);
            if (c->types.empty()) c->types = {0, 2};
        }
        c->catalog = std::move(next);
        ResolveColumns(*c);
    }
}

void ApplyAlternateLayout() {
    int mode = Mode();
    if (!mode) return;
    ClearSpecialGeometry();
    float W = L.menuBar.w, bottom = L.status.y, top = mode == 1 ? L.menuBar.B() : 0;
    L.player = {0, top, W, mode == 1 ? S(ui::kFoobarButton + 8) : S(58)};
    if (mode == 2) { L.menuBar.h = 0; L.barItems.clear(); }
    if (mode == 1) {
        float button = S(ui::kFoobarButton), x = S(4), y = top + S(3);
        for (int i = 0; i < 6; ++i) a.tools[i] = {x + i * button, y, button, button};
        x += 6 * button + S(5);
        L.visuals[0] = {x, y, S(ui::kFoobarSpectrum), button}; x = L.visuals[0].R() + S(6);
        L.volume = {x, y, S(ui::kFoobarVolume), button}; x = L.volume.R() + S(6);
        L.seek = {x, y, std::min(S(ui::kFoobarSeek), std::max(S(40), W - x - S(100))), button};
        x = L.seek.R() + S(6);
        L.visuals[1] = {x, y, std::max(0.f, W - x - S(4)), button};
        float sideW = std::min(S(ui::kFoobarPanel), W * 0.4f), cy = L.player.B();
        a.side = {0, cy, sideW, std::max(0.f, bottom - cy)};
        a.sideTabs[0] = {S(2), cy + S(2), sideW / 2 - S(2), S(25)};
        a.sideTabs[1] = {sideW / 2, cy + S(2), sideW / 2 - S(2), S(25)};
        a.view = {S(6), cy + S(34), sideW - S(12), S(26)};
        a.filter = {S(6), cy + S(66), sideW - S(12), S(24)};
        a.tree = {S(3), cy + S(96), sideW - S(6), std::max(0.f, bottom - cy - S(99))};
        float listX = sideW + S(4), listW = std::max(0.f, W - listX), tabsY = cy + S(3);
        L.tabs = {listX, tabsY, listW, S(ui::kTabRow)};
        L.radioTab = L.search = {}; L.tabRects.clear();
        float tx = listX + S(2);
        for (size_t i = 0; i < g.playlists.size(); ++i) {
            float w = TextWidth(Font::Normal, TabName(i)) + S(30);
            L.tabRects.push_back({tx, tabsY, w, S(ui::kTabRow)}); tx += w;
        }
        ListGeometry({listX, L.tabs.B() + S(2), listW, std::max(0.f, bottom - L.tabs.B() - S(2))});
    } else {
        L.tabs = L.search = L.radioTab = {}; L.tabRects.clear();
        float tb = std::min(W / 2, S(380)), volume = std::clamp(W / 5, S(110), S(190));
        float x = S(8), row = top + S(3), button = std::min(S(22), (tb - S(16)) / 15);
        for (int i = 0; i < 15; ++i) a.tools[i] = {x + i * button, row, button, S(22)};
        a.labels[0] = {tb + S(8), row, S(52), S(22)};
        L.seek = {a.labels[0].R() + S(4), row, std::max(0.f, W - volume - a.labels[0].R() - S(8)), S(22)};
        a.labels[1] = {W - volume + S(8), row, S(46), S(22)};
        L.volume = {a.labels[1].R() + S(4), row, std::max(0.f, W - a.labels[1].R() - S(8)), S(22)};
        a.statusBox = {S(8), top + S(32), tb - S(12), S(21)};
        int count = (a.analyzer ? 1 : 0) + (a.scope ? 1 : 0);
        float each = count ? (W - tb - S(6)) / count : 0, vx = tb + S(3);
        L.visuals[0] = a.analyzer ? Rect{vx, top + S(32), each, S(21)} : Rect{};
        if (a.analyzer) vx += each;
        L.visuals[1] = a.scope ? Rect{vx, top + S(32), each, S(21)} : Rect{};
        ListGeometry({S(4), L.player.B() + S(4), W - S(8), std::max(0.f, bottom - L.player.B() - S(8))});
        auto& widths = ActiveColumnWidths();
        float other = 0;
        for (int type : ActiveColumnTypes()) if (type != ui::kColApolloTitle) {
            const auto column = ColumnAt(type); other += S(widths[type] > 0 ? widths[type] : column.width);
        }
        widths[ui::kColApolloTitle] = std::max(80, (int)((L.rows.w - other) / g.scale));
    }
}
bool DrawAlternate(const Palette& p) {
    int mode = Mode();
    if (!mode) return false;
    if (mode == 2) { DrawApollo(p); return true; }
    Fill(L.player, p.surface); DrawTree(p);
    const Icon icons[] = {Icon::Play, Icon::Play, Icon::Pause, Icon::Prev, Icon::Next, Icon::Shuffle};
    for (int i = 0; i < 6; ++i) {
        if (i) DrawBandButton(a.tools[i], icons[i], p, i == 5 && g.settings.shuffle);
        else {
            if (a.tools[i].In(g_mouseX, g_mouseY)) { Fill(a.tools[i], p.hover); Frame(a.tools[i], p.border); }
            const auto& r = a.tools[i];
            Fill({r.x + (r.w - S(8)) / 2, r.y + (r.h - S(8)) / 2, S(8), S(8)}, p.text);
        }
    }
    double duration = TrackDuration(), position = g.playing == kNone ? 0 : g.engine.Position();
    DrawSlider(L.volume, g.settings.volume, true, L.volume.In(g_mouseX, g_mouseY), p);
    DrawSlider(L.seek, duration > 0 ? position / duration : 0, duration > 0, L.seek.In(g_mouseX, g_mouseY), p);
    return true;
}
bool DrawAlternateCell(TrackId id, int column, const Rect& cell, Font font, const Palette& p, int row) {
    Rgb text = row >= 0 && row < (int)g.selected.size() && g.selected[row] ? p.selectText :
        g_lib.Contains(id) && (g_lib[id].flags & (Track::kFailed | Track::kMissing)) ? p.faint : p.text;
    if (Mode() == 1 && column == ui::kColCover) {
        Rect cover{cell.x, cell.y, std::min(cell.w, cell.h), cell.h};
        DrawAlbumCover(id, g_lib[id].album, g_lib.Str(g_lib[id].album), cover, p);
        return true;
    }
    if (Mode() == 2 && column == 0) {
        DrawText(font, std::to_string(row + 1), cell.x, TextY(cell, font), text, cell.w, 2);
        return true;
    }
    if (Mode() == 2 && column == ui::kColApolloTitle) {
        Rect icon{cell.x, cell.y + (cell.h - S(16)) / 2, S(16), S(16)};
        DrawIcon(Icon::Play, icon, p.sub);
        DrawText(font, Cell(id, column), icon.R() + S(4), TextY(cell, font), text,
            std::max(0.f, cell.R() - icon.R() - S(4)));
        return true;
    }
    return false;
}

bool DrawAlternateStatus(const Palette& p) {
    if (!Mode()) return false;
    Fill(L.status, p.surface);
    auto selected = SelectedTracks();
    double total = 0, chosen = 0;
    for (TrackId id : g.rows) total += g_lib[id].Duration();
    for (TrackId id : selected) chosen += g_lib[id].Duration();
    std::string left = Mode() == 1 ? (g.playing == kNone ? "Playback stopped." : Utf8(StatusMessage())) :
        std::to_string(g.rows.size()) + " tracks (" + Utf8(FormatTotalDuration(total)) + ")";
    std::string right = selected.empty() ? "No items selected" : std::to_string(selected.size()) + " selected (" + Utf8(FormatTotalDuration(chosen)) + ")";
    Rect halves[] = {{S(2), L.status.y + S(2), L.status.w * 0.6f - S(3), L.status.h - S(4)},
        {L.status.w * 0.6f, L.status.y + S(2), L.status.w * 0.4f - S(2), L.status.h - S(4)}};
    for (int i = 0; i < 2; ++i) {
        if (Mode() == 2) Sunken(halves[i], p);
        DrawText(Font::Normal, i ? right : left, halves[i].x + S(5), TextY(halves[i]), p.text, halves[i].w - S(10));
    }
    return true;
}
void FilterAlternateLibraryTracks(std::vector<TrackId>& tracks) {
    if (Mode() != 1 || a.selection.empty()) return;
    std::unordered_set<TrackId> selected(a.selection.begin(), a.selection.end());
    std::erase_if(tracks, [&](TrackId id) { return !selected.contains(id); });
}
void ReleaseAlternateTextures() {
    if (a.skinTexture) SDL_DestroyTexture(a.skinTexture);
    if (a.toolbarTexture) SDL_DestroyTexture(a.toolbarTexture);
    a.skinTexture = a.toolbarTexture = nullptr; a.skinFor = -1;
}
std::vector<MenuItem> WaveAppearanceMenu() {
    if (!kExtraThemes) return {};
    auto custom = [](const char* label, int kind, int value, bool checked) {
        auto item = Item(label, kAltAction + kind, {}, checked); item.arg = value; return item;
    };
    std::vector<MenuItem> menu;
    const char* styles[] = {"Bars", "Fountain bars", "Dark Fountain (DELTARUNE)"};
    for (int i = 0; i < 3; ++i) menu.push_back(custom(styles[i], kStyle, i, appearance.style == i));
    menu.push_back(Separator());
    menu.push_back(custom("Moving", kMotion, 0, appearance.motion));
    menu.push_back(custom("DELTARUNE colors", kGameColors, 0, appearance.drColors));
    menu.push_back(custom("Fountain glow", kGlow, 0, appearance.glow));
    menu.push_back(custom("Black surroundings", kBlack, 0, appearance.black));
    std::vector<MenuItem> colors;
    const char* docks[] = {"At the top", "At the bottom", "On the left", "On the right"};
    for (int i = 0; i < 4; ++i) colors.push_back(Submenu(docks[i], {
        custom("Played color...", kColor, i * 2, appearance.played[i] != kNoRgb),
        custom("Unplayed color...", kColor, i * 2 + 1, appearance.rest[i] != kNoRgb)}));
    menu.push_back(Submenu("Colors", std::move(colors)));
    return menu;
}
bool RunAlternateAction(int action, int argument) {
    int kind = action - kAltAction;
    if (kind < 0 || kind > kScope) return false;
    if (kind == kStyle) appearance.style = std::clamp(argument, 0, 2);
    if (kind == kMotion) appearance.motion = !appearance.motion;
    if (kind == kGameColors) appearance.drColors = !appearance.drColors;
    if (kind == kGlow) appearance.glow = !appearance.glow;
    if (kind == kBlack) appearance.black = !appearance.black;
    if (kind == kSkin) a.skin = std::clamp(argument, 0, 4);
    if (kind == kOpacity) a.opacity = std::clamp(argument, 20, 100);
    if (kind == kAnalyzer) a.analyzer = !a.analyzer;
    if (kind == kScope) a.scope = !a.scope;
    if (kind == kColor) {
        color = ColorDialog{}; color.open = true;
        color.dock = std::clamp(argument / 2, 0, 3); color.rest = argument & 1;
        color.text = Hex((color.rest ? appearance.rest : appearance.played)[color.dock]); SDL_StartTextInput(g.window);
    } else SaveSettings();
    g.redraw = true; return true;
}
void DrawAlternateOverlay(const Palette& p) {
    if (!color.open) return;
    color.panel = DrawFrame(p, S(440), S(245), color.rest ? "Unplayed color" : "Played color");
    float x = color.panel.x + S(20), y = color.panel.y + S(55), w = color.panel.w - S(40);
    DrawText(Font::Normal, "RGB color as six hexadecimal digits:", x, y, p.sub, w);
    color.field = {x, y + S(28), w - S(70), S(28)};
    Fill(color.field, p.bg); Frame(color.field, color.focus == 0 ? p.accent : p.border);
    DrawText(Font::Normal, color.text, color.field.x + S(6), TextY(color.field), p.text, color.field.w - S(12));
    Rgb preview = ParseColor(color.text);
    Fill({color.field.R() + S(10), color.field.y, S(50), S(28)}, preview == kNoRgb ? p.surface2 : preview);
    float by = color.panel.B() - S(48);
    color.reset = {x, by, S(120), S(28)};
    color.apply = {color.panel.R() - S(200), by, S(80), S(28)};
    color.cancel = {color.panel.R() - S(110), by, S(90), S(28)};
    DrawButton(color.reset, "Theme color", p); DrawButton(color.apply, "Apply", p, true, preview != kNoRgb);
    DrawButton(color.cancel, "Cancel", p);
    const Rect controls[] = {color.field, color.reset, color.apply, color.cancel};
    if (color.focus) Frame(controls[color.focus], p.accent);
}
bool AlternateMouseDown(float x, float y, int button, int clicks) {
    if (color.open) {
        if (button != SDL_BUTTON_LEFT) return true;
        if (!color.panel.In(x, y) || color.cancel.In(x, y)) CloseColor();
        else if (color.apply.In(x, y)) ApplyColor(false);
        else if (color.reset.In(x, y)) ApplyColor(true);
        else if (color.field.In(x, y)) color.focus = 0;
        return true;
    }
    int mode = Mode();
    if (!mode) return false;
    if (mode == 1 && a.side.In(x, y)) {
        if (button != SDL_BUTTON_LEFT) return true;
        if (a.sideTabs[0].In(x, y) || a.sideTabs[1].In(x, y)) a.panel = a.sideTabs[1].In(x, y);
        else if (a.panel == 0 && a.view.In(x, y))
            OpenChoice(x, y, {"by folder structure", "by artist/album", "by album", "by genre"}, a.viewMode,
                [](int i) { a.viewMode = i; a.selection.clear(); a.revision = UINT32_MAX; RebuildRows(); SaveSettings(); });
        else if (a.panel == 0 && a.filter.In(x, y)) { a.filterFocused = true; SDL_StartTextInput(g.window); }
        else if (a.panel == 0 && a.tree.In(x, y)) {
            a.filterFocused = false; a.focus = 100;
            int row = (int)((y - a.tree.y + a.scroll) / S(ui::kRow));
            if (row >= 0 && row < (int)a.rows.size()) {
                auto [node, depth] = a.rows[row];
                if (!node->children.empty() && x < a.tree.x + S(22 + 14 * depth)) {
                    if (!a.expanded.erase(node->key)) a.expanded.insert(node->key);
                    RebuildTreeRows();
                } else PickTree(row, clicks >= 2);
            }
        }
        g.redraw = true; return true;
    }
    if (L.player.In(x, y)) {
        int count = mode == 1 ? 6 : 15;
        if (button == SDL_BUTTON_RIGHT && mode == 2) {
            std::vector<MenuItem> skins;
            const char* names[] = {"Capacala", "Butterflies", "Letters", "Notes", "None"};
            for (int i = 0; i < 5; ++i) {
                auto item = Item(names[i], kAltAction + kSkin, {}, a.skin == i); item.arg = i; skins.push_back(item);
            }
            std::vector<MenuItem> opacity;
            for (int i : {20, 40, 60, 80, 100}) {
                auto item = Item(std::to_string(i) + "%", kAltAction + kOpacity, {}, a.opacity == i);
                item.arg = i; opacity.push_back(item);
            }
            OpenPopup(x, y, {Submenu("Background", std::move(skins)), Submenu("Opacity", std::move(opacity)),
                Item("Frequency analyzer", kAltAction + kAnalyzer, {}, a.analyzer),
                Item("Oscilloscope", kAltAction + kScope, {}, a.scope)});
            return true;
        }
        for (int i = 0; i < count; ++i) if (a.tools[i].In(x, y)) {
            if (button == SDL_BUTTON_LEFT) {
                a.focus = i;
                if (mode == 2) ToolbarAction(i, x, y);
                else { const int tools[] = {4, 6, 5, 3, 7, 9}; ToolbarAction(tools[i], x, y); }
            }
            return true;
        }
    }
    a.filterFocused = false; a.focus = -1;
    return false;
}
bool AlternateWheel(float x, float y, float delta) {
    if (color.open) return true;
    if (Mode() != 1 || !a.tree.In(x, y)) return false;
    a.scroll = std::clamp(a.scroll - delta * S(ui::kRow * 3), 0.f, std::max(0.f, a.rows.size() * S(ui::kRow) - a.tree.h));
    g.redraw = true; return true;
}
bool AlternateKey(const SDL_KeyboardEvent& e) {
    if (color.open) {
        if (e.key == SDLK_ESCAPE) CloseColor();
        else if (e.key == SDLK_TAB) color.focus = (color.focus + (e.mod & SDL_KMOD_SHIFT ? 3 : 1)) % 4;
        else if ((e.mod & SDL_KMOD_CTRL) && e.key == SDLK_A) color.selectAll = true;
        else if (e.key == SDLK_BACKSPACE && color.focus == 0) {
            if (color.selectAll) color.text.clear(); else if (!color.text.empty()) color.text.pop_back();
            color.selectAll = false;
        } else if (e.key == SDLK_RETURN || (e.key == SDLK_SPACE && color.focus)) {
            if (color.focus == 1) ApplyColor(true); else if (color.focus == 3) CloseColor(); else ApplyColor(false);
        }
        g.redraw = true; return true;
    }
    if (!Mode()) return false;
    if ((e.mod & SDL_KMOD_CTRL) && e.key == SDLK_F && Mode() == 1) {
        a.filterFocused = true; SDL_StartTextInput(g.window); return true;
    }
    if (Mode() == 1 && a.filterFocused) {
        if (e.key == SDLK_ESCAPE) a.filterFocused = false;
        else if (e.key == SDLK_BACKSPACE && !a.filterText.empty()) { a.filterText.pop_back(); a.selection.clear(); }
        else if (e.key == SDLK_DOWN || e.key == SDLK_RETURN) { a.filterFocused = false; BuildTree(); SelectTreeRow(0); }
        else if (e.key == SDLK_TAB) a.filterFocused = false;
        a.revision = UINT32_MAX; g.redraw = true; return true;
    }
    if (e.key == SDLK_F6) {
        if (Mode() == 1) { if (e.mod & SDL_KMOD_CTRL) a.panel = 1 - a.panel; else a.focus = 100; }
        else a.focus = 0;
        g.redraw = true; return true;
    }
    if (Mode() == 1 && a.focus == 100) {
        BuildTree();
        if (e.key == SDLK_UP || e.key == SDLK_DOWN) SelectTreeRow(a.selected + (e.key == SDLK_UP ? -1 : 1));
        else if ((e.key == SDLK_LEFT || e.key == SDLK_RIGHT) && a.selected >= 0) {
            const auto* node = a.rows[a.selected].node;
            if (e.key == SDLK_LEFT) a.expanded.erase(node->key); else a.expanded.insert(node->key);
            RebuildTreeRows();
        } else if (e.key == SDLK_RETURN) PickTree(a.selected, true);
        else if (e.key == SDLK_ESCAPE || e.key == SDLK_TAB) a.focus = -1;
        else return false;
        g.redraw = true; return true;
    }
    if (Mode() == 2 && a.focus >= 0 && a.focus < 15) {
        if (e.key == SDLK_LEFT || e.key == SDLK_RIGHT) a.focus = (a.focus + (e.key == SDLK_LEFT ? 14 : 1)) % 15;
        else if (e.key == SDLK_RETURN || e.key == SDLK_SPACE) ToolbarAction(a.focus, a.tools[a.focus].x, a.tools[a.focus].B());
        else if (e.key == SDLK_ESCAPE || e.key == SDLK_TAB) a.focus = -1;
        else return false;
        g.redraw = true; return true;
    }
    return false;
}
bool AlternateText(const char* text) {
    if (!text) return color.open;
    if (color.open) {
        if (color.focus != 0) return true;
        if (color.selectAll) color.text.clear();
        color.selectAll = false;
        for (const char* p = text; *p && color.text.size() < 6; ++p)
            if (isxdigit((unsigned char)*p)) color.text += (char)toupper((unsigned char)*p);
        g.redraw = true; return true;
    }
    if (Mode() != 1 || !a.filterFocused) return false;
    if (a.filterText.size() < 1024) a.filterText += FromUtf8(text);
    a.selection.clear(); a.revision = UINT32_MAX; g.redraw = true;
    return true;
}
}  // namespace lx
