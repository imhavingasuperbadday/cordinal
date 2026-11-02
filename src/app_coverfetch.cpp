// Covers on Windows: "Change cover...", "Restore original cover" and Library > Fetch missing covers.
//
// The picker is a modeless window for the selected tracks' album. Its artist and album fields can
// be corrected and searched again; covers from every enabled source appear in a thumbnail grid as
// they download, each with its source, its own artist and album and its pixel size. The user's own
// image joins the grid first: a file, or a picture pasted (Ctrl+V) or dropped from a browser,
// either as image data or as its address. Google Images has no keyless API, so its button only
// opens the browser on a search for the album. Double-click or Use cover saves the chosen one (or
// hands it to the tag editor that opened the picker); closing stops the search. The batch looks
// for albums without a cover on a worker and ends in a review where only good matches start
// checked; nothing is written until Save. Covers are shown in cordinal only (customcover.h, for
// the selected tracks), saved as cover.jpg in the album folder, or embedded through the tag writer
// as one undoable edit (Preferences > Online). Searches, downloads and decoding run on workers
// (metadata_fetch.cpp); results arrive as WM_APP_COVERFETCH and are dropped when what they were
// asked for is no longer on screen.
#include "app.h"

#include "backup.h"
#include "coverfetch.h"
#include "customcover.h"
#include "metadata_fetch.h"
#include "theme_win.h"

#include <wincodec.h>

#include <atomic>
#include <memory>
#include <thread>

