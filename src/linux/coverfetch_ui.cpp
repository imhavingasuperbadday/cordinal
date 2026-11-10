// Covers on Linux, as app_coverfetch.cpp does on Windows, with the same wording:
//
//  - Change cover... opens a picker for the selected tracks' album: editable artist and album
//    fields, a grid of covers from every enabled source as they download (source, artist - album,
//    pixel size), and Use cover (or a double-click) to save one. The user's own image joins the
//    grid first: From file..., Paste (Ctrl+V) or a drop, as image data, a file or an address.
//    Google Images only opens the browser, since it has no keyless API. Closing stops the search.
//    The tag editor opens it over itself and receives the chosen image instead.
//  - Restore original cover removes the custom covers of the selected tracks.
//  - Library > Fetch missing covers searches every album without a cover on a worker, then a
//    review lists the proposals: good matches checked, lower scores apart and unchecked. Nothing
//    is written until Save.
// Covers are shown in cordinal only (customcover.h), saved as cover.jpg in the album folder or
// embedded through the tag writer as one undoable edit (Preferences > Online). Workers post
// WM_APP_COVERFETCH; results tagged with an older search are dropped.
#include "panels.h"
#include "ui_internal.h"

#include "backup.h"
#include "cover.h"
#include "coverfetch.h"
#include "customcover.h"
#include "draw.h"
#include "metadata_fetch.h"

#include <atomic>
#include <cctype>
#include <memory>
#include <tuple>

