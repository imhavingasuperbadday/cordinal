// Games (View > Games and stats) on Linux: the Windows build's two games played with your own library
// (app_games.cpp), as one panel with a tab for each.
//
//   Name That Track   a clip from the start of a song: 1 s, then 2, 4, 7, 11 and 16 s after each
//                     skip or wrong guess. Songs you play a lot come up more often.
//   Which Came First  two albums; pick the older one. The years get closer as the streak grows.
//
// Clips play on the sound-effects stream (sfx::PlayClip), never through the player: nothing here
// changes play counts, skips, the queue or what plays next. The music is paused while a clip plays.
#include "draw.h"
#include "panels.h"
#include "cover.h"
#include "cue.h"
#include "decoder.h"
#include "libview.h"

#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace lx {
namespace {

constexpr double kStages[] = {1, 2, 4, 7, 11, 16};
constexpr int kStageCount = (int)std::size(kStages);

enum class Tab { NameThatTrack, WhichCameFirst };
enum : int { kSlotClip, kSlotAnswerArt, kSlotLeft, kSlotRight };
enum class Phase { Loading, Guessing, Done, Unavailable };

// Work done off the UI thread: a decoded clip, or a cover.
struct GameResult {
    uint32_t generation = 0;
    int slot = 0;
    std::vector<float> clip;  // interleaved stereo
    unsigned rate = 0;
    CoverImage art{};
};

// A cover on the GPU; a null texture is a cover still loading or missing.
struct Art {
    SDL_Texture* texture = nullptr;
    void Set(const CoverImage& img) {
        Clear();
        if (img.width <= 0) return;
        texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.width, img.height);
        if (!texture) return;
        SDL_UpdateTexture(texture, nullptr, img.rgba.data(), img.width * 4);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    void Clear() {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
    }
};

struct Album {
    StringPool::Handle album, artist;
    int year;
    TrackId track;  // one of its tracks, for the cover
};

enum : int { kBtnPlay = 1, kBtnSkip, kBtnGiveUp, kBtnNext, kBtnPlayIt, kBtnLeft, kBtnRight, kBtnClose };
struct Button {
    Rect r;
    int id = 0;
    std::string text;
    bool primary = false;
};

struct Games {
    Tab tab = Tab::NameThatTrack;
    Rect panel, tabRects[2], guessRect;
    std::vector<Button> buttons;
    float mx = -1, my = -1;
    uint32_t roundGen = 0, pairGen = 0;  // results of an earlier round or pair are dropped
    std::vector<std::thread> workers;

    // Name That Track
    Phase phase = Phase::Loading;
    TrackId answer = kNone;
    std::vector<float> clip;
    unsigned clipRate = 0;
    int stage = 0;                       // clip length index unlocked
    std::vector<std::wstring> attempts;  // per used stage: a wrong guess, or L"" for a skip
    bool solved = false;
    std::wstring guess;
    size_t caret = 0;
    std::vector<TrackId> suggestions;
    int suggestion = 0;                  // the highlighted one
    std::vector<Rect> suggestionRects;
    std::deque<TrackId> recent;          // not picked again for a while
    Art answerArt;
    std::wstring note;

