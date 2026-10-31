// Tag-writing entry points and the low-risk file primitives shared by the per-format writers:
// dispatch by extension and whole-file replacement (atomic via ReplaceFileW). See tagwriter.h
// for the contract that a failed write leaves the original untouched.
#include "tagwriter.h"

#include "archive.h"
#include "library.h"

#include <algorithm>
#include <atomic>

bool TagEdit::Empty() const {
    return !title && !artist && !album && !albumArtist && !genre && !date && !composer && !comment && !grouping &&
           !label && !lyrics && !track && !trackTotal && !disc && !discTotal && !bpm && !compilation && !cover &&
           !rgTrackGain && !rgTrackPeak && !rgAlbumGain && !rgAlbumPeak && !r128TrackGain && !r128AlbumGain &&
           !rating && !playCount;
}

bool CanWriteTags(const std::wstring& path) {
    if (IsSubtrack(path)) return false;  // a CUE track's tags come from its sheet
    if (IsArchiveMember(path)) return false;  // a song inside a ZIP has no file to rewrite
    std::wstring ext = ExtOf(path);
    return ext == L"mp3" || ext == L"flac" || ext == L"ogg" || ext == L"oga" || ext == L"opus" || ext == L"m4a" ||
           ext == L"m4b" || ext == L"mp4" || ext == L"cord";
}

bool WriteTags(const std::wstring& path, const TagEdit& edit, std::wstring& error) {
    if (edit.Empty()) return true;
    if (IsArchiveMember(path)) {
        error = L"Tags of a song inside a ZIP archive can't be written.";
        return false;
    }
    std::wstring ext = ExtOf(path);
    if (ext == L"mp3") return WriteTagsMp3(path, edit, error);
    if (ext == L"flac") return WriteTagsFlac(path, edit, error);
    if (ext == L"ogg" || ext == L"oga" || ext == L"opus") return WriteTagsOgg(path, edit, error);
    if (ext == L"m4a" || ext == L"m4b" || ext == L"mp4") return WriteTagsMp4(path, edit, error);
    if (ext == L"cord") return WriteTagsCord(path, edit, error);
    error = L"Writing tags isn't supported for ." + ext + L" files.";
    return false;
}

// Loads the whole file into memory. Refuses files 2 GB and up: every writer that reads through
// this builds its result in memory, so a larger file is out of scope rather than a failed alloc.
bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

namespace {
std::wstring LastErrorText(const wchar_t* what) {
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0, buf,
                   (DWORD)std::size(buf), nullptr);
    std::wstring text = std::wstring(what) + L": " + buf;
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
    return text;
}
}  // namespace

bool ReplaceFileBytes(const std::wstring& path, const std::string& data, std::wstring& error) {
    std::wstring tmp = path + L".cordinal-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = LastErrorText(L"Couldn't create a temporary file");
        return false;
    }
    bool ok = true;
    for (size_t pos = 0; ok && pos < data.size();) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - pos, 1 << 24), written = 0;
        ok = WriteFile(h, data.data() + pos, chunk, &written, nullptr) && written == chunk;
        pos += chunk;
    }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        error = LastErrorText(L"Couldn't write the file");
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        error = LastErrorText(L"Couldn't replace the file (is it open in another program?)");
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}
