// A stand-in fcitx5 for linux/e2e-test.sh ("cordinal-core-test fake-fcitx LOG"): it owns
// org.fcitx.Fcitx5 on the session bus and answers the calls the X11 input method client makes
// (platform/ime.cpp), so the whole path is driven from a real key press to text in a field. The
// letter k starts a composition, "Seek bar", Return commits "Seek bar shows" in its place, and
// every other key is handed back. Every call is written to LOG, one line each.
#pragma once

#include "linux/dbus.h"

#include <poll.h>

#include <cstdio>
#include <string>

namespace ime_fake {

inline int Run(const char* logPath) {
    using lx::dbus::Writer;
    FILE* log = fopen(logPath, "w");
    if (!log) return 2;
    setvbuf(log, nullptr, _IOLBF, 0);
    auto bus = lx::dbus::Bus::OpenUser(5000);
    if (!bus) return fprintf(log, "no session bus\n"), 2;
    constexpr const char* kIc = "/org/freedesktop/portal/inputcontext/1";
    constexpr const char* kIface = "org.fcitx.Fcitx.InputContext1";
    bool composing = false;
    auto preedit = [&](const char* text, int cursor) {
        Writer w;
        Writer::Array a = w.Open(8);
        if (*text) w.Align(8), w.S(text), w.I(8);  // 8: underlined, as fcitx5 marks a composition
        w.Close(a);
        w.I(cursor);
        bus->Signal(kIc, kIface, "UpdateFormattedPreedit", "a(si)i", w);
    };
    bus->onMessage = [&](lx::dbus::Bus& b, const lx::dbus::Message& m) {
        if (m.type != lx::dbus::kCall) return;
        Writer out;
        std::string sig;
        if (m.member == "CreateInputContext") {
            out.O(kIc);
            Writer::Array a = out.Open(1);
            for (int i = 0; i < 16; ++i) out.Y(0);
            out.Close(a);
            sig = "oay";
            fprintf(log, "CreateInputContext\n");
        } else if (m.member == "ProcessKeyEvent" && m.sig == "uuubu") {
            lx::dbus::Reader r = m.Body();
            const uint32_t sym = r.U(), code = r.U(), state = r.U();
            const bool release = r.B();
            fprintf(log, "ProcessKeyEvent %x %u %x %s\n", sym, code, state, release ? "up" : "down");
            bool taken = false;
            if (!release && sym == 'k') {
                composing = taken = true;
                preedit("Seek bar", 8);
            } else if (composing && !release && sym == 0xff0d) {  // Return
                Writer w;
                w.S("Seek bar shows");
                bus->Signal(kIc, kIface, "CommitString", "s", w);
                preedit("", -1);
                composing = false;
                taken = true;
            } else {
                taken = composing;  // a composition keeps every key, releases too
            }
            out.B(taken);
            sig = "b";
        } else if (m.member == "SetCursorRect" && m.sig == "iiii") {
            lx::dbus::Reader r = m.Body();
            const int x = r.I(), y = r.I(), w = r.I(), h = r.I();
            fprintf(log, "SetCursorRect %d %d %d %d\n", x, y, w, h);
        } else {
            fprintf(log, "%s\n", m.member.c_str());
        }
        if (!(m.flags & lx::dbus::kNoReply)) b.Reply(m, sig, out);
    };
    Writer name;
    name.S("org.fcitx.Fcitx5");
    name.U(4);  // DO_NOT_QUEUE
    auto reply = bus->CallSync({"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName"},
                               "su", name, 5000);
    if (!reply || reply->type != lx::dbus::kReturn || reply->Body().U() != 1) return fprintf(log, "name taken\n"), 2;
    fprintf(log, "ready\n");
    for (;;) {
        const int more = bus->Process(64);
        if (more < 0) return 0;
        bus->Flush();
        pollfd p{bus->Fd(), bus->Events(), 0};
        poll(&p, 1, more > 0 ? 0 : bus->TimeoutMs());
    }
}

}  // namespace ime_fake