    // Which Came First
    std::vector<Album> albums;
    uint32_t albumsRevision = 0;
    int left = -1, right = -1, picked = -1;  // picked: -1 = not yet, 0 left, 1 right
    Art covers[2];
    uint64_t advanceAt = 0;  // SDL ticks when the next pair comes on its own
} q;

std::vector<int> Distribution() {
    std::vector<int> d = g.settings.nttGuesses;
    d.resize(kStageCount + 1, 0);  // stages 1..6, then misses
    return d;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// "Song (2011 Remaster)" and "Song [Live]" match "Song": what's in brackets is edition detail.
std::wstring BareTitle(TrackId id) {
    std::wstring t = Lower(g_lib.Str(g_lib[id].title)), out;
    int depth = 0;
    for (wchar_t c : t) {
        if (c == L'(' || c == L'[') ++depth;
        else if ((c == L')' || c == L']') && depth > 0) --depth;
        else if (depth == 0) out += c;
    }
    size_t dash = out.find(L" - ");
    if (dash != std::wstring::npos && out.find(L"remaster", dash) != std::wstring::npos) out.resize(dash);
    while (!out.empty() && iswspace(out.back())) out.pop_back();
    while (!out.empty() && iswspace(out.front())) out.erase(out.begin());
    return out;
}

std::wstring ArtistOf(TrackId id) {
    const Track& t = g_lib[id];
    return g_lib.Str(t.artist ? t.artist : t.albumArtist);
}

bool SameSong(TrackId a, TrackId b) {
    return a == b || (BareTitle(a) == BareTitle(b) && Lower(ArtistOf(a)) == Lower(ArtistOf(b)));
}

std::wstring SongLabel(TrackId id) {
    std::wstring artist = ArtistOf(id);
    return g_lib.Str(g_lib[id].title) + (artist.empty() ? L"" : L" \x2014 " + artist);
}

// The four-digit year at the start of a date tag, or 0 if absent or implausible.
int YearOf(StringPool::Handle date) {
    std::string_view d = g_lib.View(date);
    if (d.size() < 4) return 0;
    int y = 0;
    for (int i = 0; i < 4; ++i) {
        if (d[i] < '0' || d[i] > '9') return 0;
        y = y * 10 + (d[i] - '0');
    }
    return y >= 1000 && y <= 2999 ? y : 0;
}

void Join() {
    for (auto& t : q.workers)
        if (t.joinable()) t.join();
    q.workers.clear();
}

// Workers are joined when the panel closes; a round starts at most two.
void StartWorker(std::function<void(GameResult&)> work, int slot) {
    uint32_t generation = slot == kSlotLeft || slot == kSlotRight ? q.pairGen : q.roundGen;
    q.workers.emplace_back([generation, slot, work = std::move(work)] {
        auto* r = new GameResult;
        r->generation = generation;
        r->slot = slot;
        work(*r);
        if (!PostMessageW(nullptr, WM_APP_GAME, 0, (LPARAM)r)) delete r;
    });
}

void LoadCover(TrackId id, int slot) {
    std::wstring path = g_lib.Path(id);
    StartWorker([path](GameResult& r) { DecodeCover(TrackCoverBytes(path), 512, r.art); }, slot);
}

// --- Name That Track ---------------------------------------------------------------------------------

void StopClip() { sfx::StopClip(); }

// Picks a song, favoring the ones played most (that's the point: your music), never one of the
// last fifty.
TrackId PickSong() {
    std::vector<TrackId> ids;
    std::vector<double> weights;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& t = g_lib[id];
        if (!t.InLibrary() || !t.Loaded() || t.Failed() || !t.title || t.durationMs < 30000) continue;
        if (std::find(q.recent.begin(), q.recent.end(), id) != q.recent.end()) continue;
        ids.push_back(id);
        weights.push_back(1.0 + std::min<uint32_t>(t.playCount, 40));
    }
    if (ids.empty()) return kNone;
    std::discrete_distribution<size_t> pick(weights.begin(), weights.end());
    TrackId id = ids[pick(g.rng)];
    q.recent.push_back(id);
    if (q.recent.size() > 50) q.recent.pop_front();
    return id;
}

void ClearGuess() {
    q.guess.clear();
    q.caret = 0;
    q.suggestions.clear();
}

// Starts a round: picks a song and decodes the clip and cover off the UI thread.
void NewRound() {
    StopClip();
    ++q.roundGen;
    q.attempts.clear();
    q.stage = 0;
    q.solved = false;
    q.clip.clear();
    q.answerArt.Clear();
    q.note.clear();
    ClearGuess();
    if (g.settings.exclusive) {
        q.phase = Phase::Unavailable;
        q.note = L"Clips play next to the music, which exclusive output doesn't allow. Switch to shared output "
                 L"(Playback \x25B8 Output \x25B8 Shared) to play.";
        return;
    }
    q.answer = PickSong();
    if (q.answer == kNone) {
        q.phase = Phase::Unavailable;
        q.note = L"Name That Track needs songs of at least 30 seconds with titles in your library.";
        return;
    }
    q.phase = Phase::Loading;
    std::wstring path = g_lib.Path(q.answer);
    StartWorker([path](GameResult& r) {
        std::wstring error;
        auto dec = OpenTrackDecoder(path, 48000, error);
        if (!dec) return;
        r.rate = dec->Rate();
        // Start where the music starts: skip leading silence (below about -50 dB), at most 20 s.
        std::vector<float> buf(4096 * 2);
        size_t skipped = 0, limit = (size_t)r.rate * 20;
        size_t want = (size_t)(r.rate * kStages[kStageCount - 1]);
        bool found = false;
        while (!found && skipped < limit) {
            size_t n = dec->Read(buf.data(), 4096);
            if (n == 0) break;
            for (size_t i = 0; i < n; ++i)
                if (std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1])) > 0.003f) {
                    // Keep 50 ms before the first sound so the attack isn't cut.
                    size_t keep = std::min<size_t>(i, r.rate / 20);
                    r.clip.assign(buf.begin() + (i - keep) * 2, buf.begin() + n * 2);
                    found = true;
                    break;
                }
            skipped += n;
        }
        if (!found) {  // silent all the way: just start at the beginning
            dec->Seek(0);
            r.clip.clear();
        }
        while (r.clip.size() / 2 < want) {
            size_t n = dec->Read(buf.data(), 4096);
            if (n == 0) break;
            r.clip.insert(r.clip.end(), buf.begin(), buf.begin() + n * 2);
        }
        r.clip.resize(std::min(r.clip.size(), want * 2));
    }, kSlotClip);
    LoadCover(q.answer, kSlotAnswerArt);
}

