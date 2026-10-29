// Copy to folder or device (devicecopy.h): the plan, the manifest, the AAC .m4a writer, and the copy
// itself into a folder (each file written as ".part" and renamed) or onto a phone (files finished in
// a temp folder on this PC, then handed whole to Windows' copy engine). No UI. Linux reaches a phone
// as a mounted folder, so it has the folder destination only.
#include "devicecopy.h"

#include "convert.h"
#include "cue.h"
#include "decoder.h"
#include "organize.h"
#include "replaygain.h"
#include "tags.h"

#include <shlobj.h>
#ifdef _WIN32
#include <shobjidl.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) ? (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow : 0;
}

uint64_t FileTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)
               ? (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime : 0;
}

std::wstring ErrorText(DWORD code) {
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, (DWORD)std::size(buf), nullptr);
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

bool IsImage(const std::wstring& path) {
    const std::wstring ext = ExtOf(path);
    return ext == L"jpg" || ext == L"jpeg" || ext == L"png" || ext == L"gif" || ext == L"bmp" || ext == L"webp";
}

// The folder part of a relative path, "" for a file at the top.
std::wstring RelDir(const std::wstring& rel) {
    const size_t slash = rel.find_last_of(kPathSep);
    return slash == std::wstring::npos ? std::wstring() : rel.substr(0, slash);
}

// Lossless sources convert; so do game music, DSD, modules and MIDI, which a phone can't play as they are.
bool Convertible(const Track& t, const std::wstring& path) {
    const std::wstring ext = ExtOf(AudioFileOf(path));
    return t.codec == Codec::FLAC || t.codec == Codec::ALAC || t.codec == Codec::PCM || t.codec == Codec::Cord ||
           t.codec == Codec::SPC || t.codec == Codec::NSF || t.codec == Codec::GBS || t.codec == Codec::VGM ||
           t.codec == Codec::DSD || t.codec == Codec::SID || t.codec == Codec::Module || t.codec == Codec::MIDI || ext == L"ape" ||
           ext == L"wv" || ext == L"tta";
}

// Every field cordinal shows, for a converted file that starts with no tags of its own.
TagEdit LibraryTags(TrackId id) {
    const Track& t = g_lib[id];
    TagEdit e;
    auto text = [](StringPool::Handle h) { return std::string(g_lib.View(h)); };
    for (const auto& [field, handle] : {std::pair{&e.artist, t.artist}, {&e.album, t.album}, {&e.albumArtist, t.albumArtist},
                                        {&e.genre, t.genre}, {&e.date, t.date}, {&e.composer, t.composer}, {&e.comment, t.comment},
                                        {&e.grouping, t.grouping}, {&e.label, t.label}})
        if (handle) *field = text(handle);
    e.title = t.title ? text(t.title) : ToUtf8(StemOf(AudioFileOf(g_lib.Path(id))));
    for (const auto& [field, value] : {std::pair{&e.track, (uint32_t)t.trackNo}, {&e.trackTotal, (uint32_t)t.trackTotal},
                                       {&e.disc, (uint32_t)t.disc}, {&e.discTotal, (uint32_t)t.discTotal}, {&e.bpm, (uint32_t)t.bpm}})
        if (value) *field = value;
    if (t.flags & Track::kCompilation) e.compilation = true;
    return e;
}

void AddGain(TagEdit& e, const Track& t) {
    if (t.rgTrackGain == kNoGain) return;
    const TagEdit g = replaygain::TagsFor(t);
    e.rgTrackGain = g.rgTrackGain;
    e.rgTrackPeak = g.rgTrackPeak;
    e.rgAlbumGain = g.rgAlbumGain;
    e.rgAlbumPeak = g.rgAlbumPeak;
    e.r128TrackGain = g.r128TrackGain;
    e.r128AlbumGain = g.r128AlbumGain;
}

