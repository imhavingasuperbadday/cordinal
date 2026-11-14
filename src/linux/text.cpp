// Theme fonts, private pack fonts and script fallbacks, laid out by the in-repo text stack
// (textlayout.h) and drawn from glyph atlases through the SDL renderer.
#include "text.h"

#include "app.h"
#include "fontmatch.h"
#include "textlayout.h"
#include "theme_palette.h"

#include "third_party/stb_image.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace lx {

namespace {

#include "web_font.inc"
#include "app_icon.inc"
#include "ui_fonts.inc"

using TextFont = lx::Font;

SDL_Renderer* g_renderer = nullptr;
std::unique_ptr<type::Font> g_fonts[(int)TextFont::Count];
std::vector<std::unique_ptr<type::Font>> g_fallbacks;
float g_lineHeight[(int)TextFont::Count] = {};
bool g_usesPackFont[(int)TextFont::Count] = {};

struct MemoryFont {
    type::FontData bytes;
    std::wstring family;
    bool bold, italic;
};
std::vector<MemoryFont> g_packFonts;

// Glyph atlases: 1024-pixel ARGB pages filled row by row, white with coverage as alpha (tinted
// when drawn) or a color glyph's own pixels, as SDL_ttf's renderer text engine kept them.
constexpr int kPage = 1024;
struct Page {
    SDL_Texture* texture = nullptr;
    int x = 0, y = 0, rowHeight = 0;
};
struct Slot {
    int page = -1;
    SDL_FRect src{};
    bool color = false;
};
std::vector<Page> g_pages;
std::unordered_map<uint64_t, Slot> g_slots;  // font id << 16 | glyph

// Strings drawn recently, laid out once: the glyph quads of each.
struct Key {
    int font;
    std::string text;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& k) const { return std::hash<std::string>()(k.text) * 31 + (size_t)k.font; }
};
std::unordered_map<Key, std::vector<type::Font::Quad>, KeyHash> g_cache;

// The face for a fontconfig-style pattern ("sans-serif:weight=bold"); false with no fonts installed.
bool FindFont(const char* pattern, fonts::Match* match) { return fonts::FindFont(pattern, match); }

// Font files mapped once while fonts are open, shared by every size drawn from them.
std::unordered_map<std::string, type::FontData> g_files;
type::FontData Mapped(const std::string& path) {
    if (path.empty()) return nullptr;
    auto& data = g_files[path];
    if (!data) data = type::MapFile(path);
    return data;
}

bool DecodePng(std::string_view png, int* w, int* h, std::vector<uint8_t>* rgba) {
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory((const unsigned char*)png.data(), (int)png.size(), w, h, &channels, 4);
    if (!pixels) return false;
    rgba->assign(pixels, pixels + (size_t)*w * *h * 4);
    stbi_image_free(pixels);
    return true;
}

void ClearAtlas() {
    for (Page& p : g_pages)
        if (p.texture) SDL_DestroyTexture(p.texture);
    g_pages.clear();
    g_slots.clear();
}

