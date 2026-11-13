// Album titles for editions (album_editions_internal.h): the edition cues a title carries ("Deluxe Edition",
// "(Live)", "[Remastered 2011]"), the base title without them, and the normalized keys albums are matched by.
#include "album_editions_internal.h"

#include "textcase.h"

#include <cwctype>

namespace albumed {

std::wstring Trim(std::wstring_view text) {
    size_t first = 0, last = text.size();
    while (first < last && iswspace(text[first])) ++first;
    while (last > first && iswspace(text[last - 1])) --last;
    return std::wstring(text.substr(first, last - first));
}

bool HasWord(const std::wstring& text, std::wstring_view word) {
    for (size_t at = text.find(word); at != std::wstring::npos; at = text.find(word, at + 1)) {
        bool left = at == 0 || !iswalnum(text[at - 1]);
        size_t end = at + word.size();
        bool right = end >= text.size() || !iswalnum(text[end]);
        if (left && right) return true;
    }
    return false;
}

bool Contains(const std::wstring& text, std::wstring_view part) { return text.find(part) != std::wstring::npos; }

int KindRank(EditionKind kind) {
    switch (kind) {
        case EditionKind::Deluxe: return 12;
        case EditionKind::Expanded: return 11;
        case EditionKind::Anniversary: return 10;
        case EditionKind::Remaster: return 9;
        case EditionKind::AlternateMix: return 8;
        case EditionKind::Instrumental: return 7;
        case EditionKind::Drumless: return 7;
        case EditionKind::Reissue: return 6;
        case EditionKind::Special: return 5;
        case EditionKind::Bonus: return 4;
        case EditionKind::Regional: return 3;
        case EditionKind::Mono:
        case EditionKind::Stereo: return 2;
        case EditionKind::Other: return 1;
        default: return 0;
    }
}

void AddCue(EditionText& info, EditionKind kind) {
    ++info.cues;
    if (KindRank(kind) > KindRank(info.kind)) info.kind = kind;
}

// Whether a title segment contains an edition marker, and which one. A segment that only says
// "live" is a category, not an edition cue: live and studio albums must not collapse together.
bool ParseCue(const std::wstring& segment, EditionText& info) {
    std::wstring s = Trim(segment);
    if (s.empty()) return false;
    float year = 0;
    (void)year;
    bool found = false;
    if (Contains(s, L"drumless")) { AddCue(info, EditionKind::Drumless); found = true; }
    if (Contains(s, L"instrumental")) { AddCue(info, EditionKind::Instrumental); found = true; }
    if (Contains(s, L"alternate mix") || Contains(s, L"alternative mix") || Contains(s, L"alt. mix") ||
        Contains(s, L"alt mix") || Contains(s, L"alternate version")) {
        AddCue(info, EditionKind::AlternateMix);
        found = true;
    }
    if (Contains(s, L"deluxe")) { AddCue(info, EditionKind::Deluxe); found = true; }
    if (Contains(s, L"expanded")) { AddCue(info, EditionKind::Expanded); found = true; }
    if (Contains(s, L"anniversary")) { AddCue(info, EditionKind::Anniversary); found = true; }
    if (Contains(s, L"remaster") || Contains(s, L"re-master") || Contains(s, L"remastered")) {
        AddCue(info, EditionKind::Remaster);
        found = true;
    }
    if (Contains(s, L"reissue") || Contains(s, L"re-issue")) { AddCue(info, EditionKind::Reissue); found = true; }
    if (Contains(s, L"special edition") || Contains(s, L"limited edition") || Contains(s, L"collector")) {
        AddCue(info, EditionKind::Special);
        found = true;
    }
    if (Contains(s, L"bonus")) { AddCue(info, EditionKind::Bonus); found = true; }
    if (Contains(s, L"japanese") || Contains(s, L"japan edition") || Contains(s, L"uk edition") ||
        Contains(s, L"u.s. edition") || Contains(s, L"us edition") || Contains(s, L"european") ||
        Contains(s, L"regional") || Contains(s, L"import edition") || Contains(s, L"tour edition")) {
        AddCue(info, EditionKind::Regional);
        found = true;
    }
    if (HasWord(s, L"mono")) { AddCue(info, EditionKind::Mono); found = true; }
    if (HasWord(s, L"stereo")) { AddCue(info, EditionKind::Stereo); found = true; }
    // "2011 remaster" and "30th anniversary edition" are covered above; a bare year is not a cue.
    return found;
}

void FindCategories(const std::wstring& lower, EditionText& info) {
    info.live = HasWord(lower, L"live");
    info.tribute = HasWord(lower, L"tribute") || HasWord(lower, L"karaoke") || HasWord(lower, L"covers");
    info.karaoke = HasWord(lower, L"karaoke");
    info.soundtrack = HasWord(lower, L"soundtrack") || HasWord(lower, L"ost") || HasWord(lower, L"score") ||
                      HasWord(lower, L"motion picture") || HasWord(lower, L"original cast");
    info.rerecord = Contains(lower, L"re-record") || Contains(lower, L"rerecord") || Contains(lower, L"re-recorded") ||
                    Contains(lower, L"rerecorded") || Contains(lower, L"new recording");
}

bool IsSeparator(wchar_t c) { return iswspace(c) || c == L'-' || c == L'\x2013' || c == L'\x2014' || c == L':' || c == L'/'; }

std::wstring RemoveCueBrackets(const std::wstring& text, EditionText& info) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        wchar_t open = text[i];
        wchar_t close = open == L'(' ? L')' : open == L'[' ? L']' : open == L'{' ? L'}' : 0;
        if (!close) {
            out += text[i++];
            continue;
        }
        size_t end = text.find(close, i + 1);
        if (end == std::wstring::npos) {
            out.append(text.substr(i));
            break;
        }
        std::wstring inside = text.substr(i + 1, end - i - 1);
        EditionText probe = info;
        if (ParseCue(textcase::Invariant(inside), probe)) {
            info = probe;
            i = end + 1;
        } else {
            out.append(text.substr(i, end - i + 1));
            i = end + 1;
        }
    }
    return out;
}

