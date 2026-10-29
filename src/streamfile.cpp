// FileSource/FileRewrite implementation behind streamfile.h: positional reads of a file's ranges,
// and a temp-file rewrite of one that atomically replaces the original on Commit. All I/O uses a
// fixed 64 KiB buffer so a song is never held whole in memory.
#include "streamfile.h"

#include <algorithm>
#include <atomic>
#include <iterator>

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

FileSource::FileSource(const std::wstring& path) {
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER size{};
    if (Valid() && GetFileSizeEx(handle_, &size) && size.QuadPart >= 0) size_ = (uint64_t)size.QuadPart;
    else Close();
}

void FileSource::Close() {
    if (Valid()) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
}

// Reads exactly `bytes` bytes starting at `offset`, or returns false. A request that runs past the
// end of the file fails without reading.
bool FileSource::Read(uint64_t offset, size_t bytes, char* out) {
    if (!Valid() || offset > size_ || bytes > size_ - offset) return false;
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(handle_, at, nullptr, FILE_BEGIN)) return false;
    for (size_t done = 0; done < bytes;) {
        DWORD chunk = (DWORD)std::min<size_t>(bytes - done, 64 * 1024), got = 0;
        if (!ReadFile(handle_, out + done, chunk, &got, nullptr) || got != chunk) return false;
        done += got;
    }
    return true;
}

bool FileSource::Read(uint64_t offset, size_t bytes, std::string& out) {
    if (!Valid() || offset > size_ || bytes > size_ - offset) return false;
    out.resize(bytes);
    return Read(offset, bytes, out.data());
}

FileRewrite::FileRewrite(const std::wstring& path, FileSource& source, std::wstring& error)
    : path_(path), source_(source), error_(error) {
    static std::atomic<unsigned> serial{0};
    temp_ = path + L".cordinal-tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial);
    handle_ = CreateFileW(temp_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    ok_ = handle_ != INVALID_HANDLE_VALUE;
    if (!ok_) error_ = LastErrorText(L"Couldn't create a temporary file");
}

FileRewrite::~FileRewrite() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        DeleteFileW(temp_.c_str());
    }
}

bool FileRewrite::Write(std::string_view bytes) {
    for (size_t done = 0; ok_ && done < bytes.size();) {
        DWORD chunk = (DWORD)std::min<size_t>(bytes.size() - done, 64 * 1024), written = 0;
        ok_ = WriteFile(handle_, bytes.data() + done, chunk, &written, nullptr) && written == chunk;
        done += written;
        if (!ok_) error_ = LastErrorText(L"Couldn't write the file");
    }
    return ok_;
}

bool FileRewrite::Copy(uint64_t offset, uint64_t bytes) {
    if (!ok_) return false;
    if (offset > source_.Size() || bytes > source_.Size() - offset) {
        error_ = L"The source range is outside the file.";
        return ok_ = false;
    }
    if (!buffer_) buffer_ = std::make_unique_for_overwrite<char[]>(64 * 1024);
    while (bytes) {
        size_t n = (size_t)std::min<uint64_t>(bytes, 64 * 1024);
        if (!source_.Read(offset, n, buffer_.get())) {
            error_ = L"Couldn't read the original file.";
            return ok_ = false;
        }
        if (!Write({buffer_.get(), n})) return false;
        offset += n;
        bytes -= n;
    }
    return true;
}

bool FileRewrite::Commit() {
    if (!ok_ || handle_ == INVALID_HANDLE_VALUE) return false;
    ok_ = FlushFileBuffers(handle_) != FALSE;
    if (!ok_) {
        error_ = LastErrorText(L"Couldn't flush the file");
        return false;
    }
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    source_.Close();  // ReplaceFile needs the original's read handle released.
    ok_ = ReplaceFileW(path_.c_str(), temp_.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE;
    if (!ok_) {
        error_ = LastErrorText(L"Couldn't replace the file (is it open in another program?)");
        DeleteFileW(temp_.c_str());
    }
    return ok_;
}

bool FileRewrite::Zeros(uint64_t bytes) {
    if (!ok_) return false;
    if (!buffer_) buffer_ = std::make_unique_for_overwrite<char[]>(64 * 1024);
    memset(buffer_.get(), 0, 64 * 1024);
    while (bytes) {
        size_t n = (size_t)std::min<uint64_t>(bytes, 64 * 1024);
        if (!Write({buffer_.get(), n})) return false;
        bytes -= n;
    }
    return true;
}