// The glyph's place in an atlas page, uploading it on first use; null for an empty glyph.
const Slot* Place(const type::Font::Quad& q) {
    uint64_t key = q.font->Id() << 16 | q.glyph;
    if (auto it = g_slots.find(key); it != g_slots.end()) return it->second.page >= 0 ? &it->second : nullptr;
    const type::GlyphImage& img = const_cast<type::Font*>(q.font)->Image(q.glyph);
    Slot slot;
    slot.color = img.color;
    int w = img.width, h = img.rows;
    if (w > 0 && h > 0 && w + 2 <= kPage && h + 2 <= kPage) {
        Page* page = g_pages.empty() ? nullptr : &g_pages.back();
        if (page && page->x + w + 1 > kPage) page->x = 1, page->y += page->rowHeight + 1, page->rowHeight = 0;
        if (!page || page->y + h + 1 > kPage) {
            if (g_pages.size() >= 8) {  // full: start over, the next frames upload what they draw
                ClearAtlas();
                ClearTextCache();
            }
            Page fresh;
            fresh.texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kPage, kPage);
            if (!fresh.texture) return nullptr;
            SDL_SetTextureBlendMode(fresh.texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(fresh.texture, SDL_SCALEMODE_NEAREST);
            std::vector<uint32_t> clear((size_t)kPage * kPage, 0x00FFFFFF);
            SDL_UpdateTexture(fresh.texture, nullptr, clear.data(), kPage * 4);
            fresh.x = fresh.y = 1;
            g_pages.push_back(fresh);
            page = &g_pages.back();
        }
        std::vector<uint32_t> pixels((size_t)w * h);
        for (size_t i = 0; i < pixels.size(); ++i) {
            if (img.color) {
                const uint8_t* p = &img.pixels[i * 4];
                pixels[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            } else {
                pixels[i] = 0x00FFFFFFu | (uint32_t)img.pixels[i] << 24;
            }
        }
        SDL_Rect dst{page->x, page->y, w, h};
        SDL_UpdateTexture(page->texture, &dst, pixels.data(), w * 4);
        slot.page = (int)g_pages.size() - 1;
        slot.src = {(float)page->x, (float)page->y, (float)w, (float)h};
        page->x += w + 1;
        page->rowHeight = std::max(page->rowHeight, h);
    }
    auto& placed = g_slots[key] = slot;
    return placed.page >= 0 ? &placed : nullptr;
}

const std::vector<type::Font::Quad>& Shaped(TextFont font, const std::string& text) {
    Key key{(int)font, text};
    if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;
    if (g_cache.size() > 6000) ClearTextCache();
    std::vector<type::Font::Quad> quads;
    g_fonts[(int)font]->Quads(text, &quads);
    return g_cache.emplace(std::move(key), std::move(quads)).first->second;
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

// A font pattern for a Windows face: the face itself if installed, then the nearest
// metric-alike or generic family, so the theme keeps its character where the face is missing.
std::string Family(const wchar_t* face) {
    std::string f = face ? Utf8(face) : "";
    if (f == "Georgia") return "Georgia,Gelasio,DejaVu Serif,serif";
    if (f == "Bahnschrift") return "Bahnschrift,DIN Alternate,Barlow Semi Condensed,DejaVu Sans Condensed,sans-serif";
    if (f == "Consolas") return "Consolas,Inconsolata,DejaVu Sans Mono,monospace";
    if (!f.empty()) {
        std::string escaped;
        for (char c : f) {
            if (c == '\\' || c == '-' || c == ',' || c == ':') escaped += '\\';
            escaped += c;
        }
        return escaped + ",sans-serif";
    }
    // The Windows UI font, Segoe UI, then Selawik (its open metric match) and the desktop's sans.
    return "Segoe UI,Selawik,Noto Sans,sans-serif";
}

const MemoryFont* FindPackFont(const wchar_t* family, bool bold) {
    if (!family || !*family) return nullptr;
    const MemoryFont* best = nullptr;
    int score = 4;
    for (const auto& font : g_packFonts) {
        if (_wcsicmp(font.family.c_str(), family)) continue;
        int next = font.bold != bold;
        if (font.italic) next += 2;
        if (next <= score) {  // the later enabled pack wins when both have the same face
            score = next;
            best = &font;
        }
    }
    return best;
}

}  // namespace

void ResetPackFonts() {
    ShutdownText();
    g_packFonts.clear();
}

bool AddPackFont(const std::string& bytes) {
    if (bytes.empty()) return false;
    type::FontData data = type::FromBytes(std::make_shared<const std::string>(bytes));
    auto probe = type::Face::Load(data, 0);
    if (!probe || probe->family.empty() || (!probe->Scalable() && !probe->HasColor())) return false;
    g_packFonts.push_back({data, compat::Widen(probe->family.c_str()), probe->bold, probe->italic});
    return true;
}

bool UsesPackFont(Font font) { return g_usesPackFont[(int)font]; }

bool InitText(SDL_Renderer* renderer, float scale, const Theme& theme) {
    ShutdownText();
    g_renderer = renderer;
    type::SetPngDecoder(DecodePng);
    // Sizes in pixels at 96 DPI, as Windows: the message font is 9 pt (12 px), a theme face may set
    // its own size, and the pixel font is drawn at whole multiples of its size without smoothing.
    std::string system = Family(nullptr), face = Family(theme.font);
    fonts::Match regular, bold, menu;
    bool found = FindFont((face + ":weight=regular").c_str(), &regular);
    if (!FindFont((face + ":weight=bold").c_str(), &bold)) bold = regular;
    FindFont((system + ":weight=regular").c_str(), &menu);
    // The Windows UI face: Segoe UI or Selawik where installed, else the Selawik compiled in (a build
    // with CORDINAL_SELAWIK), with the desktop's own face kept as the first fallback for the scripts
    // Selawik lacks (it is Latin only).
    auto windowsFace = [](const fonts::Match& m) { return m.family == "Segoe UI" || m.family == "Selawik"; };
    constexpr bool kHaveSelawik = sizeof(kFont_selawk) > 1;  // CORDINAL_SELAWIK
    const bool embedded = kHaveSelawik && !theme.font && !windowsFace(regular);
    const bool embeddedMenu = kHaveSelawik && !windowsFace(menu);
    auto selawik = [](bool heavy) {
        static const type::FontData regularBytes = type::FromBytes(std::make_shared<const std::string>((const char*)kFont_selawk, sizeof(kFont_selawk)));
        static const type::FontData boldBytes = type::FromBytes(std::make_shared<const std::string>((const char*)kFont_selawkb, sizeof(kFont_selawkb)));
        return heavy ? boldBytes : regularBytes;
    };
    float px = (theme.fontPx ? theme.fontPx : 12) * scale;
    bool pixel = theme.fontResource != 0;
    if (pixel) px = (float)(theme.fontPx * std::max(1, (int)std::lround(scale)));
    struct Spec {
        Font font;
        const fonts::Match* file;
        float px;
        bool pixel;
    } specs[] = {{Font::Normal, &regular, px, pixel}, {Font::Bold, &bold, px, pixel}, {Font::Small, &regular, px, pixel},
                 {Font::Title, &bold, std::round(15 * scale), false}, {Font::Menu, &menu, std::round(12 * scale), false},
                 // Preferences' page titles: the bold message font at 4/3 (app_tweaks.cpp, titleFont).
                 {Font::Heading, &bold, std::round(16 * scale), false}};
    for (auto& s : specs) {
        std::unique_ptr<type::Font> f;
        bool heading = s.font == Font::Bold || s.font == Font::Title || s.font == Font::Heading;
        if (s.pixel) {
            size_t size = 0;
            const unsigned char* data = DeltaruneFont(&size);
            f = type::Font::Open(type::FromBytes(std::make_shared<const std::string>((const char*)data, size)), 0, s.px);
        } else if (s.font != Font::Menu) {
            if (const MemoryFont* pack = FindPackFont(theme.font, heading)) {
                f = type::Font::Open(pack->bytes, 0, s.px);
                g_usesPackFont[(int)s.font] = f != nullptr;
                if (f && heading) f->SetBold(true);
            }
        }
        if (!f && !s.pixel && (s.font == Font::Menu ? embeddedMenu : embedded)) f = type::Font::Open(selawik(heading), 0, s.px);
        // The face the matcher reported, including its index in a collection.
        if (!f && !s.pixel && (found || !s.file->file.empty())) f = type::Font::Open(Mapped(s.file->file), s.file->index & 0xFFFF, s.px);
        if (!f) return false;
        if (s.pixel && s.font == Font::Bold) f->SetBold(true);
        f->SetHinting(s.pixel ? type::Font::Hinting::Mono : type::Font::Hinting::Light);
        g_lineHeight[(int)s.font] = (float)f->Height();
        g_fonts[(int)s.font] = std::move(f);
    }
    // Scripts the main font lacks (CJK, Arabic, symbols...): the matcher's choices for them, as fontconfig's.
    std::vector<std::pair<std::string, int>> seen;  // several patterns often give one face
    if (!embedded) seen = {{regular.file, regular.index}, {bold.file, bold.index}};
    // With Selawik in front, the desktop's face comes first after it: Cyrillic, Greek, Vietnamese...
    auto addFallback = [&](const fonts::Match& match) {
        std::pair<std::string, int> id{match.file, match.index};
        if (match.file.empty() || std::find(seen.begin(), seen.end(), id) != seen.end()) return;
        seen.push_back(id);
        type::FontData data = Mapped(match.file);
        if (!data) return;
        for (int i = 0; i < (int)Font::Count; ++i) {
            // Fallbacks keep the default (normal) hinting, as the SDL3_ttf fonts did: whole-pixel advances.
            auto fb = type::Font::Open(data, match.index & 0xFFFF, g_fonts[i]->Size());
            if (!fb) continue;
            g_fonts[i]->AddFallback(fb.get());
            g_fallbacks.push_back(std::move(fb));
        }
    };
    if (embedded) addFallback(regular);
    for (const char* pattern : {"sans-serif:lang=ja", "sans-serif:lang=ko", "sans-serif:lang=zh-cn", "sans-serif:lang=ar",
                                "sans-serif:lang=he", "sans-serif:lang=th", "sans-serif:lang=hi",
                                "sans-serif:charset=2605", "sans-serif:charset=2606", "sans-serif:charset=26a1",
                                "DejaVu Sans", "emoji"}) {
        fonts::Match match;
        if (FindFont(pattern, &match)) addFallback(match);
    }
    return true;
}

bool UiSymbolsAvailable() {
    for (uint32_t ch : {0x2605u, 0x2606u, 0x26a1u}) {
        if (g_fonts[(int)Font::Normal] && g_fonts[(int)Font::Normal]->HasGlyph(ch)) continue;
        if (std::none_of(g_fallbacks.begin(), g_fallbacks.end(), [ch](const auto& f) { return f->HasGlyph(ch); })) return false;
    }
    return true;
}

void ClearTextCache() { g_cache.clear(); }

void ShutdownText() {
    ClearTextCache();
    ClearAtlas();
    std::fill(std::begin(g_usesPackFont), std::end(g_usesPackFont), false);
    for (auto& f : g_fonts) f.reset();
    g_fallbacks.clear();
    g_files.clear();
    g_renderer = nullptr;
}

float LineHeight(Font font) { return g_lineHeight[(int)font]; }

float TextWidth(Font font, const std::string& text) {
    if (text.empty() || !g_fonts[(int)font]) return 0;
    return (float)g_fonts[(int)font]->Width(text);
}

float DrawText(Font font, const std::string& text, float x, float y, uint32_t rgb, float maxWidth, int align) {
    if (text.empty() || !g_renderer || !g_fonts[(int)font]) return 0;
    std::string shown = text;
    float w = TextWidth(font, shown);
    if (maxWidth > 0 && w > maxWidth) {
        static const std::string kEllipsis = "\xE2\x80\xA6";
        float room = maxWidth - TextWidth(font, kEllipsis);
        int fitWidth = 0;
        size_t fitLength = 0;
        if (room <= 0) return 0;
        g_fonts[(int)font]->Measure(text, (int)room, &fitWidth, &fitLength);
        while (fitLength > 0 && (text[fitLength - 1] == ' ')) --fitLength;
        shown = text.substr(0, fitLength) + kEllipsis;
        w = TextWidth(font, shown);
    }
    if (maxWidth > 0 && align == 1) x += (maxWidth - w) / 2;
    if (maxWidth > 0 && align == 2) x += maxWidth - w;
    // Copy the quads: placing a glyph can fill the atlas, which clears the string cache.
    std::vector<type::Font::Quad> quads = Shaped(font, shown);
    float ox = std::floor(x), oy = std::floor(y);
    SDL_FColor tint{((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, 1.f};
    SDL_FColor white{1.f, 1.f, 1.f, 1.f};
    // One batch per page and kind, in the order the glyphs come.
    struct Batch {
        int page;
        bool color;
        std::vector<SDL_Vertex> vertices;
        std::vector<int> indices;
    };
    std::vector<Batch> batches;
    for (const auto& q : quads) {
        const Slot* slot = Place(q);
        if (!slot) continue;
        Batch* b = nullptr;
        for (auto& existing : batches)
            if (existing.page == slot->page && existing.color == slot->color) b = &existing;
        if (!b) b = &batches.emplace_back(Batch{slot->page, slot->color, {}, {}});
        float x0 = ox + q.x, y0 = oy + q.y, x1 = x0 + slot->src.w, y1 = y0 + slot->src.h;
        float u0 = slot->src.x / kPage, v0 = slot->src.y / kPage, u1 = (slot->src.x + slot->src.w) / kPage,
              v1 = (slot->src.y + slot->src.h) / kPage;
        SDL_FColor c = slot->color ? white : tint;
        int base = (int)b->vertices.size();
        b->vertices.push_back({{x0, y0}, c, {u0, v0}});
        b->vertices.push_back({{x1, y0}, c, {u1, v0}});
        b->vertices.push_back({{x1, y1}, c, {u1, v1}});
        b->vertices.push_back({{x0, y1}, c, {u0, v1}});
        for (int k : {0, 1, 2, 0, 2, 3}) b->indices.push_back(base + k);
    }
    for (const auto& b : batches)
        if (b.page < (int)g_pages.size())
            SDL_RenderGeometry(g_renderer, g_pages[b.page].texture, b.vertices.data(), (int)b.vertices.size(), b.indices.data(),
                               (int)b.indices.size());
    return w;
}

}  // namespace lx
