// File > Export collection and Import collection on Linux, as app_collection.cpp does on
// Windows: the file and folder choices, one worker with its progress in the status bar and Cancel in
// the File menu, and after an import the unpacked folders joining the library folders and the
// collection's playlists becoming tabs. The work is collection.cpp's; originals are only read.
#include "draw.h"
#include "panels.h"

#include "collection.h"

#include <memory>

namespace lx {
namespace {

enum Job { kIdle, kExport, kRead, kImport };
// WM_APP_COLLECTION's wParam: progress, a finished step (lParam owns a Done), or a chosen path
// (lParam owns a std::wstring, empty when the dialog was cancelled).
enum : WPARAM { kProgress, kFinished, kExportFile, kImportFile, kImportFolder };

struct Done {
    int job = kIdle;
    std::wstring zip, dest, error;
    collection::ExportResult exported;
    std::shared_ptr<collection::ImportPlan> plan;
    collection::ImportResult imported;
};

std::thread worker;
std::atomic<bool> cancel{false};
std::atomic<int> posted{-1};  // the progress, in tenths of a percent, last posted
std::atomic<uint64_t> doneBytes{0}, totalBytes{0};
std::atomic<size_t> doneFiles{0}, totalFiles{0};
int running = kIdle;
bool asking = false, stopping = false;
std::shared_ptr<collection::ImportPlan> waiting;  // a read archive while its folder is chosen
std::unique_ptr<collection::ExportSelection> chosen;  // Select what to export's ticks, while the file is chosen

std::wstring Songs(size_t n) { return std::to_wstring(n) + (n == 1 ? L" song" : L" songs"); }
std::wstring Playlists(size_t n) { return std::to_wstring(n) + (n == 1 ? L" playlist" : L" playlists"); }

bool Under(const std::wstring& path, const std::wstring& dir) {
    return !dir.empty() && path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
           (path[dir.size()] == L'/' || dir.back() == L'/');
}

void Chosen(void* what, const char* const* paths, int) {
    auto* path = new std::wstring(paths && *paths ? FromUtf8(*paths) : L"");
    if (!PostMessageW(nullptr, WM_APP_COLLECTION, (WPARAM)(intptr_t)what, (LPARAM)path)) delete path;
}

collection::ProgressFn Progress() {
    return [](const collection::Progress& p) {
        doneBytes = p.bytes;
        totalBytes = p.totalBytes;
        doneFiles = p.files;
        totalFiles = p.totalFiles;
        const int permille = p.totalBytes ? (int)std::min<uint64_t>(1000, p.bytes * 1000 / p.totalBytes) : 0;
        if (posted.exchange(permille) != permille) PostMessageW(nullptr, WM_APP_COLLECTION, kProgress, 0);
    };
}

void Start(int job, std::function<void(Done&)> work) {
    if (worker.joinable()) worker.join();
    running = job;
    cancel = false;
    posted = -1;
    doneBytes = totalBytes = 0;
    doneFiles = totalFiles = 0;
    worker = std::thread([job, work = std::move(work)] {
        auto* done = new Done;
        done->job = job;
        try {
            work(*done);
        } catch (...) {
            done->error = L"Couldn't finish: cordinal ran out of memory.";
        }
        if (!PostMessageW(nullptr, WM_APP_COLLECTION, kFinished, (LPARAM)done)) delete done;
    });
}

void ShowProblems(const std::wstring& summary, const std::vector<std::pair<std::wstring, std::wstring>>& problems,
                  const char* title) {
    std::string text = ToUtf8(summary) + "\n\n";
    for (size_t i = 0; i < problems.size() && i < 8; ++i)
        text += ToUtf8(problems[i].first) + "\n    " + ToUtf8(problems[i].second) + "\n";
    if (problems.size() > 8) text += "...";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, text.c_str(), g.window);
}

void FinishExport(Done& d) {
    const auto& r = d.exported;
    if (r.cancelled) {
        ShowMessage(L"Export cancelled. No archive was saved.");
        return;
    }
    if (!r.ok) {
        ShowMessage(r.error);
        PlaySfx(Sfx::Oops);
        return;
    }
    std::wstring done = L"Exported " + Songs(r.songs);
    if (r.playlists) done += L" and " + Playlists(r.playlists);
    done += L" (" + collection::SizeText(r.bytes) + L") to " + d.zip + L".";
    if (!r.skipped.empty()) ShowProblems(done, r.skipped, "Some files couldn't be included");
    ShowMessage(done);
    PlaySfx(r.skipped.empty() ? Sfx::Organized : Sfx::Oops);
}

// The unpacked music joins the library: its folders are watched, and the first scan adds it
// straight to the library; the playlists open as tabs at the end.
void FinishImport(Done& d) {
    const auto& r = d.imported;
    const size_t files = r.written + r.same;
    if (r.cancelled) {
        ShowMessage(L"Import cancelled. The " + std::to_wstring(files) + L" files unpacked so far stay in " + d.dest +
                    L"; importing again skips them.");
        return;
    }
    if (!r.error.empty()) {
        ShowMessage(r.error);
        PlaySfx(Sfx::Oops);
        return;
    }
    bool added = false;
    for (const std::wstring& folder : r.folders) {
        auto& folders = g.settings.folders;
        if (std::any_of(folders.begin(), folders.end(), [&](const std::wstring& f) { return f == folder || Under(folder, f); })) continue;
        std::erase_if(folders, [&](const std::wstring& f) { return Under(f, folder); });
        folders.push_back(folder);
        added = true;
    }
    if (added) {
        g.settings.foldersChosen = true;
        SaveSettings();
    }
    StartScanner();
    for (const auto& list : r.playlists) {
        std::vector<TrackId> ids, unloaded;
        for (const std::wstring& path : list.paths) {
            const TrackId id = g_lib.Add(path);
            if (id == kInvalidTrack) continue;
            ids.push_back(id);
            if (!g_lib[id].Loaded()) unloaded.push_back(id);
        }
        g.playlists.push_back(Playlist{UniquePlaylistName(g.playlists, list.name), std::move(ids), PlaylistKind::Static});
        QueueTags(unloaded);
    }
    g.libraryDirty = true;
    g.redraw = true;
    std::wstring done = L"Imported " + std::to_wstring(files) + (files == 1 ? L" file" : L" files") + L" into " + d.dest;
    if (!r.playlists.empty()) done += L", with " + Playlists(r.playlists.size());
    done += L".";
    if (r.same) done += L" " + std::to_wstring(r.same) + L" were there already.";
    if (r.renamed) done += L" " + std::to_wstring(r.renamed) + L" went beside a different file of the same name.";
    if (!r.failed.empty()) ShowProblems(done, r.failed, "Some files couldn't be unpacked");
    ShowMessage(done);
    PlaySfx(r.failed.empty() ? Sfx::Organized : Sfx::Oops);
}

// A read archive: where it goes, said plainly, before anything is written.
void Confirmed(const std::wstring& dest) {
    auto plan = std::move(waiting);
    if (!plan || dest.empty()) {
        ShowMessage(L"Import cancelled.");
        return;
    }
    std::wstring folders;
    for (size_t i = 0; i < plan->folders.size(); ++i)
        folders += (i ? i + 1 == plan->folders.size() ? L" and " : L", " : L"") + (dest.back() == L'/' ? dest : dest + L"/") +
                   plan->folders[i];
    std::wstring text = L"Import " + Songs(plan->songs) + L" (" + collection::SizeText(plan->bytes) + L")" +
                        (plan->playlists ? L" and " + Playlists(plan->playlists) : L"") + L" into " + dest + L"?\n\n";
    if (!folders.empty()) text += L"The music goes into " + folders + L", which join your library folders. ";
    text += L"Nothing there is replaced: a file that is already there stays, and a different file with the same name "
            L"keeps it while the collection's copy goes beside it.";
    if (!Confirm(ToUtf8(text), "Import collection")) {
        ShowMessage(L"Import cancelled.");
        return;
    }
    Start(kImport, [plan, dest, progress = Progress()](Done& d) {
        d.dest = dest;
        d.imported = collection::Import(*plan, dest, cancel, progress);
    });
    ShowMessage(L"Importing the collection...");
}


// --- Select what to export ---------------------------------------------------------------------------
// As on Windows: artist > album > song with the playlists after them, all ticked to begin with. The
// ticks live in `on` (songs, flat) and `lists`; the rows shown follow what is opened, so a large
// library lists only its artists until one is opened.
namespace pick {

enum Kind { kArtist, kAlbum, kSong, kLists, kList };
struct Range {
    size_t first = 0, last = 0;  // songs [first, last) in `paths`
};
struct Row {
    Kind kind;
    size_t a = 0, b = 0, i = 0;  // artist, album, and song (flat) or playlist
    int depth = 0;
};
struct State {
    collection::ExportChoices choices;
    std::vector<const std::wstring*> paths;  // every song, artist by artist, album by album
    std::vector<Range> artists;
    std::vector<std::vector<Range>> albums;
    std::vector<char> on, lists, openArtists;
    std::vector<std::vector<char>> openAlbums;
    bool openLists = false;
    std::vector<Row> rows;
    int first = 0, focus = 0, visible = 1;
    bool proceeding = false;
    Rect panel, list, all, none, go, close;
} s;

constexpr float kRowH = 26;

int StateOf(const std::vector<char>& v, size_t first, size_t last) {
    size_t n = 0;
    for (size_t i = first; i < last; ++i) n += v[i] ? 1 : 0;
    return n == 0 ? 0 : n == last - first ? 1 : 2;  // 2: some, drawn as a bar
}

Range RangeOf(const Row& r) {
    if (r.kind == kArtist) return s.artists[r.a];
    if (r.kind == kAlbum) return s.albums[r.a][r.b];
    return {r.i, r.i + 1};
}

int Ticked(const Row& r) {
    if (r.kind == kLists) return StateOf(s.lists, 0, s.lists.size());
    if (r.kind == kList) return s.lists[r.i] ? 1 : 0;
    const Range range = RangeOf(r);
    return StateOf(s.on, range.first, range.last);
}

bool Openable(const Row& r) { return r.kind == kArtist || r.kind == kAlbum || r.kind == kLists; }
bool IsOpen(const Row& r) {
    return r.kind == kArtist ? s.openArtists[r.a] : r.kind == kAlbum ? s.openAlbums[r.a][r.b] : r.kind == kLists && s.openLists;
}

void Rebuild() {
    s.rows.clear();
    for (size_t a = 0; a < s.choices.artists.size(); ++a) {
        s.rows.push_back({kArtist, a, 0, 0, 0});
        if (!s.openArtists[a]) continue;
        for (size_t b = 0; b < s.choices.artists[a].albums.size(); ++b) {
            s.rows.push_back({kAlbum, a, b, 0, 1});
            if (!s.openAlbums[a][b]) continue;
            const Range r = s.albums[a][b];
            for (size_t i = r.first; i < r.last; ++i) s.rows.push_back({kSong, a, b, i, 2});
        }
    }
    if (!s.lists.empty()) {
        s.rows.push_back({kLists, 0, 0, 0, 0});
        if (s.openLists)
            for (size_t i = 0; i < s.lists.size(); ++i) s.rows.push_back({kList, 0, 0, i, 1});
    }
    s.focus = std::clamp(s.focus, 0, std::max(0, (int)s.rows.size() - 1));
}

void SetOpen(const Row& r, bool open) {
    if (r.kind == kArtist) s.openArtists[r.a] = open;
    else if (r.kind == kAlbum) s.openAlbums[r.a][r.b] = open;
    else if (r.kind == kLists) s.openLists = open;
    Rebuild();
}

void Toggle(const Row& r) {
    const bool tick = Ticked(r) != 1;  // a part-ticked row ticks all of it
    if (r.kind == kLists) std::fill(s.lists.begin(), s.lists.end(), (char)tick);
    else if (r.kind == kList) s.lists[r.i] = tick;
    else {
        const Range range = RangeOf(r);
        std::fill(s.on.begin() + (ptrdiff_t)range.first, s.on.begin() + (ptrdiff_t)range.last, (char)tick);
    }
}

std::string Label(const Row& r) {
    const auto& c = s.choices;
    switch (r.kind) {
        case kArtist: {
            const auto& a = c.artists[r.a];
            return (a.name.empty() ? std::string("(no artist)") : ToUtf8(a.name)) + "  \xC2\xB7  " + ToUtf8(Songs(a.songs));
        }
        case kAlbum: {
            const auto& b = c.artists[r.a].albums[r.b];
            return (b.name.empty() ? std::string("(no album)") : ToUtf8(b.name)) + "  \xC2\xB7  " + ToUtf8(Songs(b.songs.size()));
        }
        case kSong: return ToUtf8(c.artists[r.a].albums[r.b].songs[r.i - s.albums[r.a][r.b].first].title);
        case kLists: return "Playlists";
        case kList: return ToUtf8(c.playlists[r.i].name) + "  \xC2\xB7  " + ToUtf8(Songs(c.playlists[r.i].songs));
    }
    return {};
}

size_t TickedSongs() { return (size_t)std::count(s.on.begin(), s.on.end(), (char)1); }
size_t TickedLists() { return (size_t)std::count(s.lists.begin(), s.lists.end(), (char)1); }

void Scroll(int first) { s.first = std::clamp(first, 0, std::max(0, (int)s.rows.size() - s.visible)); }
void Reveal() {
    if (s.focus < s.first) Scroll(s.focus);
    else if (s.focus >= s.first + s.visible) Scroll(s.focus - s.visible + 1);
}

void Accept() {
    if (!TickedSongs() && !TickedLists()) return;
    auto selection = std::make_unique<collection::ExportSelection>();
    for (size_t i = 0; i < s.paths.size(); ++i)
        if (s.on[i]) selection->songs.push_back(*s.paths[i]);
    for (size_t i = 0; i < s.lists.size(); ++i)
        if (s.lists[i]) selection->playlists.push_back(s.choices.playlists[i].name);
    chosen = std::move(selection);
    s.proceeding = true;
    ClosePanel();  // ExportPanelClose then asks for the file
}

}  // namespace pick

void AskWhereToExport() {
    asking = true;
    static const SDL_DialogFileFilter filter{"ZIP archive", "zip"};
    const std::string location = MusicFolder() + "/" + ToUtf8(collection::DefaultName(UnixNow()));
    SDL_ShowSaveFileDialog(Chosen, (void*)(intptr_t)kExportFile, g.window, &filter, 1, location.c_str());
}

void OpenExportPicker() {
    using namespace pick;
    s = State{};
    s.choices = collection::ListChoices(g_lib, g.playlists, g.settings.convertShowOriginals);
    for (const auto& artist : s.choices.artists) {
        Range ar{s.paths.size(), 0};
        auto& albums = s.albums.emplace_back();
        for (const auto& album : artist.albums) {
            Range al{s.paths.size(), 0};
            for (const auto& song : album.songs) s.paths.push_back(&song.path);
            al.last = s.paths.size();
            albums.push_back(al);
        }
        ar.last = s.paths.size();
        s.artists.push_back(ar);
        s.openAlbums.emplace_back(artist.albums.size(), 0);
    }
    s.on.assign(s.paths.size(), 1);
    s.lists.assign(s.choices.playlists.size(), 1);
    s.openArtists.assign(s.choices.artists.size(), 0);
    Rebuild();
    asking = true;
    ShowExportPanel();
}

}  // namespace

