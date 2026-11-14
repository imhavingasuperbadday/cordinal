// The modal pieces drawn inside the player's own window: message boxes, and the file chooser used
// when the desktop has no portal. Each runs a nested event loop over a dimmed copy of the window,
// keeping the events that aren't its input for the player to handle afterwards.
#include "platform/platform.h"

#include "text.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace lx::platform {
namespace {

constexpr uint32_t kPanel = 0xF3F3F3, kInk = 0x1B1B1B, kMuted = 0x5F5F5F, kButton = 0xE1E1E1, kEdge = 0xADADAD,
                   kAccent = 0x0067C0, kSelect = 0xCCE4F7;

void Fill(SDL_Renderer* r, float x, float y, float w, float h, uint32_t rgb, uint8_t a = 255) {
    SDL_SetRenderDrawColor(r, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, a);
    SDL_FRect f{x, y, w, h};
    SDL_RenderFillRect(r, &f);
}

void Frame(SDL_Renderer* r, float x, float y, float w, float h, uint32_t rgb) {
    SDL_SetRenderDrawColor(r, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255);
    SDL_FRect f{x, y, w, h};
    SDL_RenderRect(r, &f);
}

bool Inside(float px, float py, const SDL_FRect& f) { return px >= f.x && py >= f.y && px < f.x + f.w && py < f.y + f.h; }

// The window as it was, the renderer state to put back, and the events to hand back.
class Modal {
public:
    explicit Modal(SDL_Window* w) : w_(w), r_(RendererForWindow(w)) {
        if (!r_ || LineHeight(Font::Normal) <= 0 || !CurrentBackend() || std::string_view(CurrentBackend()->Name()) == "offscreen")
            return;
        SDL_GetRenderClipRect(r_, &clip_);
        clipOn_ = SDL_RenderClipEnabled(r_);
        if (SDL_Surface* s = SDL_RenderReadPixels(r_, nullptr)) {
            snap_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, s->w, s->h);
            if (snap_) SDL_UpdateTexture(snap_, nullptr, s->pixels, s->pitch);
            SDL_DestroySurface(s);
        }
        textWas_ = TextInputActive();
        ok_ = true;
    }
    ~Modal() {
        if (!ok_) return;
        if (snap_) SDL_DestroyTexture(snap_);
        SDL_SetRenderClipRect(r_, clipOn_ ? &clip_ : nullptr);
        SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
        if (textWas_) SDL_StartTextInput(w_);
        else SDL_StopTextInput(w_);
        for (const auto& [e, text] : kept_) PushEvent(e, text);
        SDL_Event e{};
        e.type = SDL_EVENT_WINDOW_EXPOSED;
        e.window.windowID = w_->id;
        PushEvent(e);  // the player draws itself again
    }
    bool ok() const { return ok_; }
    SDL_Renderer* r() const { return r_; }
    int W() const { return w_->pw; }
    int H() const { return w_->ph; }
    float Density() const { return SDL_GetWindowPixelDensity(w_); }
    void Backdrop() {
        SDL_SetRenderClipRect(r_, nullptr);
        SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
        Fill(r_, 0, 0, (float)W(), (float)H(), 0x202020);
        if (snap_) {
            float tw, th;
            SDL_GetTextureSize(snap_, &tw, &th);
            SDL_FRect d{0, 0, tw, th};
            SDL_RenderTexture(r_, snap_, nullptr, &d);
        }
        Fill(r_, 0, 0, (float)W(), (float)H(), 0x000000, 96);
    }
    // The next event that is input for the modal; others are kept. False on timeout.
    bool Next(SDL_Event& e, int timeoutMs) {
        if (!SDL_WaitEventTimeout(&e, timeoutMs)) return false;
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_QUIT:
            return true;
        case SDL_EVENT_DROP_FILE: case SDL_EVENT_DROP_TEXT:
            return true;  // dropped while modal: ignored, as a native dialog would
        default:
            kept_.emplace_back(e, std::string());
            if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) return true;
            return false;
        }
    }

private:
    SDL_Window* w_;
    SDL_Renderer* r_;
    SDL_Texture* snap_ = nullptr;
    SDL_Rect clip_{};
    bool clipOn_ = false, textWas_ = false, ok_ = false;
    std::vector<std::pair<SDL_Event, std::string>> kept_;
};

