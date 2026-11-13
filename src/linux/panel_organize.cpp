// Library > Organize files and Library > Copy to folder or device on Linux, as the Windows build's
// app_organize.cpp and app_copy.cpp: a destination and a naming pattern with a preview of every file, the
// work itself done by organize_run.cpp and copy_run.cpp.
#include "panels_internal.h"

#include <optional>
#include <sys/statvfs.h>
#include <unordered_map>

namespace lx::panel {
namespace {

// Organize files: the folder and naming pattern, a preview of every move (replanned once typing pauses),
// and the move itself on organize_run.cpp's worker.
struct OrganizePanel {
    std::vector<TrackId> ids;
    std::vector<OrganizeMove> plan;
    TextField root, pattern;
    int focus = 0;  // 0 the folder, 1 the pattern
    int first = 0;  // the first preview row shown
    size_t moving = 0, inPlace = 0, blocked = 0;
    bool dirty = true;
    uint64_t editedAt = 0;
} g_org;

constexpr float kOrganizeW = 900, kOrganizeH = 560;

struct OrganizeGeometry {
    Rect panel, root, browse, pattern, reset, help, list, summary, go, cancel;
    int visible = 0;
};

OrganizeGeometry OrgGeometry() {
    OrganizeGeometry G;
    Rect out = OutputRect();
    float w = std::min(S(kOrganizeW), out.w - S(16)), h = std::min(S(kOrganizeH), out.h - S(16));
    Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    G.panel = p;
    const float rh = RowH(), pad = S(16), lw = S(70), top = p.y + S(48);
    G.root = {p.x + pad + lw, top, p.w - 2 * pad - lw - S(104), rh - S(4)};
    G.browse = {p.R() - pad - S(96), top, S(96), rh - S(4)};
    G.pattern = {G.root.x, top + rh, G.root.w, rh - S(4)};
    G.reset = {G.browse.x, top + rh, S(96), rh - S(4)};
    G.help = {G.root.x, top + 2 * rh, p.R() - pad - G.root.x, 2 * LineHeight(Font::Normal) + S(4)};
    const float footerY = p.B() - S(46), listTop = G.help.B() + S(10) + rh;  // under a heading row
    G.list = {p.x + pad, listTop, p.w - 2 * pad, std::max(rh, footerY - S(10) - listTop)};
    G.visible = std::max(1, (int)std::floor(G.list.h / rh));
    G.summary = {p.x + pad, footerY, p.w - 2 * pad - S(230), rh};
    G.cancel = {p.R() - pad - S(96), footerY, S(96), rh};
    G.go = {G.cancel.x - S(116), footerY, S(108), rh};
    return G;
}

void Replan() {
    g_org.plan = PlanOrganize(g_org.ids, Trimmed(g_org.root.text), g_org.pattern.text);
    g_org.moving = g_org.inPlace = g_org.blocked = 0;
    for (OrganizeMove& m : g_org.plan) {
        if (m.skip.empty() && m.id == g.playing) m.skip = L"playing now";
        if (m.skip.empty()) ++g_org.moving;
        else if (m.skip == L"already in place") ++g_org.inPlace;
        else ++g_org.blocked;
    }
    g_org.first = std::clamp(g_org.first, 0, std::max(0, (int)g_org.plan.size() - 1));
    g_org.dirty = false;
}

void OrganizeEdited() {
    g_org.dirty = true;
    g_org.editedAt = SDL_GetTicks();
    g.redraw = true;
}

void OrganizeFocus(int f) {
    g_org.focus = f;
    TextField& t = f ? g_org.pattern : g_org.root;
    t.caret = t.text.size();
    t.all = false;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

void OrganizeStart() {
    const std::wstring root = Trimmed(g_org.root.text);
    if (g_org.dirty) Replan();
    if (root.empty() || !g_org.moving || OrganizeBusy()) return;
    g.settings.organizeFolder = root;
    g.settings.organizePattern = g_org.pattern.text;
    SaveSettings();
    StartOrganize(g_org.plan, root);
    if (OrganizeBusy()) ClosePanel();
}

void OrganizeMouseDown(float x, float y) {
    const OrganizeGeometry G = OrgGeometry();
    if (G.go.In(x, y)) return OrganizeStart();
    if (G.cancel.In(x, y)) return ClosePanel();
    if (G.root.In(x, y)) return OrganizeFocus(0);
    if (G.pattern.In(x, y)) return OrganizeFocus(1);
    if (G.reset.In(x, y)) {
        g_org.pattern.text = kDefaultOrganizePattern;
        OrganizeFocus(1);
        return OrganizeEdited();
    }
    if (G.browse.In(x, y)) PickFolder(kFileOrganizeRoot, compat::Narrow(Trimmed(g_org.root.text).c_str()));
}

void OrganizeKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return OrganizeStart();
    if (e.key == SDLK_TAB) return OrganizeFocus(1 - g_org.focus);
    if (LineFieldKey(g_org.focus ? g_org.pattern : g_org.root, e) == 1) OrganizeEdited();
}

void OrganizeText(const char* s) {
    if (LineFieldInsert(g_org.focus ? g_org.pattern : g_org.root, Printable(compat::Widen(s)))) OrganizeEdited();
}

void DrawOrganize(const Palette& p) {
    if (g_org.dirty && SDL_GetTicks() - g_org.editedAt >= kReplanDelay) Replan();
    const OrganizeGeometry G = OrgGeometry();
    const size_t n = g_org.ids.size();
    DrawFrame(p, S(kOrganizeW), S(kOrganizeH), ("Organize " + std::to_string(n) + (n == 1 ? " file" : " files")).c_str());
    const float pad = S(16), lh = LineHeight(Font::Normal);
    DrawText(Font::Normal, "Folder", G.panel.x + pad, TextY(G.root), p.sub);
    DrawLineField(g_org.root, G.root, g_org.focus == 0, true, "The folder to organize into", p);
    DrawButton(G.browse, "Browse...", p);
    DrawText(Font::Normal, "Names", G.panel.x + pad, TextY(G.pattern), p.sub);
    DrawLineField(g_org.pattern, G.pattern, g_org.focus == 1, true, nullptr, p);
    DrawButton(G.reset, "Default", p);
    DrawText(Font::Normal, "Title formatting: %album artist%, %album%, %title%, %tracknumber%, %discnumber%, %year%, %genre% ...",
             G.help.x, G.help.y, p.sub, G.help.w);
    DrawText(Font::Normal, "A backslash starts a new folder; [ ] is left out when its fields are empty. The extension is kept.",
             G.help.x, G.help.y + lh + S(2), p.sub, G.help.w);

    // The preview: From, To (shown under the chosen folder) and what happens to each file.
    const float rh = RowH(), statusW = S(130), half = std::floor((G.list.w - statusW) / 2);
    const float cols[] = {G.list.x + S(6), G.list.x + half + S(6), G.list.x + 2 * half + S(6)};
    const Rect heading{G.list.x, G.list.y - rh, G.list.w, rh};
    DrawText(Font::Normal, "From", cols[0], TextY(heading), p.sub);
    DrawText(Font::Normal, "To", cols[1], TextY(heading), p.sub);
    DrawText(Font::Normal, "Status", cols[2], TextY(heading), p.sub);
    Fill(G.list, p.surface2);
    Frame(G.list, p.border);
    const std::wstring root = Trimmed(g_org.root.text);
    for (int k = 0; k < G.visible; ++k) {
        const int i = g_org.first + k;
        if (i >= (int)g_org.plan.size()) break;
        const OrganizeMove& m = g_org.plan[i];
        const Rect row{G.list.x, G.list.y + k * rh, G.list.w, rh};
        std::wstring to = m.to;
        if (!root.empty() && to.size() > root.size() && to.compare(0, root.size(), root) == 0) to = L"\x2026" + to.substr(root.size());
        const uint32_t color = m.skip.empty() ? p.text : p.sub;
        DrawText(Font::Normal, Utf8(m.from), cols[0], TextY(row), color, half - S(12));
        DrawText(Font::Normal, Utf8(to), cols[1], TextY(row), color, half - S(12));
        DrawText(Font::Normal, m.skip.empty() ? std::string("move") : Utf8(m.skip), cols[2], TextY(row), color, statusW - S(12));
    }

    std::string status = std::to_string(g_org.moving) + (g_org.moving == 1 ? " file will move" : " files will move");
    if (g_org.inPlace) status += "  \xE2\x80\xA2  " + std::to_string(g_org.inPlace) + " already in place";
    if (g_org.blocked) status += "  \xE2\x80\xA2  " + std::to_string(g_org.blocked) + " can't move";
    if (root.empty()) status = "Choose the folder to organize into.";
    else if (g_org.dirty) status = "Updating the preview...";
    DrawText(Font::Normal, status, G.summary.x, TextY(G.summary), p.sub, G.summary.w);
    DrawButton(G.go, "Organize", p, true, !g_org.dirty && g_org.moving && !root.empty() && !OrganizeBusy());
    DrawButton(G.cancel, "Cancel", p);
}

// Copy to folder or device: the destination (a folder, a memory card, or a phone's mounted folder), an
// Organize naming pattern, the format, the playlist's options, and a preview of every file, replanned once
// typing pauses. The copy runs on copy_run.cpp's worker.
struct CopyPanel {
    std::vector<TrackId> ids;
    std::wstring set, playlistName;  // the playlist's name, or kCopySelectionSet for a selection
    std::wstring manifestFor;        // the destination whose manifest is read (once, not per keystroke)
    std::vector<CopyRecord> manifest;
    std::vector<CopyItem> plan;
    TextField dest, pattern;
    CopyFormat format = CopyFormat::Original;
    bool writePlaylist = false, removeStale = false;
    int focus = 0;  // 0 the destination, 1 the pattern
    int first = 0;
    size_t copying = 0, converting = 0, there = 0, blocked = 0, stale = 0;
    uint64_t bytes = 0;
    bool dirty = true;
    uint64_t editedAt = 0;
} g_copy;

const char* const kCopyFormats[] = {"Keep the original files", "Lossless to AAC 192 kb/s (.m4a)", "Lossless to AAC 128 kb/s (.m4a)"};
static_assert(std::size(kCopyFormats) == (size_t)CopyFormat::Count);
constexpr float kCopyW = 940, kCopyH = 580;

bool PlaylistCopy() { return g_copy.set != kCopySelectionSet; }

// The destination's free space for the summary, asked of the file system again when the destination
// changes or after a second: the summary is drawn on every redraw, and a phone's mount can be slow to answer.
std::optional<uint64_t> FreeSpace(const std::wstring& dest) {
    static std::wstring at;
    static uint64_t askedAt = 0;
    static std::optional<uint64_t> free;
    const uint64_t now = SDL_GetTicks();
    if (dest != at || now - askedAt >= 1000) {
        at = dest;
        askedAt = now;
        free.reset();
        struct statvfs fs;
        if (!dest.empty() && statvfs(compat::Narrow(dest.c_str()).c_str(), &fs) == 0)
            free = (uint64_t)fs.f_bavail * fs.f_frsize;
    }
    return free;
}

struct CopyGeometry {
    Rect panel, dest, browse, pattern, reset, format, playlist, stale, help, list, summary, go, cancel;
    int visible = 0;
};

CopyGeometry CopyGeom() {
    CopyGeometry G;
    Rect out = OutputRect();
    float w = std::min(S(kCopyW), out.w - S(16)), h = std::min(S(kCopyH), out.h - S(16));
    Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    G.panel = p;
    const float rh = RowH(), pad = S(16), lw = S(70), top = p.y + S(48), x = p.x + pad + lw;
    G.dest = {x, top, p.w - 2 * pad - lw - S(104), rh - S(4)};
    G.browse = {p.R() - pad - S(96), top, S(96), rh - S(4)};
    G.pattern = {x, top + rh, G.dest.w, rh - S(4)};
    G.reset = {G.browse.x, top + rh, S(96), rh - S(4)};
    G.format = {x, top + 2 * rh, S(260), rh - S(6)};
    G.playlist = {x + S(276), top + 2 * rh, p.R() - pad - x - S(276), rh - S(6)};
    G.stale = {x, top + 3 * rh, p.R() - pad - x, rh - S(6)};
    G.help = {x, top + 4 * rh, p.R() - pad - x, 2 * LineHeight(Font::Normal) + S(4)};
    const float footerY = p.B() - S(46), listTop = G.help.B() + S(10) + rh;  // under a heading row
    G.list = {p.x + pad, listTop, p.w - 2 * pad, std::max(rh, footerY - S(10) - listTop)};
    G.visible = std::max(1, (int)std::floor(G.list.h / rh));
    G.summary = {p.x + pad, footerY, p.w - 2 * pad - S(230), rh};
    G.cancel = {p.R() - pad - S(96), footerY, S(96), rh};
    G.go = {G.cancel.x - S(116), footerY, S(108), rh};
    return G;
}

std::wstring CopyDestination() {
    std::wstring d = Trimmed(g_copy.dest.text);
    while (d.size() > 1 && d.back() == L'/') d.pop_back();
    return d;
}

// The plan and the summary's counts: what will be copied and converted, roughly how much space it
// takes, and what earlier copies the playlist no longer wants.
void CopyReplan() {
    const std::wstring dest = CopyDestination();
    if (dest != g_copy.manifestFor) {
        g_copy.manifestFor = dest;
        g_copy.manifest.clear();
        const DWORD a = GetFileAttributesW(dest.c_str());
        if (!dest.empty() && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ReadCopyManifest(dest, false, g_copy.manifest);
    }
    g_copy.plan = PlanCopy(g_copy.ids, dest, false, g_copy.pattern.text, g_copy.format, g_copy.manifest);
    g_copy.copying = g_copy.converting = g_copy.there = g_copy.blocked = 0;
    g_copy.bytes = 0;
    for (const CopyItem& item : g_copy.plan) {
        if (!item.skip.empty()) ++g_copy.blocked;
        else if (item.done) ++g_copy.there;
        else {
            ++g_copy.copying;
            g_copy.converting += item.convert;
            g_copy.bytes += item.estimate;
        }
    }
    g_copy.stale = PlaylistCopy() ? StaleCopies(g_copy.plan, g_copy.manifest, g_copy.set).size() : 0;
    g_copy.first = std::clamp(g_copy.first, 0, std::max(0, (int)g_copy.plan.size() - 1));
    g_copy.dirty = false;
}

void CopyEdited() {
    g_copy.dirty = true;
    g_copy.editedAt = SDL_GetTicks();
    g.redraw = true;
}

void CopyFocus(int f) {
    g_copy.focus = f;
    TextField& t = f ? g_copy.pattern : g_copy.dest;
    t.caret = t.text.size();
    t.all = false;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

bool CanStartCopy() {
    return !CopyDestination().empty() && !CopyBusy() && (g_copy.copying || (g_copy.stale && g_copy.removeStale));
}

// True when `dir` is a library folder or inside one: copies there would be scanned in as duplicates.
bool InLibraryFolder(const std::wstring& dir) {
    for (std::wstring root : g.settings.folders) {
        while (root.size() > 1 && root.back() == L'/') root.pop_back();
        if (dir.compare(0, root.size(), root) == 0 && (dir.size() == root.size() || dir[root.size()] == L'/')) return true;
    }
    return false;
}

// Saves the fields, checks the destination, asks before anything would be removed, and hands the
// plan to the worker; the panel closes and the result arrives as WM_APP_COPY.
void CopyStart() {
    const std::wstring dest = CopyDestination();
    if (dest.empty() || CopyBusy()) return;
    if (OrganizeBusy()) {
        ShowMessage(L"Wait for the running organize to finish.");
        return;
    }
    g.settings.copyFolder = dest;
    g.settings.copyPattern = g_copy.pattern.text;
    g.settings.copyFormat = (uint8_t)g_copy.format;
    SaveSettings();
    if (InLibraryFolder(dest)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Copy to folder or device",
                                 "That folder is in your library, so its copies would show up as duplicates. Choose a folder outside it.",
                                 g.window);
        return;
    }
    SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
    const DWORD a = GetFileAttributesW(dest.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Copy to folder or device", ("Couldn't open or create " + Utf8(dest)).c_str(), g.window);
        return;
    }
    g_copy.manifestFor.clear();  // read it again: another program may have changed the destination
    CopyReplan();
    if (!CanStartCopy()) return;
    const bool removeStale = g_copy.stale && g_copy.removeStale;
    if (removeStale) {
        std::wstring q = L"Remove " + std::to_wstring(g_copy.stale) + (g_copy.stale == 1 ? L" earlier copy" : L" earlier copies") +
                         L" that \x201C" + g_copy.playlistName + L"\x201D no longer holds?\n\nOnly files cordinal copied there are removed. "
                         L"Memory cards and phones have no trash, so they are deleted for good (the originals on this computer stay).";
        if (!Confirm(Utf8(q), "Copy to folder or device")) return;
    }
    CopySettings settings;
    settings.dest = dest;
    settings.set = g_copy.set;
    settings.removeStale = removeStale;
    settings.kbps = CopyKbps(g_copy.format);
    if (PlaylistCopy() && g_copy.writePlaylist) {
        // Every occurrence, in the playlist's order, by its path relative to the playlist file.
        std::unordered_map<TrackId, std::string> where;
        for (const CopyItem& item : g_copy.plan)
            if (item.skip.empty()) where[item.id] = Utf8(item.rel);
        std::vector<std::pair<TrackId, std::string>> entries;
        for (TrackId id : g_copy.ids)
            if (auto w = where.find(id); w != where.end()) entries.push_back({id, w->second});
        settings.playlistName = g_copy.playlistName;
        settings.playlist = M3UText(entries);
    }
    StartCopy(std::move(g_copy.plan), std::move(settings), g_copy.manifest);
    ClosePanel();
}

void CopyMouseDown(float x, float y) {
    const CopyGeometry G = CopyGeom();
    if (G.go.In(x, y)) return CanStartCopy() && !g_copy.dirty ? CopyStart() : void();
    if (G.cancel.In(x, y)) return ClosePanel();
    if (G.dest.In(x, y)) return CopyFocus(0);
    if (G.pattern.In(x, y)) return CopyFocus(1);
    if (G.reset.In(x, y)) {
        g_copy.pattern.text = kDefaultOrganizePattern;
        CopyFocus(1);
        return CopyEdited();
    }
    if (G.format.In(x, y)) {
        OpenChoice(G.format.x, G.format.B(), {std::begin(kCopyFormats), std::end(kCopyFormats)}, (int)g_copy.format, [](int f) {
            g_copy.format = (CopyFormat)f;
            CopyReplan();
            g.redraw = true;
        });
        return;
    }
    if (G.playlist.In(x, y) && PlaylistCopy()) {
        g_copy.writePlaylist = !g_copy.writePlaylist;
        return;
    }
    if (G.stale.In(x, y) && PlaylistCopy()) {
        g_copy.removeStale = !g_copy.removeStale;
        return;
    }
    if (G.browse.In(x, y)) PickFolder(kFileCopyDest, compat::Narrow(CopyDestination().c_str()));
}

void CopyKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        if (g_copy.dirty) CopyReplan();
        return CanStartCopy() ? CopyStart() : void();
    }
    if (e.key == SDLK_TAB) return CopyFocus(1 - g_copy.focus);
    if (LineFieldKey(g_copy.focus ? g_copy.pattern : g_copy.dest, e) == 1) CopyEdited();
}

void CopyText(const char* s) {
    if (LineFieldInsert(g_copy.focus ? g_copy.pattern : g_copy.dest, Printable(compat::Widen(s)))) CopyEdited();
}

void DrawCopy(const Palette& p) {
    if (g_copy.dirty && SDL_GetTicks() - g_copy.editedAt >= kReplanDelay) CopyReplan();
    const CopyGeometry G = CopyGeom();
    const std::string what = PlaylistCopy() ? "\xE2\x80\x9C" + Utf8(g_copy.playlistName) + "\xE2\x80\x9D"
                                            : std::to_string(g_copy.ids.size()) + (g_copy.ids.size() == 1 ? " song" : " songs");
    DrawFrame(p, S(kCopyW), S(kCopyH), ("Copy " + what + " to a folder or device").c_str());
    const float pad = S(16), lh = LineHeight(Font::Normal);
    DrawText(Font::Normal, "Copy to", G.panel.x + pad, TextY(G.dest), p.sub);
    DrawLineField(g_copy.dest, G.dest, g_copy.focus == 0, true, "A folder, a memory card or a phone's folder", p);
    DrawButton(G.browse, "Browse...", p);
    DrawText(Font::Normal, "Names", G.panel.x + pad, TextY(G.pattern), p.sub);
    DrawLineField(g_copy.pattern, G.pattern, g_copy.focus == 1, true, nullptr, p);
    DrawButton(G.reset, "Default", p);
    DrawText(Font::Normal, "Format", G.panel.x + pad, TextY(G.format), p.sub);
    DrawButton(G.format, kCopyFormats[(int)g_copy.format], p);
    DrawCheck(G.playlist, "Write a playlist (.m3u8)", PlaylistCopy() && g_copy.writePlaylist, p);
    DrawCheck(G.stale, "Remove songs no longer in the playlist", PlaylistCopy() && g_copy.removeStale, p);
    DrawText(Font::Normal, "Names as in Organize files: a backslash starts a new folder. Lossy files are copied as they are.",
             G.help.x, G.help.y, p.sub, G.help.w);
    DrawText(Font::Normal, "ReplayGain goes into the copies, so players that read it (foobar2000, Poweramp) level the music.",
             G.help.x, G.help.y + lh + S(2), p.sub, G.help.w);

    const float rh = RowH(), statusW = S(130), half = std::floor((G.list.w - statusW) / 2);
    const float cols[] = {G.list.x + S(6), G.list.x + half + S(6), G.list.x + 2 * half + S(6)};
    const Rect heading{G.list.x, G.list.y - rh, G.list.w, rh};
    DrawText(Font::Normal, "From", cols[0], TextY(heading), p.sub);
    DrawText(Font::Normal, "To", cols[1], TextY(heading), p.sub);
    DrawText(Font::Normal, "Status", cols[2], TextY(heading), p.sub);
    Fill(G.list, p.surface2);
    Frame(G.list, p.border);
    for (int k = 0; k < G.visible; ++k) {
        const int i = g_copy.first + k;
        if (i >= (int)g_copy.plan.size()) break;
        const CopyItem& item = g_copy.plan[i];
        const Rect row{G.list.x, G.list.y + k * rh, G.list.w, rh};
        const std::wstring status = !item.skip.empty() ? item.skip : item.done ? L"already there" : item.convert ? L"convert to AAC" : L"copy";
        const uint32_t color = item.skip.empty() && !item.done ? p.text : p.sub;
        DrawText(Font::Normal, Utf8(item.from), cols[0], TextY(row), color, half - S(12));
        DrawText(Font::Normal, Utf8(item.rel), cols[1], TextY(row), color, half - S(12));
        DrawText(Font::Normal, Utf8(status), cols[2], TextY(row), color, statusW - S(12));
    }

    const std::wstring dest = CopyDestination();
    std::wstring s = std::to_wstring(g_copy.copying) + (g_copy.copying == 1 ? L" song" : L" songs") + L" to copy";
    if (g_copy.converting) s += L" (" + std::to_wstring(g_copy.converting) + L" as AAC)";
    if (g_copy.copying) s += L", about " + CopySize(g_copy.bytes);
    if (const std::optional<uint64_t> free = FreeSpace(dest))
        s += (g_copy.bytes > *free ? L" \x2014 more than the " : L" of ") + CopySize(*free) + L" free";
    if (g_copy.there) s += L"  \x2022  " + std::to_wstring(g_copy.there) + L" already there";
    if (g_copy.blocked) s += L"  \x2022  " + std::to_wstring(g_copy.blocked) + L" can't be copied";
    if (g_copy.stale && g_copy.removeStale) s += L"  \x2022  " + std::to_wstring(g_copy.stale) + L" earlier copies to remove";
    if (dest.empty()) s = L"Choose a folder, a memory card or a phone to copy to.";
    else if (g_copy.dirty) s = L"Updating the preview...";
    DrawText(Font::Normal, Utf8(s), G.summary.x, TextY(G.summary), p.sub, G.summary.w);
    DrawButton(G.go, "Copy", p, true, !g_copy.dirty && CanStartCopy());
    DrawButton(G.cancel, "Cancel", p);
}

void OrganizeWheel(float, float, float dy) {
    const int rows = (int)g_org.plan.size(), visible = OrgGeometry().visible;
    g_org.first = std::clamp(g_org.first - (int)std::lround(dy * 3), 0, std::max(0, rows - visible));
}

void CopyWheel(float, float, float dy) {
    const int rows = (int)g_copy.plan.size(), visible = CopyGeom().visible;
    g_copy.first = std::clamp(g_copy.first - (int)std::lround(dy * 3), 0, std::max(0, rows - visible));
}

}  // namespace