namespace app {

namespace {

const wchar_t kPickerClass[] = L"CordinalCoverSearch";
const wchar_t kGridClass[] = L"CordinalCoverGrid";
const wchar_t kReviewClass[] = L"CordinalCoverReview";
const wchar_t kRowsClass[] = L"CordinalCoverRows";
enum : WPARAM { kMsgFound, kMsgSearched, kMsgProgress, kMsgBatch, kMsgSaved, kMsgOwn };
enum {
    kIdArtist = 100, kIdAlbum, kIdSearch, kIdGrid, kIdStatus, kIdFolder, kIdEmbed, kIdUse, kIdClose, kIdRows, kIdSave,
    kIdCustom, kIdFile, kIdPaste, kIdGoogle
};

struct Found {
    uint64_t generation;
    online::CoverFound cover;
};
struct Searched {
    uint64_t generation;
    bool any;
};
struct Progress {
    size_t done, total;
};
struct BatchDone {
    coverfetch::BatchResult result;
};
struct Saved {
    coverfetch::FolderResult result;
};
struct Own {
    uint64_t generation;
    online::CoverFound cover;  // its image is empty when `error` says why
    std::wstring error;
};

void DeletePayload(WPARAM kind, LPARAM lp) {
    switch (kind) {
        case kMsgFound: delete reinterpret_cast<Found*>(lp); break;
        case kMsgSearched: delete reinterpret_cast<Searched*>(lp); break;
        case kMsgProgress: delete reinterpret_cast<Progress*>(lp); break;
        case kMsgBatch: delete reinterpret_cast<BatchDone*>(lp); break;
        case kMsgSaved: delete reinterpret_cast<Saved*>(lp); break;
        case kMsgOwn: delete reinterpret_cast<Own*>(lp); break;
    }
}

// The platform half of image validation: WIC must decode the whole picture.
bool DecodeFetched(const std::string& bytes, int thumbSize, online::Decoded& out) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), (UINT)bytes.size()));
        ArtImage image;
        if (stream && DecodeImage(stream.Get(), thumbSize > 0 ? thumbSize : 32, image, 100000000, &out.width, &out.height)) {
            if (thumbSize > 0) {
                out.thumbWidth = image.width;
                out.thumbHeight = image.height;
                out.thumb = std::move(image.pixels);
            }
            ok = true;
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

void DrawThumb(HDC dc, const RECT& r, const online::Decoded& d) {
    FillSolid(dc, r, Ref(P().surface2));
    if (d.thumb.empty() || d.thumbWidth <= 0 || d.thumbHeight <= 0) return;
    int bw = r.right - r.left, bh = r.bottom - r.top;
    int w = d.thumbWidth >= d.thumbHeight ? bw : MulDiv(bh, d.thumbWidth, d.thumbHeight);
    int h = d.thumbHeight >= d.thumbWidth ? bh : MulDiv(bw, d.thumbHeight, d.thumbWidth);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = d.thumbWidth;
    bi.bmiHeader.biHeight = -d.thumbHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, r.left + (bw - w) / 2, r.top + (bh - h) / 2, w, h, 0, 0, d.thumbWidth, d.thumbHeight,
                  d.thumb.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

std::wstring SizeText(const online::CoverCandidate& c) {
    return std::to_wstring(c.width) + L" \x00D7 " + std::to_wstring(c.height);
}

std::wstring CandidateLabel(const online::CoverCandidate& c) {
    std::wstring a = FromUtf8(c.artist), b = FromUtf8(c.album);
    return a.empty() ? b : a + L" \x2013 " + b;
}

std::wstring SourceList(const std::vector<online::CoverSource>& sources) {
    std::wstring text;
    for (size_t i = 0; i < sources.size(); ++i) {
        if (i) text += i + 1 == sources.size() ? L" and " : L", ";
        text += online::CoverSourceName(sources[i]);
    }
    return text;
}

std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

// A manual search may start while every source is off, but only after the user agrees to
// what it sends. Agreeing turns the three sources on, as Preferences would.
bool EnsureCoverSources(HWND owner) {
    if (online::AnyCoverSource()) return true;
    const wchar_t* text =
        L"Cover sources are off, so cordinal hasn't contacted any of them.\n\n"
        L"Finding a cover sends the album's artist and title to:\n"
        L"    \x2022  Deezer (api.deezer.com)\n"
        L"    \x2022  iTunes (itunes.apple.com)\n"
        L"    \x2022  MusicBrainz (musicbrainz.org), then the release it finds to coverartarchive.org\n\n"
        L"Turn these sources on? Each can be changed in Preferences > Online.";
    if (MessageBoxW(owner, text, L"Find covers online", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
    for (const char* key : {"online.covers_deezer", "online.covers_itunes", "online.covers_musicbrainz"})
        if (const Tweak* t = FindTweak(key)) SetTweak(*t, 1);
    return online::AnyCoverSource();
}

// The three ways to keep a chosen cover, as radio buttons in the picker and the review, in the
// order shown; the arrays are indexed by CoverSave. Each is as wide as its label, so the gaps
// between them stay even whatever the font.
const online::CoverSave kSaveOrder[] = {online::CoverSave::Custom, online::CoverSave::Folder, online::CoverSave::Embed};
const int kSaveIds[] = {kIdFolder, kIdEmbed, kIdCustom};
const wchar_t* const kSaveLabels[] = {L"Save as cover.jpg in the album &folder", L"&Embed in the files' tags",
                                      L"Show in &cordinal only"};
int ChoiceGlyph() { return S(16); }  // the radio circle and its gap before the label

void CreateSaveChoices(HWND parent, HWND (&radios)[3]) {
    bool first = true;
    for (online::CoverSave choice : kSaveOrder) {
        int i = (int)choice;
        radios[i] = CreateWindowExW(0, WC_BUTTONW, kSaveLabels[i],
                                    WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (first ? WS_GROUP | WS_TABSTOP : 0), 0, 0,
                                    0, 0, parent, (HMENU)(INT_PTR)kSaveIds[i], g.inst, nullptr);
        SendMessageW(radios[i], WM_SETFONT, (WPARAM)g.font, TRUE);
        ApplyControlTheme(radios[i], g.dark);
        first = false;
    }
    Button_SetCheck(radios[std::clamp(online::Config().coverSave, 0, 2)], BST_CHECKED);
}

void LayoutSaveChoices(HWND (&radios)[3], int x, int y) {
    HDC dc = GetDC(radios[0]);
    HGDIOBJ old = SelectObject(dc, g.font);
    for (online::CoverSave choice : kSaveOrder) {
        RECT text{};
        DrawTextW(dc, kSaveLabels[(int)choice], -1, &text, DT_CALCRECT | DT_SINGLELINE);
        int w = ChoiceGlyph() + text.right + S(4);
        SetWindowPos(radios[(int)choice], nullptr, x, y, w, S(22), SWP_NOZORDER);
        x += w + S(18);
    }
    SelectObject(dc, old);
    ReleaseDC(radios[0], dc);
}

// The save choices' labels in the palette's colors; false for any other control's notification.
bool SaveChoiceDraw(LPARAM lp, LRESULT& result) {
    auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lp);
    if (draw->hdr.code != NM_CUSTOMDRAW) return false;
    int id = GetDlgCtrlID(draw->hdr.hwndFrom);
    if (std::find(std::begin(kSaveIds), std::end(kSaveIds), id) == std::end(kSaveIds)) return false;
    result = ChoiceLabelCustomDraw(draw, g.font, ChoiceGlyph());
    return true;
}

// The hairline that sets a dialog's footer apart from its content.
void PaintDivider(HDC dc, const RECT& client, int y) {
    const int pad = S(14);
    FillSolid(dc, {pad, y, client.right - pad, y + 1}, Ref(P().border));
}

// A click on one of the save choices makes it the preference; false for any other control.
bool OnSaveChoice(WPARAM wp) {
    for (int i = 0; i < 3; ++i)
        if (LOWORD(wp) == kSaveIds[i]) {
            if (HIWORD(wp) == BN_CLICKED)
                if (const Tweak* t = FindTweak("online.cover_save")) SetTweak(*t, i);
            return true;
        }
    return false;
}

// --- saving --------------------------------------------------------------------------------------

std::thread g_writer;
std::atomic<bool> g_writing{false};

// A folder cover changes what an album shows without changing its files, so the cached
// thumbnails and the now-playing cover are dropped and decoded again.
void RefreshCovers(const std::vector<TrackId>& ids) {
    if (ids.empty()) return;
    LibViewForgetCovers(ids);
    ReleaseGroupArt();
    if (g.list) InvalidateRect(g.list, nullptr, FALSE);
    if (g.artFor != kNone && std::find(ids.begin(), ids.end(), g.artFor) != ids.end()) LoadArtFor(g.artFor, true);
}

void SaveCovers(std::vector<coverfetch::CoverChoice> choices) {
    if (choices.empty()) return;
    const int mode = online::Config().coverSave;
    if (mode == (int)online::CoverSave::Embed) {
        size_t unsupported = 0;
        auto edits = coverfetch::EmbedEdits(choices, unsupported);
        if (edits.empty()) {
            ShowMessage(L"These files can't hold an embedded cover. Save covers in the album folder instead "
                        L"(Preferences > Online).");
            return;
        }
        // One tag write is one undo record: Edit > Undo last tag edit takes every cover back out.
        if (SubmitTagWrites(edits, true, true) && unsupported)
            ShowMessage(L"Embedding covers; " + Plural(unsupported, L"file", L"files") + L" can't hold one and stay as they are.");
        return;
    }
    if (g_writing) {
        ShowMessage(L"Covers are still being saved. Try again in a moment.");
        return;
    }
    if (g_writer.joinable()) g_writer.join();
    g_writing = true;
    const bool overwrite = online::Config().coverOverwrite, custom = mode == (int)online::CoverSave::Custom;
    const HWND target = g.hwnd;
    g_writer = std::thread([choices = std::move(choices), overwrite, custom, target] {
        auto* saved = new Saved{custom ? coverfetch::SaveCustom(choices) : coverfetch::SaveToFolders(choices, overwrite)};
        if (!PostMessageW(target, WM_APP_COVERFETCH, kMsgSaved, (LPARAM)saved)) delete saved;
    });
    ShowMessage(L"Saving covers...");
}

void OnSaved(std::unique_ptr<Saved> saved) {
    if (g_writer.joinable()) g_writer.join();
    g_writing = false;
    if (g.closing) return;
    const coverfetch::FolderResult& r = saved->result;
    RefreshCovers(r.changed);
    std::wstring text = !r.written ? std::wstring(L"No cover was saved.")
                        : r.custom ? L"cordinal now shows " + Plural(r.written, L"chosen cover", L"chosen covers") +
                                         L"; the music files are unchanged."
                                   : L"Saved " + Plural(r.written, L"cover", L"covers") + L" in the album folder.";
    if (r.kept)
        text += L" " + Plural(r.kept, L"album folder", L"album folders") +
                L" already had a cover file and kept it (Preferences > Online can replace them).";
    if (!r.failed) {
        ShowMessage(text);
        return;
    }
    text += L"\n\n" + Plural(r.failed, L"cover", L"covers") + L" couldn't be saved:\n";
    for (size_t i = 0; i < r.errors.size() && i < 6; ++i) text += L"\n" + r.errors[i];
    MessageBoxW(g.hwnd, text.c_str(), L"Find covers online", MB_OK | MB_ICONWARNING);
}

// --- the picker ----------------------------------------------------------------------------------

struct Picker {
    HWND hwnd = nullptr, artistLabel = nullptr, artist = nullptr, albumLabel = nullptr, album = nullptr,
         search = nullptr, grid = nullptr, status = nullptr, file = nullptr, paste = nullptr, google = nullptr,
         use = nullptr, close = nullptr;
    HWND saves[3] = {};  // by CoverSave
    coverfetch::Album target;
    std::function<void(const std::string&)> pick;
    // The user's own images first, newest first; then the found ones, grouped by source in the
    // user's order, best first.
    std::vector<online::CoverFound> results;
    int selected = -1, scroll = 0;
    online::Generation generation, ownGeneration;
    std::shared_ptr<online::Cancel> cancel, ownCancel;
    std::thread worker, ownWorker;
    int divider = 0;  // the footer's top, in client pixels
    bool searching = false;
} pk;

struct GridMetrics {
    int pad, tile, cellW, cellH, cols;
};

GridMetrics Metrics() {
    RECT rc{};
    if (pk.grid) GetClientRect(pk.grid, &rc);
    GridMetrics m;
    m.pad = S(10);
    m.tile = S(140);
    m.cellW = m.tile + S(14);
    m.cellH = m.tile + S(58);
    m.cols = std::max(1, (int)(rc.right - m.pad) / m.cellW);
    return m;
}

void UpdateGrid() {
    if (!pk.grid) return;
    RECT rc;
    GetClientRect(pk.grid, &rc);
    GridMetrics m = Metrics();
    int rows = ((int)pk.results.size() + m.cols - 1) / m.cols;
    int content = rows * m.cellH + m.pad;
    pk.scroll = std::clamp(pk.scroll, 0, std::max(0, content - (int)rc.bottom));
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, 0, std::max(0, content - 1),
                  (UINT)rc.bottom, pk.scroll};
    SetScrollInfo(pk.grid, SB_VERT, &si, TRUE);
    InvalidateRect(pk.grid, nullptr, FALSE);
    EnableWindow(pk.use, pk.selected >= 0);
}

void EnsureTileVisible(int index) {
    if (index < 0) return;
    RECT rc;
    GetClientRect(pk.grid, &rc);
    GridMetrics m = Metrics();
    int top = m.pad + index / m.cols * m.cellH;
    if (top - m.pad < pk.scroll) pk.scroll = top - m.pad;
    else if (top + m.cellH > pk.scroll + rc.bottom) pk.scroll = top + m.cellH - rc.bottom;
    UpdateGrid();
}

int TileAt(int x, int y) {
    GridMetrics m = Metrics();
    int col = (x - m.pad) / m.cellW, row = (y + pk.scroll - m.pad) / m.cellH;
    if (x < m.pad || col >= m.cols || y + pk.scroll < m.pad) return -1;
    int index = row * m.cols + col;
    return index < (int)pk.results.size() ? index : -1;
}

void PaintGrid(HDC dc, RECT rc) {
    const Palette& p = P();
    FillSolid(dc, rc, Ref(p.bg));
    HGDIOBJ oldFont = SelectObject(dc, g.font);
    SetBkMode(dc, TRANSPARENT);
    GridMetrics m = Metrics();
    if (pk.results.empty()) {
        SetTextColor(dc, Ref(p.sub));
        DrawTextW(dc, pk.searching ? L"Covers appear here as they are found." : L"No covers to show.", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    const int line = S(17);
    for (int i = 0; i < (int)pk.results.size(); ++i) {
        int x = m.pad + i % m.cols * m.cellW, y = m.pad + i / m.cols * m.cellH - pk.scroll;
        if (y + m.cellH < 0 || y > rc.bottom) continue;
        const online::CoverFound& f = pk.results[(size_t)i];
        bool selected = i == pk.selected;
        RECT cell{x - S(5), y - S(5), x + m.tile + S(5), y + m.tile + S(3) + line * 3};
        if (selected) FillSolid(dc, cell, Ref(p.select));
        RECT image{x, y, x + m.tile, y + m.tile};
        DrawThumb(dc, image, f.decoded);
        DrawCoverFrame(dc, image);
        RECT text{x, y + m.tile + S(4), x + m.tile, y + m.tile + S(4) + line};
        SetTextColor(dc, Ref(selected ? p.selectText : p.sub));
        std::wstring source = f.origin.empty() ? online::CoverSourceName(f.candidate.source) : FromUtf8(f.origin);
        DrawTextW(dc, source.c_str(), -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        OffsetRect(&text, 0, line);
        SetTextColor(dc, Ref(selected ? p.selectText : p.text));
        DrawTextW(dc, CandidateLabel(f.candidate).c_str(), -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        OffsetRect(&text, 0, line);
        SetTextColor(dc, Ref(selected ? p.selectText : p.sub));
        DrawTextW(dc, SizeText(f.candidate).c_str(), -1, &text, DT_SINGLELINE | DT_NOPREFIX);
        if (selected && GetFocus() == pk.grid) DrawFocusRect(dc, &cell);
    }
    SelectObject(dc, oldFont);
}

void Status(const std::wstring& text) {
    if (pk.status) SetWindowTextW(pk.status, text.c_str());
}

void StopSearch() {
    if (pk.cancel) pk.cancel->Stop();
    if (pk.worker.joinable()) pk.worker.join();
    pk.cancel.reset();
    pk.searching = false;
    pk.generation.Next();  // results already queued are now stale
}

void StartSearch() {
    StopSearch();
    std::erase_if(pk.results, [](const online::CoverFound& f) { return f.origin.empty(); });
    pk.selected = -1;
    pk.scroll = 0;
    online::CoverSearch search;
    search.query = {ToUtf8(WindowText(pk.artist)), ToUtf8(WindowText(pk.album))};
    search.sources = online::CoverOrder();
    search.manual = true;
    search.thumbSize = std::min(S(140), 1024);
    if (search.sources.empty()) {
        Status(L"Every cover source is off. Turn one on in Preferences > Online.");
    } else if (search.query.artist.empty() && search.query.album.empty()) {
        Status(L"Type an artist or an album to search for.");
    } else {
        const uint64_t generation = pk.generation.Next();
        pk.cancel = std::make_shared<online::Cancel>();
        pk.searching = true;
        Status(L"Asking " + SourceList(search.sources) + L"...");
        const HWND target = pk.hwnd;
        pk.worker = std::thread([search, generation, target, cancel = pk.cancel] {
            bool any = online::SearchCovers(search, *cancel, DecodeFetched, [&](online::CoverFound&& cover) {
                auto* found = new Found{generation, std::move(cover)};
                if (!PostMessageW(target, WM_APP_COVERFETCH, kMsgFound, (LPARAM)found)) delete found;
            });
            auto* done = new Searched{generation, any};
            if (!PostMessageW(target, WM_APP_COVERFETCH, kMsgSearched, (LPARAM)done)) delete done;
        });
    }
    UpdateGrid();
}

// The Search button: asks before contacting any source while all are off.
void SearchClicked() {
    if (EnsureCoverSources(pk.hwnd)) StartSearch();
    else Status(L"Cover sources are off. A file, a pasted image or Google Images work without them.");
}

void AddResult(online::CoverFound&& found) {
    const auto order = online::CoverOrder();
    auto rank = [&](const online::CoverFound& f) -> ptrdiff_t {
        return f.origin.empty() ? std::find(order.begin(), order.end(), f.candidate.source) - order.begin() : -1;
    };
    auto at = std::find_if(pk.results.begin(), pk.results.end(), [&](const online::CoverFound& r) {
        auto a = rank(r), b = rank(found);
        return a > b || (a == b && r.candidate.score < found.candidate.score);
    });
    int index = (int)(at - pk.results.begin());
    if (pk.selected >= index) ++pk.selected;
    pk.results.insert(at, std::move(found));
    Status(L"Found " + Plural(pk.results.size(), L"cover", L"covers") + L" so far...");
    UpdateGrid();
}

// --- the user's own image ------------------------------------------------------------------------

void StopOwn() {
    if (pk.ownCancel) pk.ownCancel->Stop();
    if (pk.ownWorker.joinable()) pk.ownWorker.join();
    pk.ownCancel.reset();
    pk.ownGeneration.Next();
}

// Checks and decodes the image on a worker: `bytes` as handed over, or the image `text` holds or
// leads to (an address is downloaded). `origin` and `label` name it in the grid.
void TakeOwn(std::string bytes, std::string text, std::string origin, std::string label) {
    StopOwn();
    const uint64_t generation = pk.ownGeneration.Next();
    pk.ownCancel = std::make_shared<online::Cancel>();
    const int thumb = std::min(S(140), 1024);
    const HWND target = pk.hwnd;
    Status(text.empty() ? L"Reading the image..." : L"Getting the image...");
    pk.ownWorker = std::thread([=, bytes = std::move(bytes), cancel = pk.ownCancel]() mutable {
        auto* own = new Own{generation};
        if (!text.empty()) switch (online::ImageFromText(text, *cancel, bytes)) {
                case online::TextImage::NotImage: own->error = L"That isn't an image or the address of one."; break;
                case online::TextImage::Failed:
                    own->error = L"No JPEG or PNG image came from that address. Try copying the image itself.";
                    break;
                case online::TextImage::Got: break;
            }
        int w = 0, h = 0;
        if (!own->error.empty()) {
        } else if (online::InspectImage(bytes, w, h) == online::ImageType::None) {
            own->error = L"Covers have to be complete JPEG or PNG images of at most 20 MB.";
        } else if (!DecodeFetched(bytes, thumb, own->cover.decoded)) {
            own->error = L"The image couldn't be read.";
        } else {
            own->cover.origin = origin;
            own->cover.candidate.album = label;
            own->cover.candidate.width = own->cover.decoded.width;
            own->cover.candidate.height = own->cover.decoded.height;
            own->cover.image = std::move(bytes);
        }
        if (!PostMessageW(target, WM_APP_COVERFETCH, kMsgOwn, (LPARAM)own)) delete own;
    });
}

void AddOwn(online::CoverFound&& found) {
    pk.results.insert(pk.results.begin(), std::move(found));
    pk.selected = 0;
    pk.scroll = 0;
    Status(L"Your image is first in the list. Double-click it, or choose Use cover.");
    UpdateGrid();
}

void TakeFile(const std::wstring& file) {
    std::string bytes;
    if (!backup::ReadBytes(file, bytes, online::kMaxImageBytes)) {
        Status(L"Couldn't read " + FileNameOf(file) + L". Covers can be JPEG or PNG images of at most 20 MB.");
        return;
    }
    TakeOwn(std::move(bytes), {}, "From a file", ToUtf8(FileNameOf(file)));
}

void ChooseFile() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC filters[] = {{L"Images", L"*.jpg;*.jpeg;*.png"}};
    dlg->SetFileTypes(1, filters);
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(pk.hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    std::wstring file = path;
    CoTaskMemFree(path);
    TakeFile(file);
}

// One format of a pasted or dropped object as bytes, from memory or a stream; false when it isn't
// offered or is bigger than any cover may be.
bool ReadFormat(IDataObject* data, CLIPFORMAT format, std::string& out, LONG index = -1) {
    FORMATETC f{format, nullptr, DVASPECT_CONTENT, index, TYMED_HGLOBAL | TYMED_ISTREAM};
    STGMEDIUM m{};
    if (!format || FAILED(data->GetData(&f, &m))) return false;
    out.clear();
    bool ok = false;
    if (m.tymed == TYMED_HGLOBAL && m.hGlobal) {
        SIZE_T size = GlobalSize(m.hGlobal);
        if (const void* p = size <= online::kMaxImageBytes ? GlobalLock(m.hGlobal) : nullptr) {
            out.assign(static_cast<const char*>(p), size);
            GlobalUnlock(m.hGlobal);
            ok = true;
        }
    } else if (m.tymed == TYMED_ISTREAM && m.pstm) {
        char buffer[65536];
        ULONG read = 0;
        while (out.size() <= online::kMaxImageBytes && SUCCEEDED(m.pstm->Read(buffer, sizeof(buffer), &read)) && read)
            out.append(buffer, read);
        ok = out.size() <= online::kMaxImageBytes;
    }
    ReleaseStgMedium(&m);
    return ok;
}

// A copied bitmap (CF_DIB) as a PNG: WIC reads it as a .bmp file and writes it out opaque.
bool DibToPng(const std::string& dib, std::string& png) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    BITMAPINFOHEADER h;
    memcpy(&h, dib.data(), sizeof(h));
    if (h.biSize < sizeof(h) || h.biSize > dib.size()) return false;
    DWORD colors = h.biClrUsed ? h.biClrUsed : h.biBitCount <= 8 ? 1u << h.biBitCount : 0;
    DWORD masks = h.biSize == sizeof(h) && h.biCompression == BI_BITFIELDS ? 12 : 0;
    BITMAPFILEHEADER file{0x4D42, (DWORD)(sizeof(file) + dib.size()), 0, 0, (DWORD)(sizeof(file) + h.biSize + masks + colors * 4)};
    std::string bmp(reinterpret_cast<const char*>(&file), sizeof(file));
    bmp += dib;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> in, out;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> target;
    UINT w = 0, ht = 0;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    in.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bmp.data()), (UINT)bmp.size()));
    bool ok = in && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &ht)) &&
              SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame.Get(), format, WICBitmapDitherTypeNone, nullptr, 0,
                                              WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &out)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(out.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&target, nullptr)) && SUCCEEDED(target->Initialize(nullptr)) &&
              SUCCEEDED(target->SetSize(w, ht)) && SUCCEEDED(target->SetPixelFormat(&format)) &&
              SUCCEEDED(target->WriteSource(converter.Get(), nullptr)) && SUCCEEDED(target->Commit()) &&
              SUCCEEDED(encoder->Commit());
    STATSTG stat{};
    HGLOBAL memory = nullptr;
    if (!ok || FAILED(out->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(out.Get(), &memory))) return false;
    const void* p = GlobalLock(memory);
    if (!p) return false;
    png.assign(static_cast<const char*>(p), (size_t)stat.cbSize.QuadPart);
    GlobalUnlock(memory);
    return true;
}

