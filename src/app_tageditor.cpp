// The tag editor (Edit > Edit tags, Alt+Enter): every writable field for one or many tracks.
//
// With several tracks, fields that differ start empty with a "(multiple values)" hint, and only
// the fields the user changes are written. Writing runs on a worker thread; files the engine
// has open (playing now, or queued gaplessly) are written once playback lets go of them.
#include "app.h"
#include "tagundo.h"
#include "tagwriter.h"

namespace app {

namespace {

const wchar_t kTagClass[] = L"CordinalTagEditor";

enum Field { Title, Artist, AlbumArtist, Album, Date, Genre, TrackNum, TrackTotal, Disc, DiscTotal, Composer, Grouping,
             Label, Bpm, Comment, Lyrics, FieldCount };
const wchar_t* const kLabels[] = {L"Title", L"Artist", L"Album artist", L"Album", L"Year / date", L"Genre", L"Track",
                                  L"of", L"Disc", L"of", L"Composer", L"Grouping", L"Label", L"BPM", L"Comment", L"Lyrics"};

// The dialog's whole state lives here: only one tag editor exists at a time, so a file-scope
// singleton is simpler than threading a context pointer through the window procedure.
struct Editor {
    HWND hwnd = nullptr;
    std::vector<TrackId> ids;
    HWND labels[FieldCount] = {}, edits[FieldCount] = {};
    HWND compilation = nullptr, art = nullptr, artReplace = nullptr, artRemove = nullptr, info = nullptr, save = nullptr,
         cancel = nullptr;
    std::wstring initial[FieldCount];
    bool mixed[FieldCount] = {}, changed[FieldCount] = {};
    int initialCompilation = 0;  // BST_* (BST_INDETERMINATE when mixed)
    std::optional<std::string> cover;  // set when replaced or removed
    ArtImage preview;
    bool loading = false;  // set while filling the controls (EN_CHANGE isn't a user edit)
} e;

std::wstring TrackValue(TrackId id, Field f) {
    const Track& t = g_lib[id];
    auto num = [](uint32_t v) { return v ? std::to_wstring(v) : std::wstring(); };
    switch (f) {
        case Title: return g_lib.Str(t.title);
        case Artist: return g_lib.Str(t.artist);
        case AlbumArtist: return g_lib.Str(t.albumArtist);
        case Album: return g_lib.Str(t.album);
        case Date: return g_lib.Str(t.date);
        case Genre: return g_lib.Str(t.genre);
        case TrackNum: return num(t.trackNo);
        case TrackTotal: return num(t.trackTotal);
        case Disc: return num(t.disc);
        case DiscTotal: return num(t.discTotal);
        case Composer: return g_lib.Str(t.composer);
        case Grouping: return g_lib.Str(t.grouping);
        case Label: return g_lib.Str(t.label);
        case Bpm: return num(t.bpm);
        case Comment: return g_lib.Str(t.comment);
        default: return {};
    }
}

std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}

void ShowPreview() {
    InvalidateRect(e.art, nullptr, FALSE);
    EnableWindow(e.artRemove, e.cover ? !e.cover->empty() : !e.preview.Empty());
}

void LoadCoverFromFile() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC filters[] = {{L"Images", L"*.jpg;*.jpeg;*.png"}};
    dlg->SetFileTypes(1, filters);
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(e.hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    std::wstring file = path;
    CoTaskMemFree(path);
    std::string bytes;
    if (!ReadFileBytes(file, bytes) || bytes.size() < 8) return;
    bool jpeg = (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xD8, png = bytes.compare(0, 4, "\x89PNG") == 0;
    if (!jpeg && !png) {
        MessageBoxW(e.hwnd, L"Covers have to be JPEG or PNG images.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    ComPtr<IStream> s;
    s.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), (UINT)bytes.size()));
    e.preview = ArtImage{};
    if (s) DecodeImage(s.Get(), S(160), e.preview);
    e.cover = std::move(bytes);
    ShowPreview();
}

void Layout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int pad = S(14), labelW = S(84), rowH = S(26), gap = S(6);
    int artSize = S(160), x0 = pad, x1 = pad + labelW, right = rc.right - pad - artSize - S(18);
    int y = pad;
    auto row = [&](Field f, int x, int w, int lw = -1) {
        int labelWidth = lw < 0 ? labelW : lw;
        MoveWindow(e.labels[f], x, y + S(4), labelWidth - S(6), S(20), TRUE);
        MoveWindow(e.edits[f], x + labelWidth, y, w - labelWidth, S(24), TRUE);
    };
    for (Field f : {Title, Artist, AlbumArtist, Album}) {
        row(f, x0, right - x0);
        y += rowH + gap;
    }
    int half = (right - x0) / 2;
    row(Date, x0, half - S(8));
    row(Genre, x0 + half, half, S(56));
    y += rowH + gap;
    int q = (half - S(8) - labelW) / 2;
    row(TrackNum, x0, labelW + q);
    row(TrackTotal, x0 + labelW + q, q + S(10), S(26));
    row(Disc, x0 + half, S(56) + q);
    row(DiscTotal, x0 + half + S(56) + q, q + S(10), S(26));
    y += rowH + gap;
    for (Field f : {Composer, Grouping}) {
        row(f, x0, right - x0);
        y += rowH + gap;
    }
    row(Label, x0, half - S(8));
    row(Bpm, x0 + half, half, S(56));
    y += rowH + gap;
    MoveWindow(e.compilation, x1, y, S(300), S(22), TRUE);
    y += S(28);
    MoveWindow(e.labels[Comment], x0, y + S(4), labelW - S(6), S(20), TRUE);
    MoveWindow(e.edits[Comment], x1, y, rc.right - pad - x1, S(52), TRUE);
    y += S(52) + gap;
    int bottom = rc.bottom - pad - S(30) - S(26);
    MoveWindow(e.labels[Lyrics], x0, y + S(4), labelW - S(6), S(20), TRUE);
    MoveWindow(e.edits[Lyrics], x1, y, rc.right - pad - x1, std::max(S(40), bottom - y), TRUE);
    // Cover column, top right.
    int ax = rc.right - pad - artSize;
    MoveWindow(e.art, ax, pad, artSize, artSize, TRUE);
    MoveWindow(e.artReplace, ax, pad + artSize + S(6), artSize, S(26), TRUE);
    MoveWindow(e.artRemove, ax, pad + artSize + S(36), artSize, S(26), TRUE);
    MoveWindow(e.info, pad, rc.bottom - pad - S(26) - S(26), rc.right - pad * 2, S(22), TRUE);
    MoveWindow(e.save, rc.right - pad - S(200) - S(8), rc.bottom - pad - S(28), S(100), S(28), TRUE);
    MoveWindow(e.cancel, rc.right - pad - S(100), rc.bottom - pad - S(28), S(100), S(28), TRUE);
}

