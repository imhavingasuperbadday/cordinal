// Moving files to the Recycle Bin and bringing them back, for conversions and device copies on
// Windows (convert.h). The shell does both: a file is kept only when the bin really gains an item.
// The Linux build has its own version in src/linux/recycle.cpp.
#include "convert.h"

#include "cord.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>

namespace {

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// The original path a Recycle Bin item came from and when it was deleted: its $I companion records
// both (the path in a fixed MAX_PATH field in version 1, with a length in version 2).
std::wstring RecycledFrom(const std::wstring& recycled, uint64_t& deleted) {
    const size_t slash = recycled.find_last_of(L'\\');
    if (slash == std::wstring::npos || recycled.compare(slash + 1, 2, L"$R") != 0) return L"";
    std::wstring info = recycled;
    info[slash + 2] = L'I';
    HANDLE f = CreateFileW(info.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    uint8_t buf[24 + 4 + 2 * 32768];
    DWORD got = 0;
    if (!ReadFile(f, buf, sizeof buf, &got, nullptr)) got = 0;
    CloseHandle(f);
    if (got < 28) return L"";
    const uint64_t version = cord::Le64(buf);
    deleted = cord::Le64(buf + 16);
    const wchar_t* text = nullptr;
    size_t chars = 0;
    if (version == 1) {
        text = (const wchar_t*)(buf + 24);
        chars = std::min<size_t>((got - 24) / 2, 260);
    } else if (version == 2) {
        text = (const wchar_t*)(buf + 28);
        chars = std::min<size_t>(cord::Le32(buf + 24), (got - 28) / 2);
    }
    if (!text) return L"";
    std::wstring path(text, chars);
    path.resize(wcsnlen(path.c_str(), path.size()));
    return path;
}

}  // namespace

bool RecycleFile(const std::wstring& path) {
    // Only a fixed drive whose Recycle Bin this process can use keeps the file: where the bin is
    // missing or out of reach (another drive type, a restricted or sandboxed process), the shell
    // deletes outright instead, so nothing is sent there at all.
    wchar_t root[MAX_PATH];
    if (!GetVolumePathNameW(path.c_str(), root, MAX_PATH) || GetDriveTypeW(root) != DRIVE_FIXED) return false;
    SHQUERYRBINFO before{sizeof before};
    if (FAILED(SHQueryRecycleBinW(root, &before))) return false;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool ok = false;
    {
        IFileOperation* op = nullptr;
        IShellItem* item = nullptr;
        BOOL aborted = FALSE;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) &&
            SUCCEEDED(op->SetOperationFlags(FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_NOERRORUI |
                                            FOF_SILENT)) &&
            SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) && SUCCEEDED(op->DeleteItem(item, nullptr)) &&
            SUCCEEDED(op->PerformOperations()) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && !aborted) {
            // Recycled only if the bin now holds one more item; the log records what really happened.
            SHQUERYRBINFO after{sizeof after};
            ok = !Exists(path) && SUCCEEDED(SHQueryRecycleBinW(root, &after)) && after.i64NumItems > before.i64NumItems;
        }
        if (item) item->Release();
        if (op) op->Release();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

bool RestoreFromRecycleBin(const std::wstring& original) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool ok = false;
    IShellItem* bin = nullptr;
    IEnumShellItems* items = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin))) &&
        SUCCEEDED(bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) {
        // The most recently recycled copy, if the same path went to the bin more than once.
        IShellItem* best = nullptr;
        std::wstring bestName;
        uint64_t bestTime = 0;
        IShellItem* item = nullptr;
        while (items->Next(1, &item, nullptr) == S_OK) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                const std::wstring recycled = path;
                CoTaskMemFree(path);
                uint64_t deleted = 0;
                if (SamePath(RecycledFrom(recycled, deleted), original) && (!best || deleted > bestTime)) {
                    if (best) best->Release();
                    best = item;
                    bestName = recycled;
                    bestTime = deleted;
                    continue;
                }
            }
            item->Release();
        }
        if (best) {
            // The shell's own Restore, so the bin's bookkeeping stays right; moving the file back by
            // hand only if that fails, and never over a file that is there again.
            IContextMenu* menu = nullptr;
            if (SUCCEEDED(best->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu)))) {
                HMENU m = CreatePopupMenu();
                if (m && SUCCEEDED(menu->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL))) {
                    CMINVOKECOMMANDINFO invoke{sizeof invoke};
                    invoke.fMask = CMIC_MASK_FLAG_NO_UI;
                    invoke.lpVerb = "undelete";
                    invoke.nShow = SW_HIDE;
                    menu->InvokeCommand(&invoke);
                }
                if (m) DestroyMenu(m);
                menu->Release();
            }
            ok = Exists(original);
            if (!ok && Exists(bestName) && MoveFileExW(bestName.c_str(), original.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
                std::wstring info = bestName;
                info[info.find_last_of(L'\\') + 2] = L'I';
                DeleteFileW(info.c_str());  // the bin's note about a file no longer in it
                ok = true;
            }
            best->Release();
        }
    }
    if (items) items->Release();
    if (bin) bin->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}