namespace lx {

namespace {

enum : WPARAM { kMsgFound, kMsgSearched, kMsgProgress, kMsgBatch, kMsgSaved, kMsgOwn };

struct Found {
    uint64_t generation;
    online::CoverFound cover;
};
struct Searched {
    uint64_t generation;
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

template <class T> void Post(WPARAM kind, T* payload) {
    if (!PostMessageW(nullptr, WM_APP_COVERFETCH, kind, (LPARAM)payload)) delete payload;
}

std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

std::string SourceList(const std::vector<online::CoverSource>& sources) {
    std::string text;
    for (size_t i = 0; i < sources.size(); ++i) {
        if (i) text += i + 1 == sources.size() ? " and " : ", ";
        text += Utf8(online::CoverSourceName(sources[i]));
    }
    return text;
}

std::string CandidateLabel(const online::CoverCandidate& c) {
    return c.artist.empty() ? c.album : c.artist + " \xE2\x80\x93 " + c.album;
}

std::string SizeText(const online::CoverCandidate& c) {
    return std::to_string(c.width) + " \xC3\x97 " + std::to_string(c.height);
}

// A small text box: the caret is kept at the end of what fits, as the tag editor's fields do.
struct Field {
    std::wstring text;
    size_t caret = 0;
    Rect rect;
};

void DrawField(const Field& f, bool focused, const Palette& p) {
    Fill(f.rect, p.surface2);
    Frame(f.rect, focused ? p.accent : p.border);
    std::string text = Utf8(f.text);
    float width = DrawText(Font::Normal, text, f.rect.x + S(6), TextY(f.rect), p.text, f.rect.w - S(12));
    if (focused) {
        float x = f.rect.x + S(6) + std::min(TextWidth(Font::Normal, Utf8(f.text.substr(0, f.caret))), width);
        Fill({x, f.rect.y + S(4), std::max(1.0f, S(1)), f.rect.h - S(8)}, p.text);
    }
}

bool EditField(Field& f, const SDL_KeyboardEvent& key) {
    switch (key.key) {
        case SDLK_BACKSPACE: if (f.caret) f.text.erase(--f.caret, 1); return true;
        case SDLK_DELETE: if (f.caret < f.text.size()) f.text.erase(f.caret, 1); return true;
        case SDLK_LEFT: if (f.caret) --f.caret; return true;
        case SDLK_RIGHT: if (f.caret < f.text.size()) ++f.caret; return true;
        case SDLK_HOME: f.caret = 0; return true;
        case SDLK_END: f.caret = f.text.size(); return true;
        default: return false;
    }
}

void DrawCheckBox(const Rect& b, bool on, const Palette& p) {
    if (on) {
        Fill(b, p.accent);
        uint32_t mark = TextOn(p.accent);
        ThickLine(b.x + b.w * 0.22f, b.y + b.h * 0.52f, b.x + b.w * 0.42f, b.y + b.h * 0.72f, S(1.6f), mark);
        ThickLine(b.x + b.w * 0.42f, b.y + b.h * 0.72f, b.x + b.w * 0.80f, b.y + b.h * 0.30f, S(1.6f), mark);
    } else {
        Fill(b, p.surface2);
        Frame(b, p.sub);
    }
}

// The three ways to save, as radio choices: a filled box marks the one in use.
void DrawSaveChoice(const Rect& r, const char* label, bool on, const Palette& p) {
    float box = S(14);
    Rect b{r.x, std::round(r.y + (r.h - box) / 2), box, box};
    Fill(b, p.surface2);
    Frame(b, on ? p.accent : p.sub);
    if (on) Fill({b.x + S(3), b.y + S(3), box - S(6), box - S(6)}, p.accent);
    DrawText(Font::Normal, label, b.R() + S(8), TextY(r), p.text, r.R() - b.R() - S(8));
}

void SetSaveChoice(int choice) {
    online::Config().coverSave = choice;
    SaveSettings();
    g.redraw = true;
}

// In the order shown; the labels are indexed by CoverSave. Each choice is as wide as its label, so
// the gaps between them stay even whatever the font.
const online::CoverSave kSaveOrder[] = {online::CoverSave::Custom, online::CoverSave::Folder, online::CoverSave::Embed};
const char* const kSaveLabels[] = {"Save as cover.jpg in the album folder", "Embed in the files' tags",
                                   "Show in cordinal only"};

// Lays out and draws the choices from `x`, keeping each one's rectangle for clicks.
void DrawSaveChoices(Rect (&rects)[3], float x, float y, const Palette& p) {
    for (online::CoverSave choice : kSaveOrder) {
        int i = (int)choice;
        rects[i] = {x, y, S(14) + S(8) + TextWidth(Font::Normal, kSaveLabels[i]) + S(4), S(22)};
        DrawSaveChoice(rects[i], kSaveLabels[i], online::Config().coverSave == i, p);
        x = rects[i].R() + S(18);
    }
}

bool ClickSaveChoice(const Rect (&rects)[3], float x, float y) {
    for (int i = 0; i < 3; ++i)
        if (rects[i].In(x, y)) {
            SetSaveChoice(i);
            return true;
        }
    return false;
}

struct Texture {
    SDL_Texture* texture = nullptr;
    ~Texture() { Release(); }
    void Release() {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
    }
};

void DrawThumb(const Rect& r, const online::Decoded& d, std::shared_ptr<Texture>& cache, const Palette& p) {
    Fill(r, p.surface2);
    if (d.thumb.empty()) return;
    if (!cache) cache = std::make_shared<Texture>();
    if (!cache->texture) {
        cache->texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, d.thumbWidth,
                                           d.thumbHeight);
        if (!cache->texture) return;
        SDL_UpdateTexture(cache->texture, nullptr, d.thumb.data(), d.thumbWidth * 4);
        SDL_SetTextureScaleMode(cache->texture, SDL_SCALEMODE_LINEAR);
    }
    float s = std::min(r.w / d.thumbWidth, r.h / d.thumbHeight);
    float w = d.thumbWidth * s, h = d.thumbHeight * s;
    SDL_FRect dst{r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
    SDL_RenderTexture(g.renderer, cache->texture, nullptr, &dst);
}

// --- saving --------------------------------------------------------------------------------------

std::thread g_writer;
std::atomic<bool> g_writing{false};

void SaveCovers(std::vector<coverfetch::CoverChoice> choices) {
    if (choices.empty()) return;
    if (online::Config().coverSave == (int)online::CoverSave::Embed) {
        size_t unsupported = 0;
        auto edits = coverfetch::EmbedEdits(choices, unsupported);
        if (edits.empty()) {
            ShowMessage(L"These files can't hold an embedded cover. Save covers in the album folder instead "
                        L"(Preferences > Online).");
            return;
        }
        // One tag write is one undo record: Edit > Undo last tag edit takes every cover back out.
        if (SubmitTagWrites(edits, true) && unsupported)
            ShowMessage(L"Embedding covers; " + Plural(unsupported, L"file", L"files") + L" can't hold one and stay as they are.");
        return;
    }
    if (g_writing) {
        ShowMessage(L"Covers are still being saved. Try again in a moment.");
        return;
    }
    if (g_writer.joinable()) g_writer.join();
    g_writing = true;
    const bool overwrite = online::Config().coverOverwrite,
               custom = online::Config().coverSave == (int)online::CoverSave::Custom;
    g_writer = std::thread([choices = std::move(choices), overwrite, custom] {
        Post(kMsgSaved, new Saved{custom ? coverfetch::SaveCustom(choices) : coverfetch::SaveToFolders(choices, overwrite)});
    });
    ShowMessage(L"Saving covers...");
}

void OnSaved(std::unique_ptr<Saved> saved) {
    if (g_writer.joinable()) g_writer.join();
    g_writing = false;
    const coverfetch::FolderResult& r = saved->result;
    if (!r.changed.empty()) ForgetCoverArt();
    std::wstring text = !r.written ? std::wstring(L"No cover was saved.")
                        : r.custom ? L"cordinal now shows " + Plural(r.written, L"chosen cover", L"chosen covers") +
                                         L"; the music files are unchanged."
                                   : L"Saved " + Plural(r.written, L"cover", L"covers") + L" in the album folder.";
    if (r.kept)
        text += L" " + Plural(r.kept, L"album folder", L"album folders") +
                L" already had a cover file and kept it (Preferences > Online can replace them).";
    if (r.failed) {
        text += L" " + Plural(r.failed, L"cover", L"covers") + L" couldn't be saved";
        if (!r.errors.empty()) text += L": " + r.errors.front();
        text += L".";
    }
    ShowMessage(text);
}

// --- the picker and the review -------------------------------------------------------------------

struct Tile {
    online::CoverFound cover;
    std::shared_ptr<Texture> texture;
    Rect rect;
};

struct PickerState {
    bool open = false, overTags = false;
    coverfetch::Album target;
    std::function<void(const std::string&)> pick;
    Field artist, album;
    int focus = 0;  // 0 artist, 1 album, 2 the grid
    // The user's own images first, newest first; then the found ones by source, best first.
    std::vector<Tile> tiles;
    int selected = -1;
    float scroll = 0;
    std::string status;
    online::Generation generation, ownGeneration;
    std::shared_ptr<online::Cancel> cancel, ownCancel;
    std::thread worker, ownWorker;
    bool searching = false;
    Rect grid, search, use, cancelButton, file, paste, google;
    Rect saves[3];  // by CoverSave
} pk;

struct ReviewRow {
    coverfetch::Proposal proposal;
    std::shared_ptr<Texture> texture;
    bool checked = false;
};

struct ReviewState {
    bool open = false;
    std::vector<ReviewRow> rows;
    std::vector<int> lines;  // row index, or -1 / -2 for the two section headings
    int focus = -1;
    float scroll = 0;
    std::wstring summary;
    Rect list, save, cancel;
    Rect saves[3];  // by CoverSave
} rv;

coverfetch::BatchResult g_pending;  // a finished batch waiting for its review to be opened
bool g_havePending = false;
bool g_stopped = false;  // shutting down: what is still queued is only freed

struct Batch {
    std::thread worker;
    std::shared_ptr<online::Cancel> cancel;
    bool running = false;
} bt;

void StopSearch() {
    if (pk.cancel) pk.cancel->Stop();
    if (pk.worker.joinable()) pk.worker.join();
    pk.cancel.reset();
    pk.searching = false;
    pk.generation.Next();
}

void StopOwn() {
    if (pk.ownCancel) pk.ownCancel->Stop();
    if (pk.ownWorker.joinable()) pk.ownWorker.join();
    pk.ownCancel.reset();
    pk.ownGeneration.Next();
}

void StartSearch() {
    StopSearch();
    std::erase_if(pk.tiles, [](const Tile& t) { return t.cover.origin.empty(); });
    pk.selected = -1;
    pk.scroll = 0;
    online::CoverSearch search;
    search.query = {ToUtf8(pk.artist.text), ToUtf8(pk.album.text)};
    search.sources = online::CoverOrder();
    search.manual = true;
    search.thumbSize = std::min((int)S(140), 1024);
    if (search.sources.empty()) {
        pk.status = "Cover sources are off. A file, a pasted image or Google Images work without them.";
    } else if (search.query.artist.empty() && search.query.album.empty()) {
        pk.status = "Type an artist or an album to search for.";
    } else {
        const uint64_t generation = pk.generation.Next();
        pk.cancel = std::make_shared<online::Cancel>();
        pk.searching = true;
        pk.status = "Asking " + SourceList(search.sources) + "...";
        pk.worker = std::thread([search, generation, cancel = pk.cancel] {
            online::SearchCovers(search, *cancel, DecodeFetchedCover, [&](online::CoverFound&& cover) {
                Post(kMsgFound, new Found{generation, std::move(cover)});
            });
            Post(kMsgSearched, new Searched{generation});
        });
    }
    g.redraw = true;
}

void AddTile(online::CoverFound&& cover) {
    const auto order = online::CoverOrder();
    auto rank = [&](const online::CoverFound& f) -> ptrdiff_t {
        return f.origin.empty() ? std::find(order.begin(), order.end(), f.candidate.source) - order.begin() : -1;
    };
    auto at = std::find_if(pk.tiles.begin(), pk.tiles.end(), [&](const Tile& t) {
        auto a = rank(t.cover), b = rank(cover);
        return a > b || (a == b && t.cover.candidate.score < cover.candidate.score);
    });
    int index = (int)(at - pk.tiles.begin());
    if (pk.selected >= index) ++pk.selected;
    Tile tile;
    tile.cover = std::move(cover);
    pk.tiles.insert(at, std::move(tile));
    pk.status = "Found " + Utf8(Plural(pk.tiles.size(), L"cover", L"covers")) + " so far...";
    g.redraw = true;
}

// --- the user's own image ------------------------------------------------------------------------

// Checks and decodes the image on a worker: `bytes` as handed over, or the image `text` holds or
// leads to (an address is downloaded). `origin` and `label` name it in the grid.
void TakeOwn(std::string bytes, std::string text, std::string origin, std::string label) {
    StopOwn();
    const uint64_t generation = pk.ownGeneration.Next();
    pk.ownCancel = std::make_shared<online::Cancel>();
    const int thumb = std::min((int)S(140), 1024);
    pk.status = text.empty() ? "Reading the image..." : "Getting the image...";
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
        } else if (!DecodeFetchedCover(bytes, thumb, own->cover.decoded)) {
            own->error = L"The image couldn't be read.";
        } else {
            own->cover.origin = origin;
            own->cover.candidate.album = label;
            own->cover.candidate.width = own->cover.decoded.width;
            own->cover.candidate.height = own->cover.decoded.height;
            own->cover.image = std::move(bytes);
        }
        Post(kMsgOwn, own);
    });
    g.redraw = true;
}

