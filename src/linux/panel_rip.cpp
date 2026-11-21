// Library > Rip CD... on Linux, as app_rip.cpp's window and run on Windows: the disc in a drive read on a
// worker (its table of contents, CD-Text, and with Album tags from MusicBrainz on, the releases
// MusicBrainz knows it as), the names chosen and edited, the format and folder set; then the rip on a
// worker with progress in the status bar and Cancel in the Library menu. Finished tracks join the
// library already tagged. The reading, verifying and writing are cdrip.cpp's.
#include "panels_internal.h"

#include "cdrip.h"
#include "metadata_fetch.h"

#include <memory>

namespace lx {
namespace {

// What the disc worker found (WM_APP_RIP, wParam 2).
struct DiscRead {
    uint64_t generation = 0;
    std::wstring drive, error;
    cdrip::Toc toc;
    std::vector<cdrip::Disc> names;  // CD-Text or an untitled disc first, then MusicBrainz's releases
    bool lookedUp = false, offline = false;
};

std::thread reader, ripper;
std::shared_ptr<online::Cancel> lookup;
std::atomic<bool> cancel{false};
bool ripping = false, stopping = false;
uint64_t generation = 0;
std::vector<std::wstring> titles;  // the running rip's track titles, for the progress line
std::atomic<cdrip::Result*> finished{nullptr};  // the worker's result until the UI thread takes it

std::wstring Duration(uint32_t sectors) {
    const uint32_t s = sectors / 75;
    wchar_t text[32];
    swprintf_s(text, s >= 3600 ? L"%u:%02u:%02u" : L"%u:%02u", s >= 3600 ? s / 3600 : s / 60, s >= 3600 ? s / 60 % 60 : s % 60,
               s % 60);
    return text;
}

}  // namespace

namespace panel {
namespace {

enum Field { kArtist, kDate, kAlbum, kTitle, kFolder, kFields };
const char* const kFormats[] = {"FLAC (lossless)", ".cord (lossless, cordinal's own)"};
constexpr float kRipW = 720, kRipH = 580;

struct RipPanel {
    std::vector<std::wstring> drives;
    int drive = 0, names = 0;
    std::unique_ptr<DiscRead> disc;
    cdrip::Disc chosen;
    TextField fields[kFields];
    int focus = kTitle;  // a field, or kFields for the track list
    int track = 0, first = 0;
    bool flac = true;
    Rect list;
} g_rip;

struct RipGeometry {
    Rect panel, drive, names, fields[kFields], list, format, browse, status, lookUp, go, cancel;
};

RipGeometry Geom() {
    RipGeometry G;
    const Rect out = OutputRect();
    const float w = std::min(S(kRipW), out.w - S(16)), h = std::min(S(kRipH), out.h - S(16));
    const Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    G.panel = p;
    const float rh = RowH(), pad = S(16), lw = S(70), x = p.x + pad + lw, W = p.R() - pad - x, half = W / 2;
    float y = p.y + S(48);
    G.drive = {x, y, S(110), rh - S(6)};
    G.names = {x + S(190), y, W - S(190), rh - S(6)};
    y += rh;
    G.fields[kArtist] = {x, y, half - S(8), rh - S(4)};
    G.fields[kDate] = {x + half + S(48), y, S(120), rh - S(4)};
    y += rh;
    G.fields[kAlbum] = {x, y, W, rh - S(4)};
    y += rh + S(4);
    const float footerY = p.B() - S(46);
    const float bottom = footerY - 3 * rh - S(8);
    G.list = {p.x + pad, y, p.w - 2 * pad, std::max(S(60), bottom - y)};
    y = G.list.B() + S(8);
    G.fields[kTitle] = {x, y, W, rh - S(4)};
    y += rh;
    G.format = {x, y, S(280), rh - S(6)};
    y += rh;
    G.fields[kFolder] = {x, y, W - S(104), rh - S(4)};
    G.browse = {p.R() - pad - S(96), y, S(96), rh - S(4)};
    G.status = {p.x + pad, footerY, p.w - 2 * pad - S(350), rh};
    G.cancel = {p.R() - pad - S(96), footerY, S(96), rh};
    G.go = {G.cancel.x - S(116), footerY, S(108), rh};
    G.lookUp = {G.go.x - S(116), footerY, S(108), rh};
    return G;
}

size_t Tracks() { return g_rip.disc && g_rip.disc->error.empty() ? g_rip.disc->toc.Audio().size() : 0; }

std::wstring Folder() {
    std::wstring d = Trimmed(g_rip.fields[kFolder].text);
    while (d.size() > 1 && d.back() == L'/') d.pop_back();
    return d;
}

bool Ready() { return Tracks() && !ripping && !Folder().empty(); }

std::wstring TrackTitle(size_t k) {
    return k < g_rip.chosen.titles.size() && !g_rip.chosen.titles[k].empty() ? FromUtf8(g_rip.chosen.titles[k])
                                                                              : L"Track " + std::to_wstring(k + 1);
}

void Focus(int f) {
    g_rip.focus = std::clamp(f, 0, (int)kFields);
    if (g_rip.focus < kFields) {
        TextField& t = g_rip.fields[g_rip.focus];
        t.caret = t.text.size();
        t.all = !t.text.empty();
        SDL_StartTextInput(g.window);
    }
    g.redraw = true;
}

void ShowTrack(int k) {
    g_rip.track = std::clamp(k, 0, std::max(0, (int)Tracks() - 1));
    g_rip.fields[kTitle].text = Tracks() ? TrackTitle((size_t)g_rip.track) : std::wstring();
    g_rip.fields[kTitle].caret = g_rip.fields[kTitle].text.size();
    g.redraw = true;
}

// Puts the chosen names into the fields.
void ShowNames() {
    g_rip.fields[kArtist].text = FromUtf8(g_rip.chosen.artist);
    g_rip.fields[kAlbum].text = FromUtf8(g_rip.chosen.album);
    g_rip.fields[kDate].text = FromUtf8(g_rip.chosen.date);
    for (int f : {kArtist, kAlbum, kDate}) g_rip.fields[f].caret = g_rip.fields[f].text.size();
    ShowTrack(0);
}

void ReadDisc(bool lookUp) {
    g_rip.disc.reset();
    g_rip.chosen = {};
    ShowNames();
    if (lookup) lookup->Stop();
    if (reader.joinable()) reader.join();
    if (g_rip.drive < 0 || g_rip.drive >= (int)g_rip.drives.size()) return;
    lookup = std::make_shared<online::Cancel>();
    reader = std::thread([gen = ++generation, drive = g_rip.drives[(size_t)g_rip.drive], lookUp, stop = lookup]() {
        auto d = std::make_unique<DiscRead>();
        d->generation = gen;
        d->drive = drive;
        if (auto dev = cdrip::OpenDrive(drive, d->error)) {
            cdrip::Disc names;
            if (!dev->ReadToc(d->toc)) d->error = L"Put an audio CD in " + drive + L".";
            else if (d->toc.Audio().empty()) d->error = L"This disc has no audio tracks.";
            else cdrip::ParseCdText(dev->CdText(), d->toc.Audio().size(), names);
            names.discId = cdrip::DiscId(d->toc);
            d->names.push_back(std::move(names));
            if (d->error.empty() && lookUp) {
                for (auto& r : cdrip::LookUp(d->toc, *stop, d->offline)) d->names.push_back(std::move(r));
                d->lookedUp = !stop->Stopped();
            }
        }
        PostMessageW(nullptr, WM_APP_RIP, 2, (LPARAM)d.release());
    });
    g.redraw = true;
}

std::string NamesLabel(const cdrip::Disc& n) { return n.source.empty() ? "Untitled (Track 1, Track 2...)" : n.source; }

std::wstring Status() {
    if (g_rip.drives.empty()) return L"No CD drive found.";
    if (!g_rip.disc) return L"Reading the disc...";
    const DiscRead& d = *g_rip.disc;
    if (!d.error.empty()) return d.error;
    const auto audio = d.toc.Audio();
    std::wstring s = std::to_wstring(audio.size()) + (audio.size() == 1 ? L" track, " : L" tracks, ") +
                     Duration(d.toc.End(audio.back()) - d.toc.tracks[audio.front()].start) + L". ";
    const size_t found = d.names.size() - 1;
    if (found) s += L"MusicBrainz knows this disc" + std::wstring(found > 1 ? L" as " + std::to_wstring(found) + L" releases." : L".");
    else if (d.offline) s += L"MusicBrainz couldn't be reached.";
    else if (d.lookedUp) s += L"MusicBrainz doesn't know this disc.";
    else s += L"Look up finds its names on MusicBrainz.";
    return s;
}

void LookUp() {
    if (!online::Config().albumTags) {
        if (!Confirm("Album tags from MusicBrainz is off, so cordinal hasn't contacted anyone.\n\n"
                     "Looking up sends this disc's id and track lengths to musicbrainz.org.\n\n"
                     "Turn it on? It can be changed in Preferences > Online.",
                     "Rip CD"))
            return;
        online::Config().albumTags = true;
        SaveSettings();
    }
    ReadDisc(true);
}

// Saves the choices and hands the disc to the rip worker; the panel closes once it starts.
void Start() {
    if (!Ready()) return;
    cdrip::Job job;
    job.toc = g_rip.disc->toc;
    job.disc = g_rip.chosen;
    job.disc.artist = ToUtf8(Trimmed(g_rip.fields[kArtist].text));
    job.disc.album = ToUtf8(Trimmed(g_rip.fields[kAlbum].text));
    job.disc.date = ToUtf8(Trimmed(g_rip.fields[kDate].text));
    job.tracks = job.toc.Audio();
    job.folder = Folder();
    job.flac = g_rip.flac;
    job.offset = g.settings.ripOffset;
    job.threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    g.settings.ripFlac = job.flac;
    g.settings.ripFolder = job.folder;
    SaveSettings();
    SHCreateDirectoryExW(nullptr, job.folder.c_str(), nullptr);
    const DWORD a = GetFileAttributesW(job.folder.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Rip CD", ("Couldn't open or create " + ToUtf8(job.folder)).c_str(), g.window);
        return;
    }
    titles.clear();
    for (size_t k = 0; k < job.tracks.size(); ++k) titles.push_back(TrackTitle(k));
    // The cover comes from the Cover Art Archive only when MusicBrainz covers are turned on.
    const bool cover = !job.disc.release.empty() && online::Config().covers[(int)online::CoverSource::MusicBrainz];
    if (lookup) lookup->Stop();
    if (reader.joinable()) reader.join();
    ripping = true;
    cancel = false;
    if (ripper.joinable()) ripper.join();
    ripper = std::thread([drive = g_rip.disc->drive, job = std::move(job), cover]() mutable {
        std::wstring error;
        auto r = std::make_unique<cdrip::Result>();
        if (cover) {
            online::Cancel stop;
            job.cover = cdrip::FetchCover(job.disc.release, stop);
        }
        if (auto dev = cdrip::OpenDrive(drive, error)) {
            int shown = -1;
            *r = cdrip::Rip(*dev, job, cancel, [&](size_t i, double f) {
                const int now = (int)i << 8 | (int)(f * 100);
                if (now != shown) PostMessageW(nullptr, WM_APP_RIP, 0, (LPARAM)(shown = now));
            });
        } else {
            r->tracks.push_back({L"", error, {}});
        }
        finished = r.release();
        PostMessageW(nullptr, WM_APP_RIP, 1, 0);
    });
    ShowMessage(L"Ripping " + std::to_wstring(titles.size()) + L" tracks...");
    ClosePanel();
}

void MouseDown(float x, float y) {
    const RipGeometry G = Geom();
    if (G.go.In(x, y)) return Start();
    if (G.cancel.In(x, y)) return ClosePanel();
    if (G.lookUp.In(x, y)) {
        if (Tracks() && !g_rip.disc->lookedUp) LookUp();
        return;
    }
    if (G.drive.In(x, y) && !g_rip.drives.empty()) {
        std::vector<std::string> labels;
        for (const auto& d : g_rip.drives) labels.push_back(ToUtf8(d));
        OpenChoice(G.drive.x, G.drive.B(), labels, g_rip.drive, [](int d) {
            g_rip.drive = d;
            ReadDisc(online::Config().albumTags);
        });
        return;
    }
    if (G.names.In(x, y) && g_rip.disc && Tracks()) {
        std::vector<std::string> labels;
        for (const auto& n : g_rip.disc->names) labels.push_back(NamesLabel(n));
        OpenChoice(G.names.x, G.names.B(), labels, g_rip.names, [](int n) {
            if (!g_rip.disc || n < 0 || n >= (int)g_rip.disc->names.size()) return;
            g_rip.names = n;
            g_rip.chosen = g_rip.disc->names[(size_t)n];
            ShowNames();
        });
        return;
    }
    if (G.format.In(x, y)) {
        OpenChoice(G.format.x, G.format.B(), {std::begin(kFormats), std::end(kFormats)}, g_rip.flac ? 0 : 1,
                   [](int f) { g_rip.flac = f == 0; g.redraw = true; });
        return;
    }
    if (G.browse.In(x, y)) return PickFolder(kFileRipDest, ToUtf8(Folder()));
    for (int f = 0; f < kFields; ++f)
        if (G.fields[f].In(x, y)) return Focus(f);
    if (G.list.In(x, y) && Tracks()) {
        g_rip.focus = kFields;
        ShowTrack(g_rip.first + (int)((y - G.list.y - S(2)) / RowH()));
    }
}

void TitleEdited() {
    if (!Tracks()) return;
    if (g_rip.chosen.titles.size() <= (size_t)g_rip.track) g_rip.chosen.titles.resize((size_t)g_rip.track + 1);
    g_rip.chosen.titles[(size_t)g_rip.track] = ToUtf8(g_rip.fields[kTitle].text);
}

void Key(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return Start();
    if (e.key == SDLK_TAB) return Focus((g_rip.focus + ((e.mod & SDL_KMOD_SHIFT) ? kFields : 1)) % (kFields + 1));
    if (g_rip.focus == kFields || e.key == SDLK_UP || e.key == SDLK_DOWN) {
        if (e.key == SDLK_UP || e.key == SDLK_DOWN) ShowTrack(g_rip.track + (e.key == SDLK_DOWN ? 1 : -1));
        return;
    }
    if (LineFieldKey(g_rip.fields[g_rip.focus], e) == 1 && g_rip.focus == kTitle) TitleEdited();
    g.redraw = true;
}

void Text(const char* s) {
    if (g_rip.focus >= kFields) return;
    if (LineFieldInsert(g_rip.fields[g_rip.focus], Printable(compat::Widen(s))) && g_rip.focus == kTitle) TitleEdited();
    g.redraw = true;
}

void Draw(const Palette& p) {
    const RipGeometry G = Geom();
    DrawFrame(p, S(kRipW), S(kRipH), "Rip CD");
    const float pad = S(16), lx = G.panel.x + pad;
    DrawText(Font::Normal, "Drive", lx, TextY(G.drive), p.sub);
    DrawButton(G.drive, g_rip.drives.empty() ? "None" : ToUtf8(g_rip.drives[(size_t)g_rip.drive]), p, false, !g_rip.drives.empty());
    DrawText(Font::Normal, "Names", G.drive.R() + S(16), TextY(G.names), p.sub);
    const bool named = g_rip.disc && Tracks() && g_rip.names < (int)g_rip.disc->names.size();
    DrawButton(G.names, named ? NamesLabel(g_rip.disc->names[(size_t)g_rip.names]) : "", p, false, named);
    const char* labels[kFields] = {"Artist", "Date", "Album", "Title", "Folder"};
    for (int f = 0; f < kFields; ++f) {
        const float labelX = f == kDate ? G.fields[kDate].x - S(44) : lx;
        DrawText(Font::Normal, labels[f], labelX, TextY(G.fields[f]), p.sub);
        DrawLineField(g_rip.fields[f], G.fields[f], g_rip.focus == f, f != kTitle || Tracks(), "", p);
    }
    // The tracks.
    g_rip.list = G.list;
    Fill(G.list, p.bg);
    Frame(G.list, g_rip.focus == kFields ? p.accent : p.border);
    const float row = RowH();
    const int visible = std::max(1, (int)((G.list.h - S(4)) / row)), n = (int)Tracks();
    g_rip.first = std::clamp(g_rip.first, g_rip.track - visible + 1, g_rip.track);
    g_rip.first = std::clamp(g_rip.first, 0, std::max(0, n - visible));
    SDL_Rect clip{(int)G.list.x + 1, (int)G.list.y + 1, (int)G.list.w - 2, (int)G.list.h - 2};
    SDL_SetRenderClipRect(g.renderer, &clip);
    for (int k = g_rip.first; k < n && k < g_rip.first + visible; ++k) {
        const Rect r{G.list.x + S(1), G.list.y + S(2) + (k - g_rip.first) * row, G.list.w - S(2), row};
        const bool sel = k == g_rip.track;
        if (sel) Fill(r, p.select);
        const auto& toc = g_rip.disc->toc;
        const size_t i = toc.Audio()[(size_t)k];
        char number[8];
        snprintf(number, sizeof number, "%02u", toc.tracks[i].number);
        const std::string length = ToUtf8(Duration(toc.End(i) - toc.tracks[i].start));
        const float lengthW = TextWidth(Font::Normal, length);
        DrawText(Font::Normal, number, r.x + S(6), TextY(r), sel ? p.selectText : p.sub, S(34));
        DrawText(Font::Normal, ToUtf8(TrackTitle((size_t)k)), r.x + S(44), TextY(r), sel ? p.selectText : p.text,
                 r.w - S(44) - lengthW - S(16));
        DrawText(Font::Normal, length, r.R() - S(8) - lengthW, TextY(r), sel ? p.selectText : p.sub);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
    DrawText(Font::Normal, "Format", lx, TextY(G.format), p.sub);
    DrawButton(G.format, kFormats[g_rip.flac ? 0 : 1], p);
    DrawButton(G.browse, "Browse...", p);
    DrawText(Font::Normal, ToUtf8(Status()), G.status.x, TextY(G.status), p.sub, G.status.w);
    DrawButton(G.lookUp, "Look up", p, false, Tracks() && !g_rip.disc->lookedUp);
    DrawButton(G.go, "Rip", p, true, Ready());
    DrawButton(G.cancel, "Cancel", p);
}

}  // namespace

const Ops kRipOps{
    .draw = Draw,
    .mouseDown = [](float x, float y, int) { MouseDown(x, y); },
    .wheel = [](float x, float y, float dy) {
        if (g_rip.list.In(x, y)) ShowTrack(g_rip.track - (int)std::lround(dy * 3));
    },
    .key = Key,
    .text = Text,
    .file = [](int purpose, const std::wstring& path) {
        if (purpose != kFileRipDest) return;
        g_rip.fields[kFolder].text = path;
        Focus(kFolder);
    },
    .wantsText = [](float x, float y) {
        const RipGeometry G = Geom();
        for (const Rect& r : G.fields)
            if (r.In(x, y)) return true;
        return false;
    },
    .close = [] {
        if (lookup) lookup->Stop();
        ++generation;  // a disc still being read finds no panel to show in
        g_rip = RipPanel{};
    },
    .typing = true,
};

// The disc worker's result, while the panel waiting for it is open.
void RipDiscRead(std::unique_ptr<DiscRead> d) {
    if (g_kind != Kind::Rip || d->generation != generation) return;
    g_rip.disc = std::move(d);
    // A release MusicBrainz matched beats CD-Text, which often lacks dates, accents and track artists.
    g_rip.names = g_rip.disc->names.size() > 1 ? 1 : 0;
    if (!g_rip.disc->names.empty()) g_rip.chosen = g_rip.disc->names[(size_t)g_rip.names];
    ShowNames();
}

}  // namespace panel

using namespace panel;

void OpenRipCd() {
    if (ripping) {
        ShowMessage(L"A CD is already being ripped.");
        return;
    }
    g_rip = RipPanel{};
    g_rip.drives = cdrip::ListDrives();
    g_rip.flac = g.settings.ripFlac;
    std::wstring folder = g.settings.ripFolder;
    if (folder.empty() && !g.settings.folders.empty()) folder = g.settings.folders.front();
    g_rip.fields[kFolder].text = folder;
    g_kind = Kind::Rip;
    Focus(kTitle);
    ReadDisc(online::Config().albumTags);
}

bool RipBusy() { return ripping; }

void CancelRip() {
    if (!ripping) return;
    cancel = true;
    ShowMessage(L"Cancelling the rip...");
}

// On the UI thread, from WM_APP_RIP: a disc read (wParam 2), progress (0: track << 8 | percent), or the
// finished rip (1), whose tracks join the library.
void OnRipMessage(WPARAM wp, LPARAM lp) {
    if (wp == 2) {
        if (reader.joinable()) reader.join();
        return RipDiscRead(std::unique_ptr<DiscRead>((DiscRead*)lp));
    }
    if (!wp) {
        const size_t i = (size_t)lp >> 8;
        if (i < titles.size() && !stopping)
            ShowMessage(L"Ripping " + std::to_wstring(i + 1) + L" of " + std::to_wstring(titles.size()) + L" (" +
                        std::to_wstring(lp & 0xFF) + L"%): " + titles[i]);
        return;
    }
    std::unique_ptr<cdrip::Result> r(finished.exchange(nullptr));
    if (!r) return;
    if (ripper.joinable()) ripper.join();
    ripping = false;
    titles.clear();
    std::vector<TrackId> unread;
    std::string failed;
    size_t added = 0;
    for (const cdrip::TrackDone& t : r->tracks) {
        if (!t.error.empty()) {
            if (failed.size() < 2000) failed += (t.path.empty() ? "" : ToUtf8(FileNameOf(t.path)) + "\n    ") + ToUtf8(t.error) + "\n";
            continue;
        }
        const TrackId id = g_lib.Add(t.path);
        if (id == kNone) continue;
        Track& track = g_lib[id];
        track.flags = (uint16_t)((track.flags | Track::kInLibrary) & ~(Track::kExcluded | Track::kMissing | Track::kInbox));
        if (!track.Loaded()) unread.push_back(id);
        ++added;
    }
    QueueTags(unread);
    if (added) {
        g_lib.Touch();
        g.libraryDirty = true;
        RebuildRows();
    }
    g.redraw = true;
    if (stopping) return;
    const std::wstring done = cdrip::DoneText(*r);
    if (!failed.empty())
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Rip CD", (ToUtf8(done) + "\n\n" + failed).c_str(), g.window);
    ShowMessage(done);
}

// Shutdown: the track being ripped is abandoned (it leaves nothing behind), and the tracks already
// ripped join the library before it is saved.
void StopRipper() {
    stopping = true;
    if (lookup) lookup->Stop();
    if (reader.joinable()) reader.join();
    cancel = true;
    if (ripper.joinable()) ripper.join();
    if (finished) OnRipMessage(1, 0);
}

}  // namespace lx