// Words to lines no wider than `width`; a word wider than that gets a line of its own.
std::vector<std::string> Wrap(Font font, const std::string& text, float width) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string line;
        size_t i = 0;
        while (i <= para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && TextWidth(font, trial) > width) {
                out.push_back(line);
                line = word;
            } else {
                line = trial;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        out.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

struct Button {
    std::string text;
    SDL_FRect rect;
};

void DrawButton(SDL_Renderer* r, const Button& b, bool focused, bool hot) {
    Fill(r, b.rect.x, b.rect.y, b.rect.w, b.rect.h, hot ? 0xE5F1FB : kButton);
    Frame(r, b.rect.x, b.rect.y, b.rect.w, b.rect.h, focused ? kAccent : kEdge);
    if (focused) Frame(r, b.rect.x + 1, b.rect.y + 1, b.rect.w - 2, b.rect.h - 2, kAccent);
    const float lh = LineHeight(Font::Normal);
    DrawText(Font::Normal, b.text, b.rect.x, b.rect.y + (b.rect.h - lh) / 2, kInk, b.rect.w, 1);
}

float ButtonWidth(const std::string& text) {
    const float lh = LineHeight(Font::Normal);
    return std::max(TextWidth(Font::Normal, text) + 2 * lh, 5 * lh);
}

}  // namespace

bool OverlayMessageBox(const SDL_MessageBoxData& data, int& chosen) {
    SDL_Window* w = data.window;
    if (!w) w = WindowById(1);
    if (!w) return false;
    Modal m(w);
    if (!m.ok()) return false;
    SDL_Renderer* r = m.r();
    const float lh = LineHeight(Font::Normal), pad = std::round(lh * 0.9f), bh = std::round(lh * 1.8f);
    int focus = 0, escape = -1;
    for (int i = 0; i < data.numbuttons; ++i) {
        if (data.buttons[i].flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) focus = i;
        if (data.buttons[i].flags & SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT) escape = i;
    }
    const std::string title = data.title ? data.title : "", message = data.message ? data.message : "";
    int hot = -1;
    float mx = -1, my = -1;
    std::vector<Button> buttons;
    for (bool dirty = true;;) {
        if (dirty) {
            const float maxW = std::min((float)m.W() - 2 * lh, 34 * lh);
            float inner = std::max(18 * lh, TextWidth(Font::Bold, title));
            for (const std::string& line : Wrap(Font::Normal, message, maxW - 2 * pad)) inner = std::max(inner, TextWidth(Font::Normal, line));
            const float boxW = std::min(maxW, inner + 2 * pad);
            std::vector<std::string> lines = Wrap(Font::Normal, message, boxW - 2 * pad);
            const float titleH = title.empty() ? 0 : LineHeight(Font::Bold) + pad / 2;
            const float boxH = pad + titleH + lines.size() * lh + pad + bh + pad;
            const float bx = std::round((m.W() - boxW) / 2), by = std::round(std::max(lh, (m.H() - boxH) / 2));
            m.Backdrop();
            Fill(r, bx, by, boxW, boxH, kPanel);
            Frame(r, bx, by, boxW, boxH, 0x8A8A8A);
            float y = by + pad;
            if (!title.empty()) {
                DrawText(Font::Bold, title, bx + pad, y, kInk, boxW - 2 * pad);
                y += titleH;
            }
            for (const std::string& line : lines) {
                DrawText(Font::Normal, line, bx + pad, y, data.flags & SDL_MESSAGEBOX_ERROR ? 0xA4262C : kInk, boxW - 2 * pad);
                y += lh;
            }
            buttons.clear();
            float total = 0;
            for (int i = 0; i < data.numbuttons; ++i) total += ButtonWidth(data.buttons[i].text ? data.buttons[i].text : "") + (i ? pad / 2 : 0);
            float x = bx + boxW - pad - total;
            for (int i = 0; i < data.numbuttons; ++i) {
                const std::string text = data.buttons[i].text ? data.buttons[i].text : "";
                const float bw = ButtonWidth(text);
                buttons.push_back({text, SDL_FRect{std::round(x), std::round(by + boxH - pad - bh), std::round(bw), bh}});
                x += bw + pad / 2;
            }
            hot = -1;
            for (int i = 0; i < (int)buttons.size(); ++i)
                if (Inside(mx, my, buttons[i].rect)) hot = i;
            for (int i = 0; i < (int)buttons.size(); ++i) DrawButton(r, buttons[i], i == focus, i == hot);
            SDL_RenderPresent(r);
            dirty = false;
        }
        SDL_Event e;
        if (!m.Next(e, 1000)) continue;
        dirty = true;
        auto finish = [&](int index) {
            chosen = index >= 0 && index < data.numbuttons ? data.buttons[index].buttonID : -1;
            return true;
        };
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE) return finish(focus);
            if (e.key.key == SDLK_ESCAPE) return finish(escape);
            if (data.numbuttons > 0 && (e.key.key == SDLK_TAB || e.key.key == SDLK_RIGHT || e.key.key == SDLK_LEFT)) {
                const bool back = e.key.key == SDLK_LEFT || (e.key.key == SDLK_TAB && (e.key.mod & SDL_KMOD_SHIFT));
                focus = (focus + (back ? data.numbuttons - 1 : 1)) % data.numbuttons;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            mx = e.motion.x * m.Density(), my = e.motion.y * m.Density();
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button != SDL_BUTTON_LEFT) break;
            mx = e.button.x * m.Density(), my = e.button.y * m.Density();
            for (int i = 0; i < (int)buttons.size(); ++i)
                if (Inside(mx, my, buttons[i].rect)) return finish(i);
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_QUIT:
            return finish(escape);
        default: break;
        }
    }
}

