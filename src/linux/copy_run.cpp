// Copying to a folder or device on Linux, as app_copy.cpp does on Windows: the confirmed plan from
// the Copy panel (panels.cpp) runs on a worker with progress in the status bar and Cancel copy in the
// Library menu. A phone is reached through its mounted folder (GNOME's and KDE's MTP mounts), so every
// destination is a folder. The work is devicecopy.cpp's; originals are only read.
#include "app.h"

#include <cmath>
#include <memory>

namespace lx {
namespace {

std::thread worker;
std::atomic<bool> cancel{false};
bool copying = false;
bool stopping = false;               // shutting down: nothing is said
std::vector<std::wstring> names;     // plan index -> file name, for the progress line
std::vector<size_t> ordinal;
size_t total = 0;
CopyResult* finished = nullptr;      // the worker's result until the UI thread takes it

std::wstring Songs(size_t n) { return std::to_wstring(n) + (n == 1 ? L" song" : L" songs"); }

}  // namespace

bool CopyBusy() { return copying; }

std::wstring CopySize(uint64_t bytes) {
    wchar_t s[32];
    if (bytes >= 1000000000ull) swprintf_s(s, L"%.1f GB", bytes / 1e9);
    else swprintf_s(s, L"%.0f MB", std::ceil(bytes / 1e6));
    return s;
}

void StartCopy(std::vector<CopyItem> plan, CopySettings settings, std::vector<CopyRecord> manifest) {
    if (copying) return;
    copying = true;
    cancel = false;
    names.assign(plan.size(), L"");
    ordinal.assign(plan.size(), 0);
    total = 0;
    for (size_t i = 0; i < plan.size(); ++i)
        if (plan[i].skip.empty() && !plan[i].done) {
            ordinal[i] = ++total;
            names[i] = FileNameOf(plan[i].rel);
        }
    wchar_t temp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH, temp);
    settings.staging = std::wstring(temp) + L"cordinal-copy-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(SDL_GetTicks());
    if (worker.joinable()) worker.join();
    worker = std::thread([plan = std::move(plan), settings = std::move(settings), manifest = std::move(manifest)]() {
        finished = new CopyResult(ExecuteCopy(plan, settings, manifest, cancel, [](size_t i) {
            PostMessageW(nullptr, WM_APP_COPY, 0, (LPARAM)i);
        }));
        PostMessageW(nullptr, WM_APP_COPY, 1, 0);
    });
    ShowMessage(L"Copying " + Songs(total) + L"...");
}

// On the UI thread, from WM_APP_COPY: a file starting (progress), or the finished run.
void OnCopyMessage(WPARAM wp, LPARAM lp) {
    if (!wp) {
        const size_t i = (size_t)lp;
        if (copying && i < names.size())
            ShowMessage(L"Copying " + std::to_wstring(ordinal[i]) + L" of " + std::to_wstring(total) + L": " + names[i]);
        return;
    }
    if (worker.joinable()) worker.join();
    std::unique_ptr<CopyResult> r(finished);
    finished = nullptr;
    copying = false;
    std::vector<std::wstring>().swap(names);
    std::vector<size_t>().swap(ordinal);
    if (!r || stopping) return;
    std::wstring done = L"Copied " + Songs(r->copied + r->converted);
    if (r->converted) done += L" (" + std::to_wstring(r->converted) + L" as AAC)";
    if (r->bytes) done += L", " + CopySize(r->bytes);
    if (r->removed) done += L"; removed " + std::to_wstring(r->removed) + L" earlier " + (r->removed == 1 ? L"copy" : L"copies");
    done += r->cancelled ? L"; the rest was cancelled." : L".";
    if (!r->failed.empty()) {
        std::string text = ToUtf8(done) + "\n\n" + std::to_string(r->failed.size()) + " couldn't be copied:\n\n";
        for (size_t i = 0; i < r->failed.size() && i < 8; ++i)
            text += ToUtf8(r->failed[i].first) + "\n    " + ToUtf8(r->failed[i].second) + "\n";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Some songs couldn't be copied", text.c_str(), g.window);
    }
    ShowMessage(done);
    PlaySfx(r->failed.empty() ? Sfx::Organized : Sfx::Oops);
}

void CancelCopy() {
    if (!copying) return;
    cancel = true;
    ShowMessage(L"Cancelling the copy after the current file...");
}

// Shutdown: the file in progress is abandoned and leaves nothing under its real name.
void StopCopier() {
    stopping = true;
    cancel = true;
    if (worker.joinable()) worker.join();
    if (finished) OnCopyMessage(1, 0);
}

}  // namespace lx
