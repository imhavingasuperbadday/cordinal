// Games (View ▸ Games and stats): two small games played with your own library.
//
//   Name That Track   a clip from the start of a song: 1 s, then 2, 4, 7, 11 and 16 s after each
//                     skip or wrong guess. Songs you play a lot come up more often.
//   Which Came First  two albums; pick the older one. The years get closer as the streak grows.
//
// Clips play on the sound-effects stream (sfx::PlayClip), never through the player: nothing here
// changes play counts, skips, the queue or what plays next. The music is paused while a clip plays.
#include "app.h"

namespace app {

namespace {

constexpr UINT WM_APP_GAME = WM_APP + 40;  // lParam = GameResult*
constexpr int kIdGuess = 100, kIdSuggest = 101;
constexpr double kStages[] = {1, 2, 4, 7, 11, 16};
constexpr int kStageCount = (int)std::size(kStages);
const wchar_t kGamesClass[] = L"CordinalGames";

enum class Tab { NameThatTrack, WhichCameFirst };
enum : int { kSlotClip, kSlotAnswerArt, kSlotLeft, kSlotRight };
enum class Phase { Loading, Guessing, Done, Unavailable };

// Work done off the UI thread: a decoded clip, or a cover.
struct GameResult {
    uint32_t generation = 0;
    int slot = 0;  // kSlotClip, kSlotAnswerArt, kSlotLeft, kSlotRight
    std::vector<float> clip;  // interleaved stereo
    unsigned rate = 0;
    ArtImage art;
    std::wstring error;
};

struct Button {
    RECT r{};
    int id = 0;
    std::wstring text;
    bool primary = false, visible = false;
};
enum : int { kBtnPlay = 1, kBtnSkip, kBtnGiveUp, kBtnNext, kBtnPlayIt, kBtnLeft, kBtnRight };

struct Album {
    StringPool::Handle album, artist;
    int year;
    TrackId track;  // one of its tracks, for the cover
};

struct Games {
    HWND hwnd = nullptr, guess = nullptr, suggest = nullptr;
    Tab tab = Tab::NameThatTrack;
    HFONT title = nullptr, big = nullptr, icons = nullptr;
    std::vector<Button> buttons;
    int hot = 0, pressed = 0;
    RECT tabRects[2]{};
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
    std::vector<TrackId> suggestions;
    std::deque<TrackId> recent;          // not picked again for a while
    ArtImage answerArt;
    bool pausedMusic = false;
    std::wstring note;

    // Which Came First
    std::vector<Album> albums;
    uint32_t albumsRevision = 0;
    int left = -1, right = -1, picked = -1;  // picked: -1 = not yet, 0 left, 1 right
    ArtImage covers[2];
    UINT_PTR advanceTimer = 0;
} q;

std::mt19937& Rng() { return g.rng; }

// ---------------------------------------------------------------------------
// Settings: [Games] in settings.ini

std::vector<int> Distribution() {
    std::vector<int> d = ParseInts(IniRead(L"Games", L"NameThatTrackGuesses", L""));
    d.resize(kStageCount + 1, 0);  // stages 1..6, then misses
    return d;
}

// ---------------------------------------------------------------------------
// Helpers

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

void DrawArt(HDC dc, const ArtImage& art, RECT r) {
    if (art.Empty()) return;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), art.width, -art.height, 1, 32, BI_RGB};
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, art.width, art.height, art.pixels.data(),
                  &bi, DIB_RGB_COLORS, SRCCOPY);
}

void Text(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF color, UINT flags) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

void RoundRect(HDC dc, RECT r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    ::RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

// A filled circle with anti-aliased edges (GDI's Ellipse has none): per-pixel coverage, blended.
void FillCircle(HDC dc, RECT r, COLORREF color) {
    int size = std::min(r.right - r.left, r.bottom - r.top);
    if (size <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(mem);
        return;
    }
    auto* px = static_cast<uint32_t*>(bits);
    double c = size / 2.0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            double d = std::hypot(x + 0.5 - c, y + 0.5 - c);
            uint32_t a = (uint32_t)std::lround(std::clamp(c - d + 0.5, 0.0, 1.0) * 255);
            uint32_t rr = GetRValue(color) * a / 255, gg = GetGValue(color) * a / 255, bb = GetBValue(color) * a / 255;
            px[y * size + x] = a << 24 | rr << 16 | gg << 8 | bb;  // premultiplied BGRA
        }
    HGDIOBJ old = SelectObject(mem, bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, r.left, r.top, size, size, mem, 0, 0, size, size, bf);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void Join() {
    for (auto& t : q.workers)
        if (t.joinable()) t.join();
    q.workers.clear();
}

// Workers are joined when the window closes; a round starts at most two.
void StartWorker(std::function<void(GameResult&)> work, int slot) {
    HWND hwnd = q.hwnd;
    uint32_t generation = slot == kSlotLeft || slot == kSlotRight ? q.pairGen : q.roundGen;
    q.workers.emplace_back([hwnd, generation, slot, work = std::move(work)] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* r = new GameResult;
        r->generation = generation;
        r->slot = slot;
        work(*r);
        if (!PostMessageW(hwnd, WM_APP_GAME, 0, (LPARAM)r)) delete r;
        CoUninitialize();
    });
}