void AddOwn(online::CoverFound&& cover) {
    Tile tile;
    tile.cover = std::move(cover);
    pk.tiles.insert(pk.tiles.begin(), std::move(tile));
    pk.selected = 0;
    pk.scroll = 0;
    pk.focus = 2;
    pk.status = "Your image is first in the list. Double-click it, or choose Use cover.";
    g.redraw = true;
}

void TakeFile(const std::wstring& file) {
    std::string bytes;
    if (!backup::ReadBytes(file, bytes, online::kMaxImageBytes)) {
        pk.status = "Couldn't read " + Utf8(FileNameOf(file)) + ". Covers can be JPEG or PNG images of at most 20 MB.";
        g.redraw = true;
        return;
    }
    TakeOwn(std::move(bytes), {}, "From a file", Utf8(FileNameOf(file)));
}

// Text from a paste or a drop: a file:// address or an absolute path names a file; anything else
// may be an image, or the address of one.
void TakeText(std::string text, const char* origin) {
    // A uri-list may hold several lines and comments; the first address is the one.
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of("\r\n", start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line[0] != '#') {
            text = line;
            break;
        }
    }
    if (text.starts_with("file://") || text.starts_with("/")) {
        std::string path = text.starts_with("file://") ? text.substr(text.find('/', 7)) : text, decoded;
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '%' && i + 2 < path.size() && isxdigit((unsigned char)path[i + 1]) &&
                isxdigit((unsigned char)path[i + 2])) {
                decoded += (char)std::stoi(path.substr(i + 1, 2), nullptr, 16);
                i += 2;
            } else {
                decoded += path[i];
            }
        }
        TakeFile(compat::Widen(decoded.c_str()));
        return;
    }
    std::string host = text.starts_with("https://") ? text.substr(8) : std::string("Image");
    host = host.substr(0, host.find('/'));
    TakeOwn({}, std::move(text), origin, host);
}

void PasteImage() {
    for (const char* mime : {"image/png", "image/jpeg"}) {
        size_t size = 0;
        void* data = SDL_HasClipboardData(mime) ? SDL_GetClipboardData(mime, &size) : nullptr;
        if (!data) continue;
        std::string bytes(static_cast<const char*>(data), size);
        SDL_free(data);
        TakeOwn(std::move(bytes), {}, "Pasted", "Image");
        return;
    }
    size_t size = 0;
    if (void* data = SDL_HasClipboardData("text/uri-list") ? SDL_GetClipboardData("text/uri-list", &size) : nullptr) {
        std::string text(static_cast<const char*>(data), size);
        SDL_free(data);
        TakeText(std::move(text), "Pasted");
        return;
    }
    if (char* clip = SDL_HasClipboardText() ? SDL_GetClipboardText() : nullptr) {
        std::string text = clip;
        SDL_free(clip);
        if (!text.empty()) {
            TakeText(std::move(text), "Pasted");
            return;
        }
    }
    pk.status = "The clipboard holds no image. In the browser, choose Copy image (or Copy image address), then paste here.";
    g.redraw = true;
}

void ChooseImageFile() {
    static const SDL_DialogFileFilter filter{"Images", "jpg;jpeg;png"};
    auto chosen = [](void*, const char* const* list, int) {
        if (!list || !*list) return;
        auto* path = new std::wstring(compat::Widen(*list));
        if (!PostMessageW(nullptr, WM_APP_FILE, (WPARAM)kFileCoverPick, (LPARAM)path)) delete path;
    };
    SDL_ShowOpenFileDialog(chosen, nullptr, g.window, &filter, 1, nullptr, false);
}

// Google Images has no API to ask without a key, so the browser shows the search and the user
// pastes or drags back the picture they like.
void OpenGoogleImages() {
    std::string query = ToUtf8(pk.artist.text + L" " + pk.album.text);
    query = query.find_first_not_of(' ') == std::string::npos ? std::string("album cover") : query + " cover";
    SDL_OpenURL(("https://www.google.com/search?tbm=isch&q=" + online::UrlEncode(query)).c_str());
    pk.status = "Google Images is open in your browser. Copy an image or its address and press Ctrl+V here, or drag it in.";
    g.redraw = true;
}

void UsePick() {
    if (pk.selected < 0 || pk.selected >= (int)pk.tiles.size()) return;
    std::string image = pk.tiles[(size_t)pk.selected].cover.image;
    auto pick = pk.pick;
    coverfetch::Album target = pk.target;
    if (pick) pick(image);
    CoverPanelDone();
    if (!pick) SaveCovers({{std::move(target), std::move(image)}});
}