// Takes the picture a paste or a drop offers, in the order that keeps the most of it: a file, the
// image a browser drags along (a virtual file), PNG data, a bitmap, and last an address as text.
// False when it offers none of these.
bool TakeData(IDataObject* data, const char* origin) {
    std::string bytes;
    int w = 0, h = 0;
    FORMATETC drop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m{};
    if (SUCCEEDED(data->GetData(&drop, &m))) {
        std::wstring file;
        if (UINT len = m.tymed == TYMED_HGLOBAL ? DragQueryFileW((HDROP)m.hGlobal, 0, nullptr, 0) : 0) {
            file.resize(len + 1);
            DragQueryFileW((HDROP)m.hGlobal, 0, file.data(), len + 1);
            file.resize(len);
        }
        ReleaseStgMedium(&m);
        if (!file.empty()) {
            TakeFile(file);
            return true;
        }
    }
    if (ReadFormat(data, (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS), bytes, 0) &&
        online::InspectImage(bytes, w, h) != online::ImageType::None) {
        TakeOwn(std::move(bytes), {}, origin, "Image");
        return true;
    }
    if (ReadFormat(data, (CLIPFORMAT)RegisterClipboardFormatW(L"PNG"), bytes) &&
        online::InspectImage(bytes, w, h) != online::ImageType::None) {
        TakeOwn(std::move(bytes), {}, origin, "Image");
        return true;
    }
    std::string png;
    if (ReadFormat(data, CF_DIB, bytes) && DibToPng(bytes, png)) {
        TakeOwn(std::move(png), {}, origin, "Image");
        return true;
    }
    for (CLIPFORMAT format : {(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_INETURLW), (CLIPFORMAT)CF_UNICODETEXT}) {
        if (!ReadFormat(data, format, bytes) || bytes.size() < sizeof(wchar_t)) continue;
        std::wstring text(reinterpret_cast<const wchar_t*>(bytes.data()), bytes.size() / sizeof(wchar_t));
        text.resize(wcsnlen(text.c_str(), text.size()));
        if (text.empty()) continue;
        std::string address = ToUtf8(text), host = address.starts_with("https://") ? address.substr(8) : "Image";
        host = host.substr(0, host.find('/'));
        TakeOwn({}, std::move(address), origin, host);
        return true;
    }
    return false;
}

