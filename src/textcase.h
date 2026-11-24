// Case folding of wide strings, shared by everything that compares text without regard to case.
//
// There is more than one fold on purpose, because each result is compared only with strings made the
// same way and some are stored (points hashes the Linguistic fold of album names):
//   Fold        per-character towlower, the plain key for names, titles and tag values.
//   Linguistic  the user's locale with linguistic casing (auto-playlist rules and cordinal points).
//   Invariant   locale-independent lowercase (album groups, the browser player's search).
//   Path        the system's file-name folding (CharLowerBuffW), to compare paths the way Windows does.
// All but Invariant take their argument by value, so a temporary is folded in place without a copy.
#pragma once
#include <windows.h>

#include <cwctype>
#include <string>
#include <string_view>

namespace textcase {

inline std::wstring Fold(std::wstring s) {
    // ASCII is by far the commonest text and needs no locale lookup; the rest keeps towlower's result.
    for (wchar_t& c : s) c = c < 0x80 ? (c >= L'A' && c <= L'Z' ? wchar_t(c + (L'a' - L'A')) : c) : (wchar_t)std::towlower(c);
    return s;
}

inline std::wstring Linguistic(std::wstring s) {
    if (!s.empty())
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, s.c_str(), (int)s.size(), s.data(),
                      (int)s.size(), nullptr, nullptr, 0);
    return s;
}

inline std::wstring Invariant(std::wstring_view text) {
    if (text.empty()) return {};
    std::wstring out(text.size(), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), (int)text.size(), out.data(), (int)out.size(),
                      nullptr, nullptr, 0) <= 0) {
        for (size_t i = 0; i < text.size(); ++i) out[i] = (wchar_t)std::towlower(text[i]);
    }
    return out;
}

inline std::wstring Path(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

// Lowercase + Latin diacritic folding for search: "Björk" -> "bjork", "Sigur Rós" -> "sigur ros",
// decomposed "A\u030A" -> "a". Pure ASCII takes a single pass with no table lookups.
inline std::wstring SearchFold(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t c : text) {
        if (c < 0x80) {
            out += (c >= L'A' && c <= L'Z') ? wchar_t(c + (L'a' - L'A')) : c;
            continue;
        }
        // Combining diacritical marks (NFD decomposed accents).
        if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
            (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0xFE20 && c <= 0xFE2F))
            continue;
        if (c >= 0x00C0 && c <= 0x00FF) {
            // Latin-1 Supplement: À..ÿ (excluding × U+00D7 and ÷ U+00F7).
            static constexpr const char* kLatin1[64] = {
                "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
                "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "ss",
                "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
                "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "y",
            };
            if (const char* r = kLatin1[c - 0x00C0]) {
                while (*r) out += (wchar_t)*r++;
                continue;
            }
        } else if (c >= 0x0100 && c <= 0x017F) {
            // Latin Extended-A: Ā..ſ
            static constexpr const char* kExtA[128] = {
                "a","a","a","a","a","a","c","c","c","c","c","c","c","c","d","d",
                "d","d","e","e","e","e","e","e","e","e","e","e","g","g","g","g",
                "g","g","g","g","h","h","h","h","i","i","i","i","i","i","i","i",
                "i","i","ij","ij","j","j","k","k","k","l","l","l","l","l","l","l",
                "l","l","l","n","n","n","n","n","n","n","n","n","o","o","o","o",
                "o","o","oe","oe","r","r","r","r","r","r","s","s","s","s","s","s",
                "s","s","t","t","t","t","t","t","u","u","u","u","u","u","u","u",
                "u","u","u","u","w","w","y","y","y","z","z","z","z","z","z","s",
            };
            for (const char* r = kExtA[c - 0x0100]; *r; ++r) out += (wchar_t)*r;
            continue;
        } else if (c >= 0x1E00 && c <= 0x1EFF) {
            // Common Latin Extended Additional (Vietnamese, Welsh, etc.).
            wchar_t base = 0;
            if (c <= 0x1E01) base = L'a';
            else if (c >= 0x1E0C && c <= 0x1E13) base = L'd';
            else if (c >= 0x1E14 && c <= 0x1E1D) base = L'e';
            else if (c >= 0x1E22 && c <= 0x1E2B) base = L'h';
            else if (c >= 0x1E2C && c <= 0x1E2F) base = L'i';
            else if (c >= 0x1E36 && c <= 0x1E3D) base = L'l';
            else if (c >= 0x1E44 && c <= 0x1E4B) base = L'n';
            else if (c >= 0x1E4C && c <= 0x1E53) base = L'o';
            else if (c >= 0x1E5A && c <= 0x1E69) base = (c < 0x1E60) ? L'r' : L's';
            else if (c >= 0x1E6A && c <= 0x1E71) base = L't';
            else if (c >= 0x1E72 && c <= 0x1E7B) base = L'u';
            else if (c >= 0x1E80 && c <= 0x1E85) base = L'w';
            else if (c >= 0x1E8E && c <= 0x1E8F) base = L'y';
            else if (c >= 0x1E90 && c <= 0x1E95) base = L'z';
            else if (c >= 0x1EA0 && c <= 0x1EB7) base = L'a';
            else if (c >= 0x1EB8 && c <= 0x1EC7) base = L'e';
            else if (c >= 0x1EC8 && c <= 0x1ECB) base = L'i';
            else if (c >= 0x1ECC && c <= 0x1EE3) base = L'o';
            else if (c >= 0x1EE4 && c <= 0x1EF1) base = L'u';
            else if (c >= 0x1EF2 && c <= 0x1EF9) base = L'y';
            if (base) { out += base; continue; }
        }
        out += (wchar_t)std::towlower(c);
    }
    return out;
}

}  // namespace textcase