void DrawExportPanel(const Palette& p) {
    using namespace pick;
    s.panel = DrawFrame(p, S(600), S(560), "Select what to export");
    const float pad = S(16), rowH = S(kRowH), by = s.panel.B() - pad - S(28);
    s.list = {s.panel.x + pad, s.panel.y + S(48), s.panel.w - 2 * pad, by - S(40) - s.panel.y - S(48)};
    s.visible = std::max(1, (int)((s.list.h - 2) / rowH));
    Scroll(s.first);
    Fill(s.list, p.surface2);
    Frame(s.list, p.border);
    for (int k = 0; k < s.visible && s.first + k < (int)s.rows.size(); ++k) {
        const Row& r = s.rows[s.first + k];
        const Rect row{s.list.x + 1, s.list.y + 1 + k * rowH, s.list.w - 2, rowH};
        if (s.first + k == s.focus) Fill(row, p.hover);
        float x = row.x + S(8) + r.depth * S(20);
        if (Openable(r)) {  // the open/close arrow
            const float cx = x + S(6), cy = row.y + row.h / 2, h = S(4);
            if (IsOpen(r)) Polygon({{cx - h, cy - h / 2}, {cx + h, cy - h / 2}, {cx, cy + h / 2}}, p.sub);
            else Polygon({{cx - h / 2, cy - h}, {cx + h / 2, cy}, {cx - h / 2, cy + h}}, p.sub);
        }
        x += S(18);
        const float box = S(14);
        const Rect b{x, std::round(row.y + (row.h - box) / 2), box, box};
        const int state = Ticked(r);
        if (state) Fill(b, p.accent);
        else {
            Fill(b, p.surface);
            Frame(b, p.sub);
        }
        const uint32_t mark = TextOn(p.accent);
        if (state == 1) {
            ThickLine(b.x + box * 0.22f, b.y + box * 0.52f, b.x + box * 0.42f, b.y + box * 0.72f, S(1.6f), mark);
            ThickLine(b.x + box * 0.42f, b.y + box * 0.72f, b.x + box * 0.80f, b.y + box * 0.30f, S(1.6f), mark);
        } else if (state == 2) Fill({b.x + box * 0.25f, b.y + box * 0.45f, box * 0.5f, std::max(1.0f, S(2))}, mark);
        DrawText(r.kind == kLists ? Font::Bold : Font::Normal, Label(r), b.R() + S(8), TextY(row), p.text, row.R() - b.R() - S(16));
    }
    const size_t songs = TickedSongs(), lists = TickedLists();
    std::wstring summary = Songs(songs) + L" of " + std::to_wstring(s.on.size()) + L" and " + Playlists(lists) + L" ticked.";
    if (lists) summary += L" A ticked playlist brings all its songs.";
    DrawText(Font::Small, ToUtf8(summary), s.list.x, s.list.B() + S(12), p.sub, s.list.w);
    s.all = {s.panel.x + pad, by, S(96), S(28)};
    s.none = {s.all.R() + S(8), by, S(96), S(28)};
    s.close = {s.panel.R() - pad - S(88), by, S(88), S(28)};
    s.go = {s.close.x - S(8) - S(100), by, S(100), S(28)};
    DrawButton(s.all, "Tick all", p);
    DrawButton(s.none, "Tick none", p);
    DrawButton(s.go, "Export...", p, true, songs || lists);
    DrawButton(s.close, "Cancel", p);
}