void PasteImage() {
    ComPtr<IDataObject> data;
    if (FAILED(OleGetClipboard(&data)) || !TakeData(data.Get(), "Pasted"))
        Status(L"The clipboard holds no image. In the browser, choose Copy image (or Copy image address), then paste here.");
}

// Pictures dragged in from a browser or Explorer; anything else shows the no-drop cursor.
class DropTarget : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG left = InterlockedDecrement(&refs_);
        if (!left) delete this;
        return (ULONG)left;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        effect_ = Offers(data) ? (*effect & DROPEFFECT_COPY ? DROPEFFECT_COPY : *effect & DROPEFFECT_LINK) : DROPEFFECT_NONE;
        *effect = effect_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = effect_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        *effect = effect_;
        if (effect_ != DROPEFFECT_NONE && pk.hwnd && !TakeData(data, "Dropped")) Status(L"That holds no image to use.");
        return S_OK;
    }

private:
    static bool Offers(IDataObject* data) {
        for (UINT format : {(UINT)CF_HDROP, RegisterClipboardFormatW(CFSTR_FILECONTENTS), RegisterClipboardFormatW(L"PNG"),
                            (UINT)CF_DIB, RegisterClipboardFormatW(CFSTR_INETURLW), (UINT)CF_UNICODETEXT}) {
            FORMATETC f{(CLIPFORMAT)format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL | TYMED_ISTREAM};
            if (data->QueryGetData(&f) == S_OK) return true;
        }
        return false;
    }
    LONG refs_ = 1;
    DWORD effect_ = DROPEFFECT_NONE;
};

