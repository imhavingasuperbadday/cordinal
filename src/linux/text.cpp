#include "text.h"

#include "app.h"
#include "theme_palette.h"

#include <SDL3_ttf/SDL_ttf.h>
#include <fontconfig/fontconfig.h>

#include <unordered_map>
#include <vector>

namespace lx {

namespace {

#include "web_font.inc"
#include "app_icon.inc"

TTF_TextEngine* g_engine = nullptr;
TTF_Font* g_fonts[(int)Font::Count] = {};
std::vector<TTF_Font*> g_fallbacks;
float g_lineHeight[(int)Font::Count] = {};

struct Key {
    int font;
    std::string text;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& k) const { return std::hash<std::string>()(k.text) * 31 + (size_t)k.font; }
};
std::unordered_map<Key, TTF_Text*, KeyHash> g_cache;

// A font file for a fontconfig pattern ("sans-serif:weight=bold"), or "" if none matches.
std::string FindFont(const char* pattern, std::string* family = nullptr) {
    FcPattern* p = FcNameParse(reinterpret_cast<const FcChar8*>(pattern));
    if (!p) return {};
    FcConfigSubstitute(nullptr, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, p, &result);
    std::string file;
    if (match) {
        FcChar8* f = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &f) == FcResultMatch) file = reinterpret_cast<const char*>(f);
        FcChar8* fam = nullptr;
        if (family && FcPatternGetString(match, FC_FAMILY, 0, &fam) == FcResultMatch) *family = reinterpret_cast<const char*>(fam);
        FcPatternDestroy(match);
    }
    FcPatternDestroy(p);
    return file;
}

TTF_Text* Shaped(Font font, const std::string& text) {
    Key key{(int)font, text};
    if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;
    if (g_cache.size() > 6000) ClearTextCache();
    TTF_Text* t = TTF_CreateText(g_engine, g_fonts[(int)font], text.data(), text.size());
    g_cache.emplace(std::move(key), t);
    return t;
}

}  // namespace

std::string Utf8(const std::wstring& s) { return compat::Narrow(s.c_str()); }

const unsigned char* DeltaruneFont(size_t* size) {
    *size = kExtraThemes ? sizeof(kWebFont) : 0;  // a placeholder byte without the themes
    return kWebFont;
}

// An .ico file: a six-byte header with the image count, then 16-byte entries (width, where 0 means 256, and
// the image's size and offset at bytes 8 and 12).
std::string_view AppIcon(int size) {
    auto u32 = [](const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; };
    const unsigned count = kAppIcon[4] | kAppIcon[5] << 8;
    std::string_view best;
    int bestWidth = 0;
    for (unsigned i = 0; i < count && 6 + 16 * (i + 1) <= sizeof(kAppIcon); ++i) {
        const unsigned char* e = kAppIcon + 6 + 16 * i;
        const int width = e[0] ? e[0] : 256;
        const uint32_t bytes = u32(e + 8), at = u32(e + 12);
        if (at > sizeof(kAppIcon) || bytes > sizeof(kAppIcon) - at) continue;
        bool better = best.empty() || (width >= size ? bestWidth < size || width < bestWidth : bestWidth < size && width > bestWidth);
        if (better) {
            best = {(const char*)kAppIcon + at, bytes};
            bestWidth = width;
        }
    }
    return best;
}

namespace {

// A fontconfig pattern for a Windows face: the face itself if installed, then the nearest
// metric-alike or generic family, so the theme keeps its character where the face is missing.
std::string Family(const wchar_t* face) {
    std::string f = face ? Utf8(face) : "";
    if (f == "Georgia") return "Georgia,Gelasio,DejaVu Serif,serif";
    if (f == "Bahnschrift") return "Bahnschrift,DIN Alternate,Barlow Semi Condensed,DejaVu Sans Condensed,sans-serif";
    if (f == "Consolas") return "Consolas,Inconsolata,DejaVu Sans Mono,monospace";
    // The Windows UI font, Segoe UI, then Selawik (its open metric match) and the desktop's sans.
    return "Segoe UI,Selawik,Noto Sans,sans-serif";
}

}  // namespace