// A front cover for a converted file whose source has none embedded: the folder's cover image.
std::string FolderCover(const std::wstring& dir) {
    for (const wchar_t* name : {L"cover", L"folder", L"front", L"albumart"})
        for (const wchar_t* ext : {L".jpg", L".jpeg", L".png"}) {
            std::string image;
            const std::wstring path = dir + kPathSep + name + ext;
            if (FileSize(path) && FileSize(path) < 8u << 20 && ReadFileBytes(path, image)) return image;
        }
    return {};
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + kPathSep + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName, path = dir + kPathSep + name;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(path);
            else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID cancel) {
    return *static_cast<const std::atomic<bool>*>(cancel) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

// Copies a file, leaving nothing at `to` when it fails or is cancelled.
bool CopyWhole(const std::wstring& from, const std::wstring& to, const std::atomic<bool>& cancel, std::wstring& error) {
    DeleteFileW(to.c_str());
    if (CopyFileExW(from.c_str(), to.c_str(), CopyProgress, const_cast<std::atomic<bool>*>(&cancel), nullptr, 0)) {
        SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);  // a read-only original makes a copy that can't be replaced later
        return true;
    }
    error = cancel ? L"Cancelled." : ErrorText(GetLastError());
    DeleteFileW(to.c_str());
    return false;
}

