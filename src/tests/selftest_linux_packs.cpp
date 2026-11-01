// Linux pack integration in the existing --check-layout diagnostic, without screenshots. Real desktop
// fonts provide fixtures; every pack and settings write goes to a temporary profile, removed afterward.
#ifndef _WIN32
#include "../linux/app.h"
#include "../linux/text.h"
#include "../linux/ui.h"
#include "packs.h"

#include <SDL3_ttf/SDL_ttf.h>
#include <fontconfig/fontconfig.h>

#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace lx {
namespace {

struct FontFixture {
    std::string bytes;
    std::wstring family;
    int width = 0;
};

FontFixture ReadFont(const char* pattern, bool bold) {
    FontFixture out;
    FcPattern* request = FcNameParse((const FcChar8*)pattern);
    if (!request) return out;
    FcConfigSubstitute(nullptr, request, FcMatchPattern);
    FcDefaultSubstitute(request);
    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, request, &result);
    FcChar8* file = nullptr;
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        std::ifstream stream((const char*)file, std::ios::binary);
        out.bytes.assign(std::istreambuf_iterator<char>(stream), {});
        if (TTF_Font* font = TTF_OpenFont((const char*)file, 12)) {
            const char* family = TTF_GetFontFamilyName(font);
            if (family) out.family = compat::Widen(family);
            if (bold) TTF_SetFontStyle(font, TTF_STYLE_BOLD);
            TTF_SetFontHinting(font, TTF_HINTING_LIGHT_SUBPIXEL);
            TTF_GetStringSize(font, "Pack sample 123", 0, &out.width, nullptr);
            TTF_CloseFont(font);
        }
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(request);
    return out;
}

bool Write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), bytes.size());
    file.close();
    return !file.fail();
}

}  // namespace