void ExportPanelMouseDown(float x, float y) {
    using namespace pick;
    if (s.close.In(x, y) || !s.panel.In(x, y)) return ClosePanel();
    if (s.go.In(x, y)) return Accept();
    if (s.all.In(x, y) || s.none.In(x, y)) {
        const char tick = s.all.In(x, y) ? 1 : 0;
        std::fill(s.on.begin(), s.on.end(), tick);
        std::fill(s.lists.begin(), s.lists.end(), tick);
        return;
    }
    if (!s.list.In(x, y)) return;
    const int k = s.first + (int)((y - s.list.y - 1) / S(kRowH));
    if (k < 0 || k >= (int)s.rows.size()) return;
    s.focus = k;
    const Row r = s.rows[k];
    const float arrowEnd = s.list.x + 1 + S(8) + r.depth * S(20) + S(16);
    if (Openable(r) && x < arrowEnd) SetOpen(r, !IsOpen(r));
    else Toggle(r);
}

void ExportPanelWheel(float, float, float dy) { pick::Scroll(pick::s.first - (int)std::lround(dy * 3)); }

void ExportPanelKey(const SDL_KeyboardEvent& e) {
    using namespace pick;
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return Accept();
    if (s.rows.empty()) return;
    const Row r = s.rows[s.focus];
    if (e.key == SDLK_SPACE) Toggle(r);
    else if (e.key == SDLK_UP) s.focus = std::max(0, s.focus - 1);
    else if (e.key == SDLK_DOWN) s.focus = std::min((int)s.rows.size() - 1, s.focus + 1);
    else if (e.key == SDLK_PAGEUP) s.focus = std::max(0, s.focus - s.visible);
    else if (e.key == SDLK_PAGEDOWN) s.focus = std::min((int)s.rows.size() - 1, s.focus + s.visible);
    else if (e.key == SDLK_HOME) s.focus = 0;
    else if (e.key == SDLK_END) s.focus = (int)s.rows.size() - 1;
    else if (e.key == SDLK_RIGHT && Openable(r) && !IsOpen(r)) SetOpen(r, true);
    else if (e.key == SDLK_LEFT) {
        if (Openable(r) && IsOpen(r)) SetOpen(r, false);
        else
            for (int k = s.focus - 1; k >= 0; --k)  // to the row it sits under
                if (s.rows[k].depth < r.depth) {
                    s.focus = k;
                    break;
                }
    }
    Reveal();
    g.redraw = true;
}

