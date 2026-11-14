#include "platform/xkb_keymap.h"

#include "platform/platform.h"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cwctype>
#include <locale.h>
#include <wctype.h>

namespace lx::platform {
namespace {

struct Tok {
    enum Kind { End, Ident, Str, Num, KeyName, Punct } kind = End;
    std::string text;
};

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}
    Tok Next() {
        Skip();
        Tok t;
        if (at_ >= s_.size()) return t;
        const char c = s_[at_];
        if (c == '"') {
            size_t end = s_.find('"', at_ + 1);
            if (end == std::string_view::npos) end = s_.size();
            t.kind = Tok::Str;
            t.text = std::string(s_.substr(at_ + 1, end - at_ - 1));
            at_ = std::min(end + 1, s_.size());
        } else if (c == '<') {
            size_t end = s_.find('>', at_ + 1);
            if (end == std::string_view::npos || end - at_ > 64) end = at_;
            t.kind = end == at_ ? Tok::Punct : Tok::KeyName;
            t.text = end == at_ ? "<" : std::string(s_.substr(at_ + 1, end - at_ - 1));
            at_ = end + 1;
        } else if (isalpha((unsigned char)c) || c == '_') {
            size_t b = at_;
            while (at_ < s_.size() && (isalnum((unsigned char)s_[at_]) || s_[at_] == '_')) ++at_;
            t.kind = Tok::Ident;
            t.text = std::string(s_.substr(b, at_ - b));
        } else if (isdigit((unsigned char)c)) {
            size_t b = at_;
            while (at_ < s_.size() && (isalnum((unsigned char)s_[at_]))) ++at_;  // 0x1234 too
            t.kind = Tok::Num;
            t.text = std::string(s_.substr(b, at_ - b));
        } else {
            t.kind = Tok::Punct;
            t.text = std::string(1, c);
            ++at_;
        }
        return t;
    }
    Tok Peek() {
        size_t save = at_;
        Tok t = Next();
        at_ = save;
        return t;
    }

private:
    void Skip() {
        while (at_ < s_.size()) {
            const char c = s_[at_];
            if (isspace((unsigned char)c)) ++at_;
            else if (c == '/' && at_ + 1 < s_.size() && s_[at_ + 1] == '/') at_ = std::min(s_.find('\n', at_), s_.size());
            else if (c == '#') at_ = std::min(s_.find('\n', at_), s_.size());
            else if (c == '/' && at_ + 1 < s_.size() && s_[at_ + 1] == '*') {
                size_t end = s_.find("*/", at_ + 2);
                at_ = end == std::string_view::npos ? s_.size() : end + 2;
            } else break;
        }
    }
    std::string_view s_;
    size_t at_ = 0;
};

bool Is(const Tok& t, const char* p) { return t.text == p && (t.kind == Tok::Punct || t.kind == Tok::Ident); }

// Skips to the end of the current statement: past the ';' at this depth (or the closing '}').
void SkipStatement(Lexer& lx) {
    int depth = 0;
    for (Tok t = lx.Next(); t.kind != Tok::End; t = lx.Next()) {
        if (t.kind != Tok::Punct) continue;
        if (t.text == "{" || t.text == "[" || t.text == "(") ++depth;
        else if (t.text == "}" || t.text == "]" || t.text == ")") {
            if (--depth < 0) return;
        } else if (t.text == ";" && depth == 0) return;
    }
}

constexpr uint32_t kMulti = 0xfffffffeu;  // a level with several keysyms

// Case through C.UTF-8's full tables whatever the process locale is; legacy keysyms stay legacy
// when the other case has one, as xkb_keysym_to_upper/lower keep them.
locale_t Utf8() {
    static locale_t l = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
    return l;
}
uint32_t Recase(uint32_t s, bool upper) {
    if (!s || s == kMulti) return s;
    const uint32_t cp = KeysymToUnicode(s);
    if (!cp) return s;
    const uint32_t other = Utf8() ? (uint32_t)(upper ? towupper_l((wint_t)cp, Utf8()) : towlower_l((wint_t)cp, Utf8()))
                                  : (uint32_t)(upper ? towupper((wint_t)cp) : towlower((wint_t)cp));
    if (other == cp) return s;
    if (s >= 0x01000000) return 0x01000000 + other;
    const uint32_t legacy = KeysymFromUnicode(other);
    return legacy;
}
uint32_t Lower(uint32_t s) { return Recase(s, false); }
uint32_t Upper(uint32_t s) { return Recase(s, true); }
bool IsLower(uint32_t s) { return Upper(s) != s; }
bool IsUpper(uint32_t s) { return Lower(s) != s; }

}  // namespace