constexpr float kTile = 140, kCellW = 154, kCellH = 198;

int Columns() { return std::max(1, (int)((pk.grid.w - S(10)) / S(kCellW))); }

void ClampGridScroll() {
    int rows = ((int)pk.tiles.size() + Columns() - 1) / Columns();
    pk.scroll = std::clamp(pk.scroll, 0.0f, std::max(0.0f, rows * S(kCellH) + S(10) - pk.grid.h));
}

void RevealSelected() {
    if (pk.selected < 0) return;
    float top = S(10) + pk.selected / Columns() * S(kCellH);
    if (top - S(10) < pk.scroll) pk.scroll = top - S(10);
    else if (top + S(kCellH) > pk.scroll + pk.grid.h) pk.scroll = top + S(kCellH) - pk.grid.h;
    ClampGridScroll();
}

void DrawPicker(const Palette& p) {
    Rect panel = DrawFrame(p, S(780), S(620), "Change cover");
    const float pad = S(16), label = S(52), button = S(88), field = S(26), gap = S(8);
    float y = panel.y + S(48), right = panel.R() - pad - button - gap, half = (right - panel.x - pad) / 2;
    DrawText(Font::Normal, "Artist", panel.x + pad, y + S(4), p.text);
    pk.artist.rect = {panel.x + pad + label, y, half - label - gap, field};
    DrawText(Font::Normal, "Album", panel.x + pad + half, y + S(4), p.text);
    pk.album.rect = {panel.x + pad + half + label, y, right - panel.x - pad - half - label, field};
    DrawField(pk.artist, pk.focus == 0, p);
    DrawField(pk.album, pk.focus == 1, p);
    pk.search = {panel.R() - pad - button, y, button, field};
    DrawButton(pk.search, "Search", p);
    y += field + S(12);
    // The footer sits under a hairline: how to keep the cover, then the ways to add an image of
    // one's own and the dialog's buttons. The tag editor's picker has no save choices; how to save
    // is the tag editor's business.
    float bottom = panel.B() - pad - S(30), saves = bottom - S(10) - S(22);
    float divider = (pk.overTags ? bottom : saves) - S(12), statusTop = divider - S(8) - S(20);
    pk.grid = {panel.x + pad, y, panel.w - pad * 2, statusTop - S(8) - y};
    Fill(pk.grid, p.bg);
    Frame(pk.grid, pk.focus == 2 ? p.accent : p.border);
    ClampGridScroll();
    if (pk.tiles.empty())
        DrawText(Font::Normal, pk.searching ? "Covers appear here as they are found." : "No covers to show.",
                 pk.grid.x, TextY(pk.grid), p.sub, pk.grid.w, 1);
    SDL_Rect clip{(int)pk.grid.x + 1, (int)pk.grid.y + 1, (int)pk.grid.w - 2, (int)pk.grid.h - 2};
    SDL_SetRenderClipRect(g.renderer, &clip);
    const int cols = Columns();
    const float line = LineHeight(Font::Small) + S(2);
    for (int i = 0; i < (int)pk.tiles.size(); ++i) {
        Tile& t = pk.tiles[(size_t)i];
        float x = pk.grid.x + S(10) + i % cols * S(kCellW), ty = pk.grid.y + S(10) + i / cols * S(kCellH) - pk.scroll;
        t.rect = {x - S(5), ty - S(5), S(kTile) + S(10), S(kCellH) - S(8)};
        if (t.rect.B() < pk.grid.y || t.rect.y > pk.grid.B()) continue;
        bool selected = i == pk.selected;
        if (selected) Fill(t.rect, p.select);
        Rect image{x, ty, S(kTile), S(kTile)};
        DrawThumb(image, t.cover.decoded, t.texture, p);
        Frame(image, p.border);
        uint32_t text = selected ? p.selectText : p.text, sub = selected ? p.selectText : p.sub;
        float ly = ty + S(kTile) + S(4);
        std::string source = t.cover.origin.empty() ? Utf8(online::CoverSourceName(t.cover.candidate.source)) : t.cover.origin;
        DrawText(Font::Small, source, x, ly, sub, S(kTile));
        DrawText(Font::Small, CandidateLabel(t.cover.candidate), x, ly + line, text, S(kTile));
        DrawText(Font::Small, SizeText(t.cover.candidate), x, ly + line * 2, sub, S(kTile));
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
    DrawText(Font::Small, pk.status, panel.x + pad, statusTop + S(4), p.sub, panel.w - pad * 2);
    Fill({panel.x + pad, divider, panel.w - pad * 2, 1}, p.border);
    for (Rect& r : pk.saves) r = {};
    if (!pk.overTags) DrawSaveChoices(pk.saves, panel.x + pad, saves, p);
    float x = panel.x + pad;
    for (auto [rect, width, label] : {std::tuple{&pk.file, 104.0f, "From file..."}, {&pk.paste, 80.0f, "Paste"},
                                      {&pk.google, 124.0f, "Google Images"}}) {
        *rect = {x, bottom, S(width), S(30)};
        DrawButton(*rect, label, p);
        x = rect->R() + gap;
    }
    pk.use = {panel.R() - pad - S(100) - gap - S(108), bottom, S(108), S(30)};
    pk.cancelButton = {panel.R() - pad - S(100), bottom, S(100), S(30)};
    DrawButton(pk.use, "Use cover", p, true, pk.selected >= 0);
    DrawButton(pk.cancelButton, "Cancel", p);
}

void BuildLines() {
    rv.lines.clear();
    size_t confident = 0;
    for (const auto& r : rv.rows) confident += r.proposal.confident;
    if (confident) rv.lines.push_back(-1);
    for (size_t i = 0; i < confident; ++i) rv.lines.push_back((int)i);
    if (confident < rv.rows.size()) rv.lines.push_back(-2);
    for (size_t i = confident; i < rv.rows.size(); ++i) rv.lines.push_back((int)i);
    rv.focus = -1;
    for (size_t i = 0; i < rv.lines.size() && rv.focus < 0; ++i)
        if (rv.lines[i] >= 0) rv.focus = (int)i;
}

float LineH(int line) { return rv.lines[(size_t)line] < 0 ? S(28) : S(62); }
float LineTop(int line) {
    float y = 0;
    for (int i = 0; i < line; ++i) y += LineH(i);
    return y;
}

size_t CheckedCount() {
    size_t n = 0;
    for (const auto& r : rv.rows) n += r.checked;
    return n;
}

void RevealFocus() {
    if (rv.focus < 0) return;
    float top = LineTop(rv.focus), bottom = top + LineH(rv.focus);
    if (rv.focus > 0 && rv.lines[(size_t)rv.focus - 1] < 0) top -= LineH(rv.focus - 1);
    if (top < rv.scroll) rv.scroll = top;
    else if (bottom > rv.scroll + rv.list.h) rv.scroll = bottom - rv.list.h;
}

void DrawReview(const Palette& p) {
    Rect panel = DrawFrame(p, S(720), S(600), "Review fetched covers");
    const float pad = S(16);
    float y = panel.y + S(46);
    for (const std::string& text : Wrap(Font::Normal, Utf8(rv.summary), panel.w - pad * 2)) {
        DrawText(Font::Normal, text, panel.x + pad, y, p.text, panel.w - pad * 2);
        y += LineHeight(Font::Normal);
    }
    y += S(8);
    float bottom = panel.B() - pad - S(30);
    rv.list = {panel.x + pad, y, panel.w - pad * 2, bottom - S(40) - y};
    Fill(rv.list, p.bg);
    Frame(rv.list, p.border);
    rv.scroll = std::clamp(rv.scroll, 0.0f, std::max(0.0f, LineTop((int)rv.lines.size()) - rv.list.h));
    SDL_Rect clip{(int)rv.list.x + 1, (int)rv.list.y + 1, (int)rv.list.w - 2, (int)rv.list.h - 2};
    SDL_SetRenderClipRect(g.renderer, &clip);
    size_t confident = 0;
    for (const auto& r : rv.rows) confident += r.proposal.confident;
    float ly = rv.list.y - rv.scroll;
    for (int line = 0; line < (int)rv.lines.size(); ++line) {
        float h = LineH(line);
        Rect r{rv.list.x, ly, rv.list.w, h};
        ly += h;
        if (r.B() < rv.list.y || r.y > rv.list.B()) continue;
        int index = rv.lines[(size_t)line];
        if (index < 0) {
            std::string heading = index == -1
                ? "Good matches (" + std::to_string(confident) + ")"
                : "Lower scores, unchecked: check the ones to use (" + std::to_string(rv.rows.size() - confident) + ")";
            DrawText(Font::Bold, heading, r.x + S(10), r.y + S(6), p.text, r.w - S(20));
            continue;
        }
        ReviewRow& row = rv.rows[(size_t)index];
        bool focused = line == rv.focus;
        if (focused) Fill(r, p.select);
        uint32_t text = focused ? p.selectText : p.text, sub = focused ? p.selectText : p.sub;
        float box = S(16);
        DrawCheckBox({r.x + S(10), r.y + (h - box) / 2, box, box}, row.checked, p);
        Rect image{r.x + S(36), r.y + (h - S(48)) / 2, S(48), S(48)};
        DrawThumb(image, row.proposal.cover.decoded, row.texture, p);
        Frame(image, p.border);
        float tx = image.R() + S(10);
        DrawText(Font::Normal, Utf8(row.proposal.album.Label()), tx, r.y + S(12), text, r.R() - tx - S(10));
        char score[32];
        snprintf(score, sizeof(score), "%.1f", row.proposal.cover.candidate.score);
        const auto& c = row.proposal.cover.candidate;
        std::string detail = Utf8(online::CoverSourceName(c.source)) + "  \xE2\x80\xA2  " + CandidateLabel(c) +
                             "  \xE2\x80\xA2  " + SizeText(c) + "  \xE2\x80\xA2  score " + score;
        DrawText(Font::Small, detail, tx, r.y + S(33), sub, r.R() - tx - S(10));
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
    DrawSaveChoices(rv.saves, panel.x + pad, bottom - S(28), p);
    size_t count = CheckedCount();
    rv.save = {panel.R() - pad - S(236), bottom, S(128), S(30)};
    rv.cancel = {panel.R() - pad - S(100), bottom, S(100), S(30)};
    DrawButton(rv.save, count ? "Save " + Utf8(Plural(count, L"cover", L"covers")) : std::string("Save"), p, true, count > 0);
    DrawButton(rv.cancel, "Cancel", p);
}

void SaveReview() {
    std::vector<coverfetch::Proposal> proposals;
    std::vector<bool> checked;
    for (const auto& r : rv.rows) {
        proposals.push_back(r.proposal);
        checked.push_back(r.checked);
    }
    auto choices = coverfetch::CheckedChoices(proposals, checked);
    if (choices.empty()) return;
    CoverPanelDone();
    SaveCovers(std::move(choices));
}

void ToggleFocus() {
    if (rv.focus < 0 || rv.lines[(size_t)rv.focus] < 0) return;
    ReviewRow& row = rv.rows[(size_t)rv.lines[(size_t)rv.focus]];
    row.checked = !row.checked;
    g.redraw = true;
}

void MoveFocus(int direction) {
    for (int line = rv.focus + direction; line >= 0 && line < (int)rv.lines.size(); line += direction)
        if (rv.lines[(size_t)line] >= 0) {
            rv.focus = line;
            break;
        }
    RevealFocus();
    g.redraw = true;
}

std::wstring ReviewSummary(const coverfetch::BatchResult& r) {
    std::wstring text = L"Found covers for " + Plural(r.proposals.size(), L"album", L"albums") + L" of the " +
                        std::to_wstring(r.albums - r.hadCover) + L" without one.";
    if (r.limited) text += L" The search stopped at " + std::to_wstring(coverfetch::kMaxProposals) + L"; run it again after saving.";
    if (r.cancelled) text += L" The search was cancelled.";
    text += L" Nothing is saved until you choose Save.";
    return text;
}

void OpenReview(coverfetch::BatchResult&& result) {
    rv = ReviewState{};
    rv.open = true;
    rv.summary = ReviewSummary(result);
    for (auto& proposal : result.proposals) {
        ReviewRow row;
        row.checked = proposal.confident;
        row.proposal = std::move(proposal);
        rv.rows.push_back(std::move(row));
    }
    BuildLines();
    ShowCoverPanel(false);
}

// A manual search may start while every source is off, but only after the user agrees to what it
// sends. Agreeing turns the three sources on, as Preferences would.
bool EnsureCoverSources() {
    if (online::AnyCoverSource()) return true;
    const char* text =
        "Cover sources are off, so cordinal hasn't contacted any of them.\n\n"
        "Finding a cover sends the album's artist and title to:\n"
        "    \xE2\x80\xA2  Deezer (api.deezer.com)\n"
        "    \xE2\x80\xA2  iTunes (itunes.apple.com)\n"
        "    \xE2\x80\xA2  MusicBrainz (musicbrainz.org), then the release it finds to coverartarchive.org\n\n"
        "Turn these sources on? Each can be changed in Preferences > Online.";
    if (!Confirm(text, "Find covers online")) return false;
    for (bool& on : online::Config().covers) on = true;
    SaveSettings();
    return true;
}

void OpenPicker(const std::vector<TrackId>& ids, std::function<void(const std::string&)> pick, bool overTags) {
    auto albums = coverfetch::AlbumsOf(ids);
    if (albums.empty()) {
        ShowMessage(L"Select an album or some of its tracks to find a cover for.");
        return;
    }
    if (pk.open) CoverPanelClose();
    pk.open = true;
    pk.overTags = overTags;
    pk.target = std::move(albums.front());
    pk.pick = std::move(pick);
    pk.artist = {FromUtf8(pk.target.artist), 0, {}};
    pk.album = {FromUtf8(pk.target.album), 0, {}};
    pk.artist.caret = pk.artist.text.size();
    pk.album.caret = pk.album.text.size();
    pk.focus = 2;
    ShowCoverPanel(overTags);
    // With every source off nothing is asked until Search; the user's own image needs none.
    if (online::AnyCoverSource()) StartSearch();
    else pk.status = "Choose From file, Paste or Google Images, or Search to find covers online.";
}

// The Search button: asks before contacting any source while all are off.
void SearchClicked() {
    if (EnsureCoverSources()) StartSearch();
    else pk.status = "Cover sources are off. A file, a pasted image or Google Images work without them.";
    g.redraw = true;
}

std::vector<std::wstring> PathsOf(const std::vector<TrackId>& ids) {
    std::vector<std::wstring> paths;
    for (TrackId id : ids)
        if (g_lib.Contains(id)) paths.push_back(g_lib.Path(id));
    return paths;
}

void OnBatchDone(std::unique_ptr<BatchDone> done) {
    if (bt.worker.joinable()) bt.worker.join();
    bt.running = false;
    bt.cancel.reset();
    coverfetch::BatchResult& r = done->result;
    if (r.proposals.empty()) {
        if (r.cancelled) ShowMessage(L"Cover search cancelled.");
        else if (r.hadCover == r.albums) ShowMessage(L"Every album already has a cover.");
        else ShowMessage(L"No covers found for the " + Plural(r.albums - r.hadCover, L"album", L"albums") + L" without one.");
        return;
    }
    // The review opens at once only when nothing else is in the way; otherwise it waits for
    // Library > Fetch missing covers, so a search ending never interrupts other work.
    if (PanelOpen()) {
        g_pending = std::move(r);
        g_havePending = true;
        ShowMessage(L"Found covers for " + Plural(g_pending.proposals.size(), L"album", L"albums") +
                    L". Choose Library > Fetch missing covers to review them.");
        return;
    }
    ShowMessage(L"Found covers for " + Plural(r.proposals.size(), L"album", L"albums") + L". Review them before saving.");
    OpenReview(std::move(r));
}

}  // namespace

void OpenCoverSearch(const std::vector<TrackId>& ids) { OpenPicker(ids, {}, false); }

void OpenCoverSearchForTags(const std::vector<TrackId>& ids, std::function<void(const std::string&)> pick) {
    OpenPicker(ids, std::move(pick), true);
}

bool HasCustomCover(const std::vector<TrackId>& ids) { return customcover::Count(PathsOf(ids)) > 0; }

void RestoreOriginalCovers(const std::vector<TrackId>& ids) {
    auto paths = PathsOf(ids);
    size_t had = customcover::Count(paths), restored = customcover::Clear(paths);
    if (had && !restored) {
        ShowMessage(L"Couldn't save the list of custom covers, so they stay as they are.");
        return;
    }
    if (restored) ForgetCoverArt();
    ShowMessage(restored ? L"Restored the original cover of " + Plural(restored, L"track", L"tracks") + L"."
                         : std::wstring(L"These tracks already show their own covers."));
}

void CoverPanelFile(const std::wstring& path) {
    if (pk.open && !rv.open) TakeFile(path);
}

// --- covers without the picker, as on Windows ------------------------------------------------------

namespace {

std::vector<TrackId> g_coverFileFor;  // the selection Choose image file... was asked for

// Gives every album of `ids`, whole, the image. A cover belongs to the album, so one selected track
// is enough; an album spread over disc folders gets it in each.
void UseImageFor(const std::vector<TrackId>& ids, std::string image) {
    int w = 0, h = 0;
    if (online::InspectImage(image, w, h) == online::ImageType::None) {
        ShowMessage(L"Covers have to be complete JPEG or PNG images of at most 20 MB.");
        return;
    }
    auto albums = coverfetch::AlbumsOf(AlbumTracksOf(ids));
    if (albums.empty()) {
        ShowMessage(L"Select an album or one of its tracks first.");
        return;
    }
    std::vector<coverfetch::CoverChoice> choices;
    for (auto& album : albums) choices.push_back({std::move(album), image, {}});
    SaveCovers(std::move(choices));
}

}  // namespace

bool IsCoverImageFile(const std::wstring& path) {
    std::wstring ext = ExtOf(path);
    for (wchar_t& c : ext) c = (wchar_t)towlower(c);
    return ext == L"jpg" || ext == L"jpeg" || ext == L"png";
}

void SetCoverFromFile(const std::vector<TrackId>& ids, const std::wstring& file) {
    std::string bytes;
    if (!backup::ReadBytes(file, bytes, online::kMaxImageBytes)) {
        ShowMessage(L"Couldn't read " + FileNameOf(file) + L". Covers can be JPEG or PNG images of at most 20 MB.");
        return;
    }
    UseImageFor(ids, std::move(bytes));
}

void ChooseCoverFile(const std::vector<TrackId>& ids) {
    if (ids.empty()) {
        ShowMessage(L"Select an album or one of its tracks first.");
        return;
    }
    g_coverFileFor = ids;
    static const SDL_DialogFileFilter filter{"Images", "jpg;jpeg;png"};
    auto chosen = [](void*, const char* const* list, int) {
        if (!list || !*list) return;
        auto* path = new std::wstring(compat::Widen(*list));
        if (!PostMessageW(nullptr, WM_APP_FILE, (WPARAM)kFileCoverDirect, (LPARAM)path)) delete path;
    };
    // Start in the album's folder, where a scan or download usually left its pictures.
    std::string folder = g_lib.Contains(ids.front()) ? ToUtf8(DirectoryOf(g_lib.Path(ids.front()))) : std::string();
    SDL_ShowOpenFileDialog(chosen, nullptr, g.window, &filter, 1, folder.empty() ? nullptr : folder.c_str(), false);
}

void CoverFileChosen(const std::wstring& path) {
    auto ids = std::move(g_coverFileFor);
    g_coverFileFor.clear();
    std::erase_if(ids, [](TrackId id) { return !g_lib.Contains(id); });
    if (!ids.empty()) SetCoverFromFile(ids, path);
}

bool ClipboardMayHoldCover() {
    return SDL_HasClipboardData("image/png") || SDL_HasClipboardData("image/jpeg") ||
           SDL_HasClipboardData("text/uri-list") || SDL_HasClipboardText();
}

// The clipboard's picture: image data, or a copied file. An address is handed to the picker, which
// downloads it and shows it first.
void PasteCover(const std::vector<TrackId>& ids) {
    if (ids.empty()) {
        ShowMessage(L"Select an album or one of its tracks first.");
        return;
    }
    for (const char* mime : {"image/png", "image/jpeg"}) {
        size_t size = 0;
        void* data = SDL_HasClipboardData(mime) ? SDL_GetClipboardData(mime, &size) : nullptr;
        if (!data) continue;
        std::string bytes(static_cast<const char*>(data), size);
        SDL_free(data);
        UseImageFor(ids, std::move(bytes));
        return;
    }
    std::string text;
    size_t size = 0;
    if (void* data = SDL_HasClipboardData("text/uri-list") ? SDL_GetClipboardData("text/uri-list", &size) : nullptr) {
        text.assign(static_cast<const char*>(data), size);
        SDL_free(data);
    } else if (char* clip = SDL_HasClipboardText() ? SDL_GetClipboardText() : nullptr) {
        text = clip;
        SDL_free(clip);
    }
    text = text.substr(0, text.find_first_of("\r\n"));
    if (text.starts_with("file://") || text.starts_with("/")) {
        std::string path = text.starts_with("file://") ? text.substr(text.find('/', 7)) : text, decoded;
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '%' && i + 2 < path.size() && isxdigit((unsigned char)path[i + 1]) &&
                isxdigit((unsigned char)path[i + 2])) {
                decoded += (char)std::stoi(path.substr(i + 1, 2), nullptr, 16);
                i += 2;
            } else {
                decoded += path[i];
            }
        }
        std::wstring file = compat::Widen(decoded.c_str());
        if (IsCoverImageFile(file)) SetCoverFromFile(ids, file);
        else ShowMessage(L"The copied file isn't a JPEG or PNG image.");
        return;
    }
    if (text.starts_with("https://") || text.starts_with("http://")) {
        OpenPicker(AlbumTracksOf(ids), {}, false);
        if (pk.open) PasteImage();
        return;
    }
    ShowMessage(L"The clipboard holds no image. In the browser, choose Copy image, then paste it as the cover.");
}