// From ClosePanel. Export... goes on to the file; Cancel, Escape or a click outside ends here.
void ExportPanelClose() {
    const bool proceeding = pick::s.proceeding;
    pick::s = pick::State{};
    if (proceeding) AskWhereToExport();
    else asking = false;
}

bool CollectionBusy() { return running != kIdle || asking; }
bool CollectionRunning() { return running != kIdle; }
std::string CollectionCancelText() { return running == kExport ? "Cancel collection export" : "Cancel collection import"; }

void ExportCollection() {
    if (CollectionBusy()) {
        ShowMessage(L"Finish or cancel the collection export or import first.");
        return;
    }
    const auto everything = collection::CaptureExport(g_lib, g.playlists, g.settings.folders, g.settings.convertShowOriginals);
    if (everything.tracks.empty()) {
        ShowMessage(L"The library is empty, so there is nothing to export.");
        return;
    }
    const std::string count = ToUtf8(Songs(everything.tracks.size())) +
                              (everything.playlists.empty() ? "" : " and " + ToUtf8(Playlists(everything.playlists.size())));
    const std::string text = "What do you want to export?\n\nThe entire collection is " + count +
                             ". Or tick the artists, albums, songs and playlists to include.";
    SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Export entire collection"},
                                          {0, 2, "Select what to export..."},
                                          {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}};
    SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, g.window, "Export collection", text.c_str(), 3, buttons, nullptr};
    int answer = 0;
    if (!SDL_ShowMessageBox(&dialog, &answer)) answer = 1;
    chosen.reset();
    if (answer == 1) AskWhereToExport();
    else if (answer == 2) OpenExportPicker();
}

