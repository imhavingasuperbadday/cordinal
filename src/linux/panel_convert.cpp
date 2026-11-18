// Library > Convert... on Linux, as app_convert.cpp's window on Windows: the format, copies or
// replacements, where the copies go, and a live count of what will convert. The run itself is
// convert_run.cpp's.
#include "panels_internal.h"

namespace lx::panel {
namespace {

struct ConvertPanel {
    std::vector<TrackId> ids;
    std::vector<ConvertItem> plan;
    TextField folder;
    CordExport format = CordExport::Flac;
    bool replace = false, intoFolder = false;
    bool dirty = true;
    uint64_t editedAt = 0;
} g_conv;

constexpr float kConvertW = 720, kConvertH = 340;
const char* const kModes[] = {"Copies, and keep the originals", "Replacements for the originals in the library"};
const char* const kPlaces[] = {"Next to their originals", "In this folder"};

struct ConvertGeometry {
    Rect panel, format, mode, place, folder, browse, help, summary, go, cancel;
};

ConvertGeometry Geom() {
    ConvertGeometry G;
    const Rect out = OutputRect();
    const float w = std::min(S(kConvertW), out.w - S(16)), h = std::min(S(kConvertH), out.h - S(16));
    const Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    G.panel = p;
    const float rh = RowH(), pad = S(16), lw = S(90), top = p.y + S(48), x = p.x + pad + lw;
    G.format = {x, top, S(300), rh - S(6)};
    G.mode = {x, top + rh, S(380), rh - S(6)};
    G.place = {x, top + 2 * rh, S(240), rh - S(6)};
    G.folder = {x, top + 3 * rh, p.R() - pad - x - S(104), rh - S(4)};
    G.browse = {p.R() - pad - S(96), top + 3 * rh, S(96), rh - S(4)};
    G.help = {x, top + 4 * rh + S(4), p.R() - pad - x, 3 * LineHeight(Font::Normal) + S(6)};
    const float footerY = p.B() - S(46);
    G.summary = {p.x + pad, footerY, p.w - 2 * pad - S(230), rh};
    G.cancel = {p.R() - pad - S(96), footerY, S(96), rh};
    G.go = {G.cancel.x - S(116), footerY, S(108), rh};
    return G;
}

bool IntoFolder() { return !g_conv.replace && g_conv.intoFolder; }

std::wstring Folder() {
    std::wstring d = Trimmed(g_conv.folder.text);
    while (d.size() > 1 && d.back() == L'/') d.pop_back();
    return d;
}

void Replan() {
    g_conv.plan = PlanConvertTracks(g_conv.ids, g_conv.format, g_conv.replace, IntoFolder() ? Folder() : std::wstring());
    g_conv.dirty = false;
    g.redraw = true;
}

size_t Ready() {
    return (size_t)std::count_if(g_conv.plan.begin(), g_conv.plan.end(), [](const ConvertItem& i) { return i.skip.empty(); });
}

bool CanStart() { return !g_conv.dirty && Ready() && !ConvertBusy() && !(IntoFolder() && Folder().empty()); }

void Edited() {
    g_conv.dirty = true;
    g_conv.editedAt = SDL_GetTicks();
    g.redraw = true;
}

void FocusFolder() {
    TextField& t = g_conv.folder;
    t.caret = t.text.size();
    t.all = false;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

// Saves the choices, makes the copies' folder, and hands the plan to convert_run.cpp, which asks
// before replacing; the panel closes once the run starts.
void Start() {
    if (g_conv.dirty) Replan();
    if (!CanStart()) return;
    if (TagWriteBusy() || OrganizeBusy()) {
        ShowMessage(L"Wait for the running tag write or organize to finish.");
        return;
    }
    g.settings.convertFormat = (uint8_t)g_conv.format;
    g.settings.convertReplace = g_conv.replace;
    g.settings.convertIntoFolder = g_conv.intoFolder;
    g.settings.convertFolder = Folder();
    SaveSettings();
    if (IntoFolder()) {
        SHCreateDirectoryExW(nullptr, Folder().c_str(), nullptr);
        const DWORD a = GetFileAttributesW(Folder().c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Convert", ("Couldn't open or create " + Utf8(Folder())).c_str(), g.window);
            return;
        }
    }
    Replan();
    if (!CanStart()) return;
    if (StartConvert(std::move(g_conv.plan), g_conv.format, g_conv.replace)) ClosePanel();
    else Replan();
}

void MouseDown(float x, float y) {
    const ConvertGeometry G = Geom();
    if (G.go.In(x, y)) return Start();
    if (G.cancel.In(x, y)) return ClosePanel();
    if (G.format.In(x, y)) {
        std::vector<std::string> labels;
        for (const CordExportInfo& f : kCordExports) labels.push_back(Utf8(f.label));
        OpenChoice(G.format.x, G.format.B(), labels, (int)g_conv.format, [](int f) {
            g_conv.format = (CordExport)f;
            Replan();
        });
        return;
    }
    if (G.mode.In(x, y)) {
        OpenChoice(G.mode.x, G.mode.B(), {std::begin(kModes), std::end(kModes)}, g_conv.replace, [](int m) {
            g_conv.replace = m == 1;
            Replan();
        });
        return;
    }
    if (G.place.In(x, y) && !g_conv.replace) {
        OpenChoice(G.place.x, G.place.B(), {std::begin(kPlaces), std::end(kPlaces)}, g_conv.intoFolder, [](int w) {
            g_conv.intoFolder = w == 1;
            Replan();
        });
        return;
    }
    if (!IntoFolder()) return;
    if (G.folder.In(x, y)) return FocusFolder();
    if (G.browse.In(x, y)) PickFolder(kFileConvertDest, compat::Narrow(Folder().c_str()));
}

void Key(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return Start();
    if (IntoFolder() && LineFieldKey(g_conv.folder, e) == 1) Edited();
}

void Text(const char* s) {
    if (IntoFolder() && LineFieldInsert(g_conv.folder, Printable(compat::Widen(s)))) Edited();
}

void Draw(const Palette& p) {
    if (g_conv.dirty && SDL_GetTicks() - g_conv.editedAt >= kReplanDelay) Replan();
    const ConvertGeometry G = Geom();
    const size_t n = g_conv.ids.size();
    DrawFrame(p, S(kConvertW), S(kConvertH), ("Convert " + std::to_string(n) + (n == 1 ? " song" : " songs")).c_str());
    const float pad = S(16), lh = LineHeight(Font::Normal);
    const CordExportInfo& info = kCordExports[(unsigned)g_conv.format];
    DrawText(Font::Normal, "Format", G.panel.x + pad, TextY(G.format), p.sub);
    DrawButton(G.format, Utf8(info.label), p);
    DrawText(Font::Normal, "Make", G.panel.x + pad, TextY(G.mode), p.sub);
    DrawButton(G.mode, kModes[g_conv.replace], p);
    DrawText(Font::Normal, "Copies go", G.panel.x + pad, TextY(G.place), p.sub);
    DrawButton(G.place, kPlaces[g_conv.intoFolder], p, false, !g_conv.replace);
    DrawLineField(g_conv.folder, G.folder, IntoFolder(), IntoFolder(), "The folder the copies go in", p);
    DrawButton(G.browse, "Browse...", p, false, IntoFolder());
    const char* help = g_conv.replace
        ? "The library switches to each new file once it's checked, with its plays, ratings and playlists; the originals "
          "go to the trash or stay, as set in Preferences. Library > Undo last conversion brings them back."
        : "Copies get the originals' tags and covers, and the originals and the library stay as they are.";
    const char* more = info.kbps ? "MP3 and AAC copies are 48 kHz stereo, and play without gaps."
                                 : "Lossless files start from FLAC, WAV, AIFF, ALAC, WavPack, Monkey's Audio, TTA or .cord.";
    DrawText(Font::Normal, help, G.help.x, G.help.y, p.sub, G.help.w);
    DrawText(Font::Normal, more, G.help.x, G.help.y + 2 * (lh + S(2)), p.sub, G.help.w);
    const std::wstring summary = g_conv.dirty                     ? L"Updating..."
                                 : IntoFolder() && Folder().empty() ? L"Choose a folder for the copies."
                                                                    : ConvertSummary(g_conv.plan);
    DrawText(Font::Normal, Utf8(summary), G.summary.x, TextY(G.summary), p.sub, G.summary.w);
    DrawButton(G.go, "Convert", p, true, CanStart());
    DrawButton(G.cancel, "Cancel", p);
}

}  // namespace

bool ConvertPanelDirty() { return g_kind == Kind::Convert && g_conv.dirty; }

const Ops kConvertOps{
    .draw = Draw,
    .mouseDown = [](float x, float y, int) { MouseDown(x, y); },
    .key = Key,
    .text = Text,
    .file = [](int purpose, const std::wstring& path) {
        if (purpose != kFileConvertDest) return;
        g_conv.folder.text = path;
        g_conv.intoFolder = true;
        FocusFolder();
        Edited();
    },
    .wantsText = [](float x, float y) { return IntoFolder() && Geom().folder.In(x, y); },
    .close = [] { g_conv = ConvertPanel{}; },
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

// Library > Convert...: the given tracks, or the whole library when there are none.
void OpenConvert(std::vector<TrackId> ids) {
    if (ConvertBusy()) {
        ShowMessage(L"A conversion is already running.");
        return;
    }
    if (ids.empty())
        for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
            if (g_lib[t].InLibrary()) ids.push_back(t);
    if (ids.empty()) {
        ShowMessage(L"Nothing to convert.");
        return;
    }
    g_conv = ConvertPanel{};
    g_conv.ids = std::move(ids);
    g_conv.format = (CordExport)std::min<int>(g.settings.convertFormat, (int)CordExport::Count - 1);
    g_conv.replace = g.settings.convertReplace;
    g_conv.intoFolder = g.settings.convertIntoFolder;
    g_conv.folder.text = g.settings.convertFolder;
    Replan();
    g_kind = Kind::Convert;
    if (IntoFolder()) FocusFolder();
}

}  // namespace lx
