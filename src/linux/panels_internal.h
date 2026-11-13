// What the Linux window's dialogs share (panels.h is their public face). panels.cpp owns which panel is
// open and hands it every event through the table of handlers each panel file exports (Ops below):
//
//   panel_eq.cpp        the equalizer                panel_tags.cpp     the tag editor
//   panel_prefs.cpp     Preferences                  panel_palette.cpp  the command palette
//   panel_rules.cpp     auto-playlist rules          panel_organize.cpp organize files, copy to device
//   panel_points.cpp    points, year in review       panel_album.cpp    Edit > Make album
//   panel_setup.cpp     title formatting, output device, layout, packs, helpers, scrobbling
//
// Panels drawn by other files (headphones_ui.cpp, games.cpp, editions_ui.cpp, coverfetch_ui.cpp,
// artistinfo_ui.cpp, collection_run.cpp) get their table in panels.cpp. Private to those files.
#pragma once
#include "headphones_ui.h"
#include "panels.h"
#include "ui_internal.h"

namespace lx::panel {

enum class Kind { None, Eq, Headphones, Tags, Prefs, Rules, Organize, Copy, ListenBrainz, Format, Device, Points, Review,
                  Palette, Layout, Packs, Helpers, Games, Editions, Covers, Artist, Export, Album, Count };
extern Kind g_kind;
extern float g_mx, g_my;  // the mouse, for hover states

// One panel's handlers. Any may be null; mouseDown gets only the left button.
struct Ops {
    void (*draw)(const Palette& p);
    void (*mouseDown)(float x, float y, int clicks) = nullptr;
    void (*mouseMove)(float x, float y) = nullptr;
    void (*mouseUp)() = nullptr;
    void (*wheel)(float x, float y, float dy) = nullptr;
    void (*key)(const SDL_KeyboardEvent& e) = nullptr;
    void (*text)(const char* utf8) = nullptr;  // typed text, never null
    void (*file)(int purpose, const std::wstring& path) = nullptr;  // a file chosen in a dialog it opened
    bool (*wantsText)(float x, float y) = nullptr;
    void (*close)() = nullptr;  // ClosePanel, while the panel is still current: let its state go
    bool typing = false;        // takes text input, which ClosePanel stops
};
extern const Ops kEqOps, kTagsOps, kPrefsOps, kRulesOps, kOrganizeOps, kCopyOps, kListenBrainzOps, kFormatOps,
    kDeviceOps, kPointsOps, kReviewOps, kPaletteOps, kLayoutOps, kPacksOps, kHelpersOps, kAlbumOps;

// --- shared pieces (panels.cpp) -------------------------------------------------------------------------
Rect OutputRect();
// A check box and its label; `state` 2 is mixed (some of the tracks), drawn as a bar.
void DrawCheck(const Rect& r, const std::string& label, int state, const Palette& p);
// The file and folder dialogs; the choice arrives as WM_APP_FILE with `purpose`, then Ops::file.
void ChooseFile(int purpose, const SDL_DialogFileFilter& filter);
void PickFolder(int purpose, const std::string& start = {});
std::string ReadSmallFile(const std::wstring& path, size_t max);
// A cover file chosen for a tag edit: its bytes when it is a JPEG or PNG, else empty after saying so.
std::string ReadCoverFile(const std::wstring& path);
// A cover preview: the image decoded to at most 320 pixels, as a texture; empty bytes leave none.
struct CoverPreview {
    std::string bytes;
    SDL_Texture* texture = nullptr;
    float w = 0, h = 0;
    void Set(const std::string& image);
    void Release();                // the texture only; the bytes stay so Set can rebuild it
    void Draw(const Rect& box) const;  // fitted and centered
};

// A text field: the tag editor's boxes and the one-line fields of the other panels.
struct TextField {
    std::wstring text;
    size_t caret = 0;
    bool all = false;  // everything selected (Ctrl+A, or entering by Tab): typing replaces it
    bool mixed = false, changed = false, enabled = true;
    Rect rect;
    float scrollX = 0, scrollY = 0;
};
// Ctrl+A, deletion and caret movement in `t`. Returns 1 when the text changed, 0 when only the caret
// moved, and -1 for a key the field doesn't use.
int LineFieldKey(TextField& t, const SDL_KeyboardEvent& e);
// Typed text into `t`, replacing a selected whole; false when nothing was typed.
bool LineFieldInsert(TextField& t, const std::wstring& in);
// Only the characters of `s` that are >= 32, which a one-line field takes.
std::wstring Printable(const std::wstring& s);
void DrawLineField(TextField& t, const Rect& r, bool focused, bool enabled, const char* cue, const Palette& p);
std::wstring Trimmed(const std::wstring& s);
float RowH();  // a row of the list panels
constexpr uint64_t kReplanDelay = 300;  // ms after the last keystroke, as the Windows dialogs' timers

// --- between panels -----------------------------------------------------------------------------------
// The tag editor and the album maker wait under the cover picker; these let their images and state go.
void TagsForget();
void TagsRenderer(bool restore);
void AlbumRelease();
void AlbumRenderer(bool restore);
// Preferences' settings, as the command palette lists them (panel_prefs.cpp).
size_t PrefCount();
bool PrefListed(int i);
PaletteCommand PrefCommand(int i);
std::wstring PrefSearchText(int i);
void RunPref(int i);  // a switch flips; anything else opens Preferences at it

}  // namespace lx::panel