// Plays the unlocked clip from the start, pausing the music while it plays.
void PlayStage() {
    if (q.phase != Phase::Guessing && q.phase != Phase::Done) return;
    if (q.clip.empty()) return;
    if (g.playing != kNone && !IsPaused()) TogglePause();
    double seconds = q.phase == Phase::Done ? kStages[kStageCount - 1] : kStages[q.stage];
    size_t frames = std::min(q.clip.size() / 2, (size_t)(seconds * q.clipRate));
    sfx::PlayClip(std::vector<float>(q.clip.begin(), q.clip.begin() + frames * 2), q.clipRate, g.settings.volume * g.settings.volume);
    g.redraw = true;
}

// Ends the round, updates the stage distribution and the streak, and plays the verdict sound.
void Finish(bool solved) {
    q.solved = solved;
    q.phase = Phase::Done;
    q.suggestions.clear();
    auto d = Distribution();
    ++d[solved ? q.stage : kStageCount];
    Settings& s = g.settings;
    s.nttGuesses = d;
    s.nttStreak = solved ? s.nttStreak + 1 : 0;
    s.nttBest = std::max(s.nttBest, s.nttStreak);
    SaveSettings();
    PlaySfx(solved ? Sfx::Sparkle : Sfx::Oops);
}

// A wrong guess or a skip: unlock the next (longer) clip and play it.
void Advance(const std::wstring& attempt) {
    q.attempts.push_back(attempt);
    if (q.stage + 1 >= kStageCount) {
        Finish(false);
        return;
    }
    ++q.stage;
    ClearGuess();
    PlayStage();
}

void SubmitGuess(TrackId id) {
    if (q.phase != Phase::Guessing || id == kNone) return;
    StopClip();
    if (SameSong(id, q.answer)) Finish(true);
    else Advance(SongLabel(id));
}

// Up to eight distinct songs matching every typed word in title or artist; titles that start with
// what was typed come first.
void UpdateSuggestions() {
    std::wstring typed = Lower(q.guess);
    q.suggestions.clear();
    q.suggestion = 0;
    std::vector<std::wstring> words;
    for (size_t i = 0; i < typed.size();) {
        size_t j = typed.find(L' ', i);
        if (j == std::wstring::npos) j = typed.size();
        if (j > i) words.push_back(typed.substr(i, j - i));
        i = j + 1;
    }
    if (words.empty()) return;
    std::vector<std::pair<int, TrackId>> found;
    std::unordered_set<std::wstring> seen;
    for (TrackId id = 0; id < (TrackId)g_lib.Count() && found.size() < 400; ++id) {
        const Track& t = g_lib[id];
        if (!t.InLibrary() || !t.title) continue;
        std::wstring title = Lower(g_lib.Str(t.title)), hay = title + L" " + Lower(ArtistOf(id));
        bool all = std::all_of(words.begin(), words.end(), [&](const std::wstring& w) { return hay.find(w) != std::wstring::npos; });
        if (!all || !seen.insert(BareTitle(id) + L"\x1F" + Lower(ArtistOf(id))).second) continue;
        found.push_back({title.rfind(typed, 0) == 0 ? 0 : 1, id});
    }
    for (int rank = 0; rank < 2; ++rank)
        for (auto [priority, id] : found)
            if (priority == rank && q.suggestions.size() < 8) q.suggestions.push_back(id);
}

// --- Which Came First --------------------------------------------------------------------------------

// Builds the album list: one entry per album/artist that has a year.
void CollectAlbums() {
    if (q.albumsRevision == g_lib.Revision() && !q.albums.empty()) return;
    q.albumsRevision = g_lib.Revision();
    q.albums.clear();
    std::unordered_map<uint64_t, size_t> index;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& t = g_lib[id];
        int year = t.InLibrary() && t.album ? YearOf(t.date) : 0;
        if (!year) continue;
        StringPool::Handle artist = t.albumArtist ? t.albumArtist : t.artist;
        uint64_t key = (uint64_t)t.album << 32 | artist;
        auto [it, added] = index.try_emplace(key, q.albums.size());
        if (added) q.albums.push_back({t.album, artist, year, id});
        else q.albums[it->second].year = std::min(q.albums[it->second].year, year);  // reissues: the original year
    }
}