// Google Images has no API to ask without a key, so the browser shows the search and the user
// pastes or drags back the picture they like.
void OpenGoogleImages() {
    std::string query = ToUtf8(WindowText(pk.artist) + L" " + WindowText(pk.album));
    query = query.find_first_not_of(' ') == std::string::npos ? std::string("album cover") : query + " cover";
    std::wstring url = FromUtf8("https://www.google.com/search?tbm=isch&q=" + online::UrlEncode(query));
    ShellExecuteW(pk.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Status(L"Google Images is open in your browser. Copy an image or its address and press Ctrl+V here, or drag it in.");
}

void UsePick() {
    if (pk.selected < 0 || pk.selected >= (int)pk.results.size()) return;
    std::string image = pk.results[(size_t)pk.selected].image;
    auto pick = pk.pick;
    coverfetch::Album target = pk.target;
    DestroyWindow(pk.hwnd);
    if (pick) pick(image);
    else SaveCovers({{std::move(target), std::move(image)}});
}

void MoveSelection(int delta) {
    if (pk.results.empty()) return;
    int next = pk.selected < 0 ? 0 : std::clamp(pk.selected + delta, 0, (int)pk.results.size() - 1);
    pk.selected = next;
    EnsureTileVisible(next);
}

LRESULT CALLBACK GridProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: DoubleBufferedPaint(h, PaintGrid); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: UpdateGrid(); return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_GETDLGCODE:
            if (lp && reinterpret_cast<MSG*>(lp)->message == WM_KEYDOWN && reinterpret_cast<MSG*>(lp)->wParam == VK_RETURN)
                return DLGC_WANTMESSAGE | DLGC_WANTARROWS;
            return DLGC_WANTARROWS;
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            int step = Metrics().cellH;
            switch (LOWORD(wp)) {
                case SB_LINEUP: pk.scroll -= step / 3; break;
                case SB_LINEDOWN: pk.scroll += step / 3; break;
                case SB_PAGEUP: pk.scroll -= (int)si.nPage; break;
                case SB_PAGEDOWN: pk.scroll += (int)si.nPage; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pk.scroll = si.nTrackPos; break;
            }
            UpdateGrid();
            return 0;
        }
        case WM_MOUSEWHEEL:
            pk.scroll -= GET_WHEEL_DELTA_WPARAM(wp) * Metrics().cellH / (WHEEL_DELTA * 2);
            UpdateGrid();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            SetFocus(h);
            int hit = TileAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit >= 0) pk.selected = hit;
            UpdateGrid();
            if (msg == WM_LBUTTONDBLCLK && hit >= 0) UsePick();
            return 0;
        }
        case WM_KEYDOWN: {
            int cols = Metrics().cols;
            switch (wp) {
                case VK_LEFT: MoveSelection(-1); break;
                case VK_RIGHT: MoveSelection(1); break;
                case VK_UP: MoveSelection(-cols); break;
                case VK_DOWN: MoveSelection(cols); break;
                case VK_HOME: MoveSelection(-(int)pk.results.size()); break;
                case VK_END: MoveSelection((int)pk.results.size()); break;
                case VK_RETURN: UsePick(); break;
            }
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void LayoutPicker() {
    RECT rc;
    GetClientRect(pk.hwnd, &rc);
    const int pad = S(14), label = S(50), button = S(88), field = S(24), tall = S(30), gap = S(8);
    int y = pad;
    int right = rc.right - pad - button - gap, half = (right - pad) / 2;
    SetWindowPos(pk.artistLabel, nullptr, pad, y + S(4), label, S(20), SWP_NOZORDER);
    SetWindowPos(pk.artist, nullptr, pad + label, y, half - label - gap, field, SWP_NOZORDER);
    SetWindowPos(pk.albumLabel, nullptr, pad + half, y + S(4), label, S(20), SWP_NOZORDER);
    SetWindowPos(pk.album, nullptr, pad + half + label, y, right - pad - half - label, field, SWP_NOZORDER);
    SetWindowPos(pk.search, nullptr, rc.right - pad - button, y, button, field, SWP_NOZORDER);
    y += field + S(12);
    // The footer sits under a hairline: how to keep the cover, then the ways to add an image of
    // one's own and the dialog's buttons. The tag editor's picker has no save choices; how to save
    // is the tag editor's business.
    int bottom = rc.bottom - pad - tall, saves = bottom - S(10) - S(22);
    pk.divider = (pk.pick ? bottom : saves) - S(12);
    int statusTop = pk.divider - S(8) - S(20);
    SetWindowPos(pk.grid, nullptr, pad, y, rc.right - pad * 2, std::max(S(60), statusTop - S(6) - y), SWP_NOZORDER);
    SetWindowPos(pk.status, nullptr, pad, statusTop, rc.right - pad * 2, S(20), SWP_NOZORDER);
    LayoutSaveChoices(pk.saves, pad, saves);
    int x = pad;
    for (auto [button, width] : {std::pair{pk.file, S(104)}, {pk.paste, S(80)}, {pk.google, S(124)}}) {
        SetWindowPos(button, nullptr, x, bottom, width, tall, SWP_NOZORDER);
        x += width + gap;
    }
    SetWindowPos(pk.close, nullptr, rc.right - pad - S(100), bottom, S(100), tall, SWP_NOZORDER);
    SetWindowPos(pk.use, nullptr, rc.right - pad - S(100) - gap - S(108), bottom, S(108), tall, SWP_NOZORDER);
    InvalidateRect(pk.hwnd, nullptr, TRUE);
}

LRESULT CALLBACK PickerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            pk.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            pk.artistLabel = make(0, WC_STATICW, L"&Artist", SS_LEFT, 0);
            pk.artist = make(WS_EX_CLIENTEDGE, WC_EDITW, FromUtf8(pk.target.artist).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, kIdArtist);
            pk.albumLabel = make(0, WC_STATICW, L"Al&bum", SS_LEFT, 0);
            pk.album = make(WS_EX_CLIENTEDGE, WC_EDITW, FromUtf8(pk.target.album).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, kIdAlbum);
            pk.search = make(0, WC_BUTTONW, L"&Search", BS_PUSHBUTTON | WS_TABSTOP, kIdSearch);
            pk.grid = make(0, kGridClass, L"Covers found", WS_TABSTOP | WS_VSCROLL, kIdGrid);
            pk.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdStatus);
            CreateSaveChoices(h, pk.saves);
            pk.file = make(0, WC_BUTTONW, L"From f&ile...", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, kIdFile);
            pk.paste = make(0, WC_BUTTONW, L"&Paste", BS_PUSHBUTTON | WS_TABSTOP, kIdPaste);
            pk.google = make(0, WC_BUTTONW, L"&Google Images", BS_PUSHBUTTON | WS_TABSTOP, kIdGoogle);
            pk.use = make(0, WC_BUTTONW, L"&Use cover", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdUse);
            pk.close = make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, kIdClose);
            // The tag editor keeps the chosen cover with its other edits; how to save is its business.
            if (pk.pick)
                for (HWND c : pk.saves) ShowWindow(c, SW_HIDE);
            for (HWND c : {pk.artist, pk.album}) SetWindowTheme(c, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            for (HWND c : {pk.search, pk.file, pk.paste, pk.google, pk.use, pk.close, pk.grid}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            LayoutPicker();
            // Pictures dragged from a browser come through OLE; the target lives until the window goes.
            auto* drop = new DropTarget;
            RegisterDragDrop(h, drop);
            drop->Release();
            return 0;
        }
        case WM_SIZE:
            LayoutPicker();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(620), S(420)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            PaintDivider((HDC)wp, rc, pk.divider);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == pk.status ? SubColor() : TextColor());
        case WM_NOTIFY: {
            LRESULT result;
            if (SaveChoiceDraw(lp, result)) return result;
            break;
        }
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:  // Enter: search from the fields, use the cover anywhere else
                    if (GetFocus() == pk.artist || GetFocus() == pk.album) SearchClicked();
                    else UsePick();
                    break;
                case kIdSearch: SearchClicked(); break;
                case kIdUse: UsePick(); break;
                case kIdFile: ChooseFile(); break;
                case kIdPaste: PasteImage(); break;
                case kIdGoogle: OpenGoogleImages(); break;
                case kIdClose:
                case IDCANCEL: DestroyWindow(h); break;
                default: OnSaveChoice(wp); break;
            }
            return 0;
        case WM_APP_COVERFETCH:
            if (wp == kMsgFound) {
                std::unique_ptr<Found> found(reinterpret_cast<Found*>(lp));
                if (pk.generation.Accepts(found->generation)) AddResult(std::move(found->cover));
            } else if (wp == kMsgSearched) {
                std::unique_ptr<Searched> done(reinterpret_cast<Searched*>(lp));
                if (!pk.generation.Accepts(done->generation)) return 0;
                if (pk.worker.joinable()) pk.worker.join();
                pk.cancel.reset();
                pk.searching = false;
                Status(pk.results.empty() ? L"No covers found. Check the artist and album, then search again."
                                          : L"Found " + Plural(pk.results.size(), L"cover", L"covers") +
                                                L". Double-click one to use it.");
                UpdateGrid();
            } else if (wp == kMsgOwn) {
                std::unique_ptr<Own> own(reinterpret_cast<Own*>(lp));
                if (!pk.ownGeneration.Accepts(own->generation)) return 0;
                if (pk.ownWorker.joinable()) pk.ownWorker.join();
                pk.ownCancel.reset();
                if (own->error.empty()) AddOwn(std::move(own->cover));
                else Status(own->error);
            } else {
                DeletePayload(wp, lp);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            RevokeDragDrop(h);
            StopSearch();
            StopOwn();
            MSG pending;
            while (PeekMessageW(&pending, h, WM_APP_COVERFETCH, WM_APP_COVERFETCH, PM_REMOVE))
                DeletePayload(pending.wParam, pending.lParam);
            pk.hwnd = pk.grid = nullptr;
            std::vector<online::CoverFound>().swap(pk.results);
            pk.pick = nullptr;
            pk.selected = -1;
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// --- the batch and its review --------------------------------------------------------------------

struct Batch {
    std::thread worker;
    std::shared_ptr<online::Cancel> cancel;
    bool running = false;
} bt;

struct Review {
    HWND hwnd = nullptr, summary = nullptr, rows = nullptr, save = nullptr, close = nullptr;
    HWND saves[3] = {};  // by CoverSave
    std::vector<coverfetch::Proposal> proposals;
    std::vector<bool> checked;
    std::vector<int> lines;  // proposal index, or -1 / -2 for the two section headings
    int focus = -1, scroll = 0;
} rv;

int HeadingHeight() { return S(28); }
int RowHeight() { return S(62); }
int LineHeight(int line) { return rv.lines[(size_t)line] < 0 ? HeadingHeight() : RowHeight(); }

void BuildLines() {
    rv.lines.clear();
    size_t confident = 0;
    for (const auto& p : rv.proposals) confident += p.confident;
    if (confident) rv.lines.push_back(-1);
    for (size_t i = 0; i < confident; ++i) rv.lines.push_back((int)i);
    if (confident < rv.proposals.size()) rv.lines.push_back(-2);
    for (size_t i = confident; i < rv.proposals.size(); ++i) rv.lines.push_back((int)i);
    rv.focus = -1;
    for (size_t i = 0; i < rv.lines.size() && rv.focus < 0; ++i)
        if (rv.lines[i] >= 0) rv.focus = (int)i;
}

int LineTop(int line) {
    int y = 0;
    for (int i = 0; i < line; ++i) y += LineHeight(i);
    return y;
}

void UpdateReview() {
    if (!rv.rows) return;
    RECT rc;
    GetClientRect(rv.rows, &rc);
    int content = LineTop((int)rv.lines.size());
    rv.scroll = std::clamp(rv.scroll, 0, std::max(0, content - (int)rc.bottom));
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, 0, std::max(0, content - 1),
                  (UINT)rc.bottom, rv.scroll};
    SetScrollInfo(rv.rows, SB_VERT, &si, TRUE);
    InvalidateRect(rv.rows, nullptr, FALSE);
    size_t count = (size_t)std::count(rv.checked.begin(), rv.checked.end(), true);
    SetWindowTextW(rv.save, count ? (L"&Save " + Plural(count, L"cover", L"covers")).c_str() : L"&Save");
    EnableWindow(rv.save, count > 0);
}