bool XkbKeymap::Parse(std::string_view text, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    types_.clear();
    keys_.clear();
    vmodReal_.clear();
    std::map<std::string, uint32_t> codes;       // key name to code
    std::map<std::string, std::string> aliases;  // alias to key name
    std::map<std::string, int> typeIndex;
    std::vector<std::string> vmods;
    // The compatibility section's interprets, as xkbcommon applies them to keys: the most specific
    // first (a keysym before Any; Exactly, AllOf, NoneOf, AnyOf, AnyOfOrNone).
    enum Match { Exactly, AllOf, NoneOf, AnyOf, AnyOfOrNone };
    struct Interp {
        uint32_t sym = 0;  // 0: Any
        Match match = AnyOfOrNone;
        uint32_t mods = 0xff;
        int vmod = -1;
        bool repeat = false, levelOne = false;
    };
    std::vector<Interp> interps;
    Interp interpDefault;
    struct PendingKey {
        std::string name;
        std::vector<std::string> typeNames;  // by group ("" for automatic)
        std::vector<std::vector<uint32_t>> syms;
        uint32_t vmods = 0;
        bool explicitVmods = false, explicitRepeat = false, repeat = false, actions = false;
    };
    std::vector<PendingKey> pending;
    std::vector<std::pair<int, std::vector<std::string>>> modmaps;  // real mod, key names / keysym names

    auto realIndex = [](const std::string& n) -> int {
        static const char* const names[] = {"Shift", "Lock", "Control", "Mod1", "Mod2", "Mod3", "Mod4", "Mod5"};
        for (int i = 0; i < 8; ++i)
            if (n == names[i]) return i;
        return -1;
    };
    auto vmodIndex = [&](const std::string& n) -> int {
        for (size_t i = 0; i < vmods.size(); ++i)
            if (vmods[i] == n) return (int)i;
        return -1;
    };
    auto addVmod = [&](const std::string& n) {
        if (vmodIndex(n) < 0 && vmods.size() < 24) vmods.push_back(n);
    };
    // A modifier expression ("Shift+LevelThree", "none", "all") as real bits and virtual bits << 8.
    auto readMods = [&](Lexer& lx, const char* until) -> uint32_t {
        uint32_t mask = 0;
        for (Tok t = lx.Peek(); t.kind != Tok::End && !Is(t, until) && !Is(t, ";"); t = lx.Peek()) {
            lx.Next();
            if (t.kind != Tok::Ident) continue;
            if (t.text == "all") mask |= 0xffffffffu;
            else if (int r = realIndex(t.text); r >= 0) mask |= 1u << r;
            else if (int v = vmodIndex(t.text); v >= 0) mask |= 1u << (8 + v);
        }
        return mask;
    };
    auto readVmodDecl = [&](Lexer& lx) {
        for (Tok t = lx.Next(); t.kind != Tok::End && !Is(t, ";"); t = lx.Next())
            if (t.kind == Tok::Ident) {
                addVmod(t.text);
                if (Is(lx.Peek(), "=")) {  // "NumLock= Mod2": an explicit mapping, kept as a modmap
                    lx.Next();
                    Tok m = lx.Next();
                    if (int r = realIndex(m.text); r >= 0) modmaps.push_back({-(vmodIndex(t.text) + 1) * 16 - r, {}});
                }
            }
    };
    auto keysymOf = [](const Tok& t) -> uint32_t {
        if (t.kind == Tok::Num) {
            if (t.text.size() == 1) return (uint32_t)t.text[0];  // digits are their own keysyms
            if (t.text.size() > 2 && (t.text[1] == 'x' || t.text[1] == 'X')) return KeysymFromName(t.text);
            return (uint32_t)strtoul(t.text.c_str(), nullptr, 10);
        }
        if (t.kind != Tok::Ident || t.text == "NoSymbol") return 0;
        return KeysymFromName(t.text);
    };

    Lexer lx(text);
    Tok t = lx.Next();
    if (!Is(t, "xkb_keymap")) return fail("not an xkb_keymap");
    while ((t = lx.Next()).kind != Tok::End && !Is(t, "{")) {}
    if (t.kind == Tok::End) return fail("no keymap body");
    int sections = 0;
    for (;;) {
        Tok sec = lx.Next();
        if (sec.kind == Tok::End || Is(sec, "}")) break;
        if (sec.kind != Tok::Ident) continue;
        // The section's name (a string) and its body.
        while ((t = lx.Next()).kind != Tok::End && !Is(t, "{")) {}
        if (t.kind == Tok::End) return fail("unterminated section");
        ++sections;
        const std::string kind = sec.text;
        for (;;) {
            Tok s = lx.Next();
            if (s.kind == Tok::End) return fail("unterminated section body");
            if (Is(s, "}")) {
                if (Is(lx.Peek(), ";")) lx.Next();
                break;
            }
            if (kind == "xkb_keycodes") {
                if (s.kind == Tok::KeyName && Is(lx.Peek(), "=")) {
                    lx.Next();
                    Tok n = lx.Next();
                    if (n.kind == Tok::Num) {
                        unsigned long v = strtoul(n.text.c_str(), nullptr, 0);
                        if (v < 0x10000) codes[s.text] = (uint32_t)v;
                    }
                    SkipStatement(lx);
                } else if (Is(s, "alias")) {
                    Tok a = lx.Next();
                    lx.Next();  // =
                    Tok b = lx.Next();
                    if (a.kind == Tok::KeyName && b.kind == Tok::KeyName) aliases[a.text] = b.text;
                    SkipStatement(lx);
                } else if (!Is(s, ";")) {
                    SkipStatement(lx);
                }
            } else if (kind == "xkb_types") {
                if (Is(s, "virtual_modifiers")) readVmodDecl(lx);
                else if (Is(s, "type")) {
                    Tok name = lx.Next();
                    if (name.kind != Tok::Str || !Is(lx.Next(), "{")) return fail("malformed type");
                    Type type;
                    for (;;) {
                        Tok f = lx.Next();
                        if (f.kind == Tok::End) return fail("unterminated type");
                        if (Is(f, "}")) break;
                        if (Is(f, "modifiers")) {
                            lx.Next();  // =
                            type.mods = readMods(lx, ";");
                            lx.Next();
                        } else if (Is(f, "map")) {
                            lx.Next();  // [
                            Entry e;
                            e.mask = readMods(lx, "]");
                            lx.Next();  // ]
                            lx.Next();  // =
                            Tok lv = lx.Next();
                            std::string l = lv.text;
                            if (l.rfind("Level", 0) == 0 || l.rfind("level", 0) == 0) l = l.substr(5);
                            const long n = strtol(l.c_str(), nullptr, 10);
                            if (n >= 1 && n <= 8) {
                                e.level = (uint32_t)n - 1;
                                type.map.push_back(e);
                                type.levels = std::max(type.levels, e.level + 1);
                            }
                            SkipStatement(lx);
                        } else if (Is(f, "preserve")) {
                            lx.Next();  // [
                            const uint32_t mask = readMods(lx, "]");
                            lx.Next();  // ]
                            lx.Next();  // =
                            const uint32_t keep = readMods(lx, ";");
                            for (Entry& e : type.map)
                                if (e.mask == mask) e.preserve = keep;
                            SkipStatement(lx);
                        } else if (!Is(f, ";")) {
                            SkipStatement(lx);
                        }
                    }
                    if (Is(lx.Peek(), ";")) lx.Next();
                    if (types_.size() < 1024) {
                        typeIndex[name.text] = (int)types_.size();
                        types_.push_back(std::move(type));
                    }
                } else if (!Is(s, ";")) {
                    SkipStatement(lx);
                }
            } else if (kind == "xkb_compatibility" || kind == "xkb_compat") {
                if (Is(s, "virtual_modifiers")) readVmodDecl(lx);
                else if (Is(s, "interpret") && Is(lx.Peek(), ".")) {  // "interpret.repeat= False;": the defaults
                    lx.Next();
                    Tok f = lx.Next();
                    lx.Next();  // =
                    Tok v = lx.Next();
                    if (Is(f, "repeat")) interpDefault.repeat = v.text == "True" || v.text == "true" || v.text == "yes";
                    else if (Is(f, "useModMapMods") || Is(f, "usemodmapmods"))
                        interpDefault.levelOne = v.text == "level1" || v.text == "LevelOne" || v.text == "levelone";
                    SkipStatement(lx);
                } else if (Is(s, "interpret")) {
                    Interp in = interpDefault;
                    Tok sym = lx.Next();
                    in.sym = Is(sym, "Any") ? 0 : keysymOf(sym);
                    if (!in.sym && !Is(sym, "Any")) in.sym = kMulti;  // an unknown keysym matches nothing
                    if (Is(lx.Peek(), "+")) {
                        lx.Next();
                        Tok pred = lx.Next();
                        static const char* const names[] = {"Exactly", "AllOf", "NoneOf", "AnyOf", "AnyOfOrNone"};
                        int m = -1;
                        for (int i = 0; i < 5; ++i)
                            if (pred.text == names[i]) m = i;
                        if (m >= 0 && Is(lx.Peek(), "(")) {
                            lx.Next();
                            in.match = (Match)m;
                            in.mods = readMods(lx, ")") & 0xff;
                            lx.Next();
                        } else if (Is(pred, "Any")) {
                            in.match = AnyOf;
                            in.mods = 0xff;
                        } else {  // a bare modifier expression means Exactly
                            in.match = Exactly;
                            in.mods = 0;
                            if (int r = realIndex(pred.text); r >= 0) in.mods |= 1u << r;
                            else if (Is(pred, "all")) in.mods = 0xff;
                            if (Is(lx.Peek(), "+")) in.mods |= readMods(lx, "{") & 0xff;
                        }
                    }
                    while ((t = lx.Next()).kind != Tok::End && !Is(t, "{")) {}
                    for (;;) {
                        Tok f = lx.Next();
                        if (f.kind == Tok::End) return fail("unterminated interpret");
                        if (Is(f, "}")) break;
                        if (Is(f, "virtualModifier") || Is(f, "virtualmodifier") || Is(f, "virtualMod")) {
                            lx.Next();
                            in.vmod = vmodIndex(lx.Next().text);
                            SkipStatement(lx);
                        } else if (Is(f, "repeat") || Is(f, "useModMapMods") || Is(f, "usemodmapmods")) {
                            lx.Next();
                            Tok v = lx.Next();
                            if (Is(f, "repeat")) in.repeat = v.text == "True" || v.text == "true" || v.text == "yes";
                            else in.levelOne = v.text == "level1" || v.text == "LevelOne" || v.text == "levelone";
                            SkipStatement(lx);
                        } else if (!Is(f, ";")) {
                            SkipStatement(lx);
                        }
                    }
                    if (Is(lx.Peek(), ";")) lx.Next();
                    if (interps.size() < 4096) interps.push_back(in);
                } else if (!Is(s, ";")) {
                    SkipStatement(lx);
                }
            } else if (kind == "xkb_symbols") {
                if (Is(s, "virtual_modifiers")) readVmodDecl(lx);
                else if (Is(s, "key") && lx.Peek().kind == Tok::KeyName) {
                    PendingKey k;
                    k.name = lx.Next().text;
                    if (!Is(lx.Next(), "{")) return fail("malformed key");
                    size_t nextGroup = 0;
                    auto readSyms = [&](std::vector<uint32_t>& out) {  // after '['
                        int depth = 0;
                        for (Tok y = lx.Next(); y.kind != Tok::End; y = lx.Next()) {
                            if (Is(y, "]") && depth == 0) return;
                            if (Is(y, "{")) {  // several keysyms on one level, which type no single keysym
                                int n = 0;
                                uint32_t one = 0;
                                for (Tok z = lx.Next(); z.kind != Tok::End && !Is(z, "}"); z = lx.Next())
                                    if (!Is(z, ",")) one = keysymOf(z), ++n;
                                if (out.size() < 64) out.push_back(n == 1 ? one : n ? kMulti : 0);
                                continue;
                            }
                            if (Is(y, ",")) continue;
                            if (out.size() < 64) out.push_back(keysymOf(y));
                        }
                    };
                    auto groupIndex = [&](size_t& g) {  // "[Group2]" or "[2]" after a field name
                        g = 0;
                        if (!Is(lx.Peek(), "[")) return;
                        lx.Next();
                        Tok gt = lx.Next();
                        std::string v = gt.text;
                        if (v.rfind("Group", 0) == 0 || v.rfind("group", 0) == 0) v = v.substr(5);
                        long n = strtol(v.c_str(), nullptr, 10);
                        g = n >= 1 && n <= 4 ? (size_t)n - 1 : 0;
                        lx.Next();  // ]
                    };
                    for (;;) {
                        Tok f = lx.Next();
                        if (f.kind == Tok::End) return fail("unterminated key");
                        if (Is(f, "}")) break;
                        if (Is(f, "[")) {
                            if (k.syms.size() <= nextGroup) k.syms.resize(nextGroup + 1);
                            readSyms(k.syms[nextGroup]);
                            ++nextGroup;
                        } else if (Is(f, "symbols")) {
                            size_t g;
                            groupIndex(g);
                            lx.Next();  // =
                            if (Is(lx.Next(), "[")) {
                                if (k.syms.size() <= g) k.syms.resize(g + 1);
                                k.syms[g].clear();
                                readSyms(k.syms[g]);
                                nextGroup = g + 1;
                            }
                        } else if (Is(f, "type")) {
                            size_t g;
                            const bool all = !Is(lx.Peek(), "[");
                            groupIndex(g);
                            lx.Next();  // =
                            Tok n = lx.Next();
                            if (all) {
                                k.typeNames.assign(4, n.text);
                            } else {
                                if (k.typeNames.size() <= g) k.typeNames.resize(g + 1);
                                k.typeNames[g] = n.text;
                            }
                        } else if (Is(f, "virtualMods") || Is(f, "vmods") || Is(f, "virtualmods")) {
                            lx.Next();
                            k.vmods = readMods(lx, ",") >> 8;
                            k.explicitVmods = true;
                        } else if (Is(f, "repeat")) {
                            lx.Next();
                            Tok v = lx.Next();
                            k.repeat = !(v.text == "False" || v.text == "false" || v.text == "No" || v.text == "no");
                            k.explicitRepeat = true;
                        } else if (Is(f, "actions")) {
                            k.actions = true;
                            size_t g;
                            groupIndex(g);
                            lx.Next();  // =
                            if (Is(lx.Next(), "[")) {
                                int depth = 0;
                                for (Tok y = lx.Next(); y.kind != Tok::End; y = lx.Next()) {
                                    if (Is(y, "(") || Is(y, "[")) ++depth;
                                    else if (Is(y, ")")) --depth;
                                    else if (Is(y, "]")) {
                                        if (depth == 0) break;
                                        --depth;
                                    }
                                }
                            }
                        }
                        // Anything else, and the commas between fields, is skipped token by token.
                    }
                    if (Is(lx.Peek(), ";")) lx.Next();
                    if (pending.size() < 4096) pending.push_back(std::move(k));
                } else if (Is(s, "modifier_map") || Is(s, "modmap") || Is(s, "mod_map")) {
                    Tok m = lx.Next();
                    const int r = realIndex(m.text);
                    std::vector<std::string> list;
                    if (Is(lx.Next(), "{")) {
                        for (Tok y = lx.Next(); y.kind != Tok::End && !Is(y, "}"); y = lx.Next()) {
                            if (y.kind == Tok::KeyName) list.push_back("<" + y.text);
                            else if (y.kind == Tok::Ident || y.kind == Tok::Num) list.push_back(y.text);
                        }
                    }
                    if (Is(lx.Peek(), ";")) lx.Next();
                    if (r >= 0) modmaps.push_back({r, std::move(list)});
                } else if (!Is(s, ";")) {
                    SkipStatement(lx);
                }
            } else if (!Is(s, ";")) {
                SkipStatement(lx);
            }
        }
    }
    if (!sections || codes.empty() || pending.empty()) return fail("no keycodes or symbols");

    auto codeOf = [&](std::string name) -> int64_t {
        for (int i = 0; i < 4; ++i) {
            if (auto it = codes.find(name); it != codes.end()) return it->second;
            auto a = aliases.find(name);
            if (a == aliases.end()) break;
            name = a->second;
        }
        return -1;
    };
    auto typeFor = [&](const std::string& name, const std::vector<uint32_t>& syms) -> int {
        if (!name.empty())
            if (auto it = typeIndex.find(name); it != typeIndex.end()) return it->second;
        // xkbcommon's automatic types.
        size_t width = syms.size();
        while (width > 1 && !syms[width - 1]) --width;
        const uint32_t a = width > 0 ? syms[0] : 0, b = width > 1 ? syms[1] : 0;
        const char* pick;
        if (width <= 1) pick = "ONE_LEVEL";
        else if (width == 2) pick = IsLower(a) && IsUpper(b) ? "ALPHABETIC" : IsKeypadKeysym(a) || IsKeypadKeysym(b) ? "KEYPAD" : "TWO_LEVEL";
        else if (IsLower(a) && IsUpper(b))
            pick = width == 4 && IsLower(syms[2]) && IsUpper(syms[3]) ? "FOUR_LEVEL_ALPHABETIC" : "FOUR_LEVEL_SEMIALPHABETIC";
        else pick = IsKeypadKeysym(a) || IsKeypadKeysym(b) ? "FOUR_LEVEL_KEYPAD" : "FOUR_LEVEL";
        if (auto it = typeIndex.find(pick); it != typeIndex.end()) return it->second;
        return -1;
    };

    // Each key's real modifiers, then the virtual modifiers' mapping from the keys whose keysyms
    // the interprets name (and the keys' own virtualMods), as xkbcommon derives it.
    std::map<uint32_t, uint32_t> keyMods;
    for (const auto& [r, list] : modmaps) {
        if (r < 0) continue;  // an explicit "Vmod= ModN", applied below
        for (const std::string& item : list) {
            if (item[0] == '<') {
                if (int64_t c = codeOf(item.substr(1)); c >= 0) keyMods[(uint32_t)c] |= 1u << r;
                continue;
            }
            const uint32_t sym = KeysymFromName(item);
            for (const PendingKey& k : pending)
                if (!k.syms.empty() && std::find(k.syms[0].begin(), k.syms[0].end(), sym) != k.syms[0].end())
                    if (int64_t c = codeOf(k.name); c >= 0) {
                        keyMods[(uint32_t)c] |= 1u << r;
                        break;
                    }
        }
    }
    std::stable_sort(interps.begin(), interps.end(), [](const Interp& a, const Interp& b) {
        return std::make_pair(a.sym == 0, (int)a.match) < std::make_pair(b.sym == 0, (int)b.match);
    });
    // xkbcommon's FindInterpForKey: null for a level without keysyms, the default (which repeats)
    // when no interpret matches.
    static const Interp kDefault{0, AnyOfOrNone, 0, -1, true, false};
    auto findInterp = [&](uint32_t sym, uint32_t modmap, uint32_t level) -> const Interp* {
        if (!sym) return nullptr;
        for (const Interp& in : interps) {
            if (in.sym && (sym == kMulti || in.sym != sym)) continue;
            const uint32_t mods = in.levelOne && level ? 0 : modmap;
            bool found = false;
            switch (in.match) {
            case NoneOf: found = !(in.mods & mods); break;
            case AnyOfOrNone: found = !mods || (in.mods & mods); break;
            case AnyOf: found = in.mods & mods; break;
            case AllOf: found = (in.mods & mods) == in.mods; break;
            case Exactly: found = in.mods == mods; break;
            }
            if (found) return &in;
        }
        return &kDefault;
    };
    vmodReal_.assign(vmods.size(), 0);
    std::map<uint32_t, bool> repeats;
    for (PendingKey& k : pending) {
        const int64_t c = codeOf(k.name);
        if (c < 0) continue;
        const uint32_t modmap = keyMods.count((uint32_t)c) ? keyMods[(uint32_t)c] : 0;
        bool repeat = k.explicitRepeat ? k.repeat : false;
        uint32_t vm = 0;
        if (!k.actions)
            for (size_t g = 0; g < k.syms.size(); ++g) {
                const int ty = typeFor(g < k.typeNames.size() ? k.typeNames[g] : "", k.syms[g]);
                const size_t levels = ty >= 0 ? std::min<size_t>(types_[ty].levels, k.syms[g].size()) : std::min<size_t>(1, k.syms[g].size());
                for (size_t l = 0; l < levels; ++l) {
                    const Interp* in = findInterp(k.syms[g][l], modmap, (uint32_t)l);
                    if (!in) continue;
                    if (g == 0 && l == 0 && !k.explicitRepeat && in->repeat) repeat = true;
                    if ((g == 0 && l == 0) || !in->levelOne)
                        if (in->vmod >= 0) vm |= 1u << in->vmod;
                }
            }
        if (k.explicitVmods) vm = k.vmods;
        repeats[(uint32_t)c] = repeat;
        for (size_t v = 0; v < vmods.size(); ++v)
            if (vm & (1u << v)) vmodReal_[v] |= modmap;
    }
    for (const auto& [r, list] : modmaps)
        if (r < 0) {
            const int code = -r, v = code / 16 - 1, real = code % 16;
            if (v >= 0 && v < (int)vmods.size() && real < 8) vmodReal_[v] |= 1u << real;
        }
    // Unmapped virtual modifiers keep a bit of their own, which only a compositor sending virtual
    // bits can set.
    for (size_t v = 0; v < vmods.size(); ++v)
        if (!vmodReal_[v]) vmodReal_[v] = 1u << (8 + v);
    for (Type& ty : types_) {
        ty.mods = Normalize(ty.mods);
        for (Entry& e : ty.map) e.mask = Normalize(e.mask);
    }
    auto vmodMask = [&](const char* name) {
        int v = vmodIndex(name);
        return v >= 0 && vmodReal_[v] < 0x100 ? vmodReal_[v] : 0u;
    };
    alt = vmodMask("Alt");
    if (!alt) alt = 1u << 3;  // Mod1
    super = vmodMask("Super");
    if (!super) super = 1u << 6;  // Mod4
    numLock = vmodMask("NumLock");
    if (!numLock) numLock = 1u << 4;  // Mod2
    level3 = vmodMask("LevelThree");

    for (const PendingKey& k : pending) {
        const int64_t c = codeOf(k.name);
        if (c < 0) continue;
        Key key;
        key.repeat = repeats[(uint32_t)c];
        for (size_t g = 0; g < k.syms.size(); ++g) {
            Group group;
            group.syms = k.syms[g];
            group.type = typeFor(g < k.typeNames.size() ? k.typeNames[g] : "", group.syms);
            key.groups.push_back(std::move(group));
        }
        // Groups left empty borrow nothing; trailing empty ones go.
        while (!key.groups.empty() && key.groups.back().syms.empty()) key.groups.pop_back();
        if (!key.groups.empty()) keys_[(uint32_t)c] = std::move(key);
    }
    if (keys_.empty()) return fail("no keys with symbols");
    return true;
}