void Fill() {
    e.loading = true;
    for (int f = 0; f < FieldCount; ++f) {
        std::wstring first = e.ids.empty() || f == Lyrics ? std::wstring() : TrackValue(e.ids[0], (Field)f);
        bool mixed = false;
        for (size_t i = 1; i < e.ids.size() && !mixed && f != Lyrics; ++i) mixed = TrackValue(e.ids[i], (Field)f) != first;
        if (f == Lyrics && e.ids.size() == 1) {
            std::string lyrics;
            ReadLyrics(g_lib.Path(e.ids[0]), lyrics);
            first = FromUtf8(lyrics);
            // Edit controls want CR LF line breaks.
            std::wstring crlf;
            for (wchar_t c : first) {
                if (c == L'\n' && (crlf.empty() || crlf.back() != L'\r')) crlf += L'\r';
                crlf += c;
            }
            first = crlf;
        }
        e.initial[f] = mixed ? L"" : first;
        e.mixed[f] = mixed;
        e.changed[f] = false;
        SetWindowTextW(e.edits[f], e.initial[f].c_str());
        bool narrow = f == TrackNum || f == TrackTotal || f == Disc || f == DiscTotal;
        SendMessageW(e.edits[f], EM_SETCUEBANNER, TRUE, (LPARAM)(!mixed ? L"" : narrow ? L"(mixed)" : L"(multiple values)"));
    }
    int on = 0;
    for (TrackId id : e.ids) on += (g_lib[id].flags & Track::kCompilation) ? 1 : 0;
    e.initialCompilation = on == 0 ? BST_UNCHECKED : on == (int)e.ids.size() ? BST_CHECKED : BST_INDETERMINATE;
    Button_SetCheck(e.compilation, e.initialCompilation);
    bool multi = e.ids.size() > 1;
    EnableWindow(e.edits[Lyrics], !multi);
    if (multi) SendMessageW(e.edits[Lyrics], EM_SETCUEBANNER, TRUE, (LPARAM)L"(select one track to edit lyrics)");
    e.cover.reset();
    e.preview = ArtImage{};
    if (!e.ids.empty()) LoadAlbumArt(g_lib.Path(e.ids[0]), S(160), e.preview);
    // File details of the first track.
    std::wstring info;
    if (!e.ids.empty()) {
        info = FormatTitle(L"[%codec%][  \x2022  %bitrate% kbps][  \x2022  %samplerate% Hz][  \x2022  %length%]  \x2022  %path%",
                           Lookup(e.ids[0], false));
        if (IgnoresWavPackCorrection(g_lib.Path(e.ids[0])))
            info += L"  \x2022  plays lossy: the .wvc correction file isn't used";
        if (multi) info = std::to_wstring(e.ids.size()) + L" tracks  \x2022  first: " + info;
    }
    SetWindowTextW(e.info, info.c_str());
    int unsupported = 0, cueTracks = 0;
    for (TrackId id : e.ids) {
        std::wstring path = g_lib.Path(id);
        if (IsSubtrack(path)) ++cueTracks;
        else unsupported += !CanWriteTags(path);
    }
    if (cueTracks)
        SetWindowTextW(e.info, (L"Tags of CUE tracks come from the cue sheet; editing them isn't supported yet.  " + info).c_str());
    else if (unsupported)
        SetWindowTextW(e.info, (std::to_wstring(unsupported) + L" of these files can't be written (only MP3, FLAC, Ogg, Opus and M4A can).  " + info).c_str());
    e.loading = false;
    ShowPreview();
}