// Pairs two albums with different years and starts loading both covers.
void NewPair() {
    q.advanceAt = 0;
    ++q.pairGen;
    CollectAlbums();
    q.picked = -1;
    q.left = q.right = -1;
    q.covers[0].Clear();
    q.covers[1].Clear();
    // Easy at first (decades apart), then closer: at a streak of 10 the years are at most 2 apart.
    int maxGap = std::max(1, 30 - g.settings.wcfStreak * 3);
    std::uniform_int_distribution<size_t> any(0, q.albums.empty() ? 0 : q.albums.size() - 1);
    for (int tries = 0; tries < 2000 && q.albums.size() >= 2; ++tries) {
        size_t a = any(g.rng), b = any(g.rng);
        int gap = std::abs(q.albums[a].year - q.albums[b].year);
        if (a == b || gap == 0 || (gap > maxGap && tries < 1500)) continue;
        q.left = (int)a;
        q.right = (int)b;
        break;
    }
    if (q.left >= 0) {
        LoadCover(q.albums[q.left].track, kSlotLeft);
        LoadCover(q.albums[q.right].track, kSlotRight);
    }
}

// Records the pick, updates the streak and schedules the next pair.
void Pick(int side) {
    if (q.left < 0 || q.picked >= 0) return;
    q.picked = side;
    const Album& chosen = q.albums[side == 0 ? q.left : q.right];
    const Album& other = q.albums[side == 0 ? q.right : q.left];
    bool right = chosen.year < other.year;
    Settings& s = g.settings;
    s.wcfStreak = right ? s.wcfStreak + 1 : 0;
    s.wcfBest = std::max(s.wcfBest, s.wcfStreak);
    SaveSettings();
    PlaySfx(right ? Sfx::Sparkle : Sfx::Oops);
    q.advanceAt = SDL_GetTicks() + (right ? 1300 : 2600);  // a wrong answer gets time to sink in
}

// --- layout and drawing ------------------------------------------------------------------------------
// Positions are the Windows window's (560 x 600 at 96 DPI), measured from the panel's corner.

float X(float v) { return q.panel.x + S(v); }
float Y(float v) { return q.panel.y + S(v); }

void AddButton(int id, std::string text, Rect r, bool primary = false) { q.buttons.push_back({r, id, std::move(text), primary}); }

// Rebuilds the buttons for the current tab and phase.
void Relayout() {
    q.buttons.clear();
    const Rect& P = q.panel;
    const float pad = S(20), cx = P.x + P.w / 2, by = P.B() - pad - S(30);
    q.guessRect = {};
    AddButton(kBtnClose, "", {P.R() - S(40), P.y + S(10), S(30), S(30)});
    if (q.tab == Tab::NameThatTrack) {
        if (q.phase == Phase::Guessing || q.phase == Phase::Loading) {
            AddButton(kBtnPlay, "", {cx - S(30), Y(116), S(60), S(60)}, true);
            if (q.phase == Phase::Guessing) q.guessRect = {P.x + pad, Y(356), P.w - pad * 2, S(28)};
            std::string skip = q.stage + 1 < kStageCount
                                   ? "Skip (+" + std::to_string((int)(kStages[q.stage + 1] - kStages[q.stage])) + " s)"
                                   : "Skip";
            AddButton(kBtnSkip, skip, {P.x + pad, by, S(120), S(30)});
            AddButton(kBtnGiveUp, "Give up", {P.R() - pad - S(100), by, S(100), S(30)});
        } else if (q.phase == Phase::Done) {
            AddButton(kBtnPlayIt, "Play this song", {P.x + pad, by, S(140), S(30)});
            AddButton(kBtnNext, "Next song", {P.R() - pad - S(120), by, S(120), S(30)}, true);
            AddButton(kBtnPlay, "", {P.x + pad, Y(96), S(150), S(150)});  // the cover replays the clip
        } else {
            AddButton(kBtnNext, "Try again", {P.R() - pad - S(120), by, S(120), S(30)}, true);
        }
    } else if (q.left >= 0) {
        float w = std::floor((P.w - pad * 3) / 2), top = Y(96), size = std::min(w, P.B() - top - S(130));
        AddButton(kBtnLeft, "", {P.x + pad, top, w, size + S(70)});
        AddButton(kBtnRight, "", {P.x + pad * 2 + w, top, w, size + S(70)});
        if (q.picked >= 0) AddButton(kBtnNext, "Next pair", {P.R() - pad - S(120), by, S(120), S(30)}, true);
    }
}

void Circle(const Rect& r, uint32_t color) {
    std::vector<SDL_FPoint> pts;
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = std::min(r.w, r.h) / 2;
    for (int i = 0; i < 48; ++i) {
        float a = (float)(i * 2 * M_PI / 48);
        pts.push_back({cx + rad * std::cos(a), cy + rad * std::sin(a)});
    }
    Polygon(pts, color);
}

// A cover scaled into `r`, or a filled square with `placeholder` while there is none.
void DrawArt(const Art& art, const Rect& r, const std::wstring& placeholder, const Palette& p) {
    if (!art.texture) {
        Fill(r, p.surface2);
        DrawText(Font::Title, Utf8(placeholder), r.x, TextY(r, Font::Title), p.sub, r.w, 1);
        return;
    }
    SDL_FRect dst{r.x, r.y, r.w, r.h};
    SDL_RenderTexture(g.renderer, art.texture, nullptr, &dst);
}

