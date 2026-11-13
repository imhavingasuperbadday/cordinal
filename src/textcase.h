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

}  // namespace textcase