std::wstring RemoveTrailingSegments(const std::wstring& text, EditionText& info) {
    std::wstring out = text;
    for (int pass = 0; pass < 6; ++pass) {
        size_t cut = std::wstring::npos;
        for (size_t i = out.size(); i-- > 1;) {
            if (IsSeparator(out[i]) && IsSeparator(out[i - 1])) {
                std::wstring tail = Trim(out.substr(i + 1));
                EditionText probe = info;
                if (!tail.empty() && ParseCue(textcase::Invariant(tail), probe)) {
                    info = probe;
                    cut = i - 1;
                    break;
                }
            }
        }
        if (cut == std::wstring::npos) break;
        out = Trim(out.substr(0, cut));
    }
    // A marker can also run straight into the title ("Album Deluxe Edition"). Only a trailing
    // phrase counts, so "The Remastered Album" keeps its name.
    static const wchar_t* const suffixes[] = {
        L" deluxe edition", L" deluxe", L" expanded edition", L" expanded", L" anniversary edition", L" anniversary",
        L" remastered edition", L" remastered", L" remaster", L" special edition", L" limited edition",
        L" reissue", L" re-issue", L" bonus tracks", L" bonus track", L" japanese edition", L" instrumental",
        L" instrumentals", L" drumless", L" alternate mix", L" alternative mix", L" mono", L" stereo",
    };
    for (int pass = 0; pass < 4; ++pass) {
        std::wstring lower = textcase::Invariant(out);
        bool removed = false;
        for (const wchar_t* suffix : suffixes) {
            size_t n = wcslen(suffix);
            if (lower.size() > n && lower.compare(lower.size() - n, n, suffix) == 0) {
                EditionText probe = info;
                if (ParseCue(textcase::Invariant(out.substr(out.size() - n)), probe)) {
                    info = probe;
                    out = Trim(out.substr(0, out.size() - n));
                    removed = true;
                    break;
                }
            }
        }
        if (!removed) break;
    }
    return out;
}

EditionText SplitEditionTitle(std::string_view album) {
    EditionText info;
    std::wstring original = FromUtf8(album);
    std::wstring lower = textcase::Invariant(original);
    FindCategories(lower, info);
    std::wstring base = RemoveCueBrackets(original, info);
    base = RemoveTrailingSegments(base, info);
    base = Trim(base);
    if (base.empty()) base = Trim(original);
    info.base = base;
    return info;
}

std::string NormalizeWide(const std::wstring& text) {
    std::wstring lower = textcase::Invariant(text);
    std::wstring out;
    out.reserve(lower.size());
    bool space = false;
    for (wchar_t c : lower) {
        if (iswalnum(c)) {
            out += c;
            space = false;
        } else if (c == L'\'' || c == L'\x2019') {
            // Apostrophes separate words in a search; dropping them would join "don't" to "dont".
            if (!out.empty() && out.back() != L' ') out += L'\'';
            space = false;
        } else if (!space) {
            out += L' ';
            space = true;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (out.starts_with(L"the ")) out.erase(0, 4);
    return ToUtf8(out);
}

std::string AlbumKey(const std::string& artist, const std::string& album, const std::string& date) {
    return NormalizeAlbumText(artist) + '\x1f' + NormalizeAlbumText(album) + '\x1f' + NormalizeAlbumText(date);
}

}  // namespace albumed