void DrawGameButton(const Button& b, const Palette& p) {
    bool hot = b.r.In(q.mx, q.my);
    if (b.id == kBtnClose) {
        if (hot) Fill(b.r, p.hover);
        float m = S(10);
        ThickLine(b.r.x + m, b.r.y + m, b.r.R() - m, b.r.B() - m, S(1.5f), p.sub);
        ThickLine(b.r.R() - m, b.r.y + m, b.r.x + m, b.r.B() - m, S(1.5f), p.sub);
        return;
    }
    if (b.id == kBtnPlay && q.tab == Tab::NameThatTrack && q.phase != Phase::Done) {
        // The round play button; a stop square while the clip plays.
        uint32_t fill = hot ? p.accent2 : p.accent, mark = TextOn(fill);
        Circle(b.r, fill);
        float cx = b.r.x + b.r.w / 2, cy = b.r.y + b.r.h / 2, k = S(10);
        if (sfx::ClipPosition() >= 0) Fill({cx - k * 0.8f, cy - k * 0.8f, k * 1.6f, k * 1.6f}, mark);
        else Polygon({{cx - k * 0.7f, cy - k}, {cx + k, cy}, {cx - k * 0.7f, cy + k}}, mark);
        return;
    }
    if (b.text.empty()) return;  // card-sized hit areas draw themselves
    DrawButton(b.r, b.text, p, b.primary);
}

void DrawStageBar(const Rect& bar, const Palette& p) {
    // The 16 s timeline: unlocked time light, the playing clip in the accent, ticks at each stage.
    double total = kStages[kStageCount - 1];
    auto x = [&](double s) { return bar.x + std::round((float)(bar.w * s / total)); };
    Fill(bar, p.surface2);
    double unlocked = q.phase == Phase::Done ? total : kStages[q.stage];
    Fill({bar.x, bar.y, x(unlocked) - bar.x, bar.h}, p.select);
    double pos = sfx::ClipPosition();
    if (pos >= 0) Fill({bar.x, bar.y, x(std::min(pos, unlocked)) - bar.x, bar.h}, p.accent);
    for (int i = 0; i + 1 < kStageCount; ++i) Fill({x(kStages[i]) - 1, bar.y, 2, bar.h}, p.surface);
    char label[32];
    snprintf(label, sizeof(label), "%g s", unlocked);
    DrawText(Font::Normal, label, bar.x, bar.B() + S(4), p.sub);
}

void DrawGuessField(const Palette& p) {
    const Rect& r = q.guessRect;
    Fill(r, p.surface2);
    Frame(r, p.accent);
    const float pad = S(6);
    SDL_Rect clip{(int)(r.x + S(2)), (int)(r.y + S(1)), (int)(r.w - S(4)), (int)(r.h - S(2))};
    SDL_SetRenderClipRect(g.renderer, &clip);
    if (q.guess.empty()) DrawText(Font::Normal, "Know it? Type the title or artist", r.x + pad, TextY(r), p.faint);
    const float caretX = TextWidth(Font::Normal, Utf8(q.guess.substr(0, q.caret))), room = r.w - 2 * pad;
    const float shift = std::max(0.0f, caretX - room), tx = r.x + pad - shift;
    DrawText(Font::Normal, Utf8(q.guess), tx, TextY(r), p.text);
    Fill({std::round(tx + caretX), TextY(r), S(1), LineHeight(Font::Normal)}, p.text);
    SDL_SetRenderClipRect(g.renderer, nullptr);
    // The suggestions drop down under the field, over the stage rows.
    q.suggestionRects.clear();
    float y = r.B() + S(1);
    const float rowH = S(22);
    if (!q.suggestions.empty()) {
        Rect list{r.x, y, r.w, rowH * q.suggestions.size() + S(4)};
        Fill(list, p.surface2);
        Frame(list, p.border);
        y += S(2);
    }
    for (size_t i = 0; i < q.suggestions.size(); ++i, y += rowH) {
        Rect row{r.x + 1, y, r.w - 2, rowH};
        q.suggestionRects.push_back(row);
        bool on = (int)i == q.suggestion;
        if (on) Fill(row, p.select);
        else if (row.In(q.mx, q.my)) Fill(row, p.hover);
        DrawText(Font::Normal, Utf8(SongLabel(q.suggestions[i])), row.x + pad, TextY(row), on ? p.selectText : p.text, row.w - 2 * pad);
    }
}

