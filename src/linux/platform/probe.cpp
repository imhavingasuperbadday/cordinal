// cordinal-platform-probe: a window on the platform layer that prints every event it gets, one per
// line, and takes commands on stdin, so tools/platform-gui-test.sh can drive it with xdotool or
// wtype and check what arrives (keys, text, buttons, wheel, scale, clipboard, drops, close).
//
//   clip TEXT      SDL_SetClipboardText          get       prints the clipboard ("clip: ...")
//   types          prints the clipboard's types  title T   sets the title
//   size W H       resizes                       shape     cuts the window to a circle (unshape undoes)
//   quit
#include "platform/sdl_api.h"
#include "text.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace lx {
// The probe draws no text; the in-app dialogs' fallback is off without a line height.
float DrawText(Font, const std::string&, float, float, uint32_t, float, int) { return 0; }
float TextWidth(Font, const std::string&) { return 0; }
float LineHeight(Font) { return 0; }
}  // namespace lx

__attribute__((format(printf, 1, 2))) static void Print(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return Print("init failed: %s", SDL_GetError()), 1;
    SDL_Window* w = SDL_CreateWindow(argc > 1 ? argv[1] : "cordinal-probe", 400, 300, SDL_WINDOW_RESIZABLE);
    if (!w) return Print("window failed: %s", SDL_GetError()), 1;
    SDL_Renderer* r = SDL_CreateRenderer(w, nullptr);
    SDL_StartTextInput(w);
    Print("ready driver=%s scale=%.3f", SDL_GetCurrentVideoDriver(), SDL_GetWindowDisplayScale(w));
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    std::string line;
    bool quit = false, redraw = true;
    int frame = 0;
    while (!quit) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 20)) do {
            switch (e.type) {
            case SDL_EVENT_QUIT: Print("quit"); quit = true; break;
            case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
                Print("key %s sc=%d key=0x%x name=%s mod=0x%x repeat=%d", e.type == SDL_EVENT_KEY_DOWN ? "down" : "up",
                      (int)e.key.scancode, (unsigned)e.key.key, SDL_GetKeyName(e.key.key), (unsigned)e.key.mod, (int)e.key.repeat);
                break;
            case SDL_EVENT_TEXT_INPUT: Print("text [%s]", e.text.text); break;
            case SDL_EVENT_MOUSE_MOTION: Print("motion %.0f %.0f", e.motion.x, e.motion.y); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
                Print("button %s %d clicks=%d at %.0f %.0f", e.button.down ? "down" : "up", e.button.button, e.button.clicks, e.button.x, e.button.y);
                break;
            case SDL_EVENT_MOUSE_WHEEL: Print("wheel %.2f %.2f", e.wheel.x, e.wheel.y); break;
            case SDL_EVENT_WINDOW_RESIZED: Print("resized %d %d", e.window.data1, e.window.data2); redraw = true; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: Print("pixels %d %d", e.window.data1, e.window.data2); redraw = true; break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: Print("scale %.3f", SDL_GetWindowDisplayScale(w)); break;
            case SDL_EVENT_WINDOW_EXPOSED: redraw = true; break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED: Print("focus in"); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: Print("focus out"); break;
            case SDL_EVENT_WINDOW_MOUSE_ENTER: Print("enter"); break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE: Print("leave"); break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: Print("close requested"); quit = true; break;
            case SDL_EVENT_DROP_FILE: Print("drop file [%s] at %.0f %.0f", e.drop.data, e.drop.x, e.drop.y); break;
            case SDL_EVENT_DROP_TEXT: Print("drop text [%s]", e.drop.data); break;
            default: break;
            }
        } while (SDL_PollEvent(&e));
        char buf[4096];
        ssize_t n;
        while ((n = read(0, buf, sizeof buf)) > 0) line.append(buf, (size_t)n);
        for (size_t nl; (nl = line.find('\n')) != std::string::npos;) {
            std::string cmd = line.substr(0, nl);
            line.erase(0, nl + 1);
            if (cmd.rfind("clip ", 0) == 0) Print("setclip %d", (int)SDL_SetClipboardText(cmd.c_str() + 5));
            else if (cmd == "get") {
                char* c = SDL_GetClipboardText();
                Print("clip: [%s]", c);
                SDL_free(c);
            } else if (cmd == "types") {
                std::string out;
                for (const char* t : {"text/plain;charset=utf-8", "text/uri-list", "image/png"})
                    out += std::string(t) + "=" + (SDL_HasClipboardData(t) ? "1 " : "0 ");
                Print("types %s", out.c_str());
            } else if (cmd.rfind("title ", 0) == 0) SDL_SetWindowTitle(w, cmd.c_str() + 6);
            else if (cmd.rfind("size ", 0) == 0) {
                int sw = 0, sh = 0;
                sscanf(cmd.c_str() + 5, "%d %d", &sw, &sh);
                SDL_SetWindowSize(w, sw, sh);
            } else if (cmd == "shape" || cmd == "unshape") {
                SDL_Surface* s = cmd == "shape" ? SDL_CreateSurface(200, 200, SDL_PIXELFORMAT_ARGB8888) : nullptr;
                if (s)
                    for (int y = 0; y < 200; ++y)
                        for (int x = 0; x < 200; ++x)
                            ((uint32_t*)((char*)s->pixels + y * s->pitch))[x] = std::hypot(x - 99.5, y - 99.5) < 100 ? 0xffffffffu : 0;
                Print("shape %d", (int)SDL_SetWindowShape(w, s));
                if (s) SDL_DestroySurface(s);
                redraw = true;
            } else if (cmd == "quit") quit = true;
            Print("done %s", cmd.c_str());
        }
        if (redraw) {
            // A fixed picture: a dark background, a red square at 10,10 (20 px), a green band at the bottom.
            int pw, ph;
            SDL_GetWindowSizeInPixels(w, &pw, &ph);
            SDL_SetRenderDrawColor(r, 30, 30, 40, 255);
            SDL_RenderClear(r);
            SDL_SetRenderDrawColor(r, 220, 30, 30, 255);
            SDL_FRect sq{10, 10, 20, 20};
            SDL_RenderFillRect(r, &sq);
            SDL_SetRenderDrawColor(r, 30, 200, 60, 255);
            SDL_FRect band{0, (float)ph - 10, (float)pw, 10};
            SDL_RenderFillRect(r, &band);
            SDL_RenderPresent(r);
            Print("frame %d %dx%d", ++frame, pw, ph);
            redraw = false;
        }
    }
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