std::optional<std::string> TextEdit(Field f) {
    if (!e.changed[f]) return std::nullopt;
    std::wstring v = GetText(e.edits[f]);
    // Back to plain line feeds for tags.
    std::wstring lf;
    for (wchar_t c : v)
        if (c != L'\r') lf += c;
    size_t b = lf.find_first_not_of(L" \t"), end = lf.find_last_not_of(L" \t\n");
    return ToUtf8(b == std::wstring::npos ? std::wstring() : lf.substr(b, end - b + 1));
}

std::optional<uint32_t> NumberEdit(Field f) {
    if (!e.changed[f]) return std::nullopt;
    return (uint32_t)std::max(0, _wtoi(GetText(e.edits[f]).c_str()));
}

// Builds the edit from the fields the user actually touched (changed[]); untouched fields stay
// nullopt so the writers leave them alone. An empty edit just closes the dialog without a write.
void Save() {
    TagEdit edit;
    edit.title = TextEdit(Title);
    edit.artist = TextEdit(Artist);
    edit.albumArtist = TextEdit(AlbumArtist);
    edit.album = TextEdit(Album);
    edit.date = TextEdit(Date);
    edit.genre = TextEdit(Genre);
    edit.composer = TextEdit(Composer);
    edit.grouping = TextEdit(Grouping);
    edit.label = TextEdit(Label);
    edit.comment = TextEdit(Comment);
    edit.lyrics = TextEdit(Lyrics);
    edit.track = NumberEdit(TrackNum);
    edit.trackTotal = NumberEdit(TrackTotal);
    edit.disc = NumberEdit(Disc);
    edit.discTotal = NumberEdit(DiscTotal);
    edit.bpm = NumberEdit(Bpm);
    int comp = Button_GetCheck(e.compilation);
    if (comp != e.initialCompilation && comp != BST_INDETERMINATE) edit.compilation = comp == BST_CHECKED;
    edit.cover = e.cover;
    if (!edit.Empty()) WriteTagsInBackground(e.ids, edit);
    DestroyWindow(e.hwnd);
}