void DrawNameThatTrack(const Palette& p) {
    const Rect& P = q.panel;
    const float pad = S(20), x0 = P.x + pad, w = P.w - 2 * pad;
    std::string sub = "Streak " + std::to_string(g.settings.nttStreak) + "  \xE2\x80\xA2  best " + std::to_string(g.settings.nttBest);
    DrawText(Font::Normal, sub, x0, Y(64), p.sub, w);
    if (q.phase == Phase::Unavailable) {
        float y = Y(110);
        for (const std::string& line : Wrap(Font::Bold, Utf8(q.note), w)) {
            DrawText(Font::Bold, line, x0, y, p.text, w);
            y += LineHeight(Font::Bold);
        }
        return;
    }
    if (q.phase == Phase::Done) {
        Rect art{x0, Y(96), S(150), S(150)};
        DrawArt(q.answerArt, art, L"\x266A", p);
        const float tx = art.R() + S(16), tw = P.R() - pad - tx;
        const Track& t = g_lib[q.answer];
        std::string verdict = q.solved ? "Got it in " + std::to_string((int)kStages[q.stage]) + (q.stage ? " seconds!" : " second!")
                                       : "It was\xE2\x80\xA6";
        DrawText(Font::Bold, verdict, tx, Y(96), q.solved ? p.accent : p.sub, tw);
        DrawText(Font::Title, Utf8(g_lib.Str(t.title)), tx, Y(122), p.text, tw);
        DrawText(Font::Bold, Utf8(ArtistOf(q.answer)), tx, Y(154), p.text, tw);
        std::wstring album = g_lib.Str(t.album);
        if (YearOf(t.date)) album += (album.empty() ? L"" : L"  \x2022  ") + std::to_wstring(YearOf(t.date));
        DrawText(Font::Normal, Utf8(album), tx, Y(178), p.sub, tw);
        DrawText(Font::Normal, "Played " + std::to_string(t.playCount) + (t.playCount == 1 ? " time" : " times"), tx, Y(200), p.sub, tw);
        // Guess distribution, Wordle style: this round's bar in the accent.
        auto d = Distribution();
        int maxv = std::max(1, *std::max_element(d.begin(), d.end()));
        const float rowH = S(20), labelW = S(40), barMax = w - labelW - S(40);
        float y = Y(268);
        for (int i = 0; i <= kStageCount; ++i, y += rowH) {
            char label[16];
            if (i < kStageCount) snprintf(label, sizeof(label), "%g s", kStages[i]);
            else snprintf(label, sizeof(label), "\xE2\x9C\x95");
            Rect row{x0, y, labelW, rowH - S(3)};
            DrawText(Font::Normal, label, row.x, TextY(row), p.sub, labelW, 2);
            float bw = std::max(S(18), std::floor(barMax * d[i] / maxv));
            bool mine = q.solved ? i == q.stage : i == kStageCount;
            Rect bar{x0 + labelW + S(8), y + S(1), bw, rowH - S(4)};
            Fill(bar, mine ? p.accent : p.surface2);
            DrawText(Font::Normal, std::to_string(d[i]), bar.x, TextY(bar), mine ? TextOn(p.accent) : p.text, bar.w - S(5), 2);
        }
        return;
    }
    // Guessing: the timeline, then a row per stage with what happened there.
    DrawStageBar({x0, Y(96), w, S(8)}, p);
    const char* hint = q.phase == Phase::Loading ? "Finding a song\xE2\x80\xA6"
                                                 : "Space plays the clip. Type to guess, Enter picks the highlighted song.";
    DrawText(Font::Normal, hint, x0, Y(186), p.sub, w, 1);
    float y = Y(214);
    const float rowH = S(22);
    for (int i = 0; i < kStageCount; ++i, y += rowH) {
        Rect row{x0, y, w, rowH - S(4)};
        bool used = i < (int)q.attempts.size(), current = i == q.stage;
        Fill(row, current ? p.select : p.surface2);
        std::wstring text = used ? (q.attempts[i].empty() ? L"Skipped" : L"\x2715  " + q.attempts[i]) : L"";
        uint32_t color = used && !q.attempts[i].empty() ? p.error : current ? p.selectText : p.sub;
        DrawText(Font::Normal, Utf8(text), row.x + S(8), TextY(row), color, row.w - S(16));
    }
}

void DrawAlbumCard(const Button& b, int side, const Palette& p) {
    const Album& a = q.albums[side == 0 ? q.left : q.right];
    const Album& other = q.albums[side == 0 ? q.right : q.left];
    bool hot = b.r.In(q.mx, q.my) && q.picked < 0;
    const Rect& r = b.r;
    float size = std::min(r.w, r.h - S(70));
    Rect art{r.x + std::round((r.w - size) / 2), r.y, size, size};
    auto ring = [&](float by, uint32_t color) { Fill({art.x - by, art.y - by, art.w + 2 * by, art.h + 2 * by}, color); };
    if (q.picked >= 0) ring(S(4), a.year < other.year ? p.ok : side == q.picked ? p.error : p.border);
    else if (hot) ring(S(3), p.accent);
    DrawArt(q.covers[side], art, g_lib.Str(a.album).substr(0, 1), p);
    float y = art.B() + S(10);
    DrawText(Font::Bold, Utf8(g_lib.Str(a.album)), r.x, y, p.text, r.w, 1);
    DrawText(Font::Normal, Utf8(g_lib.Str(a.artist)), r.x, y + S(20), p.sub, r.w, 1);
    if (q.picked >= 0) DrawText(Font::Title, std::to_string(a.year), r.x, y + S(40), a.year < other.year ? p.ok : p.text, r.w, 1);
}

