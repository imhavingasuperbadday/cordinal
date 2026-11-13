// The Linux window's dialogs, drawn over it with the SDL renderer: which one is open, the pieces they
// share, and the dispatch of every event to the open one. Each dialog lives in its own file and exports a
// table of handlers (panels_internal.h lists them); the dialogs drawn elsewhere get theirs here. They do
// what the Windows build's dialogs do, with the same wording. One is open at a time, and while it is, it
// takes the mouse and keyboard.
#include "panels_internal.h"

#include "album_editions.h"
#include "cover.h"

#include <cstring>

namespace lx::panel {

Kind g_kind = Kind::None;
float g_mx = -1, g_my = -1;

Rect OutputRect() {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    return {0, 0, (float)pw, (float)ph};
}

void DrawCheck(const Rect& r, const std::string& label, int state, const Palette& p) {
    float box = S(14);
    Rect b{r.x, std::round(r.y + (r.h - box) / 2), box, box};
    if (state) Fill(b, p.accent);
    else {
        Fill(b, r.In(g_mx, g_my) ? p.hover : p.surface2);
        Frame(b, p.sub);
    }
    uint32_t mark = TextOn(p.accent);
    if (state == 1) {
        ThickLine(b.x + box * 0.22f, b.y + box * 0.52f, b.x + box * 0.42f, b.y + box * 0.72f, S(1.6f), mark);
        ThickLine(b.x + box * 0.42f, b.y + box * 0.72f, b.x + box * 0.80f, b.y + box * 0.30f, S(1.6f), mark);
    } else if (state == 2) {
        Fill({b.x + box * 0.25f, b.y + box * 0.45f, box * 0.5f, std::max(1.0f, S(2))}, mark);
    }
    DrawText(Font::Normal, label, b.R() + S(8), TextY(r), p.text, r.R() - b.R() - S(8));
}

// Both dialogs answer on SDL's thread; the path crosses to the UI thread as WM_APP_FILE.
static void PostChosen(void* purpose, const char* const* list, int) {
    if (!list || !*list) return;
    auto* path = new std::wstring(compat::Widen(*list));
    if (!PostMessageW(nullptr, WM_APP_FILE, (WPARAM)(intptr_t)purpose, (LPARAM)path)) delete path;
}

void ChooseFile(int purpose, const SDL_DialogFileFilter& filter) {
    SDL_ShowOpenFileDialog(PostChosen, (void*)(intptr_t)purpose, g.window, &filter, 1, nullptr, false);
}

void PickFolder(int purpose, const std::string& start) {
    SDL_ShowOpenFolderDialog(PostChosen, (void*)(intptr_t)purpose, g.window, start.empty() ? nullptr : start.c_str(), false);
}

std::string ReadSmallFile(const std::wstring& path, size_t max) {
    std::string bytes;
    if (FILE* f = fopen(compat::Narrow(path.c_str()).c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() <= max) bytes.append(buf, n);
        fclose(f);
    }
    return bytes.size() > max ? std::string() : bytes;
}

std::string ReadCoverFile(const std::wstring& path) {
    std::string bytes = ReadSmallFile(path, 16 * 1024 * 1024);
    bool jpeg = bytes.size() >= 8 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xD8;
    bool png = bytes.size() >= 8 && bytes.compare(0, 4, "\x89PNG") == 0;
    if (jpeg || png) return bytes;
    ShowMessage(L"Covers have to be JPEG or PNG images.");
    return {};
}

void CoverPreview::Set(const std::string& image) {
    bytes = image;
    Release();
    CoverImage img{};
    if (bytes.empty() || !DecodeCover(bytes, 320, img)) return;
    texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.width, img.height);
    if (!texture) return;
    SDL_UpdateTexture(texture, nullptr, img.rgba.data(), img.width * 4);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    w = (float)img.width;
    h = (float)img.height;
}

void CoverPreview::Release() {
    if (texture) SDL_DestroyTexture(texture);
    texture = nullptr;
}

void CoverPreview::Draw(const Rect& box) const {
    float s = (box.w - S(2)) / std::max(w, h);
    SDL_FRect dst{box.x + (box.w - w * s) / 2, box.y + (box.h - h * s) / 2, w * s, h * s};
    SDL_RenderTexture(g.renderer, texture, nullptr, &dst);
}

