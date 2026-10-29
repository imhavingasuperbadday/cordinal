// Linux's Library and track-menu lossless exports: copy paths on the UI thread, export and verify on a
// worker, and return progress by message. The .cord files and library records are always kept.
#include "app.h"
#include "convert.h"

namespace lx {
namespace {
std::thread worker;
std::atomic<bool> cancel{false};
bool busy = false, stopping = false;
CordExport runningFormat = CordExport::Flac;
struct Result {
    size_t done = 0;
    bool cancelled = false;
    std::wstring errors;
};
}

bool CordExportBusy() { return busy; }

void ExportCordTracks(std::vector<TrackId> ids, CordExport format) {
    if (busy || TagWriteBusy() || ConvertBusy() || OrganizeBusy()) return;
    if (ids.empty())
        for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
            if (g_lib[id].InLibrary()) ids.push_back(id);
    std::vector<std::wstring> paths;
    for (TrackId id : ids) {
        const std::wstring path = g_lib.Path(id);
        const std::wstring to = path.substr(0, path.find_last_of(L'.')) + L"." + kCordExports[(unsigned)format].extension;
        if (id != g.playing && ExtOf(path) == L"cord" && path.find(L'|') == path.npos &&
            GetFileAttributesW(to.c_str()) == INVALID_FILE_ATTRIBUTES) paths.push_back(path);
    }
    if (paths.empty()) {
        ShowMessage(L"Nothing to export: select .cord files that aren't playing and have no output file of the same name.");
        return;
    }
    const std::string question = "Export " + std::to_string(paths.size()) +
        " file(s) from .cord to " + ToUtf8(kCordExports[(unsigned)format].label) + "?\n\nThe copies go in the same folders, with their tags and covers. "
        "Audio and metadata are verified before they appear. Existing files are never overwritten.\n\n"
        "The .cord files are kept. No original files or conversion history are needed.";
    SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Export"},
                                          {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}};
    SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, g.window, "Export .cord", question.c_str(), 2, buttons, nullptr};
    int answer = 0;
    SDL_ShowMessageBox(&dialog, &answer);
    if (answer != 1) return;
    g.scanner.Stop();
    g.scanning = false;
    busy = true;
    runningFormat = format;
    cancel = false;
    if (worker.joinable()) worker.join();
    worker = std::thread([paths = std::move(paths), format] {
        auto result = std::make_unique<Result>();
        for (size_t i = 0; i < paths.size(); ++i) {
            if (cancel) { result->cancelled = true; break; }
            auto* status = new std::wstring(L"Exporting " + std::to_wstring(i + 1) + L" of " + std::to_wstring(paths.size()) +
                                           L" to " + kCordExports[(unsigned)format].label);
            if (!PostMessageW(nullptr, WM_APP_CORD_EXPORT, 0, (LPARAM)status)) delete status;
            std::wstring error;
            const auto& from = paths[i];
            if (ExportCordAudio(from, from.substr(0, from.find_last_of(L'.')) + L"." + kCordExports[(unsigned)format].extension,
                                format, error, cancel)) ++result->done;
            else if (cancel) { result->cancelled = true; break; }
            else if (result->errors.size() < 4000) result->errors += from + L"\n" + error + L"\n\n";
        }
        auto* done = result.release();
        if (!PostMessageW(nullptr, WM_APP_CORD_EXPORT, 1, (LPARAM)done)) delete done;
    });
    ShowMessage(L"Exporting .cord...");
}

void CancelCordExport() { cancel = true; }

void OnCordExport(WPARAM wp, LPARAM lp) {
    if (!wp) {
        std::unique_ptr<std::wstring> status((std::wstring*)lp);
        if (!stopping) ShowMessage(*status);
        return;
    }
    std::unique_ptr<Result> result((Result*)lp);
    if (worker.joinable()) worker.join();
    busy = false;
    if (stopping) return;
    StartScanner();
    ShowMessage(L"Exported " + std::to_wstring(result->done) + L" file(s) to " + kCordExports[(unsigned)runningFormat].label + L". The .cord files were kept." +
                (result->cancelled ? L" The rest was cancelled." : L""));
    if (!result->errors.empty())
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Some files couldn't be exported", ToUtf8(result->errors).c_str(), g.window);
}

void StopCordExporter() {
    stopping = true;
    cancel = true;
    if (worker.joinable()) worker.join();
}

}  // namespace lx