void DrawWhichCameFirst(const Palette& p) {
    const Rect& P = q.panel;
    const float pad = S(20), x0 = P.x + pad, w = P.w - 2 * pad;
    std::string sub = "Pick the older album (\xE2\x86\x90 / \xE2\x86\x92).  Streak " + std::to_string(g.settings.wcfStreak) +
                      "  \xE2\x80\xA2  best " + std::to_string(g.settings.wcfBest);
    DrawText(Font::Normal, sub, x0, Y(64), p.sub, w);
    if (q.left < 0) {
        float y = Y(110);
        for (const std::string& line :
             Wrap(Font::Bold, "Which Came First needs albums from at least two different years (from their date tags).", w)) {
            DrawText(Font::Bold, line, x0, y, p.text, w);
            y += LineHeight(Font::Bold);
        }
        return;
    }
    float cardsBottom = 0;
    for (const Button& b : q.buttons)
        if (b.id == kBtnLeft || b.id == kBtnRight) {
            DrawAlbumCard(b, b.id == kBtnLeft ? 0 : 1, p);
            cardsBottom = b.r.B();
        }
    if (q.picked >= 0) {
        int gap = std::abs(q.albums[q.left].year - q.albums[q.right].year);
        bool right = (q.picked == 0) == (q.albums[q.left].year < q.albums[q.right].year);
        std::string verdict = (right ? "Right: " : "Not quite: ") + std::to_string(gap) + (gap == 1 ? " year" : " years") + " apart.";
        DrawText(Font::Bold, verdict, x0, cardsBottom + S(14), right ? p.ok : p.error, w, 1);
    }
}

int HitButton(float x, float y) {
    for (const Button& b : q.buttons)
        if (b.r.In(x, y)) return b.id;
    return 0;
}

void SetTab(Tab tab) {
    StopClip();
    q.tab = tab;
    if (tab == Tab::WhichCameFirst && q.left < 0) NewPair();
    if (tab == Tab::NameThatTrack && q.answer == kNone) NewRound();
    g.redraw = true;
}

void OnButton(int id) {
    switch (id) {
        case kBtnClose: ClosePanel(); return;
        case kBtnPlay:
            if (sfx::ClipPosition() >= 0) StopClip();
            else PlayStage();
            break;
        case kBtnSkip:
            StopClip();
            Advance(L"");
            break;
        case kBtnGiveUp:
            StopClip();
            while ((int)q.attempts.size() < kStageCount) q.attempts.push_back(L"");
            q.stage = kStageCount - 1;
            Finish(false);
            break;
        case kBtnNext:
            if (q.tab == Tab::NameThatTrack) NewRound();
            else NewPair();
            break;
        case kBtnPlayIt:
            StopClip();
            PlayTracks({q.answer}, 0);
            break;
        case kBtnLeft: Pick(0); break;
        case kBtnRight: Pick(1); break;
    }
    g.redraw = true;
}

}  // namespace

// The panel opens on a game, and switching tabs keeps each game where it was.
void ShowGame(int which) { SetTab(which == 0 ? Tab::NameThatTrack : Tab::WhichCameFirst); }

void CloseGames() {
    sfx::StopClip();
    ++q.roundGen;  // results still on their way are dropped
    ++q.pairGen;
    Join();
    q.answerArt.Clear();
    q.covers[0].Clear();
    q.covers[1].Clear();
    std::deque<TrackId> recent = std::move(q.recent);
    q = Games{};
    q.recent = std::move(recent);
}

bool GamesAnimating() { return GamesOpen() && (sfx::ClipPosition() >= 0 || q.advanceAt); }

void DrawGames(const Palette& p, float mouseX, float mouseY) {
    q.mx = mouseX;
    q.my = mouseY;
    if (q.advanceAt && SDL_GetTicks() >= q.advanceAt && q.tab == Tab::WhichCameFirst) NewPair();
    q.panel = DrawFrame(p, S(560), S(600), "");
    Relayout();
    // Tabs: the two game names as headings; the active one in full color with an underline.
    const char* names[] = {"Name That Track", "Which Came First"};
    float x = X(20);
    for (int i = 0; i < 2; ++i) {
        float tw = TextWidth(Font::Title, names[i]);
        q.tabRects[i] = {x, Y(16), tw, LineHeight(Font::Title) + S(8)};
        bool active = (int)q.tab == i;
        DrawText(Font::Title, names[i], x, Y(16), active || q.tabRects[i].In(q.mx, q.my) ? p.text : p.sub);
        if (active) Fill({x, q.tabRects[i].B() - S(3), tw, S(2)}, p.accent);
        x += tw + S(28);
    }
    if (q.tab == Tab::NameThatTrack) DrawNameThatTrack(p);
    else DrawWhichCameFirst(p);
    for (const Button& b : q.buttons) DrawGameButton(b, p);
    if (q.guessRect.w > 0) DrawGuessField(p);
}

