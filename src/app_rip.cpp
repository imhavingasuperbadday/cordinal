// Library > Rip CD...: the Rip CD window, where the disc in a drive is read (its table of contents,
// CD-Text, and with Album tags from MusicBrainz on, the releases MusicBrainz knows it as), the names
// are chosen and edited, and the format and folder set; then the rip on a background worker, with
// progress in the status bar and Cancel in the Library menu, like Convert. Finished tracks join the
// library already tagged. The reading, verifying and writing are cdrip.cpp's.
#include "app.h"
#include "cdrip.h"
#include "metadata_fetch.h"

namespace app {

namespace {

const wchar_t kRipClass[] = L"CordinalRipCd";
constexpr UINT kDiscRead = WM_APP + 1;    // wParam: the generation; lParam: a DiscRead the window takes
constexpr UINT kOffsetDone = WM_APP + 2;  // wParam: the generation; lParam: an OffsetResult the window takes
// Control ids; the UI suite finds the controls by them.
enum { kIdDrive = 100, kIdNames, kIdArtist, kIdAlbum, kIdDate, kIdTracks, kIdTitle, kIdFormat, kIdPath, kIdBrowse, kIdLookUp,
       kIdStatus, kIdDetectOffset };
const wchar_t* const kFormats[] = {L"FLAC (lossless)", L".cord (lossless, cordinal's own)"};

// What the disc worker found.
struct DiscRead {
    std::wstring drive, error;
    cdrip::Toc toc;
    std::vector<cdrip::Disc> names;  // CD-Text or an untitled disc first, then MusicBrainz's releases
    std::vector<cdrip::AccurateRipPressing> arPressings;
    bool lookedUp = false, offline = false, arChecked = false;
};

struct OffsetResult {
    cdrip::OffsetDetection det;
    std::vector<cdrip::AccurateRipPressing> pressings;
    bool fetched = false;
    std::wstring error;
};

struct RipWindow {
    HWND hwnd = nullptr, driveLabel = nullptr, drive = nullptr, namesLabel = nullptr, names = nullptr, artistLabel = nullptr,
         artist = nullptr, albumLabel = nullptr, album = nullptr, dateLabel = nullptr, date = nullptr, tracks = nullptr,
         titleLabel = nullptr, title = nullptr, formatLabel = nullptr, format = nullptr, detectOffset = nullptr,
         whereLabel = nullptr, path = nullptr, browse = nullptr, lookUp = nullptr, status = nullptr, go = nullptr,
         cancel = nullptr;
    std::vector<std::wstring> drives;
    std::unique_ptr<DiscRead> disc;
    cdrip::Disc chosen;  // the names as edited
    bool filling = false;  // the fields are being filled in, not edited
    bool detectingOffset = false;
} w;

std::thread g_reader, g_offsetWorker, g_ripper;
unsigned g_generation = 0;  // the disc read the window waits for; older results are dropped
std::shared_ptr<online::Cancel> g_lookup;
std::atomic<bool> g_cancel{false};
bool g_ripping = false;
std::vector<std::wstring> g_titles;  // the running rip's track titles, for the progress line

std::wstring Duration(uint32_t sectors) {
    const uint32_t s = sectors / 75;
    wchar_t text[32];
    swprintf_s(text, s >= 3600 ? L"%u:%02u:%02u" : L"%u:%02u", s >= 3600 ? s / 3600 : s / 60, s >= 3600 ? s / 60 % 60 : s % 60,
               s % 60);
    return text;
}

std::wstring TrackTitle(size_t k) {
    return k < w.chosen.titles.size() && !w.chosen.titles[k].empty() ? FromUtf8(w.chosen.titles[k])
                                                                      : L"Track " + std::to_wstring(k + 1);
}

std::wstring TrackRow(size_t k) {
    const auto audio = w.disc->toc.Audio();
    const size_t i = audio[k];
    wchar_t number[8];
    swprintf_s(number, L"%02u", w.disc->toc.tracks[i].number);
    return number + (L"\t" + TrackTitle(k)) + L"\t" + Duration(w.disc->toc.End(i) - w.disc->toc.tracks[i].start);
}

bool Ready() { return w.disc && w.disc->error.empty() && !g_ripping && !WindowText(w.path).empty(); }

void UpdateButtons() {
    EnableWindow(w.go, Ready() && !w.detectingOffset);
    EnableWindow(w.lookUp, w.disc && w.disc->error.empty() && !w.disc->lookedUp && !w.detectingOffset);
    EnableWindow(w.detectOffset, w.disc && w.disc->error.empty() && !w.detectingOffset);
}

// Puts the chosen names into the fields and the track list.
void ShowNames() {
    w.filling = true;
    SetWindowTextW(w.artist, FromUtf8(w.chosen.artist).c_str());
    SetWindowTextW(w.album, FromUtf8(w.chosen.album).c_str());
    SetWindowTextW(w.date, FromUtf8(w.chosen.date).c_str());
    SendMessageW(w.tracks, LB_RESETCONTENT, 0, 0);
    if (w.disc && w.disc->error.empty())
        for (size_t k = 0; k < w.disc->toc.Audio().size(); ++k) SendMessageW(w.tracks, LB_ADDSTRING, 0, (LPARAM)TrackRow(k).c_str());
    SendMessageW(w.tracks, LB_SETCURSEL, 0, 0);
    SetWindowTextW(w.title, TrackTitle(0).c_str());
    w.filling = false;
}

void ShowStatus() {
    std::wstring s;
    if (!w.disc) s = w.drives.empty() ? L"No CD drive found." : L"Reading the disc\x2026";
    else if (w.detectingOffset) s = L"Detecting drive read offset with AccurateRip\x2026";
    else if (!w.disc->error.empty()) s = w.disc->error;
    else {
        const auto audio = w.disc->toc.Audio();
        s = std::to_wstring(audio.size()) + (audio.size() == 1 ? L" track, " : L" tracks, ") +
            Duration(w.disc->toc.End(audio.back()) - w.disc->toc.tracks[audio.front()].start) + L". ";
        const size_t found = w.disc->names.size() - 1;
        if (found) s += L"MusicBrainz knows this disc" + std::wstring(found > 1 ? L" as " + std::to_wstring(found) + L" releases." : L".");
        else if (w.disc->offline) s += L"MusicBrainz couldn't be reached.";
        else if (w.disc->lookedUp) s += L"MusicBrainz doesn't know this disc.";
        else s += L"Look up finds its names on MusicBrainz.";
        if (w.disc->arChecked) {
            if (!w.disc->arPressings.empty())
                s += L" AccurateRip: " + std::to_wstring(w.disc->arPressings.size()) +
                     (w.disc->arPressings.size() == 1 ? L" pressing." : L" pressings.");
            else s += L" Not in AccurateRip.";
        }
    }
    SetWindowTextW(w.status, s.c_str());
    UpdateButtons();
}

// Reads the disc in the chosen drive on a worker; with `lookUp`, MusicBrainz is asked about it too.
void ReadDisc(bool lookUp) {
    w.disc.reset();
    w.chosen = {};
    w.detectingOffset = false;
    ShowNames();
    SendMessageW(w.names, CB_RESETCONTENT, 0, 0);
    if (g_lookup) g_lookup->Stop();
    if (g_offsetWorker.joinable()) g_offsetWorker.join();
    if (g_reader.joinable()) g_reader.join();
    const int sel = (int)SendMessageW(w.drive, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)w.drives.size()) return ShowStatus();
    ShowStatus();
    g_lookup = std::make_shared<online::Cancel>();
    g_reader = std::thread([hwnd = w.hwnd, gen = ++g_generation, drive = w.drives[(size_t)sel], lookUp, stop = g_lookup]() {
        auto d = std::make_unique<DiscRead>();
        d->drive = drive;
        if (auto dev = cdrip::OpenDrive(drive, d->error)) {
            cdrip::Disc names;
            if (!dev->ReadToc(d->toc)) d->error = L"Put an audio CD in " + drive + L".";
            else if (d->toc.Audio().empty()) d->error = L"This disc has no audio tracks.";
            else cdrip::ParseCdText(dev->CdText(), d->toc.Audio().size(), names);
            names.discId = cdrip::DiscId(d->toc);
            d->names.push_back(std::move(names));
            if (d->error.empty() && lookUp) {
                for (auto& r : cdrip::LookUp(d->toc, *stop, d->offline)) d->names.push_back(std::move(r));
                d->lookedUp = !stop->Stopped();
            }
            if (d->error.empty() && online::Config().accurateRip && !stop->Stopped()) {
                d->arPressings = cdrip::FetchAccurateRip(d->toc, *stop);
                d->arChecked = !stop->Stopped();
            }
        }
        if (!PostMessageW(hwnd, kDiscRead, gen, (LPARAM)d.get())) return;
        d.release();
    });
}

void OnDisc(std::unique_ptr<DiscRead> d) {
    w.disc = std::move(d);
    SendMessageW(w.names, CB_RESETCONTENT, 0, 0);
    for (const cdrip::Disc& n : w.disc->names)
        SendMessageW(w.names, CB_ADDSTRING, 0,
                     (LPARAM)(n.source.empty() ? std::wstring(L"Untitled (Track 1, Track 2\x2026)") : FromUtf8(n.source)).c_str());
    // A release MusicBrainz matched beats CD-Text, which often lacks dates, accents and track artists.
    const int pick = w.disc->names.size() > 1 ? 1 : 0;
    SendMessageW(w.names, CB_SETCURSEL, pick, 0);
    if (!w.disc->names.empty()) w.chosen = w.disc->names[(size_t)pick];
    ShowNames();
    ShowStatus();
}

void LookUp() {
    if (!online::Config().albumTags) {
        if (MessageBoxW(w.hwnd,
                        L"Album tags from MusicBrainz is off, so cordinal hasn't contacted anyone.\n\n"
                        L"Looking up sends this disc's id and track lengths to musicbrainz.org.\n\n"
                        L"Turn it on? It can be changed in Preferences > Online.",
                        L"Rip CD", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        if (const Tweak* tweak = FindTweak("online.album_tags")) SetTweak(*tweak, 1);
        if (!online::Config().albumTags) return;
    }
    ReadDisc(true);
}

void DetectOffsetNow() {
    if (!w.disc || !w.disc->error.empty() || w.detectingOffset) return;
    if (!online::Config().accurateRip) {
        if (MessageBoxW(w.hwnd,
                        L"Verify CD rips with AccurateRip is off, so cordinal hasn't contacted anyone.\n\n"
                        L"Detecting your drive's read offset sends this disc's ids to accuraterip.com.\n\n"
                        L"Turn it on? It can be changed in Preferences > Online.",
                        L"Rip CD", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        if (const Tweak* tweak = FindTweak("online.accurate_rip")) SetTweak(*tweak, 1);
        if (!online::Config().accurateRip) return;
    }
    if (g_lookup) g_lookup->Stop();
    if (g_offsetWorker.joinable()) g_offsetWorker.join();
    g_lookup = std::make_shared<online::Cancel>();
    w.detectingOffset = true;
    ShowStatus();
    g_offsetWorker = std::thread([hwnd = w.hwnd, gen = g_generation, drive = w.disc->drive, toc = w.disc->toc,
                                  existing = w.disc->arPressings, hadAr = w.disc->arChecked, stop = g_lookup]() mutable {
        auto res = std::make_unique<OffsetResult>();
        if (!hadAr) {
            res->pressings = cdrip::FetchAccurateRip(toc, *stop);
            res->fetched = !stop->Stopped();
        } else {
            res->pressings = std::move(existing);
        }
        if (!stop->Stopped() && !res->pressings.empty()) {
            if (auto dev = cdrip::OpenDrive(drive, res->error))
                res->det = cdrip::DetectOffset(*dev, toc, res->pressings, stop->Flag());
        }
        if (!PostMessageW(hwnd, kOffsetDone, gen, (LPARAM)res.get())) return;
        res.release();
    });
}

void OnOffsetDone(std::unique_ptr<OffsetResult> res) {
    if (g_offsetWorker.joinable()) g_offsetWorker.join();
    w.detectingOffset = false;
    if (w.disc && res->fetched) {
        w.disc->arPressings = std::move(res->pressings);
        w.disc->arChecked = true;
    }
    ShowStatus();
    if (!res->error.empty()) {
        MessageBoxW(w.hwnd, res->error.c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    if (!res->det.found) {
        MessageBoxW(w.hwnd,
                    L"Couldn't match this disc against AccurateRip to calibrate your drive's read offset.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    const int current = (int)std::lround(g.ripOffset);
    const std::wstring signedOff = (res->det.offset > 0 ? L"+" : L"") + std::to_wstring(res->det.offset);
    if (res->det.offset == current) {
        MessageBoxW(w.hwnd,
                    (L"Your CD drive read offset (" + signedOff + L" samples) already matches AccurateRip across " +
                     std::to_wstring(res->det.matchedTracks) + (res->det.matchedTracks == 1 ? L" track." : L" tracks."))
                        .c_str(),
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (MessageBoxW(w.hwnd,
                    (L"AccurateRip matched this drive at read offset " + signedOff + L" samples (across " +
                     std::to_wstring(res->det.matchedTracks) + (res->det.matchedTracks == 1 ? L" track" : L" tracks") +
                     L").\n\nSave this as your CD drive read offset?")
                        .c_str(),
                    kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES) {
        if (const Tweak* tweak = FindTweak("conversion.rip_offset")) SetTweak(*tweak, (double)res->det.offset);
    }
}

void LayoutRip() {
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(14), lw = S(64), W = rc.right - pad * 2, x = pad + lw, half = (W - lw) / 2;
    int y = pad;
    MoveWindow(w.driveLabel, pad, y + S(4), lw, S(20), TRUE);
    MoveWindow(w.drive, x, y, S(90), S(200), TRUE);
    MoveWindow(w.namesLabel, x + S(104), y + S(4), S(56), S(20), TRUE);
    MoveWindow(w.names, x + S(160), y, W - lw - S(160), S(240), TRUE);
    y += S(34);
    MoveWindow(w.artistLabel, pad, y + S(3), lw, S(20), TRUE);
    MoveWindow(w.artist, x, y, half - S(8), S(24), TRUE);
    MoveWindow(w.dateLabel, x + half, y + S(3), S(40), S(20), TRUE);
    MoveWindow(w.date, x + half + S(44), y, S(110), S(24), TRUE);
    y += S(30);
    MoveWindow(w.albumLabel, pad, y + S(3), lw, S(20), TRUE);
    MoveWindow(w.album, x, y, W - lw, S(24), TRUE);
    y += S(32);
    const int bottom = rc.bottom - pad - S(28) - S(8) - S(26) - S(30) - S(32);
    MoveWindow(w.tracks, pad, y, W, std::max(S(80), bottom - y), TRUE);
    y = std::max(y + S(80), bottom) + S(6);
    MoveWindow(w.titleLabel, pad, y + S(3), lw, S(20), TRUE);
    MoveWindow(w.title, x, y, W - lw, S(24), TRUE);
    y += S(32);
    MoveWindow(w.formatLabel, pad, y + S(4), lw, S(20), TRUE);
    MoveWindow(w.format, x, y, S(250), S(200), TRUE);
    MoveWindow(w.detectOffset, x + S(260), y - S(1), S(124), S(26), TRUE);
    y += S(30);
    MoveWindow(w.whereLabel, pad, y + S(3), lw, S(20), TRUE);
    MoveWindow(w.path, x, y, W - lw - S(104), S(24), TRUE);
    MoveWindow(w.browse, rc.right - pad - S(96), y - S(1), S(96), S(26), TRUE);
    MoveWindow(w.status, pad, rc.bottom - pad - S(24), W - S(340), S(22), TRUE);
    MoveWindow(w.lookUp, rc.right - pad - S(332), rc.bottom - pad - S(28), S(108), S(28), TRUE);
    MoveWindow(w.go, rc.right - pad - S(216), rc.bottom - pad - S(28), S(108), S(28), TRUE);
    MoveWindow(w.cancel, rc.right - pad - S(100), rc.bottom - pad - S(28), S(100), S(28), TRUE);
    const int tabs[] = {S(28) * 4 / LOWORD(GetDialogBaseUnits()), (W - S(70)) * 4 / LOWORD(GetDialogBaseUnits())};
    SendMessageW(w.tracks, LB_SETTABSTOPS, 2, (LPARAM)tabs);
    InvalidateRect(w.tracks, nullptr, TRUE);
}

void BrowseFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Put ripped albums in this folder");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(w.hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    SetWindowTextW(w.path, path);
    CoTaskMemFree(path);
    UpdateButtons();
}

// Saves the choices, and hands the disc to the rip worker; the window closes once it starts.
void Start() {
    if (!Ready()) return;
    cdrip::Job job;
    job.toc = w.disc->toc;
    job.disc = w.chosen;
    job.disc.artist = ToUtf8(WindowText(w.artist));
    job.disc.album = ToUtf8(WindowText(w.album));
    job.disc.date = ToUtf8(WindowText(w.date));
    job.tracks = job.toc.Audio();
    job.folder = WindowText(w.path);
    job.flac = SendMessageW(w.format, CB_GETCURSEL, 0, 0) != 1;
    job.offset = (int)std::lround(g.ripOffset);
    job.threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    job.arPressings = w.disc->arPressings;
    job.arChecked = w.disc->arChecked;
    job.writeLog = true;
    job.driveName = w.disc->drive;
    IniWrite(L"Rip", L"Format", job.flac ? L"0" : L"1");
    IniWrite(L"Rip", L"Folder", job.folder);
    SHCreateDirectoryExW(nullptr, job.folder.c_str(), nullptr);
    const DWORD a = GetFileAttributesW(job.folder.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(w.hwnd, (L"Couldn't open or create " + job.folder).c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    g_titles.clear();
    for (size_t k = 0; k < job.tracks.size(); ++k) g_titles.push_back(TrackTitle(k));
    // The cover comes from the Cover Art Archive only when MusicBrainz covers are turned on.
    const bool cover = !job.disc.release.empty() && online::Config().covers[(int)online::CoverSource::MusicBrainz];
    if (g_lookup) g_lookup->Stop();
    if (g_offsetWorker.joinable()) g_offsetWorker.join();
    if (g_reader.joinable()) g_reader.join();
    g_ripping = true;
    g_cancel = false;
    if (g_ripper.joinable()) g_ripper.join();
    g_ripper = std::thread([hwnd = g.hwnd, drive = w.disc->drive, job = std::move(job), cover]() mutable {
        // Below normal, not background mode: background mode also lowers I/O priority, and the
        // drive's reads are what a rip waits on. The .cord encoder's own threads stay in background.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (!job.arChecked && online::Config().accurateRip && !g_cancel) {
            online::Cancel arStop;
            job.arPressings = cdrip::FetchAccurateRip(job.toc, arStop);
            job.arChecked = !g_cancel;
        }
        std::wstring error;
        auto r = std::make_unique<cdrip::Result>();
        if (cover) {
            online::Cancel stop;
            job.cover = cdrip::FetchCover(job.disc.release, stop);
        }
        if (auto dev = cdrip::OpenDrive(drive, error)) {
            int shown = -1;
            *r = cdrip::Rip(*dev, job, g_cancel, [&](size_t i, double f) {
                const int now = (int)i << 8 | (int)(f * 100);
                if (now != shown) PostMessageW(hwnd, WM_APP_RIP, 0, (LPARAM)(shown = now));
            });
        } else {
            r->tracks.push_back({L"", error, {}});
        }
        if (PostMessageW(hwnd, WM_APP_RIP, 1, (LPARAM)r.get())) r.release();
    });
    ShowMessage(L"Ripping " + std::to_wstring(g_titles.size()) + L" tracks\x2026");
    DestroyWindow(w.hwnd);
}

void NamesChosen() {
    const int sel = (int)SendMessageW(w.names, CB_GETCURSEL, 0, 0);
    if (!w.disc || sel < 0 || sel >= (int)w.disc->names.size()) return;
    w.chosen = w.disc->names[(size_t)sel];
    ShowNames();
}

void TrackChosen() {
    const int sel = (int)SendMessageW(w.tracks, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    w.filling = true;
    SetWindowTextW(w.title, TrackTitle((size_t)sel).c_str());
    w.filling = false;
}

void TitleEdited() {
    const int sel = (int)SendMessageW(w.tracks, LB_GETCURSEL, 0, 0);
    if (w.filling || !w.disc || sel < 0) return;
    if (w.chosen.titles.size() <= (size_t)sel) w.chosen.titles.resize((size_t)sel + 1);
    w.chosen.titles[(size_t)sel] = ToUtf8(WindowText(w.title));
    SendMessageW(w.tracks, LB_DELETESTRING, sel, 0);
    SendMessageW(w.tracks, LB_INSERTSTRING, sel, (LPARAM)TrackRow((size_t)sel).c_str());
    SendMessageW(w.tracks, LB_SETCURSEL, sel, 0);
}

LRESULT CALLBACK RipProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            const DWORD combo = CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, edit = ES_AUTOHSCROLL | WS_TABSTOP;
            w.driveLabel = make(0, WC_STATICW, L"&Drive", SS_LEFT, 0);
            w.drive = make(0, WC_COMBOBOXW, L"", combo, kIdDrive);
            w.namesLabel = make(0, WC_STATICW, L"&Names", SS_LEFT, 0);
            w.names = make(0, WC_COMBOBOXW, L"", combo, kIdNames);
            w.artistLabel = make(0, WC_STATICW, L"&Artist", SS_LEFT, 0);
            w.artist = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", edit, kIdArtist);
            w.dateLabel = make(0, WC_STATICW, L"Dat&e", SS_LEFT, 0);
            w.date = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", edit, kIdDate);
            w.albumLabel = make(0, WC_STATICW, L"A&lbum", SS_LEFT, 0);
            w.album = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", edit, kIdAlbum);
            w.tracks = make(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
                            WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_USETABSTOPS, kIdTracks);
            w.titleLabel = make(0, WC_STATICW, L"&Title", SS_LEFT, 0);
            w.title = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", edit, kIdTitle);
            w.formatLabel = make(0, WC_STATICW, L"&Format", SS_LEFT, 0);
            w.format = make(0, WC_COMBOBOXW, L"", combo, kIdFormat);
            for (const wchar_t* f : kFormats) SendMessageW(w.format, CB_ADDSTRING, 0, (LPARAM)f);
            SendMessageW(w.format, CB_SETCURSEL, IniRead(L"Rip", L"Format", L"0") == L"1" ? 1 : 0, 0);
            w.detectOffset = make(0, WC_BUTTONW, L"Detect &offset", BS_PUSHBUTTON | WS_TABSTOP, kIdDetectOffset);
            w.whereLabel = make(0, WC_STATICW, L"F&older", SS_LEFT, 0);
            std::wstring folder = IniRead(L"Rip", L"Folder", L"");
            if (folder.empty()) folder = g.libraryFolders.empty() ? MusicFolder() : g.libraryFolders.front();
            w.path = make(WS_EX_CLIENTEDGE, WC_EDITW, folder.c_str(), edit, kIdPath);
            w.browse = make(0, WC_BUTTONW, L"&Browse...", BS_PUSHBUTTON | WS_TABSTOP, kIdBrowse);
            w.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdStatus);
            w.lookUp = make(0, WC_BUTTONW, L"Look &up", BS_PUSHBUTTON | WS_TABSTOP, kIdLookUp);
            w.go = make(0, WC_BUTTONW, L"&Rip", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            w.cancel = make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND c : {w.artist, w.date, w.album, w.title, w.path}) SetWindowTheme(c, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            SetWindowTheme(w.tracks, g.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            for (HWND c : {w.drive, w.names, w.format, w.detectOffset, w.browse, w.lookUp, w.go, w.cancel})
                ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            w.drives = cdrip::ListDrives();
            for (const auto& d : w.drives) SendMessageW(w.drive, CB_ADDSTRING, 0, (LPARAM)d.c_str());
            SendMessageW(w.drive, CB_SETCURSEL, 0, 0);
            ReadDisc(online::Config().albumTags);
            return 0;
        }
        case WM_SIZE:
            LayoutRip();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(600), S(520)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.status ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case kDiscRead: {
            std::unique_ptr<DiscRead> d(reinterpret_cast<DiscRead*>(lp));
            if ((unsigned)wp == g_generation) OnDisc(std::move(d));
            return 0;
        }
        case kOffsetDone: {
            std::unique_ptr<OffsetResult> res(reinterpret_cast<OffsetResult*>(lp));
            if ((unsigned)wp == g_generation) OnOffsetDone(std::move(res));
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            switch (id) {
                case kIdDrive: if (code == CBN_SELCHANGE) ReadDisc(online::Config().albumTags); break;
                case kIdNames: if (code == CBN_SELCHANGE) NamesChosen(); break;
                case kIdTracks: if (code == LBN_SELCHANGE) TrackChosen(); break;
                case kIdTitle: if (code == EN_CHANGE) TitleEdited(); break;
                case kIdPath: if (code == EN_CHANGE) UpdateButtons(); break;
                case kIdBrowse: BrowseFolder(); break;
                case kIdDetectOffset: DetectOffsetNow(); break;
                case kIdLookUp: LookUp(); break;
                case IDOK: Start(); break;
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            if (g_lookup) g_lookup->Stop();
            ++g_generation;  // a disc still being read finds no window to show in
            if (g_offsetWorker.joinable()) g_offsetWorker.join();
            w = RipWindow{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowRipCd() {
    if (g_ripping) {
        ShowMessage(L"A CD is already being ripped.");
        return;
    }
    if (w.hwnd) {
        SetForegroundWindow(w.hwnd);
        return;
    }
    RegisterAppClass(kRipClass, RipProc);
    RECT r{0, 0, S(680), S(600)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kRipClass, L"Rip CD", WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - width) / 2, owner.top + S(40), width, height, g.hwnd, nullptr, g.inst,
                    nullptr);
    if (w.hwnd) ShowWindow(w.hwnd, SW_SHOW);
}

bool RipBusy() { return g_ripping; }
bool RipDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }

// UI thread, from WM_APP_RIP: progress (track << 8 | percent), or the finished rip, whose tracks
// join the library.
void OnRipMessage(WPARAM wp, LPARAM lp) {
    if (!wp) {
        const size_t i = (size_t)lp >> 8;
        if (i < g_titles.size())
            ShowMessage(L"Ripping " + std::to_wstring(i + 1) + L" of " + std::to_wstring(g_titles.size()) + L" (" +
                        std::to_wstring(lp & 0xFF) + L"%): " + g_titles[i]);
        return;
    }
    std::unique_ptr<cdrip::Result> r(reinterpret_cast<cdrip::Result*>(lp));
    if (g_ripper.joinable()) g_ripper.join();
    g_ripping = false;
    std::vector<std::wstring>().swap(g_titles);
    std::vector<std::wstring> files;
    std::wstring failed;
    for (const cdrip::TrackDone& t : r->tracks) {
        if (t.error.empty()) files.push_back(t.path);
        else if (failed.size() < 2000) failed += (t.path.empty() ? L"" : FileNameOf(t.path) + L"\n    ") + t.error + L"\n";
    }
    if (g.closing) {  // shutting down: the tracks join the library quietly before it is saved
        for (const std::wstring& f : files)
            if (const TrackId id = g_lib.Add(f); id != kNone) g_lib[id].flags |= Track::kInLibrary;
        g_lib.Touch();
        g.dirty = true;
        return;
    }
    if (!files.empty()) AppendFiles(0, files, false);
    const std::wstring done = cdrip::DoneText(*r);
    if (!failed.empty()) MessageBoxW(g.hwnd, (done + L"\n\n" + failed).c_str(), kAppName, MB_OK | MB_ICONWARNING);
    else if (!files.empty()) PlaySfx(Sfx::Organized);
    ShowMessage(done);
    UpdateStatus();
}

bool RipCommand(int id) {
    if (id == IDM_RIP_CD) {
        ShowRipCd();
        return true;
    }
    if (id == IDM_RIP_CANCEL) {
        if (g_ripping) {
            g_cancel = true;
            ShowMessage(L"Cancelling the rip\x2026");
        }
        return true;
    }
    return false;
}

// Shutdown: the track being ripped is abandoned (it leaves nothing behind), and the tracks already
// ripped join the library before it is saved.
void StopRipper() {
    if (w.hwnd) DestroyWindow(w.hwnd);
    if (g_lookup) g_lookup->Stop();
    if (g_offsetWorker.joinable()) g_offsetWorker.join();
    if (g_reader.joinable()) g_reader.join();
    g_cancel = true;
    if (!g_ripper.joinable()) return;
    g_ripper.join();
    MSG done;
    while (PeekMessageW(&done, g.hwnd, WM_APP_RIP, WM_APP_RIP, PM_REMOVE))
        if (done.wParam) OnRipMessage(done.wParam, done.lParam);
}

}  // namespace app