void PaintRows(HDC dc, RECT rc) {
    const Palette& p = P();
    FillSolid(dc, rc, Ref(p.bg));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, g.font);
    const int pad = S(10), box = S(16), thumb = S(48);
    int y = -rv.scroll;
    size_t confident = 0;
    for (const auto& proposal : rv.proposals) confident += proposal.confident;
    for (int line = 0; line < (int)rv.lines.size(); ++line) {
        int height = LineHeight(line), index = rv.lines[(size_t)line];
        RECT r{0, y, rc.right, y + height};
        y += height;
        if (r.bottom < 0 || r.top > rc.bottom) continue;
        if (index < 0) {
            SelectObject(dc, g.fontBold);
            SetTextColor(dc, Ref(p.text));
            RECT text{pad, r.top + S(6), r.right - pad, r.bottom};
            std::wstring heading = index == -1
                ? L"Good matches (" + std::to_wstring(confident) + L")"
                : L"Lower scores, unchecked: check the ones to use (" + std::to_wstring(rv.proposals.size() - confident) + L")";
            DrawTextW(dc, heading.c_str(), -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, g.font);
            continue;
        }
        const coverfetch::Proposal& proposal = rv.proposals[(size_t)index];
        bool focused = line == rv.focus && GetFocus() == rv.rows;
        if (focused) FillSolid(dc, r, Ref(p.select));
        COLORREF text = Ref(focused ? p.selectText : p.text), sub = Ref(focused ? p.selectText : p.sub);
        RECT b{pad, r.top + (height - box) / 2, pad + box, r.top + (height - box) / 2 + box};
        if (rv.checked[(size_t)index]) {
            FillSolid(dc, b, Ref(p.accent));
            HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), Ref(TextOn(p.accent)));
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, b.left + box * 22 / 100, b.top + box * 52 / 100, nullptr);
            LineTo(dc, b.left + box * 42 / 100, b.top + box * 72 / 100);
            LineTo(dc, b.left + box * 80 / 100, b.top + box * 30 / 100);
            SelectObject(dc, old);
            DeleteObject(pen);
        } else {
            FillSolid(dc, b, Ref(p.sub));
            RECT inner{b.left + 1, b.top + 1, b.right - 1, b.bottom - 1};
            FillSolid(dc, inner, Ref(p.surface2));
        }
        RECT image{b.right + pad, r.top + (height - thumb) / 2, b.right + pad + thumb, r.top + (height - thumb) / 2 + thumb};
        DrawThumb(dc, image, proposal.cover.decoded);
        DrawCoverFrame(dc, image);
        RECT label{image.right + pad, r.top + S(12), r.right - pad, r.top + S(30)};
        SetTextColor(dc, text);
        DrawTextW(dc, proposal.album.Label().c_str(), -1, &label, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        OffsetRect(&label, 0, S(19));
        wchar_t score[32];
        swprintf_s(score, L"%.1f", proposal.cover.candidate.score);
        std::wstring detail = std::wstring(online::CoverSourceName(proposal.cover.candidate.source)) + L"  \x2022  " +
                              CandidateLabel(proposal.cover.candidate) + L"  \x2022  " + SizeText(proposal.cover.candidate) +
                              L"  \x2022  score " + score;
        SetTextColor(dc, sub);
        DrawTextW(dc, detail.c_str(), -1, &label, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, oldFont);
}