// --- the file chooser ---------------------------------------------------------------------------------------

namespace {

struct Entry {
    std::string name;
    bool dir;
};

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool Matches(const std::string& name, const std::string& pattern) {
    if (pattern.empty()) return true;
    const std::string lower = Lower(name);
    for (size_t i = 0; i <= pattern.size();) {
        size_t j = pattern.find(';', i);
        if (j == std::string::npos) j = pattern.size();
        std::string ext = Lower(pattern.substr(i, j - i));
        i = j + 1;
        if (ext == "*") return true;
        if (!ext.empty() && lower.size() > ext.size() + 1 && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0 &&
            lower[lower.size() - ext.size() - 1] == '.')
            return true;
    }
    return false;
}

std::vector<Entry> List(const fs::path& dir, bool foldersOnly, const std::string& pattern) {
    std::vector<Entry> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code e2;
        const bool isDir = it->is_directory(e2);
        if (!isDir && (foldersOnly || !Matches(name, pattern))) continue;
        out.push_back({name, isDir});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return Lower(a.name) < Lower(b.name);
    });
    return out;
}

}  // namespace

std::optional<std::vector<std::string>> OverlayFileChooser(SDL_Window* w, const ChooserRequest& rq) {
    if (!w) w = WindowById(1);
    if (!w) return std::nullopt;
    Modal m(w);
    if (!m.ok()) return std::nullopt;
    SDL_Renderer* r = m.r();
    const bool save = rq.kind == ChooserKind::SaveFile, folders = rq.kind == ChooserKind::OpenFolder;
    std::error_code ec;
    fs::path dir = rq.location.empty() ? fs::path(getenv("HOME") ? getenv("HOME") : "/") : fs::path(rq.location);
    std::string field;
    if (!fs::is_directory(dir, ec)) {
        if (save) field = dir.filename().string();
        dir = dir.parent_path();
        if (!fs::is_directory(dir, ec)) dir = getenv("HOME") ? getenv("HOME") : "/";
    }
    dir = fs::weakly_canonical(dir, ec);
    size_t filter = 0;
    std::vector<Entry> entries;
    std::vector<bool> selected;
    int cursor = -1, top = 0;
    auto reload = [&] {
        entries = List(dir, folders, rq.filters.empty() ? std::string() : rq.filters[filter].second);
        selected.assign(entries.size(), false);
        cursor = -1, top = 0;
    };
    reload();
    SDL_StartTextInput(w);
    const float lh = LineHeight(Font::Normal), pad = std::round(lh * 0.8f), rowH = std::round(lh * 1.35f), bh = std::round(lh * 1.8f);
    float mx = -1, my = -1;
    uint64_t lastClick = 0;
    int lastClickRow = -1;
    int rows = 1;
    SDL_FRect listRect{}, accept{}, cancel{}, filterButton{}, upButton{};
    auto chosenPaths = [&]() -> std::optional<std::vector<std::string>> {
        std::vector<std::string> out;
        if (!field.empty()) {
            fs::path p = field[0] == '/' ? fs::path(field) : dir / field;
            if (fs::is_directory(p, ec) && !save && !folders) {
                dir = fs::weakly_canonical(p, ec);
                field.clear();
                reload();
                return std::nullopt;
            }
            if (save && fs::exists(p, ec)) {
                SDL_MessageBoxButtonData b[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Replace"},
                                                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}};
                std::string q = p.filename().string() + " already exists. Replace it?";
                SDL_MessageBoxData d{SDL_MESSAGEBOX_WARNING, w, rq.title.c_str(), q.c_str(), 2, b, nullptr};
                int answer = 0;
                if (!OverlayMessageBox(d, answer) || answer != 1) return std::nullopt;
            }
            out.push_back(p.string());
            return out;
        }
        for (size_t i = 0; i < entries.size(); ++i)
            if (selected[i] && (folders || !entries[i].dir)) out.push_back((dir / entries[i].name).string());
        if (out.empty() && folders) out.push_back(dir.string());
        if (out.empty()) {
            if (cursor >= 0 && entries[cursor].dir) {
                dir = dir / entries[cursor].name;
                reload();
            }
            return std::nullopt;
        }
        return out;
    };
    auto enter = [&](int i) {
        if (i < 0 || i >= (int)entries.size()) return false;
        if (entries[i].dir) {
            dir = dir / entries[i].name;
            reload();
            return false;
        }
        return true;
    };
    for (bool dirty = true;;) {
        if (dirty) {
            const float boxW = std::min((float)m.W() - 2 * lh, 40 * lh), boxH = std::min((float)m.H() - 2 * lh, 30 * lh);
            const float bx = std::round((m.W() - boxW) / 2), by = std::round((m.H() - boxH) / 2);
            m.Backdrop();
            Fill(r, bx, by, boxW, boxH, kPanel);
            Frame(r, bx, by, boxW, boxH, 0x8A8A8A);
            float y = by + pad;
            DrawText(Font::Bold, rq.title, bx + pad, y, kInk, boxW - 2 * pad);
            y += LineHeight(Font::Bold) + pad / 2;
            upButton = {bx + pad, y, ButtonWidth("Up"), bh};
            DrawButton(r, {"Up", upButton}, false, Inside(mx, my, upButton));
            DrawText(Font::Normal, dir.string(), upButton.x + upButton.w + pad / 2, y + (bh - lh) / 2, kMuted,
                     boxW - 2 * pad - upButton.w - pad / 2);
            y += bh + pad / 2;
            const float bottom = by + boxH - pad - bh - pad / 2 - bh - pad / 2;
            listRect = {bx + pad, y, boxW - 2 * pad, std::max(rowH, bottom - y)};
            Fill(r, listRect.x, listRect.y, listRect.w, listRect.h, 0xFFFFFF);
            Frame(r, listRect.x, listRect.y, listRect.w, listRect.h, kEdge);
            rows = std::max(1, (int)(listRect.h / rowH));
            if (cursor >= 0) {
                if (cursor < top) top = cursor;
                if (cursor >= top + rows) top = cursor - rows + 1;
            }
            top = std::clamp(top, 0, std::max(0, (int)entries.size() - rows));
            SDL_Rect clip{(int)listRect.x + 1, (int)listRect.y + 1, (int)listRect.w - 2, (int)listRect.h - 2};
            SDL_SetRenderClipRect(r, &clip);
            for (int i = top; i < (int)entries.size() && i < top + rows; ++i) {
                const float ry = listRect.y + 1 + (i - top) * rowH;
                if (selected[i] || i == cursor) Fill(r, listRect.x + 1, ry, listRect.w - 2, rowH, selected[i] ? kSelect : 0xEDEDED);
                DrawText(Font::Normal, entries[i].name + (entries[i].dir ? "/" : ""), listRect.x + pad / 2, ry + (rowH - lh) / 2,
                         entries[i].dir ? kAccent : kInk, listRect.w - pad);
            }
            if (entries.empty()) DrawText(Font::Normal, "(empty)", listRect.x + pad / 2, listRect.y + pad / 2, kMuted);
            SDL_SetRenderClipRect(r, nullptr);
            y = listRect.y + listRect.h + pad / 2;
            const char* label = save ? "Name:" : "Location:";
            const float labelW = TextWidth(Font::Normal, label) + pad / 2;
            DrawText(Font::Normal, label, bx + pad, y + (bh - lh) / 2, kInk);
            const float fx = bx + pad + labelW, fw = boxW - 2 * pad - labelW;
            Fill(r, fx, y, fw, bh, 0xFFFFFF);
            Frame(r, fx, y, fw, bh, kAccent);
            const float tw = DrawText(Font::Normal, field, fx + pad / 2, y + (bh - lh) / 2, kInk, fw - pad);
            if ((SDL_GetTicks() / 530) % 2 == 0) Fill(r, fx + pad / 2 + tw + 1, y + (bh - lh) / 2, std::max(1.0f, lh / 14), lh, kInk);
            y += bh + pad / 2;
            const std::string okText = save ? "Save" : folders ? "Choose" : "Open";
            cancel = {bx + boxW - pad - ButtonWidth("Cancel"), y, ButtonWidth("Cancel"), bh};
            accept = {cancel.x - pad / 2 - ButtonWidth(okText), y, ButtonWidth(okText), bh};
            DrawButton(r, {okText, accept}, true, Inside(mx, my, accept));
            DrawButton(r, {"Cancel", cancel}, false, Inside(mx, my, cancel));
            if (!rq.filters.empty()) {
                const std::string ft = rq.filters[filter].first;
                filterButton = {bx + pad, y, std::min(ButtonWidth(ft), accept.x - bx - 2 * pad), bh};
                DrawButton(r, {ft, filterButton}, false, Inside(mx, my, filterButton));
            }
            SDL_RenderPresent(r);
            dirty = false;
        }
        SDL_Event e;
        const bool got = m.Next(e, 530);
        dirty = true;  // the caret blinks
        if (!got) continue;
        switch (e.type) {
        case SDL_EVENT_TEXT_INPUT:
            field += e.text.text;
            break;
        case SDL_EVENT_KEY_DOWN: {
            const SDL_Keycode k = e.key.key;
            if (k == SDLK_ESCAPE) return std::vector<std::string>{};
            if (k == SDLK_BACKSPACE) {
                if (!field.empty()) {
                    size_t n = field.size() - 1;
                    while (n > 0 && ((unsigned char)field[n] & 0xC0) == 0x80) --n;  // a whole character
                    field.resize(n);
                } else if (dir.has_parent_path() && dir != dir.root_path()) {
                    dir = dir.parent_path();
                    reload();
                }
            } else if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_PAGEUP || k == SDLK_PAGEDOWN || k == SDLK_HOME || k == SDLK_END) {
                if (entries.empty()) break;
                int step = k == SDLK_UP ? -1 : k == SDLK_DOWN ? 1 : k == SDLK_PAGEUP ? -rows : k == SDLK_PAGEDOWN ? rows : k == SDLK_HOME ? -100000 : 100000;
                cursor = std::clamp(cursor + step, 0, (int)entries.size() - 1);
                std::fill(selected.begin(), selected.end(), false);
                selected[cursor] = true;
                if (save && !entries[cursor].dir) field = entries[cursor].name;
            } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                if (field.empty() && cursor >= 0 && entries[cursor].dir && !(folders && (e.key.mod & SDL_KMOD_CTRL))) {
                    enter(cursor);
                    break;
                }
                if (auto p = chosenPaths()) return p;
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            mx = e.motion.x * m.Density(), my = e.motion.y * m.Density();
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            top -= (int)std::round(e.wheel.y * 3);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (e.button.button != SDL_BUTTON_LEFT) break;
            mx = e.button.x * m.Density(), my = e.button.y * m.Density();
            if (!Inside(mx, my, listRect)) break;
            const int row = top + (int)((my - listRect.y - 1) / rowH);
            if (row < 0 || row >= (int)entries.size()) break;
            const uint64_t now = SDL_GetTicks();
            const bool dbl = row == lastClickRow && now - lastClick < 500;
            lastClick = now, lastClickRow = row;
            if (dbl) {
                if (enter(row)) {
                    field.clear();
                    std::fill(selected.begin(), selected.end(), false);
                    selected[row] = true;
                    if (auto p = chosenPaths()) return p;
                }
                lastClickRow = -1;
                break;
            }
            if (rq.many && (SDL_GetModState() & SDL_KMOD_CTRL)) {
                selected[row] = !selected[row];
            } else {
                std::fill(selected.begin(), selected.end(), false);
                selected[row] = true;
            }
            cursor = row;
            if (save && !entries[row].dir) field = entries[row].name;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button != SDL_BUTTON_LEFT) break;
            mx = e.button.x * m.Density(), my = e.button.y * m.Density();
            if (Inside(mx, my, cancel)) return std::vector<std::string>{};
            if (Inside(mx, my, accept))
                if (auto p = chosenPaths()) return p;
            if (Inside(mx, my, upButton) && dir != dir.root_path()) {
                dir = dir.parent_path();
                reload();
            }
            if (!rq.filters.empty() && Inside(mx, my, filterButton)) {
                filter = (filter + 1) % rq.filters.size();
                reload();
            }
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_QUIT:
            return std::vector<std::string>{};
        default: break;
        }
    }
}

}  // namespace lx::platform