bool DropCover(const char* data, float x, float y) {
    if (pk.open || !data || !*data) return false;
    const std::wstring file = compat::Widen(data);
    if (!IsCoverImageFile(file)) return false;
    std::vector<TrackId> ids;
    int line = !CanvasActive() && L.rows.In(x, y) ? LineAt(y) : -1;
    if (line >= 0 && line < (int)g_lines.size() && g_lines[line].row >= 0 && g_lines[line].row < (int)g.rows.size())
        ids.push_back(g.rows[(size_t)g_lines[line].row]);
    if (ids.empty()) ids = SelectedTracks();
    if (ids.empty() && g.playing != kNone && g_lib.Contains(g.playing)) ids.push_back(g.playing);
    if (ids.empty()) ShowMessage(L"Select an album, then drop its cover image on cordinal.");
    else SetCoverFromFile(ids, file);
    return true;
}

bool CoverPanelDrop(const char* data, bool file) {
    if (!pk.open || rv.open || !data || !*data) return false;
    std::string text = data;
    if (file && !text.starts_with("http")) TakeFile(compat::Widen(data));
    else TakeText(std::move(text), "Dropped");
    return true;
}

void FetchMissingCovers() {
    if (bt.running) {
        ShowMessage(L"Cordinal is already looking for covers.");
        return;
    }
    if (g_havePending) {
        g_havePending = false;
        OpenReview(std::move(g_pending));
        g_pending = {};
        return;
    }
    if (!EnsureCoverSources()) return;
    auto albums = coverfetch::LibraryAlbums();
    if (albums.empty()) {
        ShowMessage(L"The library has no albums with an album name to look for.");
        return;
    }
    if (bt.worker.joinable()) bt.worker.join();
    bt.cancel = std::make_shared<online::Cancel>();
    bt.running = true;
    const int thumb = std::min((int)S(48), 512);
    ShowMessage(L"Looking for covers of albums that have none...");
    bt.worker = std::thread([albums = std::move(albums), cancel = bt.cancel, thumb] {
        uint64_t last = 0;
        auto* done = new BatchDone;
        coverfetch::RunBatch(albums, *cancel, DecodeFetchedCover, thumb, [&](size_t i, size_t n) {
            uint64_t now = SDL_GetTicks();
            if (now - last < 500) return;
            last = now;
            Post(kMsgProgress, new Progress{i, n});
        }, done->result);
        Post(kMsgBatch, done);
    });
}