bool InitText(SDL_Renderer* renderer, float scale, const Theme& theme) {
    ShutdownText();
    if (!TTF_WasInit() && !TTF_Init()) return false;
    g_engine = TTF_CreateRendererTextEngine(renderer);
    if (!g_engine) return false;
    // Sizes in pixels at 96 DPI, as Windows: the message font is 9 pt (12 px), a theme face may set
    // its own size, and the pixel font is drawn at whole multiples of its size without smoothing.
    std::string system = Family(nullptr), face = Family(theme.font);
    std::string regular = FindFont((face + ":weight=regular").c_str()), bold = FindFont((face + ":weight=bold").c_str());
    std::string menu = FindFont((system + ":weight=regular").c_str());
    if (bold.empty()) bold = regular;
    float px = (theme.fontPx ? theme.fontPx : 12) * scale;
    bool pixel = theme.fontResource != 0;
    if (pixel) px = (float)(theme.fontPx * std::max(1, (int)std::lround(scale)));
    struct Spec {
        Font font;
        const std::string* file;
        float px;
        bool pixel;
    } specs[] = {{Font::Normal, &regular, px, pixel}, {Font::Bold, &bold, px, pixel}, {Font::Small, &regular, px, pixel},
                 {Font::Title, &bold, std::round(15 * scale), false}, {Font::Menu, &menu, std::round(12 * scale), false}};
    for (auto& s : specs) {
        TTF_Font* f = nullptr;
        if (s.pixel) {
            size_t size = 0;
            const unsigned char* data = DeltaruneFont(&size);
            f = TTF_OpenFontIO(SDL_IOFromConstMem(data, size), true, s.px);
        } else {
            f = TTF_OpenFont(s.file->c_str(), s.px);
        }
        if (!f) return false;
        if (s.pixel && s.font == Font::Bold) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
        TTF_SetFontHinting(f, s.pixel ? TTF_HINTING_MONO : TTF_HINTING_LIGHT_SUBPIXEL);
        g_fonts[(int)s.font] = f;
        g_lineHeight[(int)s.font] = (float)TTF_GetFontHeight(f);
    }
    // Scripts the main font lacks (CJK, Arabic, symbols...): fontconfig's choices for them.
    std::vector<std::string> seen{regular, bold};  // fontconfig often answers several patterns with one file
    for (const char* pattern : {"sans-serif:lang=ja", "sans-serif:lang=ko", "sans-serif:lang=zh-cn", "sans-serif:lang=ar",
                                "sans-serif:lang=he", "sans-serif:lang=th", "sans-serif:lang=hi",
                                "sans-serif:charset=2605", "sans-serif:charset=2606", "sans-serif:charset=26a1",
                                "DejaVu Sans", "emoji"}) {
        std::string file = FindFont(pattern);
        if (file.empty() || std::find(seen.begin(), seen.end(), file) != seen.end()) continue;
        seen.push_back(file);
        for (int i = 0; i < (int)Font::Count; ++i) {
            TTF_Font* fb = TTF_OpenFont(file.c_str(), TTF_GetFontSize(g_fonts[i]));
            if (!fb) continue;
            TTF_AddFallbackFont(g_fonts[i], fb);
            g_fallbacks.push_back(fb);
        }
    }
    return true;
}

bool UiSymbolsAvailable() {
    for (Uint32 ch : {0x2605u, 0x2606u, 0x26a1u}) {
        if (TTF_FontHasGlyph(g_fonts[(int)Font::Normal], ch)) continue;
        if (std::none_of(g_fallbacks.begin(), g_fallbacks.end(), [ch](TTF_Font* f) { return TTF_FontHasGlyph(f, ch); }))
            return false;
    }
    return true;
}

void ClearTextCache() {
    for (auto& [key, t] : g_cache) TTF_DestroyText(t);
    g_cache.clear();
}

void ShutdownText() {
    ClearTextCache();
    for (auto& f : g_fonts) {
        if (f) TTF_CloseFont(f);
        f = nullptr;
    }
    for (TTF_Font* f : g_fallbacks) TTF_CloseFont(f);
    g_fallbacks.clear();
    if (g_engine) TTF_DestroyRendererTextEngine(g_engine);
    g_engine = nullptr;
}

float LineHeight(Font font) { return g_lineHeight[(int)font]; }

float TextWidth(Font font, const std::string& text) {
    if (text.empty()) return 0;
    int w = 0, h = 0;
    TTF_GetStringSize(g_fonts[(int)font], text.data(), text.size(), &w, &h);
    return (float)w;
}

float DrawText(Font font, const std::string& text, float x, float y, uint32_t rgb, float maxWidth, int align) {
    if (text.empty() || !g_engine) return 0;
    std::string shown = text;
    float w = TextWidth(font, shown);
    if (maxWidth > 0 && w > maxWidth) {
        static const std::string kEllipsis = "\xE2\x80\xA6";
        float room = maxWidth - TextWidth(font, kEllipsis);
        int fitWidth = 0;
        size_t fitLength = 0;
        if (room <= 0) return 0;
        TTF_MeasureString(g_fonts[(int)font], text.data(), text.size(), (int)room, &fitWidth, &fitLength);
        while (fitLength > 0 && (text[fitLength - 1] == ' ')) --fitLength;
        shown = text.substr(0, fitLength) + kEllipsis;
        w = TextWidth(font, shown);
    }
    if (maxWidth > 0 && align == 1) x += (maxWidth - w) / 2;
    if (maxWidth > 0 && align == 2) x += maxWidth - w;
    TTF_Text* t = Shaped(font, shown);
    if (!t) return 0;
    TTF_SetTextColor(t, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
    TTF_DrawRendererText(t, std::floor(x), std::floor(y));
    return w;
}

}  // namespace lx
