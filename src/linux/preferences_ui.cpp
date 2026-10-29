// Startup defaults on Linux, as the Windows build's app_preferences.cpp: the theme, library view and browser
// (and the window size and layout once those can be chosen) are how cordinal opens only when the user says
// so. A change is a temporary choice for this session; settings.ini keeps the approved defaults. Depending
// on Preferences > Startup > Preference saving, a settled change is asked about once, in one compact
// checklist, saved as it is made, or kept only when chosen in Startup defaults. See docs/product-notes.md.
#include "draw.h"
#include "panels.h"

#include <ctime>

namespace lx {
namespace {

float g_hotX = -1, g_hotY = -1;  // where the pointer was when the card was last drawn

// Words wrapped to `width`.
std::vector<std::string> WrapLines(Font font, const std::string& text, float width) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find(' ', i);
        std::string word = text.substr(i, end == std::string::npos ? std::string::npos : end - i);
        std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextWidth(font, next) > width) {
            lines.push_back(line);
            line = word;
        } else line = next;
        i = end == std::string::npos ? text.size() : end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

constexpr unsigned kDefaultTheme = 1, kDefaultLibraryView = 2, kDefaultBrowser = 4;
constexpr unsigned kChoiceCount = 3;
constexpr uint64_t kSettleMs = 5 * 60 * 1000, kQuietMs = 15 * 1000, kReviewInterval = 24 * 60 * 60;

struct Choices {
    ThemeMode theme = ThemeMode::System;
    LibraryMode library = LibraryMode::Albums;
    bool browser = true;
    bool operator==(const Choices&) const = default;
};

Choices Current() { return {g.settings.theme, g.settings.libraryMode, g.settings.browserVisible}; }
Choices Approved() { return {g.settings.defaultTheme, g.settings.defaultLibraryMode, g.settings.defaultBrowserVisible}; }

unsigned Difference(const Choices& a, const Choices& b) {
    return (a.theme != b.theme ? kDefaultTheme : 0) | (a.library != b.library ? kDefaultLibraryView : 0) |
           (a.browser != b.browser ? kDefaultBrowser : 0);
}

Choices g_observed;
uint64_t g_changedAt = 0;
bool g_reviewed = false, g_tracking = false;

// The modeless checklist: a card over the window that takes no keyboard focus.
struct Review {
    bool open = false, policy = false, automatic = false;
    Choices choices;       // what the checks save: the values shown when it opened
    unsigned mask = 0;     // the choices that differ from the approved defaults
    unsigned checked = 0;  // the ones ticked
    bool automaticChoice = false;  // the policy question's radio
    Rect card, checks[kChoiceCount], radios[2], save, later;
} g_review;

std::wstring ChoiceText(unsigned index) {
    static const char* views[] = {"Tracks", "Albums and tracks", "Albums", "Artists"};
    const Choices& c = g_review.choices;
    switch (index) {
        case 0: return L"Theme: " + std::wstring(ThemeInfo(c.theme).name);
        case 1: return L"Library view: " + compat::Widen(views[(int)c.library]);
        default: return c.browser ? L"Library browser: shown" : L"Library browser: hidden";
    }
}

uint64_t WallSeconds() { return (uint64_t)time(nullptr); }

void Close() {
    g_review.open = false;
    g.redraw = true;
}

}  // namespace

static unsigned PendingStartupDefaults() { return Difference(Current(), Approved()); }

static void SaveStartupDefaults(const Choices& choices, unsigned selected) {
    if (!selected) return;
    if (selected & kDefaultTheme) g.settings.defaultTheme = choices.theme;
    if (selected & kDefaultLibraryView) g.settings.defaultLibraryMode = choices.library;
    if (selected & kDefaultBrowser) g.settings.defaultBrowserVisible = choices.browser;
    SaveSettings();
}

void ResetPreferenceSuggestions() {
    g_observed = Current();
    g_changedAt = 0;
    g_reviewed = false;
    g_tracking = true;
}

namespace {

void Observe(uint64_t now) {
    if (!g_tracking) return;
    Choices choices = Current();
    if (choices == g_observed) return;
    unsigned changes = Difference(choices, g_observed);
    g_observed = choices;
    g_changedAt = now;
    if (g.settings.preferenceSaving == PreferenceSaving::Automatic) SaveStartupDefaults(choices, changes);
}

}  // namespace

