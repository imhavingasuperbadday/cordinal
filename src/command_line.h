// Windows command-line quoting shared by the diagnostic forwarder and test tools. QuoteArg
// reproduces the rules CommandLineToArgvW reverses, so a rebuilt command line parses back into the
// same arguments.
#pragma once
#include <string>

// Wraps `arg` in double quotes, escaping embedded quotes. A run of backslashes is doubled only
// where it abuts a quote (including the closing one); elsewhere the backslashes are literal.
inline std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'\"') out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'\"';
}

// Explorer's folder verbs substitute a path into `"<exe>" "%1"`. A path that ends in a backslash
// (a drive root, or a path the user typed that way) turns the closing quote into an escaped one,
// so CommandLineToArgvW hands back `C:\Music"` instead of `C:\Music\`. Undo exactly that shape: the
// stray quote goes, and a bare drive gets its root back. Windows file names cannot contain a
// quote, so no real path is harmed.
inline std::wstring RepairTrailingQuote(std::wstring arg) {
    if (arg.size() < 2 || arg.back() != L'\"') return arg;
    arg.pop_back();
    if (arg.size() == 2 && arg[1] == L':') arg += L'\\';
    return arg;
}