void LoadCover(TrackId id, int slot) {
    std::wstring path = g_lib.Path(id);
    StartWorker([path](GameResult& r) { LoadAlbumArt(path, 512, r.art); }, slot);
}

void Relayout();
void Invalidate() {
    if (q.hwnd) InvalidateRect(q.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Name That Track

void StopClip() {
    sfx::StopClip();
    KillTimer(q.hwnd, 2);
}

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
    TrackId id = ids[pick(Rng())];
    q.recent.push_back(id);
    if (q.recent.size() > 50) q.recent.pop_front();
    return id;
}

// Starts a round: picks a song and decodes the clip and cover off the UI thread.
void NewRound() {
    StopClip();
    ++q.roundGen;
    q.attempts.clear();
    q.stage = 0;
    q.solved = false;
    q.clip.clear();
    q.answerArt = {};
    q.note.clear();
    SetWindowTextW(q.guess, L"");
    ShowWindow(q.suggest, SW_HIDE);
    if (g.exclusive) {
        q.phase = Phase::Unavailable;
        q.note = L"Clips play next to the music, which exclusive output doesn't allow. Switch to shared output "
                 L"(Preferences \x25B8 Output) to play.";
        Relayout();
        return;
    }
    q.answer = PickSong();
    if (q.answer == kNone) {
        q.phase = Phase::Unavailable;
        q.note = L"Name That Track needs songs of at least 30 seconds with titles in your library.";
        Relayout();
        return;
    }
    q.phase = Phase::Loading;
    std::wstring path = g_lib.Path(q.answer);
    StartWorker([path](GameResult& r) {
        auto dec = OpenTrackDecoder(path, 48000, r.error);
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
    Relayout();
}

// Plays the unlocked clip from the start, pausing the music while it plays.
void PlayStage() {
    if (q.phase != Phase::Guessing && q.phase != Phase::Done) return;
    if (q.clip.empty()) return;
    if (g.playing != kNone && !g.engine.Paused()) {
        TogglePause();
        q.pausedMusic = true;
    }
    double seconds = q.phase == Phase::Done ? kStages[kStageCount - 1] : kStages[q.stage];
    size_t frames = std::min(q.clip.size() / 2, (size_t)(seconds * q.clipRate));
    sfx::PlayClip(std::vector<float>(q.clip.begin(), q.clip.begin() + frames * 2), q.clipRate,
                  (float)std::pow(g.volumePercent / 100.0, 2.0));
    SetTimer(q.hwnd, 2, 30, nullptr);  // animates the progress bar
    Invalidate();
}

// Ends the round, updates the stage distribution and the streak, and plays the verdict sound.
void Finish(bool solved) {
    q.solved = solved;
    q.phase = Phase::Done;
    ShowWindow(q.suggest, SW_HIDE);
    auto d = Distribution();
    ++d[solved ? q.stage : kStageCount];
    IniWrite(L"Games", L"NameThatTrackGuesses", JoinInts(d));
    int streak = solved ? IniInt(L"Games", L"NameThatTrackStreak", 0) + 1 : 0;
    IniWrite(L"Games", L"NameThatTrackStreak", std::to_wstring(streak));
    if (streak > IniInt(L"Games", L"NameThatTrackBest", 0)) IniWrite(L"Games", L"NameThatTrackBest", std::to_wstring(streak));
    PlaySfx(solved ? Sfx::Sparkle : Sfx::Oops);
    Relayout();
}

// A wrong guess or a skip: unlock the next (longer) clip and play it.
void Advance(const std::wstring& attempt) {
    q.attempts.push_back(attempt);
    if (q.stage + 1 >= kStageCount) {
        Finish(false);
        return;
    }
    ++q.stage;
    SetWindowTextW(q.guess, L"");
    ShowWindow(q.suggest, SW_HIDE);
    Relayout();
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
    wchar_t buf[256];
    GetWindowTextW(q.guess, buf, 256);
    std::wstring typed = Lower(buf);
    q.suggestions.clear();
    SendMessageW(q.suggest, LB_RESETCONTENT, 0, 0);
    std::vector<std::wstring> words;
    for (size_t i = 0; i < typed.size();) {
        size_t j = typed.find(L' ', i);
        if (j == std::wstring::npos) j = typed.size();
        if (j > i) words.push_back(typed.substr(i, j - i));
        i = j + 1;
    }
    if (words.empty()) {
        ShowWindow(q.suggest, SW_HIDE);
        return;
    }
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
            if (priority == rank && q.suggestions.size() < 8) {
                q.suggestions.push_back(id);
                SendMessageW(q.suggest, LB_ADDSTRING, 0, (LPARAM)SongLabel(id).c_str());
            }
    if (!q.suggestions.empty()) SendMessageW(q.suggest, LB_SETCURSEL, 0, 0);
    Relayout();
}

LRESULT CALLBACK GuessSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && IsWindowVisible(q.suggest)) {
        int sel = (int)SendMessageW(q.suggest, LB_GETCURSEL, 0, 0), n = (int)q.suggestions.size();
        if (wp == VK_DOWN || wp == VK_UP) {
            sel = std::clamp(sel + (wp == VK_DOWN ? 1 : -1), 0, n - 1);
            SendMessageW(q.suggest, LB_SETCURSEL, sel, 0);
            return 0;
        }
        if (wp == VK_RETURN && sel >= 0 && sel < n) {
            SubmitGuess(q.suggestions[sel]);
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        if (IsWindowVisible(q.suggest)) ShowWindow(q.suggest, SW_HIDE);
        else SendMessageW(GetParent(h), WM_CLOSE, 0, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;  // no beep
    if (msg == WM_KEYDOWN && wp == VK_RETURN && q.phase == Phase::Done) {
        NewRound();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_SPACE && GetWindowTextLengthW(h) == 0) {
        PlayStage();
        return 0;
    }
    if (msg == WM_CHAR && wp == L' ' && GetWindowTextLengthW(h) == 0) return 0;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, GuessSubclass, 1);
    return DefSubclassProc(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Which Came First

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

// Builds the album list for Which Came First: one entry per album/artist that has a year.
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
    KillTimer(q.hwnd, 3);
    ++q.pairGen;
    CollectAlbums();
    q.picked = -1;
    q.left = q.right = -1;
    q.covers[0] = q.covers[1] = {};
    int streak = IniInt(L"Games", L"WhichCameFirstStreak", 0);
    // Easy at first (decades apart), then closer: at a streak of 10 the years are at most 2 apart.
    int maxGap = std::max(1, 30 - streak * 3);
    std::uniform_int_distribution<size_t> any(0, q.albums.empty() ? 0 : q.albums.size() - 1);
    for (int tries = 0; tries < 2000 && q.albums.size() >= 2; ++tries) {
        size_t a = any(Rng()), b = any(Rng());
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
    Relayout();
}

// Records the pick, updates the streak and schedules the next pair.
void Pick(int side) {
    if (q.left < 0 || q.picked >= 0) return;
    q.picked = side;
    const Album& chosen = q.albums[side == 0 ? q.left : q.right];
    const Album& other = q.albums[side == 0 ? q.right : q.left];
    bool right = chosen.year < other.year;
    int streak = right ? IniInt(L"Games", L"WhichCameFirstStreak", 0) + 1 : 0;
    IniWrite(L"Games", L"WhichCameFirstStreak", std::to_wstring(streak));
    if (streak > IniInt(L"Games", L"WhichCameFirstBest", 0)) IniWrite(L"Games", L"WhichCameFirstBest", std::to_wstring(streak));
    PlaySfx(right ? Sfx::Sparkle : Sfx::Oops);
    SetTimer(q.hwnd, 3, right ? 1300 : 2600, nullptr);  // a wrong answer gets time to sink in
    Relayout();
}

// ---------------------------------------------------------------------------
// Layout and painting

void AddButton(int id, const std::wstring& text, RECT r, bool primary = false) {
    q.buttons.push_back({r, id, text, primary, true});
}

// Rebuilds the buttons and positions the child controls for the current tab and phase.
void Relayout() {
    if (!q.hwnd) return;
    RECT rc;
    GetClientRect(q.hwnd, &rc);
    q.buttons.clear();
    int pad = S(20), cx = rc.right / 2;
    bool ntt = q.tab == Tab::NameThatTrack;
    bool guessing = ntt && q.phase == Phase::Guessing;
    ShowWindow(q.guess, guessing ? SW_SHOW : SW_HIDE);
    if (!guessing) ShowWindow(q.suggest, SW_HIDE);
    if (ntt) {
        int y = S(206);
        if (q.phase == Phase::Guessing || q.phase == Phase::Loading) {
            AddButton(kBtnPlay, L"", {cx - S(30), S(116), cx + S(30), S(176)}, true);
            int ey = y + S(150);
            MoveWindow(q.guess, pad, ey, rc.right - pad * 2, S(28), TRUE);
            int n = (int)q.suggestions.size();
            if (guessing && n && GetWindowTextLengthW(q.guess)) {
                int itemH = (int)SendMessageW(q.suggest, LB_GETITEMHEIGHT, 0, 0);
                MoveWindow(q.suggest, pad, ey + S(29), rc.right - pad * 2, itemH * n + S(4), TRUE);
                ShowWindow(q.suggest, SW_SHOWNA);
                SetWindowPos(q.suggest, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } else {
                ShowWindow(q.suggest, SW_HIDE);
            }
            int by = rc.bottom - pad - S(30);
            std::wstring skip = q.stage + 1 < kStageCount
                                    ? L"Skip (+" + std::to_wstring((int)(kStages[q.stage + 1] - kStages[q.stage])) + L" s)"
                                    : L"Skip";
            AddButton(kBtnSkip, skip, {pad, by, pad + S(120), by + S(30)});
            AddButton(kBtnGiveUp, L"Give up", {rc.right - pad - S(100), by, rc.right - pad, by + S(30)});
        } else if (q.phase == Phase::Done) {
            int by = rc.bottom - pad - S(30);
            AddButton(kBtnPlayIt, L"Play this song", {pad, by, pad + S(140), by + S(30)});
            AddButton(kBtnNext, L"Next song", {rc.right - pad - S(120), by, rc.right - pad, by + S(30)}, true);
            AddButton(kBtnPlay, L"", {pad, S(96), pad + S(150), S(246)});  // the cover replays the clip
        } else {
            int by = rc.bottom - pad - S(30);
            AddButton(kBtnNext, L"Try again", {rc.right - pad - S(120), by, rc.right - pad, by + S(30)}, true);
        }
    } else if (q.left >= 0) {
        int w = (rc.right - pad * 3) / 2, top = S(96), size = std::min(w, (int)rc.bottom - top - S(130));
        AddButton(kBtnLeft, L"", {pad, top, pad + w, top + size + S(70)});
        AddButton(kBtnRight, L"", {pad * 2 + w, top, pad * 2 + w * 2, top + size + S(70)});
        if (q.picked >= 0) {
            int by = rc.bottom - pad - S(30);
            AddButton(kBtnNext, L"Next pair", {rc.right - pad - S(120), by, rc.right - pad, by + S(30)}, true);
        }
    }
    Invalidate();
}

void PaintButton(HDC dc, const Button& b) {
    bool hot = q.hot == b.id, down = q.pressed == b.id && hot;
    if (b.id == kBtnPlay && q.tab == Tab::NameThatTrack && q.phase != Phase::Done) {
        // The round play button; a stop square while the clip plays.
        FillCircle(dc, b.r, Ref(hot ? P().accent2 : P().accent));
        bool playing = sfx::ClipPosition() >= 0;
        RECT gr = b.r;
        if (!playing) OffsetRect(&gr, S(2), 0);
        Text(dc, playing ? L"\xE71A" : L"\xE768", gr, q.icons, Ref(TextOn(P().accent)),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (b.text.empty()) return;  // card-sized hit areas draw themselves
    COLORREF fill = b.primary ? Ref(down ? P().accent2 : P().accent) : down ? Ref(P().select) : hot ? Ref(P().hover) : Ref(P().surface);
    RoundRect(dc, b.r, fill, b.primary ? fill : Ref(P().border), S(6));
    Text(dc, b.text, b.r, g.font, b.primary ? Ref(TextOn(P().accent)) : TextColor(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PaintStageBar(HDC dc, RECT bar) {
    // The 16 s timeline: unlocked time light, the playing clip in the accent, ticks at each stage.
    double total = kStages[kStageCount - 1];
    auto x = [&](double s) { return bar.left + (int)std::lround((bar.right - bar.left) * s / total); };
    FillSolid(dc, bar, Ref(P().surface));
    double unlocked = q.phase == Phase::Done ? total : kStages[q.stage];
    FillSolid(dc, {bar.left, bar.top, x(unlocked), bar.bottom}, Ref(P().select));
    double pos = sfx::ClipPosition();
    if (pos >= 0) FillSolid(dc, {bar.left, bar.top, x(std::min(pos, unlocked)), bar.bottom}, Ref(P().accent));
    for (int i = 0; i + 1 < kStageCount; ++i) FillSolid(dc, {x(kStages[i]) - 1, bar.top, x(kStages[i]) + 1, bar.bottom}, WindowBg());
    wchar_t label[32];
    swprintf_s(label, L"%g s", unlocked);
    Text(dc, label, {bar.left, bar.bottom + S(4), bar.right, bar.bottom + S(22)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE);
}

void PaintNameThatTrack(HDC dc, RECT rc) {
    int pad = S(20);
    int streak = IniInt(L"Games", L"NameThatTrackStreak", 0), best = IniInt(L"Games", L"NameThatTrackBest", 0);
    std::wstring sub = L"Streak " + std::to_wstring(streak) + L"  \x2022  best " + std::to_wstring(best);
    Text(dc, sub, {pad, S(64), rc.right - pad, S(84)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE);
    if (q.phase == Phase::Unavailable) {
        Text(dc, q.note, {pad, S(110), rc.right - pad, rc.bottom - S(80)}, q.big, TextColor(), DT_LEFT | DT_WORDBREAK);
        return;
    }
    if (q.phase == Phase::Done) {
        RECT art{pad, S(96), pad + S(150), S(246)};
        if (q.answerArt.Empty()) {
            FillSolid(dc, art, Ref(P().surface));
            Text(dc, L"\xE8D6", art, q.icons, SubColor(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            DrawArt(dc, q.answerArt, art);
        }
        int tx = art.right + S(16);
        const Track& t = g_lib[q.answer];
        std::wstring verdict = q.solved ? L"Got it in " + std::to_wstring((int)kStages[q.stage]) + (q.stage ? L" seconds" : L" second") + L"!"
                                        : L"It was\x2026";
        Text(dc, verdict, {tx, S(96), rc.right - pad, S(120)}, g.fontBold, q.solved ? Ref(P().accent) : SubColor(),
             DT_LEFT | DT_SINGLELINE);
        Text(dc, g_lib.Str(t.title), {tx, S(122), rc.right - pad, S(152)}, q.title, TextColor(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, ArtistOf(q.answer), {tx, S(154), rc.right - pad, S(176)}, q.big, TextColor(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        std::wstring album = g_lib.Str(t.album);
        if (YearOf(t.date)) album += (album.empty() ? L"" : L"  \x2022  ") + std::to_wstring(YearOf(t.date));
        Text(dc, album, {tx, S(178), rc.right - pad, S(198)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, L"Played " + std::to_wstring(t.playCount) + (t.playCount == 1 ? L" time" : L" times"),
             {tx, S(200), rc.right - pad, S(220)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE);
        // Guess distribution, Wordle style: this round's bar in the accent.
        auto d = Distribution();
        int maxv = std::max(1, *std::max_element(d.begin(), d.end()));
        int y = S(268), rowH = S(20), labelW = S(40), barMax = rc.right - pad * 2 - labelW - S(40);
        for (int i = 0; i <= kStageCount; ++i, y += rowH) {
            wchar_t label[16];
            if (i < kStageCount) swprintf_s(label, L"%g s", kStages[i]);
            else wcscpy_s(label, L"\x2715");
            Text(dc, label, {pad, y, pad + labelW, y + rowH - S(3)}, g.font, SubColor(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            int w = std::max(S(18), barMax * d[i] / maxv);
            bool mine = q.solved ? i == q.stage : i == kStageCount;
            RECT bar{pad + labelW + S(8), y + S(1), pad + labelW + S(8) + w, y + rowH - S(3)};
            FillSolid(dc, bar, mine ? Ref(P().accent) : Ref(P().surface));
            Text(dc, std::to_wstring(d[i]), {bar.left, bar.top, bar.right - S(5), bar.bottom}, g.font,
                 mine ? Ref(TextOn(P().accent)) : TextColor(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        return;
    }
    // Guessing: the timeline, then a row per stage with what happened there.
    PaintStageBar(dc, {pad, S(96), rc.right - pad, S(104)});
    if (q.phase == Phase::Loading) {
        Text(dc, L"Finding a song\x2026", {pad, S(186), rc.right - pad, S(206)}, g.font, SubColor(), DT_CENTER | DT_SINGLELINE);
    } else {
        Text(dc, L"Space plays the clip. Type to guess, Enter picks the highlighted song.", {pad, S(186), rc.right - pad, S(206)},
             g.font, SubColor(), DT_CENTER | DT_SINGLELINE);
    }
    int y = S(214), rowH = S(22);
    for (int i = 0; i < kStageCount; ++i, y += rowH) {
        RECT row{pad, y, rc.right - pad, y + rowH - S(4)};
        bool used = i < (int)q.attempts.size(), current = i == q.stage;
        FillSolid(dc, row, current ? Ref(P().select) : Ref(P().surface));
        std::wstring text = used ? (q.attempts[i].empty() ? L"Skipped" : L"\x2715  " + q.attempts[i]) : L"";
        Text(dc, text, {row.left + S(8), row.top, row.right - S(8), row.bottom}, g.font, used && !q.attempts[i].empty() ? Ref(P().error) : SubColor(),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void PaintAlbumCard(HDC dc, const Button& b, int side) {
    const Album& a = q.albums[side == 0 ? q.left : q.right];
    const Album& other = q.albums[side == 0 ? q.right : q.left];
    bool hot = q.hot == b.id && q.picked < 0;
    RECT r = b.r;
    int size = r.right - r.left;
    RECT art{r.left, r.top, r.right, r.top + std::min(size, (int)(r.bottom - r.top) - S(70))};
    art.right = art.left + (art.bottom - art.top);
    OffsetRect(&art, (size - (art.right - art.left)) / 2, 0);
    if (q.picked >= 0) {
        bool older = a.year < other.year;
        COLORREF ring = older ? Ref(P().ok) : side == q.picked ? Ref(P().error) : Ref(P().border);
        RECT outer = art;
        InflateRect(&outer, S(4), S(4));
        FillSolid(dc, outer, ring);
    } else if (hot) {
        RECT outer = art;
        InflateRect(&outer, S(3), S(3));
        FillSolid(dc, outer, Ref(P().accent));
    }
    if (q.covers[side].Empty()) {
        FillSolid(dc, art, Ref(P().surface));
        std::wstring initial = g_lib.Str(a.album).substr(0, 1);
        Text(dc, initial, art, g.fontArt, SubColor(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        DrawArt(dc, q.covers[side], art);
    }
    int y = art.bottom + S(10);
    Text(dc, g_lib.Str(a.album), {r.left, y, r.right, y + S(20)}, g.fontBold, TextColor(), DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(dc, g_lib.Str(a.artist), {r.left, y + S(20), r.right, y + S(38)}, g.font, SubColor(), DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (q.picked >= 0)
        Text(dc, std::to_wstring(a.year), {r.left, y + S(38), r.right, y + S(66)}, q.title, a.year < other.year ? Ref(P().ok) : TextColor(),
             DT_CENTER | DT_SINGLELINE);
}

void PaintWhichCameFirst(HDC dc, RECT rc) {
    int pad = S(20);
    int streak = IniInt(L"Games", L"WhichCameFirstStreak", 0), best = IniInt(L"Games", L"WhichCameFirstBest", 0);
    std::wstring sub = L"Pick the older album (\x2190 / \x2192).  Streak " + std::to_wstring(streak) + L"  \x2022  best " +
                       std::to_wstring(best);
    Text(dc, sub, {pad, S(64), rc.right - pad, S(84)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE);
    if (q.left < 0) {
        Text(dc, L"Which Came First needs albums from at least two different years (from their date tags).",
             {pad, S(110), rc.right - pad, rc.bottom - S(80)}, q.big, TextColor(), DT_LEFT | DT_WORDBREAK);
        return;
    }
    int cardsBottom = 0;
    for (const Button& b : q.buttons)
        if (b.id == kBtnLeft || b.id == kBtnRight) {
            PaintAlbumCard(dc, b, b.id == kBtnLeft ? 0 : 1);
            cardsBottom = b.r.bottom;
        }
    if (q.picked >= 0) {
        int gap = std::abs(q.albums[q.left].year - q.albums[q.right].year);
        bool right = (q.picked == 0) == (q.albums[q.left].year < q.albums[q.right].year);
        std::wstring verdict = (right ? L"Right: " : L"Not quite: ") + std::to_wstring(gap) + (gap == 1 ? L" year" : L" years") + L" apart.";
        Text(dc, verdict, {pad, cardsBottom + S(14), rc.right - pad, cardsBottom + S(36)}, q.big,
             right ? Ref(P().ok) : Ref(P().error), DT_CENTER | DT_SINGLELINE);
    }
}

void Paint(HDC target) {
    RECT rc;
    GetClientRect(q.hwnd, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    FillSolid(dc, rc, WindowBg());
    SetBkMode(dc, TRANSPARENT);
    // Tabs: the two game names as big headings; the active one in full color with an underline.
    const wchar_t* names[] = {L"Name That Track", L"Which Came First"};
    int x = S(20);
    for (int i = 0; i < 2; ++i) {
        HGDIOBJ old = SelectObject(dc, q.title);
        SIZE sz;
        GetTextExtentPoint32W(dc, names[i], (int)wcslen(names[i]), &sz);
        SelectObject(dc, old);
        q.tabRects[i] = {x, S(16), x + sz.cx, S(16) + sz.cy + S(8)};
        bool active = (int)q.tab == i;
        Text(dc, names[i], q.tabRects[i], q.title, active ? TextColor() : SubColor(), DT_LEFT | DT_TOP | DT_SINGLELINE);
        if (active) FillSolid(dc, {x, q.tabRects[i].bottom - S(3), x + sz.cx, q.tabRects[i].bottom - S(1)}, Ref(P().accent));
        x += sz.cx + S(28);
    }
    if (q.tab == Tab::NameThatTrack) PaintNameThatTrack(dc, rc);
    else PaintWhichCameFirst(dc, rc);
    for (const Button& b : q.buttons) PaintButton(dc, b);
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

int HitButton(POINT pt) {
    for (const Button& b : q.buttons)
        if (b.visible && PtInRect(&b.r, pt)) return b.id;
    return 0;
}

void SetTab(Tab tab) {
    StopClip();
    q.tab = tab;
    if (tab == Tab::WhichCameFirst && q.left < 0) NewPair();
    if (tab == Tab::NameThatTrack && q.answer == kNone) NewRound();
    Relayout();
    if (tab == Tab::NameThatTrack) SetFocus(q.guess);
    else SetFocus(q.hwnd);
}

void OnButton(int id) {
    switch (id) {
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
            PlayList({q.answer}, 0);
            break;
        case kBtnLeft: Pick(0); break;
        case kBtnRight: Pick(1); break;
    }
    if (q.tab == Tab::NameThatTrack && q.phase == Phase::Guessing) SetFocus(q.guess);
}

LRESULT CALLBACK GamesProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            q.hwnd = h;
            LOGFONTW lf{};
            GetObjectW(g.font, sizeof(lf), &lf);
            LONG base = lf.lfHeight;
            wcscpy_s(lf.lfFaceName, L"Segoe UI Semibold");
            lf.lfHeight = base * 16 / 9;
            q.title = CreateFontIndirectW(&lf);
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            lf.lfHeight = base * 12 / 9;
            q.big = CreateFontIndirectW(&lf);
            wcscpy_s(lf.lfFaceName, IconFace());
            lf.lfHeight = -S(22);
            q.icons = CreateFontIndirectW(&lf);
            q.guess = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0, 0, 0, 0, h,
                                      (HMENU)(INT_PTR)kIdGuess, g.inst, nullptr);
            SendMessageW(q.guess, WM_SETFONT, (WPARAM)q.big, TRUE);
            SendMessageW(q.guess, EM_SETCUEBANNER, TRUE, (LPARAM)L"Know it? Type the title or artist");
            SetWindowSubclass(q.guess, GuessSubclass, 1, 0);
            q.suggest = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, 0, 0,
                                        h, (HMENU)(INT_PTR)kIdSuggest, g.inst, nullptr);
            SendMessageW(q.suggest, WM_SETFONT, (WPARAM)g.font, TRUE);
            SendMessageW(q.suggest, LB_SETITEMHEIGHT, 0, S(22));
            for (HWND c : {q.guess, q.suggest}) SetWindowTheme(c, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            ApplyWindowTheme(h, P());
            return 0;
        }
        case WM_SIZE:
            Relayout();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(480), S(520)};
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hot = HitButton(pt);
            bool overTab = PtInRect(&q.tabRects[0], pt) || PtInRect(&q.tabRects[1], pt);
            SetCursor(LoadCursorW(nullptr, hot || overTab ? IDC_HAND : IDC_ARROW));
            if (hot != q.hot) {
                q.hot = hot;
                Invalidate();
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            q.hot = 0;
            Invalidate();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;  // WM_MOUSEMOVE picks it
            break;
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (int i = 0; i < 2; ++i)
                if (PtInRect(&q.tabRects[i], pt)) {
                    SetTab((Tab)i);
                    return 0;
                }
            q.pressed = HitButton(pt);
            if (q.pressed) SetCapture(h);
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            int pressed = std::exchange(q.pressed, 0);
            ReleaseCapture();
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pressed && HitButton(pt) == pressed) OnButton(pressed);
            Invalidate();
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(h);
            else if (q.tab == Tab::WhichCameFirst && (wp == VK_LEFT || wp == VK_RIGHT) && q.picked < 0) Pick(wp == VK_LEFT ? 0 : 1);
            else if (wp == VK_RETURN || wp == VK_SPACE) {
                if (q.tab == Tab::WhichCameFirst && q.picked >= 0) NewPair();
                else if (q.tab == Tab::NameThatTrack && q.phase == Phase::Done) NewRound();
                else if (q.tab == Tab::NameThatTrack) PlayStage();
            } else if (wp == VK_TAB) SetTab(q.tab == Tab::NameThatTrack ? Tab::WhichCameFirst : Tab::NameThatTrack);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kIdGuess && HIWORD(wp) == EN_CHANGE) UpdateSuggestions();
            if (LOWORD(wp) == kIdSuggest && HIWORD(wp) == LBN_SELCHANGE) {
                int sel = (int)SendMessageW(q.suggest, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)q.suggestions.size()) SubmitGuess(q.suggestions[sel]);
            }
            return 0;
        case WM_TIMER:
            if (wp == 2) {  // clip progress
                Invalidate();
                if (sfx::ClipPosition() < 0) KillTimer(h, 2);
            } else if (wp == 3) {
                KillTimer(h, 3);
                if (q.tab == Tab::WhichCameFirst) NewPair();
            }
            return 0;
        case WM_APP_GAME: {
            std::unique_ptr<GameResult> r(reinterpret_cast<GameResult*>(lp));
            bool pair = r->slot == kSlotLeft || r->slot == kSlotRight;
            if (r->generation != (pair ? q.pairGen : q.roundGen)) return 0;  // a round that's over
            if (r->slot == kSlotClip) {
                // Couldn't decode a second of it: take the next song.
                if (!r->rate || r->clip.empty() || r->clip.size() < 2 * (size_t)r->rate) {
                    g_lib[q.answer].flags |= Track::kFailed;
                    NewRound();
                    return 0;
                }
                q.clip = std::move(r->clip);
                q.clipRate = r->rate;
                q.phase = Phase::Guessing;
                Relayout();
                SetFocus(q.guess);
                PlayStage();
            } else if (r->slot == kSlotAnswerArt) {
                q.answerArt = std::move(r->art);
            } else {
                q.covers[r->slot == kSlotLeft ? 0 : 1] = std::move(r->art);
            }
            Invalidate();
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            sfx::StopClip();
            Join();
            return 0;
        case WM_NCDESTROY:
            for (HFONT f : {q.title, q.big, q.icons})
                if (f) DeleteObject(f);
            {
                std::deque<TrackId> recent = std::move(q.recent);
                q = Games{};
                q.recent = std::move(recent);
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowGames(int which) {
    if (!q.hwnd) {
        RegisterAppClass(kGamesClass, GamesProc);
        RECT r{0, 0, S(560), S(600)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, g.dpi);
        RECT owner;
        GetWindowRect(g.hwnd, &owner);
        int w = r.right - r.left, hgt = r.bottom - r.top;
        CreateWindowExW(0, kGamesClass, L"Games", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                        owner.left + (owner.right - owner.left - w) / 2, owner.top + S(40), w, hgt, g.hwnd, nullptr, g.inst, nullptr);
        ShowWindow(q.hwnd, SW_SHOW);
    }
    SetForegroundWindow(q.hwnd);
    SetTab(which == 0 ? Tab::NameThatTrack : Tab::WhichCameFirst);
}

}  // namespace app