void GamesMouseDown(float x, float y) {
    if (!q.panel.In(x, y)) return ClosePanel();
    for (int i = 0; i < 2; ++i)
        if (q.tabRects[i].In(x, y)) return SetTab((Tab)i);
    for (size_t i = 0; i < q.suggestionRects.size() && i < q.suggestions.size(); ++i)
        if (q.suggestionRects[i].In(x, y)) return SubmitGuess(q.suggestions[i]);
    if (int id = HitButton(x, y)) OnButton(id);
}

void GamesKey(const SDL_KeyboardEvent& e) {
    g.redraw = true;
    const bool guessing = q.tab == Tab::NameThatTrack && q.phase == Phase::Guessing;
    if (e.key == SDLK_ESCAPE) {
        if (!q.suggestions.empty()) q.suggestions.clear();
        else ClosePanel();
        return;
    }
    if (e.key == SDLK_TAB) return SetTab(q.tab == Tab::NameThatTrack ? Tab::WhichCameFirst : Tab::NameThatTrack);
    if (guessing && !q.suggestions.empty() && (e.key == SDLK_DOWN || e.key == SDLK_UP)) {
        q.suggestion = std::clamp(q.suggestion + (e.key == SDLK_DOWN ? 1 : -1), 0, (int)q.suggestions.size() - 1);
        return;
    }
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        if (guessing && q.suggestion < (int)q.suggestions.size()) SubmitGuess(q.suggestions[q.suggestion]);
        else if (q.tab == Tab::WhichCameFirst && q.picked >= 0) NewPair();
        else if (q.tab == Tab::NameThatTrack && q.phase == Phase::Done) NewRound();
        else if (q.tab == Tab::NameThatTrack) PlayStage();
        return;
    }
    if (q.tab == Tab::WhichCameFirst) {
        if ((e.key == SDLK_LEFT || e.key == SDLK_RIGHT) && q.picked < 0) Pick(e.key == SDLK_LEFT ? 0 : 1);
        return;
    }
    if (!guessing) return;
    bool edited = false;
    if (e.key == SDLK_BACKSPACE && q.caret) q.guess.erase(--q.caret, 1), edited = true;
    else if (e.key == SDLK_DELETE && q.caret < q.guess.size()) q.guess.erase(q.caret, 1), edited = true;
    else if (e.key == SDLK_LEFT && q.caret) --q.caret;
    else if (e.key == SDLK_RIGHT && q.caret < q.guess.size()) ++q.caret;
    else if (e.key == SDLK_HOME) q.caret = 0;
    else if (e.key == SDLK_END) q.caret = q.guess.size();
    if (edited) UpdateSuggestions();
}

void GamesText(const std::wstring& text) {
    if (q.tab != Tab::NameThatTrack) return;
    // Space with nothing typed plays the clip, as Enter does when nothing is suggested.
    if (text == L" " && q.guess.empty()) {
        if (q.phase == Phase::Done) NewRound();
        else PlayStage();
        g.redraw = true;
        return;
    }
    if (q.phase != Phase::Guessing) return;
    std::wstring in;
    for (wchar_t c : text)
        if (c >= 32) in += c;
    if (in.empty()) return;
    q.guess.insert(q.caret, in);
    q.caret += in.size();
    UpdateSuggestions();
    g.redraw = true;
}

bool GamesWantsText(float x, float y) { return q.guessRect.In(x, y); }

void OnGameResult(LPARAM lp) {
    std::unique_ptr<GameResult> r(reinterpret_cast<GameResult*>(lp));
    bool pair = r->slot == kSlotLeft || r->slot == kSlotRight;
    if (!GamesOpen() || r->generation != (pair ? q.pairGen : q.roundGen)) return;  // a round that's over
    if (r->slot == kSlotClip) {
        // Couldn't decode a second of it: take the next song.
        if (!r->rate || r->clip.size() < 2 * (size_t)r->rate) {
            g_lib[q.answer].flags |= Track::kFailed;
            NewRound();
        } else {
            q.clip = std::move(r->clip);
            q.clipRate = r->rate;
            q.phase = Phase::Guessing;
            PlayStage();
        }
    } else if (r->slot == kSlotAnswerArt) {
        q.answerArt.Set(r->art);
    } else {
        q.covers[r->slot == kSlotLeft ? 0 : 1].Set(r->art);
    }
    g.redraw = true;
}

}  // namespace lx
