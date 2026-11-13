// Edit > Make album on Linux, as app_albummaker.cpp on Windows: the tracks given become one album. The
// fields, compilation flag and cover apply to every track; the list sets their order and numbers. Saving is
// one tag write (album_maker.h), so Edit > Undo last tag edit takes it back. The panel takes the window as
// the others do, so tracks are chosen before it opens rather than added from behind it.
#include "panels_internal.h"

#include "album_maker.h"
#include "cover.h"

namespace lx::panel {
namespace {

enum AlbumField { kAlbumTitle, kAlbumArtist, kAlbumDate, kAlbumGenre, kAlbumDisc, kAlbumDiscTotal, kAlbumFields };
const char* const kAlbumLabels[] = {"Album", "Album artist", "Year", "Genre", "Disc", "of"};

struct AlbumPanel {
    AlbumDraft draft;
    TextField fields[kAlbumFields];
    int focus = kAlbumTitle;   // a field, or kAlbumFields for the track list
    std::vector<char> selected;  // per track of draft.tracks
    int caret = -1;              // the list's focused row
    int first = 0;               // the first row shown
    CoverPreview preview;
    uint32_t generation = 0;  // a cover search answers only the album maker it was opened for
    Rect list, art, compilation, artistToTracks, number, save, cancel, buttons[7];
} g_album;

void AlbumReadFields() {
    auto& d = g_album.draft;
    d.title = g_album.fields[kAlbumTitle].text;
    d.artist = g_album.fields[kAlbumArtist].text;
    d.date = g_album.fields[kAlbumDate].text;
    d.genre = g_album.fields[kAlbumGenre].text;
    d.disc = (uint32_t)std::max(0l, wcstol(g_album.fields[kAlbumDisc].text.c_str(), nullptr, 10));
    d.discTotal = (uint32_t)std::max(0l, wcstol(g_album.fields[kAlbumDiscTotal].text.c_str(), nullptr, 10));
}

std::vector<size_t> AlbumSelection() {
    std::vector<size_t> out;
    for (size_t i = 0; i < g_album.selected.size(); ++i)
        if (g_album.selected[i]) out.push_back(i);
    return out;
}

void AlbumSelect(const std::vector<size_t>& rows) {
    g_album.selected.assign(g_album.draft.tracks.size(), 0);
    for (size_t i : rows)
        if (i < g_album.selected.size()) g_album.selected[i] = 1;
    g_album.caret = rows.empty() ? -1 : (int)rows.front();
    g.redraw = true;
}

void AlbumFocus(int f) {
    g_album.focus = std::clamp(f, 0, (int)kAlbumFields);
    if (g_album.focus < kAlbumFields) {
        TextField& t = g_album.fields[g_album.focus];
        t.caret = t.text.size();
        t.all = !t.text.empty();
        SDL_StartTextInput(g.window);
    }
    g.redraw = true;
}

void AlbumMove(int delta) {
    AlbumSelect(MoveAlbumTracks(g_album.draft, AlbumSelection(), delta));
    g_album.draft.numberTracks = true;  // a new order is what the numbers should say
}

void AlbumRemoveSelected() {
    auto rows = AlbumSelection();
    for (size_t k = rows.size(); k-- > 0;) g_album.draft.tracks.erase(g_album.draft.tracks.begin() + (ptrdiff_t)rows[k]);
    size_t next = rows.empty() ? 0 : std::min(rows.front(), g_album.draft.tracks.size());
    AlbumSelect(next < g_album.draft.tracks.size() ? std::vector<size_t>{next} : std::vector<size_t>{});
}

void AlbumSave() {
    AlbumReadFields();
    if (!AlbumDraftProblem(g_album.draft).empty()) return;
    auto edits = AlbumEdits(g_lib, g_album.draft);
    if (!edits.empty() && TagWriteBusy()) {
        ShowMessage(L"Tags are still being written; try again in a moment.");
        return;
    }
    ClosePanel();
    if (!edits.empty()) SubmitTagWrites(edits, true);
}

void AlbumChooseCover(const std::wstring& path) {
    std::string bytes = ReadCoverFile(path);
    if (bytes.empty()) return;
    g_album.preview.Set(bytes);
    g_album.draft.cover = std::move(bytes);
    g.redraw = true;
}

void AlbumOrderMenu(float x, float y) {
    OpenChoice(x, y, {"By track number", "By title", "By file name", "Reverse"}, -1, [](int order) {
        OrderAlbum(g_lib, g_album.draft, (AlbumOrder)order);
        g_album.draft.numberTracks = true;
        AlbumSelect({});
    });
}

constexpr float kAlbumRow = 26;

void DrawAlbumMaker(const Palette& p) {
    AlbumReadFields();
    auto& d = g_album.draft;
    Rect panel = DrawFrame(p, S(760), S(660), "Make album");
    float pad = S(16), labelW = S(100), rowH = S(26), gap = S(6), artSize = S(150);
    float x0 = panel.x + pad, right = panel.R() - pad - artSize - S(18), y = panel.y + S(48);
    auto field = [&](int f, float x, float w, float lw, const char* cue) {
        DrawText(Font::Normal, kAlbumLabels[f], x, y + (rowH - LineHeight(Font::Normal)) / 2, p.sub, lw - S(8), 2);
        g_album.fields[f].rect = {x + lw, y, w - lw, rowH};
        DrawLineField(g_album.fields[f], g_album.fields[f].rect, g_album.focus == f, true, cue, p);
    };
    field(kAlbumTitle, x0, right - x0, labelW, nullptr);
    y += rowH + gap;
    field(kAlbumArtist, x0, right - x0, labelW, d.compilation ? "Various Artists" : nullptr);
    y += rowH + gap;
    float half = std::round((right - x0) / 2);
    field(kAlbumDate, x0, half - S(8), labelW, "(each keeps its own)");
    field(kAlbumGenre, x0 + half, half, S(60), "(each keeps its own)");
    y += rowH + gap;
    field(kAlbumDisc, x0, labelW + S(60), labelW, "(keep)");
    field(kAlbumDiscTotal, x0 + labelW + S(60), S(90), S(30), nullptr);
    y += rowH + gap + S(2);
    g_album.compilation = {x0 + labelW, y, right - x0 - labelW, S(24)};
    DrawCheck(g_album.compilation, "Part of a compilation (various artists)", d.compilation ? 1 : 0, p);
    y += S(28);
    g_album.artistToTracks = {x0 + labelW, y, right - x0 - labelW, S(24)};
    DrawCheck(g_album.artistToTracks, "Set every track's artist to the album artist", d.artistToTracks ? 1 : 0, p);
    // The cover, top right.
    float ax = panel.R() - pad - artSize, ay = panel.y + S(48);
    g_album.art = {ax, ay, artSize, artSize};
    Fill(g_album.art, p.surface2);
    Frame(g_album.art, p.border);
    bool removed = d.cover && d.cover->empty();
    if (g_album.preview.texture && !removed) {
        g_album.preview.Draw(g_album.art);
    } else {
        DrawText(Font::Normal, removed ? "(cover removed)" : "No cover", ax, TextY(g_album.art), p.faint, artSize, 1);
    }
    const char* coverLabels[] = {"Choose cover...", "Find cover online...", "Remove cover"};
    for (int i = 0; i < 3; ++i) {
        g_album.buttons[i] = {ax, ay + artSize + S(6) + i * S(30), artSize, S(26)};
        DrawButton(g_album.buttons[i], coverLabels[i], p, false,
                   i < 2 || (d.cover ? !d.cover->empty() : g_album.preview.texture != nullptr));
    }
    // The tracks, with their buttons beside them.
    y = std::max(y + S(36), ay + artSize + S(100));
    double seconds = 0;
    for (TrackId id : d.tracks) seconds += g_lib[id].Duration();
    std::string heading = "Tracks  (" + std::to_string(d.tracks.size()) + ", " + Utf8(FormatTotalDuration(seconds)) + ")";
    float buttonsW = S(128), listRight = panel.R() - pad - buttonsW - S(10);
    DrawText(Font::Normal, heading, x0, y, p.sub, listRight - x0);
    y += S(24);
    float bottom = panel.B() - pad - S(28) - S(30) - S(30);
    Rect list = g_album.list = {x0, y, listRight - x0, std::max(S(60), bottom - y)};
    Fill(list, p.bg);
    Frame(list, g_album.focus == kAlbumFields ? p.accent : p.border);
    const float row = S(kAlbumRow);
    const int visible = std::max(1, (int)((list.h - S(4)) / row));
    if (g_album.caret >= 0) g_album.first = std::clamp(g_album.first, g_album.caret - visible + 1, g_album.caret);
    g_album.first = std::clamp(g_album.first, 0, std::max(0, (int)d.tracks.size() - visible));
    SDL_Rect clip{(int)list.x + 1, (int)list.y + 1, (int)list.w - 2, (int)list.h - 2};
    SDL_SetRenderClipRect(g.renderer, &clip);
    for (int i = g_album.first; i < (int)d.tracks.size() && i < g_album.first + visible; ++i) {
        Rect r{list.x + S(1), list.y + S(2) + (i - g_album.first) * row, list.w - S(2), row};
        bool sel = i < (int)g_album.selected.size() && g_album.selected[i];
        if (sel) Fill(r, p.select);
        const Track& t = g_lib[d.tracks[i]];
        uint32_t n = d.numberTracks ? (uint32_t)i + 1 : t.trackNo;
        uint32_t color = sel ? p.selectText : p.text;
        if (n) DrawText(Font::Normal, std::to_string(n) + ".", r.x + S(6), TextY(r), sel ? p.selectText : p.sub, S(34));
        std::wstring line = g_lib.Str(t.title ? t.title : t.file);
        if (t.artist) line += L"  \x2014  " + g_lib.Str(t.artist);
        std::string length = Utf8(FormatDuration(t.Duration()));
        float lengthW = TextWidth(Font::Normal, length);
        DrawText(Font::Normal, Utf8(line), r.x + S(44), TextY(r), color, r.w - S(44) - lengthW - S(16));
        DrawText(Font::Normal, length, r.R() - S(8) - lengthW, TextY(r), sel ? p.selectText : p.sub);
        if (i == g_album.caret && g_album.focus == kAlbumFields) Frame(r, p.accent);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
    auto rows = AlbumSelection();
    const char* listLabels[] = {"Move up", "Move down", "Remove", "Order..."};
    const bool enabled[] = {!rows.empty() && rows.front() > 0, !rows.empty() && rows.back() + 1 < d.tracks.size(), !rows.empty(),
                            d.tracks.size() > 1};
    for (int i = 0; i < 4; ++i) {
        g_album.buttons[3 + i] = {panel.R() - pad - buttonsW, y + i * S(32), buttonsW, S(28)};
        DrawButton(g_album.buttons[3 + i], listLabels[i], p, false, enabled[i]);
    }
    g_album.number = {x0, list.B() + S(6), list.w, S(24)};
    DrawCheck(g_album.number, "Number the tracks in this order", d.numberTracks ? 1 : 0, p);
    // What Save will do, or what keeps it from saving.
    std::wstring problem = AlbumDraftProblem(d), info;
    if (problem.empty()) {
        size_t files = AlbumEdits(g_lib, d).size();
        info = files == 0 ? L"These tracks already make this album."
                          : L"Saving writes the tags of " + std::to_wstring(files) + (files == 1 ? L" file" : L" files") +
                                L". Edit > Undo last tag edit takes it back.";
    }
    float by = panel.B() - pad - S(28);
    DrawText(Font::Small, Utf8(problem.empty() ? info : problem), x0, by - S(26), p.sub, panel.R() - pad - x0 - S(260));
    g_album.save = {panel.R() - pad - S(248), by, S(140), S(28)};
    g_album.cancel = {panel.R() - pad - S(100), by, S(100), S(28)};
    DrawButton(g_album.save, "Make album", p, true, problem.empty());
    DrawButton(g_album.cancel, "Cancel", p);
}

void AlbumMouseDown(float x, float y, int clicks) {
    auto& d = g_album.draft;
    for (int f = 0; f < kAlbumFields; ++f)
        if (g_album.fields[f].rect.In(x, y)) {
            AlbumFocus(f);
            g_album.fields[f].all = false;
            return;
        }
    if (g_album.compilation.In(x, y)) d.compilation = !d.compilation;
    else if (g_album.artistToTracks.In(x, y)) d.artistToTracks = !d.artistToTracks;
    else if (g_album.number.In(x, y)) d.numberTracks = !d.numberTracks;
    else if (g_album.buttons[0].In(x, y)) {
        static const SDL_DialogFileFilter filter{"Images", "jpg;jpeg;png"};  // the dialog keeps a pointer to it
        ChooseFile(kFileAlbumCover, filter);
    }
    else if (g_album.buttons[1].In(x, y)) {
        uint32_t generation = ++g_album.generation;
        OpenCoverSearchForTags(d.tracks, [generation](const std::string& bytes) {
            if (g_album.generation != generation || g_album.draft.tracks.empty()) return;  // closed or reopened
            g_album.preview.Set(bytes);
            g_album.draft.cover = bytes;
        });
    } else if (g_album.buttons[2].In(x, y)) d.cover = std::string();
    else if (g_album.buttons[3].In(x, y)) AlbumMove(-1);
    else if (g_album.buttons[4].In(x, y)) AlbumMove(1);
    else if (g_album.buttons[5].In(x, y)) AlbumRemoveSelected();
    else if (g_album.buttons[6].In(x, y) && d.tracks.size() > 1) AlbumOrderMenu(x, g_album.buttons[6].B());
    else if (g_album.save.In(x, y)) AlbumSave();
    else if (g_album.cancel.In(x, y)) ClosePanel();
    else if (g_album.list.In(x, y)) {
        g_album.focus = kAlbumFields;
        int i = g_album.first + (int)((y - g_album.list.y - S(2)) / S(kAlbumRow));
        if (i < 0 || i >= (int)d.tracks.size()) return AlbumSelect({});
        SDL_Keymod mod = SDL_GetModState();
        if (mod & SDL_KMOD_CTRL) {
            g_album.selected[i] = !g_album.selected[i];
            g_album.caret = i;
        } else if ((mod & SDL_KMOD_SHIFT) && g_album.caret >= 0) {
            std::vector<size_t> rows;
            for (int k = std::min(i, g_album.caret); k <= std::max(i, g_album.caret); ++k) rows.push_back(k);
            int caret = g_album.caret;
            AlbumSelect(rows);
            g_album.caret = caret;
        } else {
            AlbumSelect({(size_t)i});
        }
    }
    (void)clicks;
    g.redraw = true;
}

void AlbumKey(const SDL_KeyboardEvent& e) {
    auto& d = g_album.draft;
    const bool alt = e.mod & SDL_KMOD_ALT, shift = e.mod & SDL_KMOD_SHIFT;
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return AlbumSave();
    if (e.key == SDLK_TAB) return AlbumFocus((g_album.focus + (shift ? kAlbumFields : 1)) % (kAlbumFields + 1));
    if (g_album.focus < kAlbumFields) {
        LineFieldKey(g_album.fields[g_album.focus], e);
        return;
    }
    // The track list.
    if (alt && (e.key == SDLK_UP || e.key == SDLK_DOWN)) return AlbumMove(e.key == SDLK_UP ? -1 : 1);
    if (e.key == SDLK_DELETE) return AlbumRemoveSelected();
    if ((e.mod & SDL_KMOD_CTRL) && e.key == SDLK_A) {
        std::vector<size_t> all(d.tracks.size());
        for (size_t i = 0; i < all.size(); ++i) all[i] = i;
        return AlbumSelect(all);
    }
    if ((e.key == SDLK_UP || e.key == SDLK_DOWN) && !d.tracks.empty()) {
        int to = std::clamp(g_album.caret + (e.key == SDLK_DOWN ? 1 : -1), 0, (int)d.tracks.size() - 1);
        AlbumSelect({(size_t)to});
    }
}

void AlbumText(const std::wstring& text) {
    if (g_album.focus >= kAlbumFields) return;
    std::wstring in;
    const bool numeric = g_album.focus == kAlbumDisc || g_album.focus == kAlbumDiscTotal;
    for (wchar_t c : text)
        if (numeric ? (c >= L'0' && c <= L'9') : c >= 32) in += c;
    LineFieldInsert(g_album.fields[g_album.focus], in);
}

void AlbumWheel(float x, float y, float dy) {
    if (!g_album.list.In(x, y)) return;
    g_album.caret = -1;  // let the list scroll away from the focused row
    g_album.first = std::max(0, g_album.first - (int)std::lround(dy * 3));
}

}  // namespace

void AlbumRelease() {
    g_album.preview.Release();
    uint32_t generation = g_album.generation;
    g_album = AlbumPanel{};
    g_album.generation = generation;
}

void AlbumRenderer(bool restore) {
    if (!restore) g_album.preview.Release();
    else if (!g_album.preview.bytes.empty()) g_album.preview.Set(g_album.preview.bytes);
}

const Ops kAlbumOps{
    .draw = DrawAlbumMaker,
    .mouseDown = AlbumMouseDown,
    .wheel = AlbumWheel,
    .key = AlbumKey,
    .text = [](const char* s) { AlbumText(compat::Widen(s)); },
    .file = [](int purpose, const std::wstring& path) { if (purpose == kFileAlbumCover) AlbumChooseCover(path); },
    .wantsText = [](float x, float y) {
        for (const TextField& t : g_album.fields)
            if (t.rect.In(x, y)) return true;
        return false;
    },
    .close = AlbumRelease,
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OpenAlbumMaker(const std::vector<TrackId>& ids) {
    if (ids.empty()) return;
    ClosePanel();
    AlbumRelease();
    g_album.draft = DraftAlbum(g_lib, ids);
    const auto& d = g_album.draft;
    const std::wstring values[] = {d.title, d.artist, d.date, d.genre, d.disc ? std::to_wstring(d.disc) : L"",
                                   d.discTotal ? std::to_wstring(d.discTotal) : L""};
    for (int f = 0; f < kAlbumFields; ++f) g_album.fields[f].text = values[f];
    for (TrackId id : d.tracks)
        if (std::string bytes = TrackCoverBytes(g_lib.Path(id)); !bytes.empty()) {
            g_album.preview.Set(bytes);
            break;
        }
    g_album.selected.assign(d.tracks.size(), 0);
    g_kind = Kind::Album;
    AlbumFocus(kAlbumTitle);
}

}  // namespace lx