uint32_t XkbKeymap::Normalize(uint32_t mods) const {
    if (mods == 0xffffffffu) return 0xff;
    uint32_t out = mods & 0xff;
    for (size_t v = 0; v < vmodReal_.size(); ++v)
        if (mods & (1u << (8 + v))) out |= vmodReal_[v];
    return out;
}

uint32_t XkbKeymap::LevelKeysym(uint32_t code, uint32_t group, uint32_t level) const {
    auto it = keys_.find(code);
    if (it == keys_.end() || it->second.groups.empty()) return 0;
    const Group& g = it->second.groups[group % it->second.groups.size()];
    return level < g.syms.size() && g.syms[level] != kMulti ? g.syms[level] : 0;
}

uint32_t XkbKeymap::Keysym(uint32_t code, uint32_t mods, uint32_t group) const {
    auto it = keys_.find(code);
    if (it == keys_.end() || it->second.groups.empty()) return 0;
    const Group& g = it->second.groups[group % it->second.groups.size()];
    mods = Normalize(mods);
    uint32_t level = 0, consumed = 0;
    if (g.type >= 0) {
        const Type& ty = types_[g.type];
        consumed = ty.mods;
        const uint32_t m = mods & ty.mods;
        for (const Entry& e : ty.map)
            if (e.mask == m) {
                level = e.level;
                consumed &= ~e.preserve;
                break;
            }
    }
    uint32_t sym = level < g.syms.size() ? g.syms[level] : 0;
    if (sym == kMulti) return 0;
    if ((mods & lock) && !(consumed & lock)) sym = Upper(sym);
    return sym;
}

bool XkbKeymap::Repeats(uint32_t code) const {
    auto it = keys_.find(code);
    return it == keys_.end() || it->second.repeat;
}

}  // namespace lx::platform