int LineFieldKey(TextField& t, const SDL_KeyboardEvent& e) {
    if ((e.mod & SDL_KMOD_CTRL) && e.key == SDLK_A) {
        t.all = !t.text.empty();
        t.caret = t.text.size();
        g.redraw = true;
        return 0;
    }
    int changed = 0;
    if (e.key == SDLK_BACKSPACE || e.key == SDLK_DELETE) {
        if (t.all) t.text.clear();
        else if (e.key == SDLK_BACKSPACE && t.caret) t.text.erase(--t.caret, 1);
        else if (e.key == SDLK_DELETE && t.caret < t.text.size()) t.text.erase(t.caret, 1);
        t.caret = std::min(t.caret, t.text.size());
        changed = 1;
    } else if (e.key == SDLK_LEFT) {
        t.caret = t.all || !t.caret ? 0 : t.caret - 1;
    } else if (e.key == SDLK_RIGHT) {
        t.caret = t.all ? t.text.size() : std::min(t.caret + 1, t.text.size());
    } else if (e.key == SDLK_HOME) {
        t.caret = 0;
    } else if (e.key == SDLK_END) {
        t.caret = t.text.size();
    } else {
        return -1;
    }
    t.all = false;
    g.redraw = true;
    return changed;
}

bool LineFieldInsert(TextField& t, const std::wstring& in) {
    if (in.empty()) return false;
    if (t.all) {
        t.text.clear();
        t.caret = 0;
    }
    t.all = false;
    t.text.insert(t.caret, in);
    t.caret += in.size();
    g.redraw = true;
    return true;
}

std::wstring Printable(const std::wstring& s) {
    std::wstring in;
    for (wchar_t c : s)
        if (c >= 32) in += c;
    return in;
}