void CancelCoverFetch() {
    if (!bt.running || !bt.cancel) return;
    bt.cancel->Stop();
    ShowMessage(L"Stopping the cover search...");
}

bool CoverFetchBusy() { return bt.running; }

void OnCoverFetchMessage(WPARAM wp, LPARAM lp) {
    if (g_stopped) {
        DeletePayload(wp, lp);
        return;
    }
    switch (wp) {
        case kMsgFound: {
            std::unique_ptr<Found> found(reinterpret_cast<Found*>(lp));
            if (pk.open && pk.generation.Accepts(found->generation)) AddTile(std::move(found->cover));
            break;
        }
        case kMsgSearched: {
            std::unique_ptr<Searched> done(reinterpret_cast<Searched*>(lp));
            if (!pk.open || !pk.generation.Accepts(done->generation)) break;
            if (pk.worker.joinable()) pk.worker.join();
            pk.cancel.reset();
            pk.searching = false;
            pk.status = pk.tiles.empty() ? "No covers found. Check the artist and album, then search again."
                                         : "Found " + Utf8(Plural(pk.tiles.size(), L"cover", L"covers")) +
                                               ". Double-click one to use it.";
            g.redraw = true;
            break;
        }
        case kMsgProgress: {
            std::unique_ptr<Progress> progress(reinterpret_cast<Progress*>(lp));
            if (bt.running && !(bt.cancel && bt.cancel->Stopped()))
                ShowMessage(L"Looking for covers: album " + std::to_wstring(progress->done + 1) + L" of " +
                            std::to_wstring(progress->total) + L"...");
            break;
        }
        case kMsgOwn: {
            std::unique_ptr<Own> own(reinterpret_cast<Own*>(lp));
            if (!pk.open || !pk.ownGeneration.Accepts(own->generation)) break;
            if (pk.ownWorker.joinable()) pk.ownWorker.join();
            pk.ownCancel.reset();
            if (own->error.empty()) AddOwn(std::move(own->cover));
            else pk.status = Utf8(own->error);
            g.redraw = true;
            break;
        }
        case kMsgBatch: OnBatchDone(std::unique_ptr<BatchDone>(reinterpret_cast<BatchDone*>(lp))); break;
        case kMsgSaved: OnSaved(std::unique_ptr<Saved>(reinterpret_cast<Saved*>(lp))); break;
        default: DeletePayload(wp, lp); break;
    }
}