void FlushAutomaticPreferences() {
    if (g.settings.preferenceSaving == PreferenceSaving::Automatic) Observe(SDL_GetTicks());
}

void SetPreferenceSaving(PreferenceSaving mode) {
    if (mode >= PreferenceSaving::Count) return;
    FlushAutomaticPreferences();
    g.settings.preferenceSaving = mode;
    g.settings.preferenceSavingChosen = true;
    // Automatic saving starts from this moment: unchecked choices in a review stay unapproved.
    g_observed = Current();
    g_changedAt = mode == PreferenceSaving::Ask && PendingStartupDefaults() ? SDL_GetTicks() : 0;
    SaveSettings();
    if (mode != PreferenceSaving::Ask && g_review.open && g_review.automatic) Close();
}

// The Startup defaults checklist. `automatic` reviews are the patient ones, opened by cordinal itself.
void ShowStartupDefaults(bool automatic) {
    if (automatic && g.settings.preferenceSaving != PreferenceSaving::Ask) return;
    if (g_review.open) return;
    g_review = Review{};
    g_review.choices = Current();
    g_review.automatic = automatic;
    g_review.mask = PendingStartupDefaults();
    if (automatic && !g_review.mask) return;
    g_review.open = true;
    g_reviewed = true;
    g.settings.suggestionLastReview = WallSeconds();
    SaveSettings();
    g.redraw = true;
}

bool StartupReviewOpen() { return g_review.open; }

void OpenStartupDefaults() { ShowStartupDefaults(false); }

// Once per tick: notices changes, and offers the review when they have settled and nobody is busy.
void PreferenceTick(uint64_t lastInput) {
    const uint64_t now = SDL_GetTicks();
    Observe(now);
    if (g_reviewed || g_review.open || g.settings.preferenceSaving != PreferenceSaving::Ask || !g_changedAt || !PendingStartupDefaults()) return;
    if (now < g_changedAt || now - g_changedAt < kSettleMs) return;
    const uint64_t last = g.settings.suggestionLastReview;
    if (last && (WallSeconds() < last || WallSeconds() - last < kReviewInterval)) return;
    // Only when cordinal is the active window and input has been quiet for a while.
    if (!(SDL_GetWindowFlags(g.window) & SDL_WINDOW_INPUT_FOCUS) || (SDL_GetWindowFlags(g.window) & SDL_WINDOW_MINIMIZED)) return;
    if (PanelOpen() || now - lastInput < kQuietMs) return;
    ShowStartupDefaults(true);
}

namespace {

void DrawBox(const Rect& r, bool on, bool radio, const Palette& p) {
    Rect b{r.x, std::round(r.y + (r.h - S(14)) / 2), S(14), S(14)};
    Fill(b, on ? p.accent : r.In(g_hotX, g_hotY) ? p.hover : p.surface2);
    if (!on) Frame(b, p.sub);
    if (!on) return;
    if (radio) {
        Fill({b.x + S(4), b.y + S(4), S(6), S(6)}, TextOn(p.accent));
    } else {
        ThickLine(b.x + S(14) * 0.22f, b.y + S(14) * 0.52f, b.x + S(14) * 0.42f, b.y + S(14) * 0.72f, S(1.6f), TextOn(p.accent));
        ThickLine(b.x + S(14) * 0.42f, b.y + S(14) * 0.72f, b.x + S(14) * 0.80f, b.y + S(14) * 0.30f, S(1.6f), TextOn(p.accent));
    }
}

void DrawPlainButton(const Rect& r, const char* label, bool accent, bool enabled, const Palette& p) {
    bool hot = enabled && r.In(g_hotX, g_hotY);
    if (accent && enabled) Fill(r, hot ? p.accent2 : p.accent);
    else {
        Fill(r, hot ? p.hover : p.surface2);
        Frame(r, p.border);
    }
    DrawText(Font::Normal, label, r.x, TextY(r), !enabled ? p.faint : accent ? TextOn(hot ? p.accent2 : p.accent) : p.text, r.w, 1);
}

}  // namespace