void PaintArt(HDC dc, RECT r) {
    FillSolid(dc, r, Ref(P().border));
    RECT in{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    bool removed = e.cover && e.cover->empty();
    if (e.preview.Empty() || removed) {
        FillSolid(dc, in, Ref(P().surface2));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Ref(P().faint));
        DrawTextW(dc, removed ? L"(cover removed)" : L"No cover", -1, &in, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    int size = in.right - in.left;
    int w = e.preview.width >= e.preview.height ? size : MulDiv(size, e.preview.width, e.preview.height);
    int h = e.preview.height >= e.preview.width ? size : MulDiv(size, e.preview.height, e.preview.width);
    FillSolid(dc, in, Ref(P().surface2));
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = e.preview.width;
    bi.bmiHeader.biHeight = -e.preview.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, in.left + (size - w) / 2, in.top + (size - h) / 2, w, h, 0, 0, e.preview.width, e.preview.height,
                  e.preview.pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

LRESULT CALLBACK TagEditorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            e.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            for (int f = 0; f < FieldCount; ++f) {
                e.labels[f] = make(0, WC_STATICW, kLabels[f], SS_RIGHT | SS_NOPREFIX);
                bool multiline = f == Comment || f == Lyrics;
                DWORD style = WS_TABSTOP | (multiline ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL : ES_AUTOHSCROLL);
                if (f == TrackNum || f == TrackTotal || f == Disc || f == DiscTotal || f == Bpm) style |= ES_NUMBER;
                e.edits[f] = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", style, 100 + f);
                SetWindowTheme(e.edits[f], g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            }
            e.compilation = make(0, WC_BUTTONW, L"Part of a &compilation (various artists)", BS_AUTO3STATE | WS_TABSTOP, 200);
            e.art = make(0, WC_STATICW, L"", SS_OWNERDRAW);
            e.artReplace = make(0, WC_BUTTONW, L"Choose &cover...", BS_PUSHBUTTON | WS_TABSTOP, 201);
            e.artRemove = make(0, WC_BUTTONW, L"&Remove cover", BS_PUSHBUTTON | WS_TABSTOP, 202);
            e.info = make(0, WC_STATICW, L"", SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX);
            e.save = make(0, WC_BUTTONW, L"&Save", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            e.cancel = make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND c : {e.compilation, e.artReplace, e.artRemove, e.save, e.cancel}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            Fill();
            return 0;
        }
        case WM_SIZE:
            Layout(h);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(640), S(560)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_DRAWITEM: {
            auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (di->hwndItem == e.art) {
                PaintArt(di->hDC, di->rcItem);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == e.info ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id >= 100 && id < 100 + FieldCount && HIWORD(wp) == EN_CHANGE && !e.loading) e.changed[id - 100] = true;
            if (id == 201) LoadCoverFromFile();
            if (id == 202) {
                e.cover = std::string();
                ShowPreview();
            }
            if (id == IDOK) Save();
            if (id == IDCANCEL) DestroyWindow(h);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            e.hwnd = nullptr;
            e.preview = ArtImage{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool TagEditorDialogMessage(MSG* msg) { return e.hwnd && IsDialogMessageW(e.hwnd, msg); }

void ShowTagEditor(const std::vector<TrackId>& ids) {
    if (ids.empty()) return;
    if (e.hwnd) DestroyWindow(e.hwnd);
    RegisterAppClass(kTagClass, TagEditorProc);
    e.ids = ids;
    RECT r{0, 0, S(720), S(620)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, h = r.bottom - r.top;
    std::wstring title = ids.size() == 1 ? L"Edit tags" : L"Edit tags (" + std::to_wstring(ids.size()) + L" tracks)";
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kTagClass, title.c_str(),
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN, owner.left + (owner.right - owner.left - w) / 2,
                    owner.top + S(60), w, h, g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(e.hwnd, SW_SHOW);
    SetFocus(e.edits[Title]);
}

// ---------------------------------------------------------------------------
// Writing

namespace {

struct WriteJob {
    std::vector<TrackPath> files;
    std::vector<TagEdit> edits;  // one per file
    bool announce;               // the user just saved (not a deferred write catching up later)
    std::wstring journalDir;     // empty: this write is not recorded for undo (it is an undo)
    std::wstring journalAfter;   // an existing undo record to remove once every write succeeds
};
struct WriteDone {
    bool announce = false;
    std::vector<TrackId> written;
    std::vector<std::pair<TrackId, std::wstring>> failed;  // track, error
    std::wstring journalFile;    // the new undo record, kept for the next Undo
    std::wstring removeJournal;  // an older record to remove after this undo lands cleanly
};
struct Deferred {
    TrackId id;
    TagEdit edit;
    bool undoable;
};

std::thread g_writer;
std::atomic<int> g_writeDone{0}, g_writeTotal{0};
std::vector<Deferred> g_deferred;  // waiting for the engine to let go of the file
std::unordered_set<TrackId> g_writing;  // the running write's tracks (UI thread)

std::wstring TagUndoDir() { return g.dataDir + L"\\tagundo"; }

// Runs one write job on the calling thread. `progressTo`, when set, receives a status update per
// file; shutdown writes pass none. The outcome is left in `done`.
void RunWriteJob(WriteJob job, WriteDone& done, HWND progressTo) {
    done.announce = job.announce;
    // The old values are recorded before any file changes. A file whose old values can't be
    // read is not written, since the edit could not be undone.
    std::vector<bool> capturedOk(job.files.size(), true);
    TagJournal journal;
    bool journaled = false;
    std::wstring journalError;
    if (!job.journalDir.empty()) {
        for (size_t i = 0; i < job.files.size(); ++i) {
            TagUndoFile undo;
            if (CaptureTagUndo(job.files[i].Path(), job.edits[i], undo)) journal.files.push_back(std::move(undo));
            else capturedOk[i] = false;
        }
        if (!journal.files.empty()) {
            journaled = SaveTagJournal(job.journalDir, journal, journalError);
            if (!journaled) journalError = L"couldn't save the undo record: " + journalError;
            else done.journalFile = job.journalDir + L"\\" + std::to_wstring(journal.time) + L".cdu";
        }
    }
    done.removeJournal = job.journalAfter;
    for (size_t i = 0; i < job.files.size(); ++i) {
        const auto& file = job.files[i];
        std::wstring error;
        if (!journalError.empty()) {
            done.failed.emplace_back(file.id, journalError);
        } else if (!capturedOk[i]) {
            done.failed.emplace_back(file.id, L"couldn't read its tags, so the change can't be undone");
        } else if (WriteTags(file.Path(), job.edits[i], error)) {
            done.written.push_back(file.id);
        } else {
            done.failed.emplace_back(file.id, error);
        }
        if (progressTo) {
            ++g_writeDone;
            PostMessageW(progressTo, WM_APP_TAGWRITE, 1, 0);
        }
    }
    // Nothing changed, so there is nothing to undo.
    if (journaled && done.written.empty()) {
        DeleteTagJournal(done.journalFile);
        done.journalFile.clear();
    }
}

void WriterThread(HWND hwnd, WriteJob job) {
    auto* done = new WriteDone();
    RunWriteJob(std::move(job), *done, hwnd);
    if (!PostMessageW(hwnd, WM_APP_TAGWRITE, 0, (LPARAM)done)) delete done;
}

bool EngineHasOpen(TrackId id) {
    if (id == g.playing) return true;
    auto it = g.tokens.find(g.decodingToken);
    return it != g.tokens.end() && it->second.id == id;
}

}  // namespace

bool TagWriteBusy() { return g_writeTotal > 0; }
bool TagWriteUsing(TrackId id) { return g_writing.count(id) != 0; }
bool TagWriterRetainsTracks() { return g_writeTotal > 0 || !g_deferred.empty(); }

std::wstring TagWriteStatus() {
    return L"Writing tags " + std::to_wstring(g_writeDone.load()) + L"/" + std::to_wstring(g_writeTotal.load());
}

// Queues one edit per track. Files the engine has open are deferred. False when a write is still
// running. `journalAfter` is an undo record to remove once this write lands cleanly.
bool SubmitTagWrites(const std::vector<std::pair<TrackId, TagEdit>>& items, bool announce, bool undoable,
                     std::wstring journalAfter) {
    if (g_writeTotal > 0) {
        MessageBoxW(g.hwnd, L"Tags are still being written; try again in a moment.", kAppName, MB_OK | MB_ICONINFORMATION);
        return false;
    }
    WriteJob job{{}, {}, announce, undoable ? TagUndoDir() : std::wstring(), std::move(journalAfter)};
    size_t deferred = 0, unsupported = 0;
    bool cueSkipped = false;
    for (const auto& [id, edit] : items) {
        std::wstring path = g_lib.Path(id);
        if (IsSubtrack(path)) {
            cueSkipped = true;
            continue;
        } else if (!CanWriteTags(path)) {
            ++unsupported;
            continue;
        } else if (EngineHasOpen(id)) {
            g_deferred.push_back({id, edit, undoable});
            ++deferred;
        } else {
            job.files.push_back(g_lib.SnapshotPath(id));
            job.edits.push_back(edit);
        }
    }
    std::wstring skipped;
    if (cueSkipped) skipped = L"Tags of CUE tracks come from the cue sheet; editing them isn't supported yet.";
    if (unsupported) {
        if (!skipped.empty()) skipped += L" ";
        skipped += std::to_wstring(unsupported) + (unsupported == 1 ? L" file's tags can't be written: "
                                                                   : L" files' tags can't be written: ") +
                   L"only MP3, FLAC, Ogg, Opus, M4A and .cord files are editable.";
    }
    if (deferred) {
        if (!skipped.empty()) skipped += L" ";
        skipped += std::to_wstring(deferred) + L" playing file(s) will be updated when playback moves on.";
    }
    if (!skipped.empty()) ShowMessage(skipped);
    if (job.files.empty()) return true;
    if (g_writer.joinable()) g_writer.join();
    g_writeDone = 0;
    g_writeTotal = (int)job.files.size();
    for (const auto& file : job.files) g_writing.insert(file.id);
    g_writer = std::thread(WriterThread, g.hwnd, std::move(job));
    UpdateStatus();
    return true;
}

void WriteTagsInBackground(const std::vector<TrackId>& ids, const TagEdit& edit, bool announce) {
    std::vector<std::pair<TrackId, TagEdit>> items;
    for (TrackId id : ids) items.emplace_back(id, edit);
    SubmitTagWrites(items, announce, true);
}

bool CanUndoTagEdit(std::wstring& description) {
    std::wstring file;
    uint32_t files = 0;
    if (!FindNewestTagJournal(TagUndoDir(), file, files)) return false;
    description = L"Undo tag edit (" + std::to_wstring(files) + (files == 1 ? L" track)" : L" tracks)");
    return true;
}

void UndoLastTagEdit() {
    std::wstring file;
    uint32_t count = 0;
    if (!FindNewestTagJournal(TagUndoDir(), file, count)) {
        ShowMessage(L"There's no tag edit to undo.");
        return;
    }
    if (g_writeTotal > 0) {
        MessageBoxW(g.hwnd, L"Tags are still being written; try again in a moment.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring question = L"Put back the tags that the last tag edit changed on " + std::to_wstring(count) +
                            (count == 1 ? L" file?" : L" files?");
    if (MessageBoxW(g.hwnd, question.c_str(), kAppName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    TagJournal journal;
    if (!LoadTagJournal(file, journal)) {
        ShowMessage(L"The undo record for the last tag edit can't be read.");
        return;
    }
    std::vector<std::pair<TrackId, TagEdit>> items;
    for (const TagUndoFile& f : journal.files) {
        TrackId id = g_lib.Find(f.path);
        if (id == kNone) continue;  // moved or removed since the edit
        TagEdit edit = f.old;
        if (!f.coverRestorable) edit.cover.reset();
        if (!edit.Empty()) items.emplace_back(id, edit);
    }
    if (items.empty()) {
        DeleteTagJournal(file);
        ShowMessage(L"None of those files are in the library any more.");
        return;
    }
    // A playing file waits for playback to move on; its record has to stay until that write lands.
    const bool deferred = std::any_of(items.begin(), items.end(),
                                      [](const auto& item) { return EngineHasOpen(item.first); });
    SubmitTagWrites(items, false, false, deferred ? std::wstring() : file);
}

void OnTagWriteMessage(WPARAM wp, LPARAM lp) {
    if (wp == 1) {
        UpdateStatus();
        return;
    }
    std::unique_ptr<WriteDone> done(reinterpret_cast<WriteDone*>(lp));
    if (g_writer.joinable()) g_writer.join();
    g_writeTotal = 0;
    std::unordered_set<TrackId>().swap(g_writing);
    if (g.closing) {
        for (TrackId id : done->written) if (g_lib.Contains(id)) g_lib[id].flags &= ~Track::kLoaded;
    } else QueueTags(done->written);  // read the new tags back into the library
    if (!done->failed.empty()) {
        std::wstring text = L"Couldn't write tags to " + std::to_wstring(done->failed.size()) + L" file(s):\n\n";
        for (size_t i = 0; i < done->failed.size() && i < 8; ++i)
            text += FileNameOf(g_lib.Path(done->failed[i].first)) + L"\n    " + done->failed[i].second + L"\n";
        MessageBoxW(g.hwnd, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    } else if (done->announce && !done->written.empty()) {
        PlaySfx(Sfx::TagsSaved);
    }
    // The old record only goes once every file has the new tags; a partial failure must stay undoable.
    if (!done->removeJournal.empty() && done->failed.empty()) DeleteTagJournal(done->removeJournal);
    if (done->written.empty()) {
        ShowMessage(done->failed.empty() ? L"No tags were changed." : L"No tags were saved.");
    } else if (done->failed.empty()) {
        g.dirty = true;  // the library's saved metadata now matches the files
        ShowMessage(L"Saved tags to " + std::to_wstring(done->written.size()) + L" file(s).");
    } else {
        g.dirty = true;
        ShowMessage(L"Saved tags to " + std::to_wstring(done->written.size()) + L" file(s); " +
                    std::to_wstring(done->failed.size()) + L" couldn't be written.");
    }
    UpdateStatus();
}

// Called when playback moves on: write what was waiting for the engine to close the file.
void FlushDeferredTags() {
    if (g_deferred.empty() || g_writeTotal > 0) return;
    for (size_t i = 0; i < g_deferred.size(); ++i) {
        Deferred d = g_deferred[i];
        if (EngineHasOpen(d.id)) continue;
        g_deferred.erase(g_deferred.begin() + (ptrdiff_t)i);
        SubmitTagWrites({{d.id, d.edit}}, false, d.undoable);
        return;  // one job at a time; the next flush picks up the rest
    }
}

// Writes deferred edits during shutdown, after the engine has released every file. The normal
// timer cannot pick them up once the workers are stopped, so a playing file's edit would otherwise
// disappear when cordinal closes. This runs synchronously on the UI thread and writes no status
// messages; an edit that fails is returned in `error` for the caller to show before the window goes.
bool FlushDeferredTagWrites(std::wstring& error) {
    error.clear();
    if (g_deferred.empty()) return true;
    std::vector<Deferred> deferred = std::move(g_deferred);
    g_deferred.clear();
    size_t failed = 0;
    for (Deferred& d : deferred) {
        if (!g_lib.Contains(d.id)) {
            ++failed;
            if (error.empty()) error = L"the track left the library";
            continue;
        }
        WriteJob job;
        job.files.push_back(g_lib.SnapshotPath(d.id));
        job.edits.push_back(std::move(d.edit));
        job.journalDir = d.undoable ? TagUndoDir() : std::wstring();
        WriteDone done;
        RunWriteJob(std::move(job), done, nullptr);
        if (!done.failed.empty()) {
            ++failed;
            if (error.empty()) error = done.failed.front().second;
        }
        if (!done.journalFile.empty() && done.failed.empty()) DeleteTagJournal(done.journalFile);
        if (!done.written.empty()) {
            g_lib[d.id].flags &= ~Track::kLoaded;  // the tags on disk changed; read them again next start
            g.dirty = true;
        }
    }
    if (!failed) return true;
    error = std::to_wstring(failed) + (failed == 1 ? L" playing file's tag edit couldn't be saved: "
                                                   : L" playing files' tag edits couldn't be saved: ") + error;
    return false;
}

void StopTagWriter() {
    if (g_writer.joinable()) g_writer.join();
}

}  // namespace app