void StopCoverFetch() {
    g_stopped = true;
    if (pk.open || rv.open) ClosePanel();
    StopSearch();
    StopOwn();
    if (bt.cancel) bt.cancel->Stop();
    if (bt.worker.joinable()) bt.worker.join();
    if (g_writer.joinable()) g_writer.join();
    bt.running = false;
    g_writing = false;
    g_pending = {};
    g_havePending = false;
    online::Stop();
}

void DrawCoverPanel(const Palette& p, float, float) {
    if (rv.open) DrawReview(p);
    else if (pk.open) DrawPicker(p);
}

void CoverPanelMouseDown(float x, float y, int button, int clicks) {
    if (button != SDL_BUTTON_LEFT) return;
    if (rv.open) {
        if (rv.save.In(x, y) && CheckedCount()) SaveReview();
        else if (rv.cancel.In(x, y)) CoverPanelDone();
        else if (ClickSaveChoice(rv.saves, x, y)) {
        } else if (rv.list.In(x, y)) {
            float at = y - rv.list.y + rv.scroll;
            for (int line = 0; line < (int)rv.lines.size(); ++line)
                if (at >= LineTop(line) && at < LineTop(line) + LineH(line)) {
                    if (rv.lines[(size_t)line] >= 0) {
                        rv.focus = line;
                        ToggleFocus();
                    }
                    break;
                }
        }
        g.redraw = true;
        return;
    }
    if (!pk.open) return;
    if (pk.artist.rect.In(x, y)) pk.focus = 0;
    else if (pk.album.rect.In(x, y)) pk.focus = 1;
    else if (pk.search.In(x, y)) SearchClicked();
    else if (pk.use.In(x, y)) { UsePick(); return; }
    else if (pk.cancelButton.In(x, y)) { CoverPanelDone(); return; }
    else if (pk.file.In(x, y)) ChooseImageFile();
    else if (pk.paste.In(x, y)) PasteImage();
    else if (pk.google.In(x, y)) OpenGoogleImages();
    else if (ClickSaveChoice(pk.saves, x, y)) {
    } else if (pk.grid.In(x, y)) {
        pk.focus = 2;
        for (int i = 0; i < (int)pk.tiles.size(); ++i)
            if (pk.tiles[(size_t)i].rect.In(x, y)) {
                pk.selected = i;
                if (clicks >= 2) { UsePick(); return; }
                break;
            }
    }
    g.redraw = true;
}