void EnsureLineVisible(int line) {
    RECT rc;
    GetClientRect(rv.rows, &rc);
    int top = LineTop(line), bottom = top + LineHeight(line);
    if (line > 0 && rv.lines[(size_t)line - 1] < 0 && top - HeadingHeight() < rv.scroll) top -= HeadingHeight();
    if (top < rv.scroll) rv.scroll = top;
    else if (bottom > rv.scroll + rc.bottom) rv.scroll = bottom - rc.bottom;
    UpdateReview();
}

// The next proposal up or down, stepping over the section headings.
void MoveFocus(int direction) {
    for (int line = rv.focus + direction; line >= 0 && line < (int)rv.lines.size(); line += direction)
        if (rv.lines[(size_t)line] >= 0) {
            rv.focus = line;
            break;
        }
    if (rv.focus >= 0) EnsureLineVisible(rv.focus);
}

void ToggleFocused() {
    if (rv.focus < 0 || rv.lines[(size_t)rv.focus] < 0) return;
    size_t index = (size_t)rv.lines[(size_t)rv.focus];
    rv.checked[index] = !rv.checked[index];
    UpdateReview();
}

void SaveReview() {
    auto choices = coverfetch::CheckedChoices(rv.proposals, rv.checked);
    if (choices.empty()) return;
    DestroyWindow(rv.hwnd);
    SaveCovers(std::move(choices));
}

LRESULT CALLBACK RowsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: DoubleBufferedPaint(h, PaintRows); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: UpdateReview(); return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            switch (LOWORD(wp)) {
                case SB_LINEUP: rv.scroll -= RowHeight() / 2; break;
                case SB_LINEDOWN: rv.scroll += RowHeight() / 2; break;
                case SB_PAGEUP: rv.scroll -= (int)si.nPage; break;
                case SB_PAGEDOWN: rv.scroll += (int)si.nPage; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: rv.scroll = si.nTrackPos; break;
            }
            UpdateReview();
            return 0;
        }
        case WM_MOUSEWHEEL:
            rv.scroll -= GET_WHEEL_DELTA_WPARAM(wp) * RowHeight() / WHEEL_DELTA;
            UpdateReview();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            SetFocus(h);
            int y = GET_Y_LPARAM(lp) + rv.scroll;
            for (int line = 0, top = 0; line < (int)rv.lines.size(); top += LineHeight(line++))
                if (y >= top && y < top + LineHeight(line)) {
                    if (rv.lines[(size_t)line] >= 0) {
                        rv.focus = line;
                        ToggleFocused();
                    }
                    break;
                }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_UP) MoveFocus(-1);
            else if (wp == VK_DOWN) MoveFocus(1);
            return 0;
        case WM_CHAR:
            if (wp == L' ') ToggleFocused();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void LayoutReview() {
    RECT rc;
    GetClientRect(rv.hwnd, &rc);
    const int pad = S(14);
    SetWindowPos(rv.summary, nullptr, pad, pad, rc.right - pad * 2, S(40), SWP_NOZORDER);
    int bottom = rc.bottom - pad - S(30);
    SetWindowPos(rv.rows, nullptr, pad, pad + S(46), rc.right - pad * 2, std::max(S(60), bottom - S(40) - pad - S(46)),
                 SWP_NOZORDER);
    LayoutSaveChoices(rv.saves, pad, bottom - S(28));
    SetWindowPos(rv.save, nullptr, rc.right - pad - S(236), bottom, S(128), S(30), SWP_NOZORDER);
    SetWindowPos(rv.close, nullptr, rc.right - pad - S(100), bottom, S(100), S(30), SWP_NOZORDER);
}

std::wstring ReviewSummary(const coverfetch::BatchResult& r) {
    std::wstring text = L"Found covers for " + Plural(r.proposals.size(), L"album", L"albums") + L" of the " +
                        std::to_wstring(r.albums - r.hadCover) + L" without one.";
    if (r.limited) text += L" The search stopped at " + std::to_wstring(coverfetch::kMaxProposals) + L"; run it again after saving.";
    if (r.cancelled) text += L" The search was cancelled.";
    text += L" Nothing is saved until you choose Save.";
    return text;
}

LRESULT CALLBACK ReviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            rv.hwnd = h;
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            auto* summary = reinterpret_cast<std::wstring*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            rv.summary = make(WC_STATICW, summary->c_str(), SS_LEFT | SS_NOPREFIX, 0);
            rv.rows = make(kRowsClass, L"Proposed covers", WS_TABSTOP | WS_VSCROLL, kIdRows);
            CreateSaveChoices(h, rv.saves);
            rv.save = make(WC_BUTTONW, L"&Save", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, kIdSave);
            rv.close = make(WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, kIdClose);
            for (HWND c : {rv.rows, rv.save, rv.close}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            LayoutReview();
            UpdateReview();
            return 0;
        }
        case WM_SIZE:
            LayoutReview();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(620), S(380)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, TextColor());
        case WM_NOTIFY: {
            LRESULT result;
            if (SaveChoiceDraw(lp, result)) return result;
            break;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                case kIdSave: SaveReview(); break;
                case kIdClose:
                case IDCANCEL: DestroyWindow(h); break;
                default: OnSaveChoice(wp); break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            rv.hwnd = rv.rows = nullptr;
            std::vector<coverfetch::Proposal>().swap(rv.proposals);
            rv.checked.clear();
            rv.lines.clear();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowReview(coverfetch::BatchResult&& result) {
    if (rv.hwnd) DestroyWindow(rv.hwnd);
    std::wstring summary = ReviewSummary(result);
    rv.proposals = std::move(result.proposals);
    rv.checked.clear();
    for (const auto& p : rv.proposals) rv.checked.push_back(p.confident);
    rv.scroll = 0;
    BuildLines();
    RegisterAppClass(kRowsClass, RowsProc, CS_DBLCLKS, false);
    RegisterAppClass(kReviewClass, ReviewProc);
    RECT r{0, 0, S(680), S(560)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    // The review opens when the search ends, so it appears without taking the keyboard.
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kReviewClass, L"Review fetched covers",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - width) / 2, owner.top + S(60), width, height, g.hwnd,
                    nullptr, g.inst, &summary);
    if (rv.hwnd) ShowWindow(rv.hwnd, SW_SHOWNOACTIVATE);
}

void OnBatchDone(std::unique_ptr<BatchDone> done) {
    if (bt.worker.joinable()) bt.worker.join();
    bt.running = false;
    bt.cancel.reset();
    if (g.closing) return;
    coverfetch::BatchResult& r = done->result;
    if (r.proposals.empty()) {
        if (r.cancelled) ShowMessage(L"Cover search cancelled.");
        else if (r.hadCover == r.albums) ShowMessage(L"Every album already has a cover.");
        else ShowMessage(L"No covers found for the " + Plural(r.albums - r.hadCover, L"album", L"albums") + L" without one.");
        return;
    }
    ShowMessage(L"Found covers for " + Plural(r.proposals.size(), L"album", L"albums") + L". Review them before saving.");
    ShowReview(std::move(r));
}

}  // namespace