const Ops kOrganizeOps{
    .draw = DrawOrganize,
    .mouseDown = [](float x, float y, int) { OrganizeMouseDown(x, y); },
    .wheel = OrganizeWheel,
    .key = OrganizeKey,
    .text = OrganizeText,
    .file = [](int purpose, const std::wstring& path) {
        if (purpose != kFileOrganizeRoot) return;
        g_org.root.text = path;
        OrganizeFocus(0);
        OrganizeEdited();
    },
    .wantsText = [](float x, float y) {
        const OrganizeGeometry G = OrgGeometry();
        return G.root.In(x, y) || G.pattern.In(x, y);
    },
    .close = [] {
        // The preview holds one entry per file, which can be the whole library: let it go now.
        std::vector<OrganizeMove>().swap(g_org.plan);
        std::vector<TrackId>().swap(g_org.ids);
    },
    .typing = true,
};

const Ops kCopyOps{
    .draw = DrawCopy,
    .mouseDown = [](float x, float y, int) { CopyMouseDown(x, y); },
    .wheel = CopyWheel,
    .key = CopyKey,
    .text = CopyText,
    .file = [](int purpose, const std::wstring& path) {
        if (purpose != kFileCopyDest) return;
        g_copy.dest.text = path;
        CopyFocus(0);
        CopyEdited();
    },
    .wantsText = [](float x, float y) {
        const CopyGeometry G = CopyGeom();
        return G.dest.In(x, y) || G.pattern.In(x, y);
    },
    .close = [] { g_copy = CopyPanel{}; },
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

// Library > Organize files: the given tracks, or the whole library when there are none.
void OpenOrganize(std::vector<TrackId> ids) {
    if (OrganizeBusy()) return;
    if (ids.empty())
        for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
            if (g_lib[t].InLibrary()) ids.push_back(t);
    if (ids.empty()) {
        ShowMessage(L"Nothing to organize.");
        return;
    }
    g_org = OrganizePanel{};
    g_org.ids = std::move(ids);
    g_org.root.text = !g.settings.organizeFolder.empty() ? g.settings.organizeFolder
                      : g.settings.folders.empty()       ? std::wstring()
                                                         : g.settings.folders[0];
    g_org.pattern.text = g.settings.organizePattern.empty() ? std::wstring(kDefaultOrganizePattern) : g.settings.organizePattern;
    Replan();
    g_kind = Kind::Organize;
    OrganizeFocus(0);
}

void OpenCopy(const std::vector<TrackId>& ids, const std::wstring& playlist) {
    if (ids.empty()) {
        ShowMessage(L"Nothing to copy.");
        return;
    }
    if (CopyBusy()) {
        ShowMessage(L"A copy is already running.");
        return;
    }
    g_copy = CopyPanel{};
    g_copy.ids = ids;
    g_copy.set = playlist.empty() ? kCopySelectionSet : playlist;
    g_copy.playlistName = playlist;
    g_copy.writePlaylist = !playlist.empty();
    g_copy.dest.text = g.settings.copyFolder;
    g_copy.pattern.text = g.settings.copyPattern.empty() ? std::wstring(kDefaultOrganizePattern) : g.settings.copyPattern;
    g_copy.format = (CopyFormat)std::min<int>(g.settings.copyFormat, (int)CopyFormat::Count - 1);
    CopyReplan();
    g_kind = Kind::Copy;
    CopyFocus(0);
}

bool PanelReplanPending() { return (g_kind == Kind::Organize && g_org.dirty) || (g_kind == Kind::Copy && g_copy.dirty); }

}  // namespace lx