void CoverPanelKey(const SDL_KeyboardEvent& e) {
    const bool enter = e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER;
    if (rv.open) {
        if (e.key == SDLK_ESCAPE) CoverPanelDone();
        else if (e.key == SDLK_UP) MoveFocus(-1);
        else if (e.key == SDLK_DOWN) MoveFocus(1);
        else if (e.key == SDLK_SPACE) ToggleFocus();
        else if (enter && CheckedCount()) SaveReview();
        return;
    }
    if (!pk.open) return;
    if (e.key == SDLK_ESCAPE) { CoverPanelDone(); return; }
    if (e.key == SDLK_V && (e.mod & SDL_KMOD_CTRL)) {
        // In a field it pastes text, as fields do; anywhere else it pastes an image.
        if (pk.focus < 2) {
            if (char* clip = SDL_GetClipboardText()) {
                CoverPanelText(clip);
                SDL_free(clip);
            }
        } else {
            PasteImage();
        }
    } else if (e.key == SDLK_TAB) {
        pk.focus = (pk.focus + ((e.mod & SDL_KMOD_SHIFT) ? 2 : 1)) % 3;
    } else if (pk.focus < 2) {
        if (enter) SearchClicked();
        else EditField(pk.focus == 0 ? pk.artist : pk.album, e);
    } else if (enter) {
        UsePick();
        return;
    } else if (!pk.tiles.empty()) {
        int cols = Columns(), n = (int)pk.tiles.size(), step = 0;
        if (e.key == SDLK_LEFT) step = -1;
        else if (e.key == SDLK_RIGHT) step = 1;
        else if (e.key == SDLK_UP) step = -cols;
        else if (e.key == SDLK_DOWN) step = cols;
        else if (e.key == SDLK_HOME) step = -n;
        else if (e.key == SDLK_END) step = n;
        if (step) {
            pk.selected = pk.selected < 0 ? 0 : std::clamp(pk.selected + step, 0, n - 1);
            RevealSelected();
        }
    }
    g.redraw = true;
}

void CoverPanelText(const char* text) {
    if (!pk.open || rv.open || pk.focus > 1 || !text) return;
    Field& f = pk.focus == 0 ? pk.artist : pk.album;
    std::wstring wide = FromUtf8(text);
    f.text.insert(std::min(f.caret, f.text.size()), wide);
    f.caret += wide.size();
    g.redraw = true;
}

void CoverPanelWheel(float x, float y, float dy) {
    if (rv.open && rv.list.In(x, y)) rv.scroll -= dy * S(62);
    else if (pk.open && pk.grid.In(x, y)) {
        pk.scroll -= dy * S(kCellH) / 2;
        ClampGridScroll();
    }
    g.redraw = true;
}

bool CoverPanelWantsText(float x, float y) {
    return pk.open && !rv.open && (pk.artist.rect.In(x, y) || pk.album.rect.In(x, y));
}

void CoverPanelClose() {
    StopSearch();
    StopOwn();
    pk.open = false;
    pk.tiles.clear();
    pk.pick = nullptr;
    pk.selected = -1;
    rv = ReviewState{};
}

void CoverPanelRenderer(bool restore) {
    if (restore) return;  // textures are made again as the panel draws
    for (Tile& t : pk.tiles)
        if (t.texture) t.texture->Release();
    for (ReviewRow& r : rv.rows)
        if (r.texture) r.texture->Release();
}

}  // namespace lx