void ShowCoverSearch(const std::vector<TrackId>& ids, std::function<void(const std::string&)> pick) {
    auto albums = coverfetch::AlbumsOf(ids);
    if (albums.empty()) {
        ShowMessage(L"Select an album or some of its tracks to find a cover for.");
        return;
    }
    HWND owner = pick ? GetActiveWindow() : g.hwnd;
    if (pk.hwnd) DestroyWindow(pk.hwnd);  // one picker at a time
    // Dropping and OLE's clipboard need OLE on the UI thread, already in COM's apartment.
    static const bool ole = SUCCEEDED(OleInitialize(nullptr));
    (void)ole;
    pk.target = std::move(albums.front());
    pk.pick = std::move(pick);
    RegisterAppClass(kGridClass, GridProc, CS_DBLCLKS, false);
    RegisterAppClass(kPickerClass, PickerProc);
    RECT r{0, 0, S(760), S(580)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT box;
    GetWindowRect(owner ? owner : g.hwnd, &box);
    const int width = r.right - r.left, height = r.bottom - r.top;
    std::wstring what = pk.target.Label();
    if (pk.target.tracks.size() == 1 && g_lib.Contains(pk.target.tracks[0]))
        if (std::wstring title = g_lib.Str(g_lib[pk.target.tracks[0]].title); !title.empty()) what = title;
    std::wstring title = L"Change cover \x2013 " + what;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kPickerClass, title.c_str(),
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                    box.left + (box.right - box.left - width) / 2, box.top + S(40), width, height, owner, nullptr,
                    g.inst, nullptr);
    if (!pk.hwnd) return;
    ShowWindow(pk.hwnd, SW_SHOW);
    // With every source off nothing is asked until Search; the user's own image needs none.
    if (online::AnyCoverSource()) StartSearch();
    else Status(L"Choose From file, Paste or Google Images, or Search to find covers online.");
}

bool CoverSearchDialogMessage(MSG* msg) {
    // Ctrl+V pastes an image anywhere in the picker but its two text fields.
    if (pk.hwnd && msg->message == WM_KEYDOWN && msg->wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) &&
        (msg->hwnd == pk.hwnd || IsChild(pk.hwnd, msg->hwnd)) && msg->hwnd != pk.artist && msg->hwnd != pk.album) {
        PasteImage();
        return true;
    }
    return (pk.hwnd && IsDialogMessageW(pk.hwnd, msg)) || (rv.hwnd && IsDialogMessageW(rv.hwnd, msg));
}

namespace {

std::vector<std::wstring> PathsOf(const std::vector<TrackId>& ids) {
    std::vector<std::wstring> paths;
    for (TrackId id : ids)
        if (g_lib.Contains(id)) paths.push_back(g_lib.Path(id));
    return paths;
}

}  // namespace

bool HasCustomCover(const std::vector<TrackId>& ids) { return customcover::Count(PathsOf(ids)) > 0; }

void RestoreOriginalCovers(const std::vector<TrackId>& ids) {
    auto paths = PathsOf(ids);
    size_t had = customcover::Count(paths), restored = customcover::Clear(paths);
    if (had && !restored) {
        ShowMessage(L"Couldn't save the list of custom covers, so they stay as they are.");
        return;
    }
    RefreshCovers(ids);
    ShowMessage(restored ? L"Restored the original cover of " + Plural(restored, L"track", L"tracks") + L"."
                         : std::wstring(L"These tracks already show their own covers."));
}

void FetchMissingCovers() {
    if (bt.running) {
        ShowMessage(L"Cordinal is already looking for covers.");
        return;
    }
    if (rv.hwnd) {
        ShowWindow(rv.hwnd, SW_RESTORE);
        SetForegroundWindow(rv.hwnd);
        return;
    }
    if (!EnsureCoverSources(g.hwnd)) return;
    auto albums = coverfetch::LibraryAlbums();
    if (albums.empty()) {
        ShowMessage(L"The library has no albums with an album name to look for.");
        return;
    }
    if (bt.worker.joinable()) bt.worker.join();
    bt.cancel = std::make_shared<online::Cancel>();
    bt.running = true;
    const int thumb = std::min(S(48), 512);
    const HWND target = g.hwnd;
    ShowMessage(L"Looking for covers of albums that have none...");
    bt.worker = std::thread([albums = std::move(albums), cancel = bt.cancel, thumb, target] {
        ULONGLONG last = 0;
        auto* done = new BatchDone;
        coverfetch::RunBatch(albums, *cancel, DecodeFetched, thumb, [&](size_t i, size_t n) {
            ULONGLONG now = GetTickCount64();
            if (now - last < 500) return;
            last = now;
            auto* progress = new Progress{i, n};
            if (!PostMessageW(target, WM_APP_COVERFETCH, kMsgProgress, (LPARAM)progress)) delete progress;
        }, done->result);
        if (!PostMessageW(target, WM_APP_COVERFETCH, kMsgBatch, (LPARAM)done)) delete done;
    });
}

void CancelCoverFetch() {
    if (!bt.running || !bt.cancel) return;
    bt.cancel->Stop();
    ShowMessage(L"Stopping the cover search...");
}

bool CoverFetchBusy() { return bt.running; }

void OnCoverFetchMessage(WPARAM wp, LPARAM lp) {
    switch (wp) {
        case kMsgProgress: {
            std::unique_ptr<Progress> progress(reinterpret_cast<Progress*>(lp));
            if (bt.running && !g.closing && !(bt.cancel && bt.cancel->Stopped()))
                ShowMessage(L"Looking for covers: album " + std::to_wstring(progress->done + 1) + L" of " +
                            std::to_wstring(progress->total) + L"...");
            break;
        }
        case kMsgBatch: OnBatchDone(std::unique_ptr<BatchDone>(reinterpret_cast<BatchDone*>(lp))); break;
        case kMsgSaved: OnSaved(std::unique_ptr<Saved>(reinterpret_cast<Saved*>(lp))); break;
        default: DeletePayload(wp, lp); break;
    }
}

void StopCoverFetch() {
    if (pk.hwnd) DestroyWindow(pk.hwnd);
    if (rv.hwnd) DestroyWindow(rv.hwnd);
    if (bt.cancel) bt.cancel->Stop();
    if (bt.worker.joinable()) bt.worker.join();
    if (g_writer.joinable()) g_writer.join();
    MSG pending;
    while (PeekMessageW(&pending, g.hwnd, WM_APP_COVERFETCH, WM_APP_COVERFETCH, PM_REMOVE))
        DeletePayload(pending.wParam, pending.lParam);
    bt.running = false;
    bt.cancel.reset();
    g_writing = false;
    online::Stop();
}

void OnlineSettingsChanged() {
    const int save = std::clamp(online::Config().coverSave, 0, 2);
    for (HWND* radios : {pk.hwnd ? pk.saves : nullptr, rv.hwnd ? rv.saves : nullptr})
        for (int i = 0; radios && i < 3; ++i) Button_SetCheck(radios[i], i == save ? BST_CHECKED : BST_UNCHECKED);
    if (pk.hwnd && !pk.searching && pk.results.empty())
        Status(online::AnyCoverSource() ? L"Choose Search to ask the sources now on."
                                        : L"Choose From file, Paste or Google Images, or Search to find covers online.");
}

}  // namespace app