void DrawStartupReview(const Palette& p, float mouseX, float mouseY) {
    if (!g_review.open) return;
    g_hotX = mouseX;
    g_hotY = mouseY;
    Review& v = g_review;
    unsigned count = 0;
    for (unsigned i = 0; i < kChoiceCount; ++i) count += (v.mask >> i) & 1;
    const float w = S(440), pad = S(18), rowH = S(30);
    const float h = v.policy ? S(252) : S(192) + count * rowH;
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    v.card = {(float)pw - w - S(16), (float)ph - h - S(40), w, h};
    Fill(v.card, p.surface);
    Frame(v.card, p.accent);
    float y = v.card.y + S(16);
    DrawText(Font::Bold, v.policy ? "How should I remember future choices?" : "Keep these for next time?", v.card.x + pad, y, p.text, w - 2 * pad);
    y += S(30);
    const char* body = v.policy ? "Save future presentation changes automatically, or keep asking which choices to keep?"
                       : v.mask ? "You've made cordinal your own. Pick the choices you'd like to use when it opens."
                                : "Your current choices already match your saved startup defaults.";
    for (const std::string& line : WrapLines(Font::Normal, body, w - 2 * pad)) {
        DrawText(Font::Normal, line, v.card.x + pad, y, p.sub, w - 2 * pad);
        y += LineHeight(Font::Normal);
    }
    y = v.card.y + S(96);
    if (v.policy) {
        const char* labels[2] = {"Save future preferences automatically", "Continue asking me"};
        for (int i = 0; i < 2; ++i, y += rowH) {
            v.radios[i] = {v.card.x + pad, y, w - 2 * pad, rowH};
            DrawBox(v.radios[i], i == 0 ? v.automaticChoice : !v.automaticChoice, true, p);
            DrawText(Font::Normal, labels[i], v.radios[i].x + S(22), TextY(v.radios[i]), p.text, v.radios[i].w - S(22));
        }
    } else {
        for (unsigned i = 0; i < kChoiceCount; ++i) {
            v.checks[i] = {};
            if (!(v.mask & (1u << i))) continue;
            v.checks[i] = {v.card.x + pad, y, w - 2 * pad, rowH};
            DrawBox(v.checks[i], v.checked & (1u << i), false, p);
            DrawText(Font::Normal, Utf8(ChoiceText(i)), v.checks[i].x + S(22), TextY(v.checks[i]), p.text, v.checks[i].w - S(22));
            y += rowH;
        }
    }
    DrawText(Font::Small, v.policy ? "Future changes only. Change this anytime in Preferences > Startup."
                                   : "Only checked choices are saved. Your current session stays as it is.",
             v.card.x + pad, y + S(8), p.faint, w - 2 * pad);
    const float by = v.card.B() - S(46);
    v.later = {v.card.R() - pad - S(86), by, S(86), S(28)};
    v.save = {v.later.x - S(8) - S(132), by, S(132), S(28)};
    DrawPlainButton(v.save, v.policy ? "Use this choice" : "Save selected", true, v.policy || v.checked != 0, p);
    DrawPlainButton(v.later, v.policy ? "Not now" : "Later", false, true, p);
}

bool StartupReviewMouseDown(float x, float y, int button) {
    Review& v = g_review;
    if (!v.open || !v.card.In(x, y)) return false;
    g.redraw = true;
    if (button != SDL_BUTTON_LEFT) return true;
    if (v.later.In(x, y)) {
        Close();
    } else if (v.save.In(x, y) && (v.policy || v.checked)) {
        if (v.policy) {
            SetPreferenceSaving(v.automaticChoice ? PreferenceSaving::Automatic : PreferenceSaving::Ask);
            Close();
        } else {
            SaveStartupDefaults(v.choices, v.checked);
            if (g.settings.preferenceSaving == PreferenceSaving::Ask && !g.settings.preferenceSavingChosen) {
                v.policy = true;  // one more question, once
                v.automaticChoice = false;
            } else {
                Close();
            }
        }
    } else if (v.policy) {
        for (int i = 0; i < 2; ++i)
            if (v.radios[i].In(x, y)) v.automaticChoice = i == 0;
    } else {
        for (unsigned i = 0; i < kChoiceCount; ++i)
            if (v.checks[i].w > 0 && v.checks[i].In(x, y)) v.checked ^= 1u << i;
    }
    return true;
}

}  // namespace lx