void DrawLineField(TextField& t, const Rect& r, bool focused, bool enabled, const char* cue, const Palette& p) {
    Fill(r, enabled ? p.surface2 : p.surface);
    Frame(r, focused ? p.accent : p.border);
    const float pad = S(5), lineH = LineHeight(Font::Normal);
    SDL_Rect clip{(int)(r.x + S(2)), (int)(r.y + S(1)), (int)(r.w - S(4)), (int)(r.h - S(2))};
    SDL_SetRenderClipRect(g.renderer, &clip);
    if (t.text.empty() && !focused && cue) DrawText(Font::Normal, cue, r.x + pad, TextY(r), p.faint);
    const std::string all = Utf8(t.text);
    const float caretX = TextWidth(Font::Normal, Utf8(t.text.substr(0, t.caret))), room = r.w - 2 * pad;
    if (caretX - t.scrollX > room) t.scrollX = caretX - room;
    if (caretX < t.scrollX) t.scrollX = caretX;
    const float tx = r.x + pad - t.scrollX, ty = TextY(r);
    if (focused && t.all) Fill({tx, r.y + S(3), TextWidth(Font::Normal, all), r.h - S(6)}, p.select);
    DrawText(Font::Normal, all, tx, ty, focused && t.all ? p.selectText : p.text);
    if (focused && enabled && SDL_GetTicks() % 1060 < 530) Fill({std::round(tx + caretX), ty, S(1), lineH}, p.text);
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

std::wstring Trimmed(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t"), e = s.find_last_not_of(L" \t");
    return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

float RowH() { return std::round(LineHeight(Font::Normal) + S(12)); }

namespace {

// The cover picker can open over the tag editor or the album maker, which wait under it unchanged.
bool g_coverOverTags = false, g_coverOverAlbum = false;

void CloseCovers() {
    CoverPanelClose();
    if (g_coverOverTags) TagsForget();      // the tag editor underneath closes with it, unsaved
    if (g_coverOverAlbum) AlbumRelease();  // so does the album maker
    g_coverOverTags = g_coverOverAlbum = false;
}

// The panels other files draw.
const Ops kHeadphonesOps{
    .draw = DrawHeadphones,
    .mouseDown = [](float x, float y, int) { HeadphonesMouseDown(x, y); },
    .wheel = [](float, float, float dy) { HeadphonesWheel(dy); },
    .key = HeadphonesKey,
};
const Ops kGamesOps{
    .draw = [](const Palette& p) { DrawGames(p, g_mx, g_my); },
    .mouseDown = [](float x, float y, int) { GamesMouseDown(x, y); },
    .key = GamesKey,
    .text = [](const char* s) { GamesText(compat::Widen(s)); },
    .wantsText = GamesWantsText,
    .close = CloseGames,
    .typing = true,
};
const Ops kEditionsOps{
    .draw = DrawEditionsPanel,
    .mouseDown = [](float x, float y, int clicks) { EditionsMouseDown(x, y, SDL_BUTTON_LEFT, clicks); },
    .wheel = EditionsWheel,
    .key = EditionsKey,
    .text = [](const char* s) { EditionsText(s); },
    .wantsText = EditionsWantsText,
    .close = EditionsClose,
    .typing = true,
};
const Ops kCoversOps{
    .draw = [](const Palette& p) { DrawCoverPanel(p, g_mx, g_my); },
    .mouseDown = [](float x, float y, int clicks) { CoverPanelMouseDown(x, y, SDL_BUTTON_LEFT, clicks); },
    .wheel = CoverPanelWheel,
    .key = CoverPanelKey,
    .text = CoverPanelText,
    .file = [](int purpose, const std::wstring& path) { if (purpose == kFileCoverPick) CoverPanelFile(path); },
    .wantsText = CoverPanelWantsText,
    .close = CloseCovers,
    .typing = true,
};
const Ops kArtistOps{
    .draw = DrawArtistPanel,
    .mouseDown = [](float x, float y, int) { ArtistPanelMouseDown(x, y); },
    .wheel = ArtistPanelWheel,
    .key = ArtistPanelKey,
    .close = ArtistPanelClose,
};
// Export's close runs last of all, from ClosePanel: Export... goes on to the save dialog.
const Ops kExportOps{
    .draw = DrawExportPanel,
    .mouseDown = [](float x, float y, int) { ExportPanelMouseDown(x, y); },
    .wheel = ExportPanelWheel,
    .key = ExportPanelKey,
};

const Ops* const kOps[] = {
    nullptr, &kEqOps, &kHeadphonesOps, &kTagsOps, &kPrefsOps, &kRulesOps, &kOrganizeOps, &kCopyOps, &kListenBrainzOps,
    &kFormatOps, &kDeviceOps, &kPointsOps, &kReviewOps, &kPaletteOps, &kLayoutOps, &kPacksOps, &kHelpersOps, &kGamesOps,
    &kEditionsOps, &kCoversOps, &kArtistOps, &kExportOps, &kAlbumOps,
};
static_assert(std::size(kOps) == (size_t)Kind::Count);

const Ops* OpenOps() { return kOps[(size_t)g_kind]; }

}  // namespace
}  // namespace lx::panel

namespace lx {
using namespace panel;

// The dimmed window behind a dialog, and the dialog's own surface, centered.
Rect DrawFrame(const Palette& p, float w, float h, const char* title) {
    Rect out = OutputRect();
    Fill(out, p.bg, 190);
    w = std::min(out.w - S(16), w);
    h = std::min(out.h - S(16), h);
    Rect panel{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    Fill(panel, p.surface);
    Frame(panel, p.border);
    DrawText(Font::Title, title, panel.x + S(16), panel.y + S(12), p.text);
    return panel;
}

void DrawButton(const Rect& r, const std::string& label, const Palette& p, bool accent, bool enabled) {
    bool hot = enabled && r.In(g_mx, g_my);
    if (accent && enabled) Fill(r, hot ? p.accent2 : p.accent);
    else {
        Fill(r, hot ? p.hover : p.surface2);
        Frame(r, p.border);
    }
    uint32_t color = !enabled ? p.faint : accent ? TextOn(hot ? p.accent2 : p.accent) : p.text;
    DrawText(Font::Normal, label, r.x, TextY(r), color, r.w, 1);
}

std::vector<std::string> Wrap(Font font, const std::string& text, float width) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find(' ', i);
        std::string word = text.substr(i, end == std::string::npos ? std::string::npos : end - i);
        std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextWidth(font, next) > width) {
            lines.push_back(line);
            line = word;
        } else line = next;
        i = end == std::string::npos ? text.size() : end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

void OpenHeadphones() {
    ClosePanel();
    g_kind = Kind::Headphones;
    PrepareHeadphonesPanel();
    g.redraw = true;
}

void OpenGames(int which) {
    if (g_kind != Kind::Games) {
        ClosePanel();
        g_kind = Kind::Games;
        SDL_StartTextInput(g.window);  // typing guesses
    }
    ShowGame(which);
    g.redraw = true;
}

bool GamesOpen() { return g_kind == Kind::Games; }

void OpenAlbumEditions(uint32_t group) {
    if (!FindAlbumGroupConst(group)) return;
    ClosePanel();
    EditionsOpenManager(group);
    g_kind = Kind::Editions;
    g.redraw = true;
}

void OpenAlbumEditionComparison(uint32_t group, TrackId focusTrack) {
    if (!FindAlbumGroupConst(group)) return;
    ClosePanel();
    EditionsOpenComparison(group, focusTrack);
    g_kind = Kind::Editions;
    g.redraw = true;
}

void ResetPanelRenderer(bool restore) {
    TagsRenderer(restore);
    AlbumRenderer(restore);
    CoverPanelRenderer(restore);
    ResetGameRenderer(restore);
}

bool PanelOpen() { return g_kind != Kind::None; }

void ClosePanel() {
    const Ops* ops = OpenOps();
    if (ops && ops->close) ops->close();
    if (ops && ops->typing) SDL_StopTextInput(g.window);
    const bool exporting = g_kind == Kind::Export;
    g_kind = Kind::None;
    g.redraw = true;
    if (exporting) ExportPanelClose();  // last: Export... goes on to the save dialog
}

void ShowCoverPanel(bool overTags) {
    if (overTags && g_kind == Kind::Tags) {
        g_coverOverTags = true;  // not ClosePanel: the editor's fields stay as they are
    } else if (overTags && g_kind == Kind::Album) {
        g_coverOverAlbum = true;  // the album maker waits underneath in the same way
    } else if (g_kind != Kind::Covers) {
        ClosePanel();
        g_coverOverTags = g_coverOverAlbum = false;
    }
    g_kind = Kind::Covers;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

void ShowExportPanel() {
    if (g_kind != Kind::Export) ClosePanel();
    g_kind = Kind::Export;
    g.redraw = true;
}

void ShowArtistPanel() {
    if (g_kind != Kind::Artist) ClosePanel();
    g_kind = Kind::Artist;
    g.redraw = true;
}

void CoverPanelDone() {
    if (g_kind != Kind::Covers) return;
    if (g_coverOverTags || g_coverOverAlbum) {
        CoverPanelClose();
        g_kind = g_coverOverTags ? Kind::Tags : Kind::Album;
        g_coverOverTags = g_coverOverAlbum = false;
        g.redraw = true;
        return;
    }
    ClosePanel();
}

void DrawPanel(const Palette& p, float mouseX, float mouseY) {
    g_mx = mouseX;
    g_my = mouseY;
    if (const Ops* ops = OpenOps()) ops->draw(p);
}

void PanelMouseDown(float x, float y, int button, int clicks) {
    g_mx = x;
    g_my = y;
    g.redraw = true;
    if (button != SDL_BUTTON_LEFT) return;
    if (const Ops* ops = OpenOps(); ops && ops->mouseDown) ops->mouseDown(x, y, clicks);
}

void PanelMouseMove(float x, float y) {
    g_mx = x;
    g_my = y;
    g.redraw = true;
    if (const Ops* ops = OpenOps(); ops && ops->mouseMove) ops->mouseMove(x, y);
}

void PanelMouseUp() {
    if (const Ops* ops = OpenOps(); ops && ops->mouseUp) ops->mouseUp();
    g.redraw = true;
}

void PanelWheel(float x, float y, float dy) {
    if (const Ops* ops = OpenOps(); ops && ops->wheel) ops->wheel(x, y, dy);
    g.redraw = true;
}

void PanelKey(const SDL_KeyboardEvent& e) {
    if (const Ops* ops = OpenOps(); ops && ops->key) ops->key(e);
}

void PanelText(const char* text) {
    if (const Ops* ops = OpenOps(); text && ops && ops->text) ops->text(text);
}

void PanelFileChosen(int purpose, const std::wstring& path) {
    if (purpose == kFileCoverDirect) CoverFileChosen(path);  // the track menu's, with no panel open
    else if (const Ops* ops = OpenOps(); ops && ops->file) ops->file(purpose, path);
    g.redraw = true;
}

bool PanelWantsText(float x, float y) {
    const Ops* ops = OpenOps();
    return ops && ops->wantsText && ops->wantsText(x, y);
}

}  // namespace lx
