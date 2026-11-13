// The tag editor on Linux (Edit > Edit tags), as the Windows build's app_tageditor.cpp: every writable field
// of one or many tracks. Fields that differ start empty with a "(multiple values)" hint, and only the fields
// changed are written (tagwrite.cpp: a worker, an undo journal, the playing file once playback moves on).
// Names are completed from the library, and empty titles, artists and numbers can come from file names.
#include "panels_internal.h"

#include "cover.h"
#include "tag_suggest.h"
#include "tags.h"
#include "tagwriter.h"
#include "titleformat.h"

#include <optional>

namespace lx::panel {
namespace {

enum Field { Title, Artist, AlbumArtist, Album, Date, Genre, TrackNum, TrackTotal, Disc, DiscTotal, Composer, Grouping,
             Label, Bpm, Comment, Lyrics, FieldCount };
const char* const kLabels[] = {"Title", "Artist", "Album artist", "Album", "Year / date", "Genre", "Track", "of", "Disc", "of",
                               "Composer", "Grouping", "Label", "BPM", "Comment", "Lyrics"};
bool Numeric(int f) { return f == TrackNum || f == TrackTotal || f == Disc || f == DiscTotal || f == Bpm; }
bool Multiline(int f) { return f == Comment || f == Lyrics; }

struct TagPanel {
    std::vector<TrackId> ids;
    TextField fields[FieldCount];
    int focus = Title;
    int compilation = 0, initialCompilation = 0;  // 0 off, 1 on, 2 some of the tracks
    std::optional<std::string> cover;             // set when replaced (the image) or removed ("")
    CoverPreview preview;
    std::string info, warning;
    TagSuggestionSet suggestions;  // values already in the library, built when the editor opens
    std::vector<std::wstring> matches;
    int suggestField = -1, suggestSel = -1;
    Rect suggestRect;
    // What each track's file name says (by ids index); "Fill empty title, artist and track number from
    // the file name" writes it on Save for the fields a track leaves empty and nobody typed in.
    std::vector<FileNameGuess> guesses;
    bool guessable[FieldCount] = {}, fillNames = false, anyGuess = false;
    Rect panel, compilationRect, fillNamesRect, art, choose, remove, online, save, cancel;
} g_tags;

// The guess for one track's field, or "" when its tag already has a value or the name says nothing.
std::wstring NameGuessFor(size_t index, int f) {
    const Track& t = g_lib[g_tags.ids[index]];
    const FileNameGuess& guess = g_tags.guesses[index];
    switch (f) {
        case Title: return t.title ? std::wstring() : guess.title;
        case Artist: return t.artist ? std::wstring() : guess.artist;
        case TrackNum: return t.trackNo || !guess.track ? std::wstring() : std::to_wstring(guess.track);
        default: return {};
    }
}

// The gray hint an empty field shows: what Save fills in from the file name, as on Windows.
std::string FieldHint(int f) {
    const TextField& t = g_tags.fields[f];
    if (g_tags.fillNames && (f == Title || f == Artist || f == TrackNum) && g_tags.guessable[f]) {
        if (g_tags.ids.size() == 1) return Utf8(NameGuessFor(0, f));
        if (f == TrackNum) return "(names)";
        return t.mixed ? "(multiple values; empty ones from the file names)" : "(from each file name)";
    }
    return !t.enabled ? "(select one track to edit lyrics)" : !t.mixed ? "" : Numeric(f) ? "(mixed)" : "(multiple values)";
}

std::wstring TrackValue(TrackId id, int f) {
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

std::optional<TagSuggestField> SuggestFieldFor(int field) {
    switch (field) {
        case Artist: return TagSuggestField::Artist;
        case AlbumArtist: return TagSuggestField::AlbumArtist;
        case Album: return TagSuggestField::Album;
        case Genre: return TagSuggestField::Genre;
        case Composer: return TagSuggestField::Composer;
        default: return std::nullopt;
    }
}

void HideSuggestions();

void Focus(int f, bool selectAll) {
    if (f < 0 || f >= FieldCount || !g_tags.fields[f].enabled) return;
    g_tags.focus = f;
    TextField& t = g_tags.fields[f];
    t.caret = t.text.size();
    t.all = selectAll && !t.text.empty();
    HideSuggestions();
    SDL_StartTextInput(g.window);
}

void HideSuggestions() {
    g_tags.matches.clear();
    g_tags.suggestField = -1;
    g_tags.suggestSel = -1;
    g_tags.suggestRect = {};
}

void RefreshSuggestions() {
    HideSuggestions();
    const auto field = SuggestFieldFor(g_tags.focus);
    if (!field) return;
    std::vector<std::wstring> matches =
        MatchTagSuggestions(g_tags.suggestions, *field, g_tags.fields[g_tags.focus].text);
    if (matches.empty()) return;
    g_tags.matches = std::move(matches);
    g_tags.suggestField = g_tags.focus;
    g_tags.suggestSel = 0;
    g.redraw = true;
}

void AcceptSuggestion() {
    if (g_tags.suggestField < 0 || g_tags.suggestSel < 0 ||
        g_tags.suggestSel >= (int)g_tags.matches.size()) {
        HideSuggestions();
        return;
    }
    TextField& t = g_tags.fields[g_tags.suggestField];
    t.text = g_tags.matches[g_tags.suggestSel];
    t.caret = t.text.size();
    t.all = false;
    t.changed = true;
    HideSuggestions();
    g.redraw = true;
}

// Builds the edit from the fields actually changed; untouched fields stay unset so the writers
// leave them alone. An empty edit just closes the editor.
void SaveTags() {
    auto text = [](int f) -> std::optional<std::string> {
        const TextField& t = g_tags.fields[f];
        if (!t.changed) return std::nullopt;
        size_t b = t.text.find_first_not_of(L" \t"), e = t.text.find_last_not_of(L" \t\n");
        return ToUtf8(b == std::wstring::npos ? std::wstring() : t.text.substr(b, e - b + 1));
    };
    auto number = [](int f) -> std::optional<uint32_t> {
        const TextField& t = g_tags.fields[f];
        if (!t.changed) return std::nullopt;
        return (uint32_t)std::max(0l, wcstol(t.text.c_str(), nullptr, 10));
    };
    TagEdit edit;
    edit.title = text(Title);
    edit.artist = text(Artist);
    edit.albumArtist = text(AlbumArtist);
    edit.album = text(Album);
    edit.date = text(Date);
    edit.genre = text(Genre);
    edit.composer = text(Composer);
    edit.grouping = text(Grouping);
    edit.label = text(Label);
    edit.comment = text(Comment);
    edit.lyrics = text(Lyrics);
    edit.track = number(TrackNum);
    edit.trackTotal = number(TrackTotal);
    edit.disc = number(Disc);
    edit.discTotal = number(DiscTotal);
    edit.bpm = number(Bpm);
    if (g_tags.compilation != g_tags.initialCompilation && g_tags.compilation != 2) edit.compilation = g_tags.compilation == 1;
    edit.cover = g_tags.cover;
    // Each track takes its own name's guesses for the fields it leaves empty and nobody typed in; all
    // of it is still one write and one undo.
    std::vector<std::pair<TrackId, TagEdit>> items;
    const bool names = g_tags.anyGuess && g_tags.fillNames;
    for (size_t i = 0; i < g_tags.ids.size(); ++i) {
        TagEdit item = edit;
        if (names && i < g_tags.guesses.size()) {
            if (!g_tags.fields[Title].changed) if (auto v = NameGuessFor(i, Title); !v.empty()) item.title = ToUtf8(v);
            if (!g_tags.fields[Artist].changed) if (auto v = NameGuessFor(i, Artist); !v.empty()) item.artist = ToUtf8(v);
            if (!g_tags.fields[TrackNum].changed)
                if (auto v = NameGuessFor(i, TrackNum); !v.empty()) item.track = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        }
        if (!item.Empty()) items.emplace_back(g_tags.ids[i], std::move(item));
    }
    ClosePanel();
    if (!items.empty()) SubmitTagWrites(items, true);
}

void ChooseCover(const std::wstring& path) {
    std::string bytes = ReadCoverFile(path);
    if (bytes.empty()) return;
    g_tags.preview.Set(bytes);
    g_tags.cover = std::move(bytes);
    g.redraw = true;
}

// The text box: the field's surface, its text scrolled to keep the caret in view, and the caret.
void DrawField(int f, const Palette& p) {
    TextField& t = g_tags.fields[f];
    const Rect& r = t.rect;
    bool focused = g_tags.focus == f;
    Fill(r, t.enabled ? p.surface2 : p.surface);
    Frame(r, focused ? p.accent : p.border);
    if (focused) Fill({r.x + S(1), r.B() - S(1), r.w - S(2), S(1)}, p.accent);
    float pad = S(5), lineH = LineHeight(Font::Normal);
    SDL_Rect clip{(int)(r.x + S(2)), (int)(r.y + S(1)), (int)(r.w - S(4)), (int)(r.h - S(2))};
    SDL_SetRenderClipRect(g.renderer, &clip);
    // A file-name hint stays visible in the focused field too, so the guess is seen before typing.
    if (std::string cue = t.text.empty() ? FieldHint(f) : std::string();
        !cue.empty() && (!focused || (g_tags.fillNames && g_tags.guessable[f])))
        DrawText(Font::Normal, cue, r.x + pad, Multiline(f) ? r.y + S(4) : TextY(r), p.faint);
    if (Multiline(f)) {
        // Lines from the text's line breaks; the caret's line stays visible.
        size_t lineStart = 0, caretLine = 0, line = 0;
        float caretX = 0;
        for (size_t i = 0; i <= t.text.size(); ++i)
            if (i == t.text.size() || t.text[i] == L'\n') {
                if (t.caret >= lineStart && t.caret <= i) {
                    caretLine = line;
                    caretX = TextWidth(Font::Normal, Utf8(t.text.substr(lineStart, t.caret - lineStart)));
                }
                lineStart = i + 1;
                ++line;
            }
        float caretY = caretLine * lineH;
        if (caretY - t.scrollY > r.h - S(8) - lineH) t.scrollY = caretY - (r.h - S(8) - lineH);
        if (caretY < t.scrollY) t.scrollY = caretY;
        lineStart = 0;
        line = 0;
        for (size_t i = 0; i <= t.text.size(); ++i)
            if (i == t.text.size() || t.text[i] == L'\n') {
                float y = r.y + S(4) + line * lineH - t.scrollY;
                if (y > r.y - lineH && y < r.B())
                    DrawText(Font::Normal, Utf8(t.text.substr(lineStart, i - lineStart)), r.x + pad, y, t.all && focused ? p.selectText : p.text);
                lineStart = i + 1;
                ++line;
            }
        if (focused && t.all) Fill({r.x + S(2), r.y + S(2), r.w - S(4), r.h - S(4)}, p.select, 90);
        if (focused && SDL_GetTicks() % 1060 < 530)
            Fill({std::round(r.x + pad + caretX), r.y + S(4) + caretY - t.scrollY, S(1), lineH}, p.text);
    } else {
        std::string all = Utf8(t.text);
        float caretX = TextWidth(Font::Normal, Utf8(t.text.substr(0, t.caret))), room = r.w - 2 * pad;
        if (caretX - t.scrollX > room) t.scrollX = caretX - room;
        if (caretX < t.scrollX) t.scrollX = caretX;
        float tx = r.x + pad - t.scrollX, ty = TextY(r);
        if (focused && t.all) Fill({tx, r.y + S(3), TextWidth(Font::Normal, all), r.h - S(6)}, p.select);
        DrawText(Font::Normal, all, tx, ty, focused && t.all ? p.selectText : p.text);
        if (focused && SDL_GetTicks() % 1060 < 530) Fill({std::round(tx + caretX), ty, S(1), lineH}, p.text);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void DrawTags(const Palette& p) {
    std::string title = g_tags.ids.size() == 1 ? "Edit tags" : "Edit tags (" + std::to_string(g_tags.ids.size()) + " tracks)";
    Rect panel = g_tags.panel = DrawFrame(p, S(760), S(640), title.c_str());
    float pad = S(16), labelW = S(92), rowH = S(26), gap = S(6), artSize = S(160);
    float x0 = panel.x + pad, right = panel.R() - pad - artSize - S(18), y = panel.y + S(48);
    auto row = [&](int f, float x, float w, float lw = -1) {
        float labelWidth = lw < 0 ? labelW : lw;
        DrawText(Font::Normal, kLabels[f], x, y + (rowH - LineHeight(Font::Normal)) / 2, p.sub, labelWidth - S(8), 2);
        g_tags.fields[f].rect = {x + labelWidth, y, w - labelWidth, rowH};
        DrawField(f, p);
    };
    for (int f : {Title, Artist, AlbumArtist, Album}) {
        row(f, x0, right - x0);
        y += rowH + gap;
    }
    float half = std::round((right - x0) / 2);
    row(Date, x0, half - S(8));
    row(Genre, x0 + half, half, S(56));
    y += rowH + gap;
    float q = std::round((half - S(8) - labelW) / 2);
    row(TrackNum, x0, labelW + q);
    row(TrackTotal, x0 + labelW + q, q + S(10), S(26));
    row(Disc, x0 + half, S(56) + q);
    row(DiscTotal, x0 + half + S(56) + q, half - S(56) - q, S(26));
    y += rowH + gap;
    for (int f : {Composer, Grouping}) {
        row(f, x0, right - x0);
        y += rowH + gap;
    }
    row(Label, x0, half - S(8));
    row(Bpm, x0 + half, half, S(56));
    y += rowH + gap;
    g_tags.compilationRect = {x0 + labelW, y, S(320), S(24)};
    DrawCheck(g_tags.compilationRect, "Part of a compilation (various artists)", g_tags.compilation, p);
    y += S(30);
    g_tags.fillNamesRect = {};
    if (g_tags.anyGuess) {
        g_tags.fillNamesRect = {x0 + labelW, y, S(460), S(24)};
        DrawCheck(g_tags.fillNamesRect, "Fill empty title, artist and track number from the file name",
                  g_tags.fillNames ? 1 : 0, p);
        y += S(30);
    }
    float full = panel.R() - pad - x0;
    DrawText(Font::Normal, kLabels[Comment], x0, y + S(4), p.sub, labelW - S(8), 2);
    g_tags.fields[Comment].rect = {x0 + labelW, y, full - labelW, S(54)};
    DrawField(Comment, p);
    y += S(54) + gap;
    float bottom = panel.B() - pad - S(30) - S(28);
    DrawText(Font::Normal, kLabels[Lyrics], x0, y + S(4), p.sub, labelW - S(8), 2);
    g_tags.fields[Lyrics].rect = {x0 + labelW, y, full - labelW, std::max(S(40), bottom - y)};
    DrawField(Lyrics, p);
    // The cover column, top right.
    float ax = panel.R() - pad - artSize, ay = panel.y + S(48);
    g_tags.art = {ax, ay, artSize, artSize};
    Fill(g_tags.art, p.surface2);
    Frame(g_tags.art, p.border);
    bool removed = g_tags.cover && g_tags.cover->empty();
    if (g_tags.preview.texture && !removed) {
        g_tags.preview.Draw(g_tags.art);
    } else {
        DrawText(Font::Normal, removed ? "(cover removed)" : "No cover", ax, TextY(g_tags.art), p.faint, artSize, 1);
    }
    g_tags.choose = {ax, ay + artSize + S(6), artSize, S(26)};
    g_tags.remove = {ax, ay + artSize + S(36), artSize, S(26)};
    DrawButton(g_tags.choose, "Choose cover...", p);
    DrawButton(g_tags.remove, "Remove cover", p, false, g_tags.cover ? !g_tags.cover->empty() : g_tags.preview.texture != nullptr);
    g_tags.online = {ax, ay + artSize + S(66), artSize, S(26)};
    DrawButton(g_tags.online, "Find cover online...", p);
    float by = panel.B() - pad - S(28);
    DrawText(Font::Small, g_tags.warning.empty() ? g_tags.info : g_tags.warning, x0, by - S(24),
             g_tags.warning.empty() ? p.sub : p.error, panel.R() - pad - x0);
    g_tags.save = {panel.R() - pad - S(208), by, S(100), S(28)};
    g_tags.cancel = {panel.R() - pad - S(100), by, S(100), S(28)};
    DrawButton(g_tags.save, "Save", p, true);
    DrawButton(g_tags.cancel, "Cancel", p);
    if (g_tags.suggestField == g_tags.focus && !g_tags.matches.empty()) {
        const Rect& field = g_tags.fields[g_tags.focus].rect;
        const float rowH = S(22), width = field.w;
        float height = g_tags.matches.size() * rowH + S(2);
        float x = field.x, y = field.B() + S(2);
        if (y + height > panel.B() - S(8) && field.y - height >= panel.y + S(40))
            y = field.y - height - S(2);
        height = std::min(height, std::max(rowH + S(2), panel.B() - S(8) - y));
        g_tags.suggestRect = {x, y, width, height};
        Fill(g_tags.suggestRect, p.surface2);
        Frame(g_tags.suggestRect, p.border);
        SDL_Rect clip{(int)(x + S(1)), (int)(y + S(1)), (int)(width - S(2)), (int)(height - S(2))};
        SDL_SetRenderClipRect(g.renderer, &clip);
        for (size_t i = 0; i < g_tags.matches.size(); ++i) {
            const float top = y + S(1) + i * rowH;
            if (top + rowH > y + height) break;
            const Rect row{x + S(1), top, width - S(2), rowH};
            const bool selected = (int)i == g_tags.suggestSel;
            if (selected) Fill(row, p.select);
            DrawText(Font::Normal, Utf8(g_tags.matches[i]), x + S(6), TextY(row),
                     selected ? p.selectText : p.text, width - S(12));
        }
        SDL_SetRenderClipRect(g.renderer, nullptr);
    } else {
        g_tags.suggestRect = {};
    }
}

void Edited(TextField& t) {
    t.changed = true;
    t.all = false;
    g.redraw = true;
}

void InsertText(const std::wstring& s) {
    TextField& t = g_tags.fields[g_tags.focus];
    if (!t.enabled) return;
    std::wstring in;
    for (wchar_t c : s) {
        if (Numeric(g_tags.focus) ? (c >= L'0' && c <= L'9') : (c >= 32 || (c == L'\n' && Multiline(g_tags.focus)))) in += c;
        else if (c == L'\t' && !Numeric(g_tags.focus)) in += L' ';
    }
    if (in.empty()) return;
    if (t.all) {
        t.text.clear();
        t.caret = 0;
    }
    t.text.insert(t.caret, in);
    t.caret += in.size();
    Edited(t);
    RefreshSuggestions();
}

// The caret position for a click at (x, y) inside the field.
size_t CaretAt(const TextField& t, int f, float x, float y) {
    float pad = S(5);
    size_t start = 0, end = t.text.size();
    if (Multiline(f)) {
        int want = (int)std::floor((y - t.rect.y - S(4) + t.scrollY) / LineHeight(Font::Normal));
        int line = 0;
        for (size_t i = 0; i <= t.text.size(); ++i)
            if (i == t.text.size() || t.text[i] == L'\n') {
                if (line == want || i == t.text.size()) {
                    end = i;
                    break;
                }
                start = i + 1;
                ++line;
            }
    }
    float offset = x - (t.rect.x + pad) + (Multiline(f) ? 0 : t.scrollX);
    float a = 0;  // the width before character i, measured once as the previous step's right edge
    for (size_t i = start; i < end; ++i) {
        float b = TextWidth(Font::Normal, Utf8(t.text.substr(start, i + 1 - start)));
        if (offset < (a + b) / 2) return i;
        a = b;
    }
    return end;
}

void TagsMouseDown(float x, float y) {
    if (g_tags.suggestField >= 0 && g_tags.suggestRect.In(x, y)) {
        const int row = (int)((y - g_tags.suggestRect.y - S(1)) / S(22));
        if (row >= 0 && row < (int)g_tags.matches.size()) {
            g_tags.suggestSel = row;
            AcceptSuggestion();
        } else {
            HideSuggestions();
        }
        return;
    }
    if (g_tags.suggestField >= 0) HideSuggestions();
    if (!g_tags.panel.In(x, y) || g_tags.cancel.In(x, y)) {
        ClosePanel();
        return;
    }
    if (g_tags.save.In(x, y)) {
        SaveTags();
        return;
    }
    if (g_tags.choose.In(x, y)) {
        static const SDL_DialogFileFilter filter{"Images", "jpg;jpeg;png"};
        ChooseFile(kFileCover, filter);
        return;
    }
    if (g_tags.remove.In(x, y) && (g_tags.cover ? !g_tags.cover->empty() : g_tags.preview.texture != nullptr)) {
        g_tags.cover = std::string();
        return;
    }
    if (g_tags.online.In(x, y)) {
        // The picker opens over the editor and hands its choice back, to be written with the rest on Save.
        std::vector<TrackId> ids = g_tags.ids;
        OpenCoverSearchForTags(ids, [ids](const std::string& bytes) {
            if (g_tags.ids != ids) return;
            g_tags.preview.Set(bytes);
            g_tags.cover = bytes;
        });
        return;
    }
    if (g_tags.compilationRect.In(x, y)) {
        g_tags.compilation = g_tags.compilation == 1 ? 0 : 1;
        return;
    }
    if (g_tags.fillNamesRect.In(x, y)) {
        g_tags.fillNames = !g_tags.fillNames;
        return;
    }
    for (int f = 0; f < FieldCount; ++f) {
        TextField& t = g_tags.fields[f];
        if (t.rect.In(x, y) && t.enabled) {
            Focus(f, false);
            t.caret = CaretAt(t, f, x, y);
            return;
        }
    }
}

// The previous or next line break around `pos` in a multi-line field, for Up and Down.
size_t LineStart(const std::wstring& s, size_t pos) {
    size_t b = pos ? s.rfind(L'\n', pos - 1) : std::wstring::npos;
    return b == std::wstring::npos ? 0 : b + 1;
}

void TagsKey(const SDL_KeyboardEvent& e) {
    bool ctrl = e.mod & SDL_KMOD_CTRL, shift = e.mod & SDL_KMOD_SHIFT;
    TextField& t = g_tags.fields[g_tags.focus];
    auto step = [&](int dir) {  // Tab order: the fields as laid out, skipping the disabled one
        int f = g_tags.focus;
        for (int i = 0; i < FieldCount; ++i) {
            f = (f + dir + FieldCount) % FieldCount;
            if (g_tags.fields[f].enabled) break;
        }
        Focus(f, true);
    };
    if (g_tags.suggestField == g_tags.focus && !g_tags.matches.empty()) {
        if (e.key == SDLK_UP || e.key == SDLK_DOWN) {
            const int count = (int)g_tags.matches.size();
            g_tags.suggestSel = std::clamp(g_tags.suggestSel + (e.key == SDLK_DOWN ? 1 : -1), 0, count - 1);
            g.redraw = true;
            return;
        }
        if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
            AcceptSuggestion();
            return;
        }
        if (e.key == SDLK_ESCAPE) {
            HideSuggestions();
            g.redraw = true;
            return;
        }
        if (e.key == SDLK_TAB) AcceptSuggestion();  // then step to the next field
    }
    switch (e.key) {
        case SDLK_ESCAPE: ClosePanel(); return;
        case SDLK_TAB: step(shift ? -1 : 1); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (ctrl || !Multiline(g_tags.focus)) SaveTags();
            else InsertText(L"\n");
            return;
        case SDLK_BACKSPACE:
        case SDLK_DELETE:
            if (t.all) {
                t.text.clear();
                t.caret = 0;
                Edited(t);
            } else if (e.key == SDLK_BACKSPACE && t.caret > 0) {
                size_t from = t.caret - 1;
                if (ctrl) {  // the word before the caret
                    while (from > 0 && t.text[from - 1] == L' ') --from;
                    while (from > 0 && t.text[from - 1] != L' ' && t.text[from - 1] != L'\n') --from;
                }
                t.text.erase(from, t.caret - from);
                t.caret = from;
                Edited(t);
            } else if (e.key == SDLK_DELETE && t.caret < t.text.size()) {
                t.text.erase(t.caret, 1);
                Edited(t);
            }
            RefreshSuggestions();
            break;
        case SDLK_LEFT:
            HideSuggestions();
            if (t.all) t.caret = 0;
            else if (t.caret > 0) --t.caret;
            t.all = false;
            break;
        case SDLK_RIGHT:
            HideSuggestions();
            if (t.all) t.caret = t.text.size();
            else if (t.caret < t.text.size()) ++t.caret;
            t.all = false;
            break;
        case SDLK_HOME:
            HideSuggestions();
            t.caret = Multiline(g_tags.focus) && !ctrl ? LineStart(t.text, t.caret) : 0;
            t.all = false;
            break;
        case SDLK_END: {
            HideSuggestions();
            size_t end = Multiline(g_tags.focus) && !ctrl ? t.text.find(L'\n', t.caret) : std::wstring::npos;
            t.caret = end == std::wstring::npos ? t.text.size() : end;
            t.all = false;
            break;
        }
        case SDLK_UP:
        case SDLK_DOWN:
            if (Multiline(g_tags.focus)) {
                size_t start = LineStart(t.text, t.caret), column = t.caret - start;
                if (e.key == SDLK_UP && start > 0) {
                    size_t prev = LineStart(t.text, start - 1);
                    t.caret = std::min(prev + column, start - 1);
                } else if (e.key == SDLK_DOWN) {
                    size_t next = t.text.find(L'\n', t.caret);
                    if (next != std::wstring::npos) {
                        size_t after = t.text.find(L'\n', next + 1);
                        t.caret = std::min(next + 1 + column, after == std::wstring::npos ? t.text.size() : after);
                    }
                }
                t.all = false;
            } else step(e.key == SDLK_UP ? -1 : 1);
            break;
        case SDLK_A:
            if (ctrl) t.all = !t.text.empty();
            break;
        case SDLK_C:
            if (ctrl) SDL_SetClipboardText(Utf8(t.text).c_str());
            break;
        case SDLK_X:
            if (ctrl && t.enabled) {
                SDL_SetClipboardText(Utf8(t.text).c_str());
                t.text.clear();
                t.caret = 0;
                Edited(t);
                RefreshSuggestions();
            }
            break;
        case SDLK_V:
            if (ctrl) {
                if (char* clip = SDL_GetClipboardText()) {
                    InsertText(compat::Widen(clip));
                    SDL_free(clip);
                }
            }
            break;
        case SDLK_S:
            if (ctrl) SaveTags();
            return;
        default: break;
    }
    g.redraw = true;
}

void OpenTags(const std::vector<TrackId>& ids) {
    g_tags.ids = ids;
    g_tags.suggestions = BuildTagSuggestions(g_lib);
    bool multi = ids.size() > 1;
    for (int f = 0; f < FieldCount; ++f) {
        TextField& t = g_tags.fields[f];
        t = TextField{};
        std::wstring first = f == Lyrics ? std::wstring() : TrackValue(ids[0], f);
        bool mixed = false;
        for (size_t i = 1; i < ids.size() && !mixed && f != Lyrics; ++i) mixed = TrackValue(ids[i], f) != first;
        if (f == Lyrics && !multi) {
            std::string lyrics;
            ReadLyrics(g_lib.Path(ids[0]), lyrics);
            first = compat::Widen(lyrics.c_str());
            std::erase(first, L'\r');
        }
        t.text = mixed ? std::wstring() : first;
        t.mixed = mixed;
        t.enabled = f != Lyrics || !multi;
    }
    int on = 0;
    for (TrackId id : ids) on += (g_lib[id].flags & Track::kCompilation) ? 1 : 0;
    g_tags.compilation = g_tags.initialCompilation = on == 0 ? 0 : on == (int)ids.size() ? 1 : 2;
    // Names are guessed only for whole files: a CUE track's name is its sheet's.
    g_tags.guesses.assign(ids.size(), {});
    for (size_t i = 0; i < ids.size(); ++i)
        if (!IsSubtrack(g_lib.Path(ids[i]))) g_tags.guesses[i] = GuessTagsFromFileName(StemOf(g_lib.Str(g_lib[ids[i]].file)));
    g_tags.anyGuess = false;
    for (int f : {Title, Artist, TrackNum}) {
        g_tags.guessable[f] = false;
        for (size_t i = 0; i < ids.size() && !g_tags.guessable[f]; ++i) g_tags.guessable[f] = !NameGuessFor(i, f).empty();
        g_tags.anyGuess |= g_tags.guessable[f];
    }
    g_tags.fillNames = g_tags.anyGuess;
    g_tags.cover.reset();
    g_tags.preview.Set(TrackCoverBytes(g_lib.Path(ids[0])));
    std::wstring info = FormatTitle(L"[%codec%][  \x2022  %bitrate% kbps][  \x2022  %samplerate% Hz][  \x2022  %length%]  \x2022  %path%",
                                    [id = ids[0]](const std::wstring& name, std::wstring& out) { return LookupField(id, name, out); });
    if (multi) info = std::to_wstring(ids.size()) + L" tracks  \x2022  first: " + info;
    g_tags.info = Utf8(info);
    int unsupported = 0, cueTracks = 0;
    for (TrackId id : ids) {
        std::wstring path = g_lib.Path(id);
        if (IsSubtrack(path)) ++cueTracks;
        else unsupported += !CanWriteTags(path);
    }
    g_tags.warning.clear();
    if (cueTracks) g_tags.warning = "Tags of CUE tracks come from the cue sheet; editing them isn't supported yet.";
    else if (unsupported)
        g_tags.warning = std::to_string(unsupported) + " of these files can't be written (only MP3, FLAC, Ogg, Opus, M4A and cord can).";
    g_kind = Kind::Tags;
    Focus(Title, false);
}

void TagsWheel(float x, float y, float dy) {
    for (int f : {Comment, Lyrics}) {
        TextField& t = g_tags.fields[f];
        if (t.rect.In(x, y)) t.scrollY = std::max(0.0f, t.scrollY - dy * LineHeight(Font::Normal) * 3);
    }
}

bool TagsWantsText(float x, float y) {
    for (const TextField& t : g_tags.fields)
        if (t.enabled && t.rect.In(x, y)) return true;
    return false;
}

}  // namespace

void TagsForget() {
    g_tags.preview.Release();
    g_tags.preview.bytes.clear();
    g_tags.ids.clear();
}

void TagsRenderer(bool restore) {
    if (!restore) g_tags.preview.Release();
    else if (!g_tags.preview.bytes.empty()) g_tags.preview.Set(g_tags.preview.bytes);
}

const Ops kTagsOps{
    .draw = DrawTags,
    .mouseDown = [](float x, float y, int) { TagsMouseDown(x, y); },
    .wheel = TagsWheel,
    .key = TagsKey,
    .text = [](const char* s) { InsertText(compat::Widen(s)); },
    .file = [](int purpose, const std::wstring& path) { if (purpose == kFileCover) ChooseCover(path); },
    .wantsText = TagsWantsText,
    .close = [] {
        TagsForget();
        HideSuggestions();
        g_tags.suggestions = TagSuggestionSet{};
    },
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OpenTagEditor(const std::vector<TrackId>& ids) {
    if (ids.empty()) {
        ShowMessage(L"Select the tracks whose tags you want to edit.");
        return;
    }
    OpenTags(ids);
    g.redraw = true;
}

// A one-track library of its own, so the check needs no profile. Typing into Artist through the
// panel's own key and text paths must open the completion list and Enter must take its first value.
int CheckTagSuggestions() {
    Library saved = std::move(g_lib);
    g_lib = Library();
    TrackId id = g_lib.Add(L"/checks/tag-suggestion.flac");
    TagData tags;
    tags.artist = "Test artist";
    tags.album = "Test album";
    tags.genre = "Rock";
    g_lib.ApplyTags(id, tags);
    g_lib[id].flags |= Track::kInLibrary;
    ClosePanel();  // whatever the preceding checks left open
    OpenTagEditor({id});
    SDL_KeyboardEvent key{};
    key.key = SDLK_TAB;
    TagsKey(key);  // Title -> Artist, selecting the field
    const bool focused = g_tags.focus == Artist;
    InsertText(L"Test a");
    const bool shown = g_tags.suggestField == Artist && g_tags.matches.size() == 1 &&
                       g_tags.matches[0] == L"Test artist";
    key.key = SDLK_RETURN;
    TagsKey(key);
    const bool accepted = g_tags.fields[Artist].text == L"Test artist" && g_tags.matches.empty() &&
                          g_kind == Kind::Tags;
    ClosePanel();
    g_lib = std::move(saved);
    const bool ok = focused && shown && accepted;
    printf("%s the tag editor completes artist names from the library and keeps the editor open\n",
           ok ? "ok  " : "FAIL");
    return !ok;
}

}  // namespace lx