int CheckLinuxPacks() {
    int failed = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        failed += !ok;
    };
    const FontFixture regular = ReadFont("monospace:weight=regular", false);
    const FontFixture bold = ReadFont("monospace:weight=bold", true);
    if (regular.bytes.empty() || regular.family.empty() || regular.family.size() >= LF_FACESIZE ||
        regular.width <= 0 || bold.bytes.empty() || bold.family != regular.family) {
        check(false, "pack font fixtures are available from the desktop fonts");
        return failed;
    }
    char temporary[] = "/tmp/cordinal-packs-XXXXXX";
    if (!mkdtemp(temporary)) {
        check(false, "pack checks have an isolated temporary profile");
        return failed;
    }
    namespace fs = std::filesystem;
    const fs::path root(temporary), pack = root / "packs" / "test";
    std::error_code error;
    fs::create_directories(pack / "fonts", error);
    bool ready = !error && Write(pack / "pack.ini", "[pack]\nname = Test\n") &&
        Write(pack / "theme.ini", "[theme]\nname = Test\nbase = dark\nfont = " + ToUtf8(regular.family) + "\n") &&
        Write(pack / "fonts/regular.ttf", regular.bytes) && Write(pack / "fonts/bold.otf", bold.bytes) &&
        Write(pack / "fonts/bad.ttf", "invalid font") &&
        Write(pack / "keys.ini", "[keys]\nCtrl+Alt+J = Playback > Shuffle\n") &&
        Write(pack / "eq.ini", "[curve]\nname = Pack curve\nbands = 3,2,1,0,0,0,0,-1,-2,-3\n");
    check(ready, "pack checks prepare fonts, a shortcut and an EQ preset");
    if (!ready) {
        fs::remove_all(root, error);
        return failed;
    }

    const Settings saved = g.settings;
    const std::wstring dataDir = g.dataDir, settingsPath = g.settingsPath;
    const float scale = g.scale;
    g.settings = Settings{};
    g.dataDir = compat::Widen(temporary);
    g.settingsPath = g.dataDir + L"/settings.ini";
    const size_t builtinPresets = EqPresets().size();
    LoadPacks();
    check(PackCount() == 1 && !PackAt(0).on && !UsesPackFont(Font::Normal) &&
              AllEqPresets().size() == builtinPresets,
          "a discovered pack leaves fonts and EQ presets off until enabled");
    TogglePack(0);
    g.settings.theme = ThemeMode::Count;
    ApplyTheme();
    check(UsesPackFont(Font::Normal) && UsesPackFont(Font::Bold) && UsesPackFont(Font::Small) &&
              UsesPackFont(Font::Title) && !UsesPackFont(Font::Menu) && UiSymbolsAvailable(),
          "enabled pack fonts cover UI text and headings while menus keep the system font and symbols fall back");
    check(TextWidth(Font::Normal, "Pack sample 123") == regular.width &&
              TextWidth(Font::Bold, "Pack sample 123") == bold.width,
          "the pack's regular and bold faces retain their own metrics");
    check(PackAt(0).warnings.size() == 1 && PackAt(0).warnings[0].find(L"fonts/bad.ttf") != std::wstring::npos,
          "an invalid font is reported without preventing the rest of the pack from loading");
    const auto presets = AllEqPresets();
    check(presets.size() == builtinPresets + 1 && std::wstring(presets.back().name) == L"Pack curve" &&
              presets.back().gains[0] == 3 && presets.back().gains[9] == -3,
          "an enabled pack offers its EQ curve after the built-in presets");
    SDL_KeyboardEvent key{};
    key.key = SDLK_J;
    key.mod = SDL_KMOD_CTRL | SDL_KMOD_ALT;
    check(PackKeyDown(key) && g.settings.shuffle == kShuffleTracks,
          "an enabled pack shortcut dispatches the named menu action");

    g.scale = 2;
    ApplyTheme();
    check(UsesPackFont(Font::Normal) && TextWidth(Font::Normal, "Pack sample 123") > regular.width &&
              DrawText(Font::Bold, "Pack sample 123", 0, 0, CurrentPalette().text) > 0,
          "pack fonts reopen and shape text at 200 percent scaling");
    g.scale = scale;
    LoadPacks();
    check(g.settings.theme == ThemeMode::Count && UsesPackFont(Font::Normal) &&
              DrawText(Font::Normal, "After reload", 0, 0, CurrentPalette().text) > 0,
          "reloading an active pack safely replaces font bytes and retains the chosen theme");
    TogglePack(0);
    const int shuffle = g.settings.shuffle;
    check(g.settings.theme == ThemeMode::System && !UsesPackFont(Font::Normal) && !UsesPackFont(Font::Bold) &&
              AllEqPresets().size() == builtinPresets && !PackKeyDown(key) && g.settings.shuffle == shuffle &&
              PackAt(0).warnings.empty() && UiSymbolsAvailable(),
          "disabling a pack releases its fonts, shortcut and presets and restores the system theme");

    // A single supplied face still gives headings bold text, and catalog bytes outlive the caller's copy.
    ResetPackFonts();
    {
        std::string bytes = regular.bytes;
        check(AddPackFont(bytes), "a single private font face loads");
    }
    std::wstring family = regular.family;
    for (wchar_t& c : family) c = (wchar_t)towlower(c);
    Theme theme = ThemeInfo(ThemeMode::Dark);
    theme.font = family.c_str();
    check(InitText(g.renderer, scale, theme) && UsesPackFont(Font::Normal) && UsesPackFont(Font::Bold) &&
              DrawText(Font::Bold, "Owned font bytes", 0, 0, DarkPalette().text) > 0,
          "private font matching ignores family case and owns bytes while synthesizing a missing bold face");
    theme.font = L"cordinal absent font";
    check(InitText(g.renderer, scale, theme) && !UsesPackFont(Font::Normal) && UiSymbolsAvailable(),
          "an unavailable theme font falls back to an installed face");
    ResetPackFonts();
    theme.font = regular.family.c_str();
    check(InitText(g.renderer, scale, theme) && !UsesPackFont(Font::Normal) &&
              TextWidth(Font::Normal, "Pack sample 123") == regular.width,
          "an installed custom theme face is selected by its family name without a pack font");

    g.settings = Settings{};
    g.dataDir = dataDir;
    g.settingsPath = settingsPath;
    LoadPacks();
    g.settings = saved;
    ApplyTheme();
    fs::remove_all(root, error);
    check(!error, "the temporary pack profile is removed");
    return failed;
}

}  // namespace lx
#endif