void ImportCollection() {
    if (CollectionBusy()) {
        ShowMessage(L"Finish or cancel the collection export or import first.");
        return;
    }
    asking = true;
    static const SDL_DialogFileFilter filter{"cordinal collection", "zip"};
    const std::string location = MusicFolder();
    SDL_ShowOpenFileDialog(Chosen, (void*)(intptr_t)kImportFile, g.window, &filter, 1, location.c_str(), false);
}

void CancelCollection() {
    if (running == kIdle) return;
    cancel = true;
    ShowMessage(running == kExport ? L"Cancelling the export..." : L"Cancelling the import...");
}

void OnCollectionMessage(WPARAM wp, LPARAM lp) {
    if (wp == kProgress) {
        if (running != kExport && running != kImport) return;
        const uint64_t total = totalBytes, bytes = doneBytes;
        const unsigned percent = total ? (unsigned)(bytes * 100 / total) : 0;
        ShowMessage((running == kExport ? L"Exporting the collection: " : L"Importing the collection: ") + std::to_wstring(percent) +
                    L"% (" + std::to_wstring((size_t)doneFiles) + L" of " + std::to_wstring((size_t)totalFiles) + L" files)...");
        return;
    }
    if (wp != kFinished) {
        std::unique_ptr<std::wstring> path((std::wstring*)lp);
        asking = false;
        if (stopping) return;
        if (wp == kImportFolder) {
            Confirmed(*path);
            return;
        }
        if (path->empty()) {
            chosen.reset();
            return;
        }
        if (wp == kExportFile) {
            std::wstring zip = *path;
            if (ExtOf(zip) != L"zip") zip += L".zip";
            const auto selection = std::move(chosen);
            auto source = std::make_shared<collection::ExportSource>(collection::CaptureExport(
                g_lib, g.playlists, g.settings.folders, g.settings.convertShowOriginals, selection.get()));
            if (source->tracks.empty()) {
                ShowMessage(selection ? L"Nothing was ticked, so there is nothing to export."
                                      : L"The library is empty, so there is nothing to export.");
                return;
            }
            Start(kExport, [source, zip, progress = Progress()](Done& d) {
                d.zip = zip;
                d.exported = collection::Export(*source, zip, cancel, progress);
            });
            ShowMessage(L"Exporting the collection...");
        } else {
            const std::wstring zip = *path;
            Start(kRead, [zip](Done& d) {
                d.zip = zip;
                auto plan = std::make_shared<collection::ImportPlan>();
                if (collection::ReadCollection(zip, *plan, d.error)) d.plan = std::move(plan);
            });
            ShowMessage(L"Reading the collection...");
        }
        return;
    }
    std::unique_ptr<Done> d((Done*)lp);
    if (worker.joinable()) worker.join();
    running = kIdle;
    if (stopping) return;
    if (!d->error.empty()) {
        ShowMessage(d->error);
        PlaySfx(Sfx::Oops);
        return;
    }
    if (d->job == kExport) FinishExport(*d);
    else if (d->job == kImport) FinishImport(*d);
    else if (cancel) ShowMessage(L"Import cancelled.");
    else {
        waiting = std::move(d->plan);
        asking = true;
        const std::string location = MusicFolder();
        SDL_ShowOpenFolderDialog(Chosen, (void*)(intptr_t)kImportFolder, g.window, location.c_str(), false);
    }
}

// Shutdown: the file in progress is abandoned, leaving nothing under its real name.
void StopCollection() {
    stopping = true;
    cancel = true;
    if (worker.joinable()) worker.join();
    waiting.reset();
    chosen.reset();
    running = kIdle;
}

}  // namespace lx