std::wstring Field(std::wstring s) {
    for (wchar_t& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

// ---------------------------------------------------------------------------
// Destinations.

// Puts finished files under the destination. A folder takes each one at once; a phone gets them in
// batches through the copy engine, so Flush says when they have really arrived.
class Target {
public:
    virtual ~Target() = default;
    virtual bool Put(const std::wstring& file, const std::wstring& rel, bool temp, const std::atomic<bool>& cancel, std::wstring& error) = 0;
    virtual bool Full() const { return true; }
    virtual bool Flush(std::wstring&) { return true; }
    virtual bool Exists(const std::wstring& rel) const = 0;
    virtual bool SaveManifest(const std::string& text) = 0;
    virtual bool Remove(const std::vector<std::wstring>& rels, std::vector<bool>& removed) = 0;
};

class FolderTarget : public Target {
public:
    explicit FolderTarget(std::wstring root) : root_(std::move(root)) {}
    bool Put(const std::wstring& file, const std::wstring& rel, bool temp, const std::atomic<bool>& cancel, std::wstring& error) override {
        const std::wstring to = root_ + kPathSep + rel, part = to + L".part";
        SHCreateDirectoryExW(nullptr, DirectoryOf(to).c_str(), nullptr);
        // Written whole as .part, renamed only when complete: a card pulled out mid-copy never holds
        // half a song under its real name.
        bool ok = CopyWhole(file, part, cancel, error);
        if (ok && !MoveFileExW(part.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = ErrorText(GetLastError());
            DeleteFileW(part.c_str());
            ok = false;
        }
        if (temp) DeleteFileW(file.c_str());
        return ok;
    }
    bool Exists(const std::wstring& rel) const override { return ::Exists(root_ + kPathSep + rel); }
    bool SaveManifest(const std::string& text) override {
        const std::wstring path = root_ + kPathSep + kCopyManifest, temp = path + L".tmp";
        HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size() && FlushFileBuffers(f);
        CloseHandle(f);
        if (ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileW(temp.c_str());
        return false;
    }
    // A card has no Recycle Bin, so removed copies are deleted; on a drive with one they go there.
    // Folders left empty go too, never the destination itself.
    bool Remove(const std::vector<std::wstring>& rels, std::vector<bool>& removed) override {
        removed.assign(rels.size(), false);
        for (size_t i = 0; i < rels.size(); ++i) {
            const std::wstring path = root_ + kPathSep + rels[i];
            removed[i] = !::Exists(path) || RecycleFile(path) || DeleteFileW(path.c_str());
            for (std::wstring dir = RelDir(rels[i]); removed[i] && !dir.empty(); dir = RelDir(dir))
                if (!RemoveDirectoryW((root_ + kPathSep + dir).c_str())) break;
        }
        return true;
    }

private:
    std::wstring root_;
};

#ifdef _WIN32
// A phone (or anything else the shell reaches without a drive letter). Files gather in a batch
// folder on this PC and go over in one copy-engine operation; folders merge and files replace.
class DeviceTarget : public Target {
public:
    DeviceTarget(const std::wstring& dest, std::wstring staging) : staging_(std::move(staging)) {
        SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(&dest_));
    }
    bool Put(const std::wstring& file, const std::wstring& rel, bool temp, const std::atomic<bool>& cancel, std::wstring& error) override {
        const std::wstring to = staging_ + L"\\batch\\" + rel;
        SHCreateDirectoryExW(nullptr, DirectoryOf(to).c_str(), nullptr);
        const uint64_t size = FileSize(file);
        bool ok = temp ? MoveFileExW(file.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE
                       : CopyWhole(file, to, cancel, error);
        if (!ok) {
            if (temp) {
                error = ErrorText(GetLastError());
                DeleteFileW(file.c_str());
            }
            return false;
        }
        bytes_ += size;
        ++files_;
        return true;
    }
    bool Full() const override { return files_ >= 50 || bytes_ >= 1ull << 30; }
    bool Flush(std::wstring& error) override {
        if (!files_) return true;
        const bool ok = CopyInto(staging_ + L"\\batch");
        RemoveTree(staging_ + L"\\batch");
        files_ = 0;
        bytes_ = 0;
        if (!ok) error = L"Windows couldn't copy the files to the device. Is it still connected and unlocked?";
        return ok;
    }
    // Without a cheap way to look, the manifest is trusted.
    bool Exists(const std::wstring&) const override { return true; }
    bool SaveManifest(const std::string& text) override {
        const std::wstring dir = staging_ + L"\\manifest", path = dir + L"\\" + kCopyManifest;
        CreateDirectoryW(dir.c_str(), nullptr);
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        ok = ok && CopyInto(dir);
        RemoveTree(dir);
        return ok;
    }
    bool Remove(const std::vector<std::wstring>& rels, std::vector<bool>& removed) override {
        removed.assign(rels.size(), false);
        ComPtr<IFileOperation> op;
        if (!dest_ || !Operation(op)) return false;
        std::vector<size_t> queued;
        for (size_t i = 0; i < rels.size(); ++i)
            if (ComPtr<IShellItem> item = Child(rels[i]); item && SUCCEEDED(op->DeleteItem(item.Get(), nullptr))) queued.push_back(i);
        BOOL aborted = FALSE;
        if (queued.empty() || FAILED(op->PerformOperations()) || FAILED(op->GetAnyOperationsAborted(&aborted)) || aborted) return false;
        for (size_t i : queued) removed[i] = !Child(rels[i]);
        return true;
    }

private:
    bool Operation(ComPtr<IFileOperation>& op) {
        return SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) &&
               SUCCEEDED(op->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT));
    }
    // The item at a relative path, walked one folder at a time: a phone's shell folder may not
    // parse a whole path at once.
    ComPtr<IShellItem> Child(const std::wstring& rel) {
        ComPtr<IShellItem> at = dest_;
        for (size_t start = 0; at && start <= rel.size();) {
            size_t slash = rel.find(L'\\', start);
            if (slash == std::wstring::npos) slash = rel.size();
            ComPtr<IShellItem> next;
            if (FAILED(SHCreateItemFromRelativeName(at.Get(), rel.substr(start, slash - start).c_str(), nullptr, IID_PPV_ARGS(&next))))
                return {};
            at = next;
            start = slash + 1;
        }
        return at;
    }
    // Copies everything inside `folder` into the destination.
    bool CopyInto(const std::wstring& folder) {
        ComPtr<IFileOperation> op;
        if (!dest_ || !Operation(op)) return false;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
        bool any = false, ok = h != INVALID_HANDLE_VALUE;
        if (ok) {
            do {
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                ComPtr<IShellItem> item;
                ok = ok && SUCCEEDED(SHCreateItemFromParsingName((folder + L"\\" + name).c_str(), nullptr, IID_PPV_ARGS(&item))) &&
                     SUCCEEDED(op->CopyItem(item.Get(), dest_.Get(), nullptr, nullptr));
                any = true;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        BOOL aborted = FALSE;
        return ok && (!any || (SUCCEEDED(op->PerformOperations()) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && !aborted));
    }

    ComPtr<IShellItem> dest_;
    std::wstring staging_;
    uint64_t bytes_ = 0;
    size_t files_ = 0;
};
#endif

// The manifest separates folders with '\\' on every system, so a card filled from Windows and from
// Linux keeps one manifest.
std::wstring ManifestRel(std::wstring rel, wchar_t from, wchar_t to) {
    std::replace(rel.begin(), rel.end(), from, to);
    return rel;
}

}  // namespace

unsigned CopyKbps(CopyFormat f) { return f == CopyFormat::Aac192 ? 192 : f == CopyFormat::Aac128 ? 128 : 0; }

std::string CopyManifestText(const std::vector<CopyRecord>& records) {
    std::string text = "cordinal-copy 1\n";
    for (const CopyRecord& r : records) {
        text += ToUtf8(ManifestRel(r.rel, kPathSep, L'\\')) + "\t" + ToUtf8(r.from) + "\t" + std::to_string(r.size) + "\t" +
                std::to_string(r.time) + "\t" + std::to_string(r.format);
        for (const std::wstring& set : r.sets) text += "\t" + ToUtf8(Field(set));
        text += "\n";
    }
    return text;
}

bool ParseCopyManifest(const std::string& text, std::vector<CopyRecord>& out) {
    out.clear();
    if (text.rfind("cordinal-copy ", 0) != 0) return text.empty();
    for (size_t start = text.find('\n'); start != std::string::npos && start < text.size();) {
        size_t end = text.find('\n', ++start);
        if (end == std::string::npos) end = text.size();
        std::vector<std::string> fields;
        for (size_t at = start; at <= end;) {
            size_t tab = text.find('\t', at);
            if (tab == std::string::npos || tab > end) tab = end;
            fields.push_back(text.substr(at, tab - at));
            at = tab + 1;
        }
        start = end;
        if (fields.size() < 5 || fields[0].empty() || fields[0].find("..") != std::string::npos || fields[0][0] == '\\' ||
            fields[0][0] == '/' || fields[0].find(':') != std::string::npos)
            continue;  // never a path outside the destination
        CopyRecord r{ManifestRel(FromUtf8(fields[0]), L'\\', kPathSep), FromUtf8(fields[1]), strtoull(fields[2].c_str(), nullptr, 10),
                     strtoull(fields[3].c_str(), nullptr, 10), (unsigned)strtoul(fields[4].c_str(), nullptr, 10), {}};
        for (size_t i = 5; i < fields.size(); ++i)
            if (!fields[i].empty()) r.sets.push_back(FromUtf8(fields[i]));
        out.push_back(std::move(r));
    }
    return true;
}

bool ReadCopyManifest(const std::wstring& dest, bool device, std::vector<CopyRecord>& out) {
    out.clear();
    std::string text;
    if (!device) {
        const std::wstring path = dest + kPathSep + kCopyManifest;
        if (!Exists(path)) return true;
        if (!ReadFileBytes(path, text)) return false;
        return ParseCopyManifest(text, out);
    }
#ifdef _WIN32
    ComPtr<IShellItem> folder, item;
    ComPtr<IStream> stream;
    if (FAILED(SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(&folder)))) return false;
    if (FAILED(SHCreateItemFromRelativeName(folder.Get(), kCopyManifest, nullptr, IID_PPV_ARGS(&item)))) return true;  // none yet
    if (FAILED(item->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(&stream)))) return false;
    char buf[65536];
    for (ULONG got = 0; SUCCEEDED(stream->Read(buf, sizeof buf, &got)) && got && text.size() < (64u << 20);) text.append(buf, got);
    return ParseCopyManifest(text, out);
#else
    return false;
#endif
}

std::vector<CopyItem> PlanCopy(const std::vector<TrackId>& ids, const std::wstring& destIn, bool device, const std::wstring& pattern,
                               CopyFormat format, const std::vector<CopyRecord>& manifest) {
    std::wstring dest = destIn;
    while (!device && dest.size() > 3 && (dest.back() == L'\\' || dest.back() == L'/')) dest.pop_back();
    std::unordered_map<std::wstring, const CopyRecord*> records;
    for (const CopyRecord& r : manifest) records[Lower(r.rel)] = &r;
    const unsigned kbps = CopyKbps(format);
    std::vector<CopyItem> plan;
    std::unordered_set<std::wstring> taken;
    std::unordered_set<TrackId> seen;
    for (TrackId id : ids) {
        if (!g_lib.Contains(id) || !seen.insert(id).second) continue;  // a playlist may hold a track twice
        const Track& t = g_lib[id];
        CopyItem item;
        item.id = id;
        item.from = g_lib.Path(id);
        item.size = t.fileSize;
        item.time = t.fileTime;
        item.convert = kbps && Convertible(t, item.from);
        if (IsSubtrack(item.from) && !item.convert) {
            item.skip = kbps ? L"part of a file that can't be split" : L"part of a cue sheet: choose AAC to copy it";
            plan.push_back(std::move(item));
            continue;
        }
        if (t.Failed() || (t.flags & Track::kMissing)) {
            item.skip = L"the file is missing or unreadable";
            plan.push_back(std::move(item));
            continue;
        }
        std::vector<std::wstring> parts = PatternParts(id, pattern);
        std::wstring dir;
        for (size_t i = 0; i + 1 < parts.size(); ++i) dir += parts[i] + kPathSep;
        std::wstring name = parts.back();
        const std::wstring ext = item.convert ? L"m4a" : ExtOf(AudioFileOf(item.from));
        // Stay under MAX_PATH on the destination, with room for ".part".
        const size_t budget = 240 - std::min<size_t>(240, (device ? 0 : dest.size()) + dir.size() + ext.size() + 12);
        if (name.size() > budget) name.resize(std::max<size_t>(budget, 8));
        item.rel = dir + name + L"." + ext;
        auto free = [&](const std::wstring& rel) {
            const std::wstring key = Lower(rel);
            if (taken.count(key)) return false;
            auto r = records.find(key);
            if (r != records.end()) return true;  // cordinal's own copy: it may be replaced
            return device || !Exists(dest + kPathSep + rel);  // never over a file cordinal didn't put there
        };
        for (int n = 2; !free(item.rel) && n <= 99; ++n) item.rel = dir + name + L" (" + std::to_wstring(n) + L")." + ext;
        if (!free(item.rel)) {
            item.skip = L"a file with that name already exists";
            plan.push_back(std::move(item));
            continue;
        }
        taken.insert(Lower(item.rel));
        if (auto r = records.find(Lower(item.rel)); r != records.end()) {
            const CopyRecord& c = *r->second;
            item.done = c.from == item.from && c.size == item.size && c.time == item.time && c.format == (item.convert ? kbps : 0) &&
                        (device || Exists(dest + kPathSep + item.rel));
        }
        item.estimate = item.convert ? (uint64_t)(t.Duration() * kbps * 125) + 4096 : t.fileSize;
        if (item.convert) item.tags = LibraryTags(id);
        AddGain(item.tags, t);
        plan.push_back(std::move(item));
    }
    return plan;
}

std::vector<std::wstring> StaleCopies(const std::vector<CopyItem>& plan, const std::vector<CopyRecord>& manifest, const std::wstring& set) {
    std::unordered_set<std::wstring> wanted, folders;
    for (const CopyItem& item : plan)
        if (item.skip.empty()) {
            wanted.insert(Lower(item.rel));
            folders.insert(Lower(RelDir(item.rel)));
        }
    std::vector<std::wstring> stale;
    for (const CopyRecord& r : manifest) {
        if (r.sets.size() != 1 || r.sets[0] != set) continue;  // another playlist or a selection still wants it
        // A cover image stays while its folder still has songs this playlist wants.
        if (IsImage(r.rel) ? !folders.count(Lower(RelDir(r.rel))) : !wanted.count(Lower(r.rel))) stale.push_back(r.rel);
    }
    return stale;
}

CopyResult ExecuteCopy(const std::vector<CopyItem>& plan, const CopySettings& s, std::vector<CopyRecord> manifest,
                       const std::atomic<bool>& cancel, const std::function<void(size_t)>& progress) {
    CopyResult result;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHCreateDirectoryExW(nullptr, s.staging.c_str(), nullptr);
    std::unique_ptr<Target> target;
#ifdef _WIN32
    if (s.device) target = std::make_unique<DeviceTarget>(s.dest, s.staging);
#endif
    if (!target) target = std::make_unique<FolderTarget>(s.dest);

    std::unordered_map<std::wstring, size_t> index;
    for (size_t i = 0; i < manifest.size(); ++i) index[Lower(manifest[i].rel)] = i;
    auto record = [&](CopyRecord r) {
        auto [it, added] = index.try_emplace(Lower(r.rel), manifest.size());
        if (added) manifest.push_back(std::move(r));
        else {
            std::vector<std::wstring> sets = std::move(manifest[it->second].sets);
            manifest[it->second] = std::move(r);
            manifest[it->second].sets = std::move(sets);
        }
        auto& sets = manifest[it->second].sets;
        if (std::find(sets.begin(), sets.end(), s.set) == sets.end()) sets.push_back(s.set);
    };
    // A copy counts, and enters the manifest, only once it is really in the destination.
    struct Sent {
        CopyRecord record;
        uint64_t bytes;
        bool song;
    };
    std::vector<Sent> sent;
    size_t sinceSave = 0;
    auto flush = [&](bool force) {
        if (sent.empty() || (!force && !target->Full())) return;
        std::wstring error;
        const bool ok = target->Flush(error);
        for (Sent& x : sent) {
            if (!ok) {
                result.failed.push_back({x.record.from, error});
                continue;
            }
            if (x.song) ++(x.record.format ? result.converted : result.copied);
            result.bytes += x.bytes;
            record(std::move(x.record));
        }
        sent.clear();
        if (ok && (s.device || force || ++sinceSave >= 20)) {
            target->SaveManifest(CopyManifestText(manifest));
            sinceSave = 0;
        }
    };
    auto put = [&](const std::wstring& file, bool temp, CopyRecord rec, bool song) {
        const uint64_t bytes = FileSize(file);
        std::wstring error;
        if (target->Put(file, rec.rel, temp, cancel, error)) {
            sent.push_back({std::move(rec), bytes, song});
            flush(false);
        } else if (cancel) {
            result.cancelled = true;
        } else {
            result.failed.push_back({rec.from, error});
        }
    };

    std::map<std::wstring, std::pair<std::wstring, std::wstring>> folders;  // source folder -> (its path, the copies' folder)
    const std::wstring work = s.staging + kPathSep + L"work";
    for (size_t i = 0; i < plan.size() && !result.cancelled; ++i) {
        const CopyItem& item = plan[i];
        if (!item.skip.empty()) continue;
        const std::wstring source = AudioFileOf(item.from), srcDir = DirectoryOf(source);
        auto [folder, first] = folders.try_emplace(Lower(srcDir), srcDir, RelDir(item.rel));
        if (!first && Lower(folder->second.second) != Lower(RelDir(item.rel))) folder->second.second = L"?";  // spread over folders
        CopyRecord rec{item.rel, item.from, item.size, item.time, item.convert ? s.kbps : 0, {}};
        if (item.done) {
            record(std::move(rec));
            continue;
        }
        if (cancel) {
            result.cancelled = true;
            break;
        }
        if (progress) progress(i);
        std::wstring error;
        const std::wstring staged = work + L"." + ExtOf(item.rel);
        if (item.convert) {
            TagEdit tags = item.tags;
            std::string bytes;
            if (ReadEmbeddedArt(source, bytes) && !bytes.empty()) tags.cover = std::move(bytes);
            else if (std::string cover = FolderCover(srcDir); !cover.empty()) tags.cover = std::move(cover);
            if (ReadLyrics(source, bytes) && !bytes.empty()) tags.lyrics = std::move(bytes);
            if (!EncodeM4a(item.from, staged, s.kbps, cancel, error) || !WriteTags(staged, tags, error)) {
                DeleteFileW(staged.c_str());
                if (cancel) result.cancelled = true;
                else result.failed.push_back({item.from, error});
                continue;
            }
            put(staged, true, std::move(rec), true);
        } else if (!item.tags.Empty() && CanWriteTags(source) && CopyWhole(source, staged, cancel, error)) {
            // ReplayGain goes into the copy; if the copy's tags can't take it, the copy goes as it was.
            if (!WriteTags(staged, item.tags, error)) {
                DeleteFileW(staged.c_str());
                put(source, false, std::move(rec), true);
            } else {
                put(staged, true, std::move(rec), true);
            }
        } else if (cancel) {
            result.cancelled = true;
        } else {
            put(source, false, std::move(rec), true);
        }
    }

    // Cover images follow when every copied song of a folder went to one folder (not the top).
    for (auto& [lower, where] : folders) {
        const auto& [srcDir, relDir] = where;
        if (result.cancelled || relDir.empty() || relDir == L"?") continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((srcDir + kPathSep + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsImage(fd.cFileName)) continue;
            const std::wstring from = srcDir + kPathSep + fd.cFileName;
            CopyRecord rec{relDir + kPathSep + fd.cFileName, from, FileSize(from), FileTime(from), 0, {}};
            auto r = index.find(Lower(rec.rel));
            const bool ours = r != index.end();
            if (ours && manifest[r->second].size == rec.size && manifest[r->second].time == rec.time && target->Exists(rec.rel))
                record(std::move(rec));
            else if (ours || !target->Exists(rec.rel) || s.device)
                put(from, false, std::move(rec), false);
        } while (FindNextFileW(h, &fd) && !cancel);
        FindClose(h);
    }

    if (!result.cancelled && !s.playlistName.empty()) {
        const std::wstring file = work + L".m3u8";
        HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD n = 0;
        const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, s.playlist.data(), (DWORD)s.playlist.size(), &n, nullptr) &&
                        n == s.playlist.size();
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        std::wstring error;
        const std::wstring rel = SafePathComponent(s.playlistName) + L".m3u8";
        if (!ok || !target->Put(file, rel, true, cancel, error)) result.failed.push_back({rel, ok ? error : L"Couldn't write the playlist."});
    }
    flush(true);

    if (s.removeStale && !result.cancelled) {
        std::vector<CopyItem> kept;
        for (const CopyItem& item : plan)
            if (item.skip.empty()) kept.push_back({item.id, {}, item.rel});
        const std::vector<std::wstring> stale = StaleCopies(kept, manifest, s.set);
        std::vector<bool> removed;
        target->Remove(stale, removed);
        std::unordered_set<std::wstring> gone;
        for (size_t i = 0; i < stale.size(); ++i) {
            if (removed[i]) gone.insert(Lower(stale[i]));
            else result.failed.push_back({stale[i], L"Couldn't remove this earlier copy."});
        }
        result.removed = gone.size();
        // Files other playlists still want stay, but no longer count as this playlist's.
        std::unordered_set<std::wstring> wanted;
        for (const CopyItem& item : kept) wanted.insert(Lower(item.rel));
        std::vector<CopyRecord> left;
        for (CopyRecord& r : manifest) {
            if (gone.count(Lower(r.rel))) continue;
            if (!wanted.count(Lower(r.rel)) && !IsImage(r.rel) && r.sets.size() > 1) std::erase(r.sets, s.set);
            left.push_back(std::move(r));
        }
        manifest = std::move(left);
    }
    if (!s.device || result.copied || result.converted || result.removed) target->SaveManifest(CopyManifestText(manifest));
    target.reset();
    RemoveTree(s.staging);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
