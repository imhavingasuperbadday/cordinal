// Packs (packs.h, app_packs.cpp): the stored-ZIP and ini readers, every kind of pack file, $define
// functions and their bounds, the contrast check pack themes share with the built-in ones, and, in
// the window, turning a pack on and off.
#include "selftest_common.h"

#include "packs.h"
#include "app_packfiles.h"
#include <winioctl.h>

namespace {

// Directory junctions exercise link handling without requiring the symbolic-link privilege.
bool MakeJunction(const std::wstring& link, const std::wstring& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE file = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        const std::wstring substitute = L"\\??\\" + target;
        std::wstring paths = substitute + L'\0' + target + L'\0';
        struct Header { DWORD tag; WORD bytes, reserved, substituteAt, substituteBytes, printAt, printBytes; };
        std::vector<unsigned char> data(sizeof(Header) + paths.size() * sizeof(wchar_t));
        Header header{IO_REPARSE_TAG_MOUNT_POINT, (WORD)(data.size() - 8), 0, 0,
                      (WORD)(substitute.size() * sizeof(wchar_t)), (WORD)((substitute.size() + 1) * sizeof(wchar_t)),
                      (WORD)(target.size() * sizeof(wchar_t))};
        memcpy(data.data(), &header, sizeof(header));
        memcpy(data.data() + sizeof(header), paths.data(), paths.size() * sizeof(wchar_t));
        DWORD returned;
        ok = DeviceIoControl(file, FSCTL_SET_REPARSE_POINT, data.data(), (DWORD)data.size(), nullptr, 0,
                             &returned, nullptr) != FALSE;
        CloseHandle(file);
    }
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok;
}

void Put16(std::string& s, uint32_t v) { s += (char)(v & 0xFF), s += (char)(v >> 8 & 0xFF); }
void Put32(std::string& s, uint32_t v) { Put16(s, v & 0xFFFF), Put16(s, v >> 16); }

// A ZIP of `files` with every entry's method set to `method`: 0 stores the data, 8 deflates it (as
// one stored deflate block, which is still a deflate stream), anything else just names the method.
std::string MakeZip(const std::vector<std::pair<std::string, std::string>>& files, uint16_t method = 0, bool badCrc = false) {
    std::string zip, dir;
    for (const auto& [name, data] : files) {
        uint32_t crc = Crc32((const unsigned char*)data.data(), data.size()) ^ (badCrc ? 1u : 0u), offset = (uint32_t)zip.size();
        std::string body = data;
        if (method == 8) {
            body = "\x01";
            Put16(body, (uint32_t)data.size()), Put16(body, ~(uint32_t)data.size() & 0xFFFF);
            body += data;
        }
        Put32(zip, 0x04034B50), Put16(zip, 20), Put16(zip, 0), Put16(zip, method), Put32(zip, 0), Put32(zip, crc);
        Put32(zip, (uint32_t)body.size()), Put32(zip, (uint32_t)data.size()), Put16(zip, (uint32_t)name.size()), Put16(zip, 0);
        zip += name + body;
        Put32(dir, 0x02014B50), Put16(dir, 20), Put16(dir, 20), Put16(dir, 0), Put16(dir, method), Put32(dir, 0), Put32(dir, crc);
        Put32(dir, (uint32_t)body.size()), Put32(dir, (uint32_t)data.size()), Put16(dir, (uint32_t)name.size());
        Put16(dir, 0), Put16(dir, 0), Put16(dir, 0), Put16(dir, 0), Put32(dir, 0), Put32(dir, offset);
        dir += name;
    }
    uint32_t at = (uint32_t)zip.size();
    zip += dir;
    Put32(zip, 0x06054B50), Put16(zip, 0), Put16(zip, 0), Put16(zip, (uint32_t)files.size()), Put16(zip, (uint32_t)files.size());
    Put32(zip, (uint32_t)dir.size()), Put32(zip, at), Put16(zip, 0);
    return zip;
}

std::vector<PackEntry> Files(std::initializer_list<std::pair<const wchar_t*, std::string>> files) {
    std::vector<PackEntry> out;
    for (const auto& [path, bytes] : files) out.push_back({path, bytes});
    return out;
}

std::wstring UpperCopy(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

std::wstring Format(const wchar_t* pattern, std::initializer_list<std::pair<const wchar_t*, const wchar_t*>> fields) {
    return FormatTitle(pattern, [&](const std::wstring& name, std::wstring& out) {
        for (const auto& [k, v] : fields)
            if (name == k) return !(out = v).empty();
        return false;
    });
}

}  // namespace

void TestPacks(const std::wstring& scratch) {
    testlog::Group group(L"packs");

    {
        namespace files = app::packfiles;
        testlog::Group io(L"pack staging files");
        const std::wstring root = scratch + L"\\file-ops", from = root + L"\\source", to = root + L"\\copy";
        if (!Ready(files::Remove(root) == 0 && files::CreateDirectories(from + L"\\\x96EA\\empty") == 0 &&
                   WriteTestBytes(from + L"\\\x96EA\\bytes.bin", std::string("a\0b\xff", 4)), L"make pack file fixtures")) return;
        SetFileAttributesW((from + L"\\\x96EA\\bytes.bin").c_str(), FILE_ATTRIBUTE_READONLY);
        Check(files::CreateDirectories(from) == 0 && files::Exists(from) &&
                  !files::Exists(from + L"\\missing") && files::Remove(from + L"\\missing") == 0 &&
                  files::CreateDirectories(from + L"\\\x96EA\\bytes.bin\\child") != 0,
              L"pack directories tolerate existing/missing entries and refuse a file as a parent");
        Check(files::Copy(from, to) == 0 && ReadTestBytes(to + L"\\\x96EA\\bytes.bin") == std::string("a\0b\xff", 4) &&
                  files::Exists(to + L"\\\x96EA\\empty") &&
                  (GetFileAttributesW((to + L"\\\x96EA\\bytes.bin").c_str()) & FILE_ATTRIBUTE_READONLY) &&
                  (GetFileAttributesW((to + L"\\\x96EA\\empty").c_str()) & FILE_ATTRIBUTE_DIRECTORY),
              L"pack copies retain Unicode names, binary bytes, read-only attributes and empty nested directories");
        Check(files::Copy(from + L"\\\x96EA\\bytes.bin", to + L"\\\x96EA\\bytes.bin") != 0 &&
                  ReadTestBytes(to + L"\\\x96EA\\bytes.bin") == std::string("a\0b\xff", 4),
              L"a pack copy never overwrites an existing file");
        const std::wstring original = to + L"\\\x96EA\\bytes.bin", alias = to + L"\\alias.bin";
        if (CreateHardLinkW(alias.c_str(), original.c_str(), nullptr)) {
            Check(files::Same(original, alias) && files::Same(to, to + L"\\.\\\x96EA\\..") &&
                      !files::Same(original, from + L"\\\x96EA\\bytes.bin"),
                  L"pack identity follows file/directory identities rather than spelling or contents");
        } else testlog::Skip(L"pack file identity aliases: Windows did not allow creating a hard link");
        HANDLE held = CreateFileW(original.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (Ready(held != INVALID_HANDLE_VALUE, L"hold a pack file against deletion")) {
            DWORD failed = files::Remove(original);
            CloseHandle(held);
            Check(failed != 0 && ReadTestBytes(original) == std::string("a\0b\xff", 4),
                  L"a denied pack deletion leaves the original file intact");
        }
        SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_READONLY);
        Check(files::Remove(to) == 0 && !files::Exists(to),
              L"pack cleanup removes read-only files and the enclosing tree");
        std::wstring longDir = L"\\\\?\\" + root;
        for (int i = 0; i < 22; ++i) longDir += L"\\long-directory";
        const std::wstring longCopy = longDir + L"-copy";
        Check(files::CreateDirectories(longDir) == 0 && WriteTestBytes(longDir + L"\\\x96EA.bin", "long path") &&
                  files::Copy(longDir, longCopy) == 0 && ReadTestBytes(longCopy + L"\\\x96EA.bin") == "long path" &&
                  files::Remove(longCopy) == 0 && files::Remove(longDir) == 0,
              L"pack staging preserves extended-length Unicode paths");
        const std::wstring outside = root + L"\\outside", link = root + L"\\link", linkedCopy = root + L"\\linked-copy";
        if (!Ready(files::CreateDirectories(outside) == 0 && WriteTestBytes(outside + L"\\keep.bin", "keep"),
                   L"make a link's outside target")) return;
        if (MakeJunction(link, outside)) {
            Check(files::Same(link, outside) && files::Copy(link, linkedCopy) == 0 &&
                      !(GetFileAttributesW(linkedCopy.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) &&
                      ReadTestBytes(linkedCopy + L"\\keep.bin") == "keep" && files::Remove(link) == 0 &&
                      ReadTestBytes(outside + L"\\keep.bin") == "keep",
                  L"pack copy follows a source directory link; cleanup removes only the link");
            const std::wstring gone = root + L"\\gone";
            if (Ready(MakeJunction(gone, root + L"\\not-there"),
                      L"make a broken directory link"))
                Check(!files::Exists(gone) && files::Remove(gone) == 0,
                      L"a broken pack link is absent to lookup but can still be cleaned up");
        } else testlog::Skip(L"pack directory links: the filesystem did not allow creating a junction");
        Check(files::Move(outside, root + L"\\moved") == 0 && !files::Exists(outside) &&
                  ReadTestBytes(root + L"\\moved\\keep.bin") == "keep",
              L"pack publication renames a completed directory without changing its bytes");
        files::Remove(root);
    }

    // $define: arguments bind to %param%, built-ins can't be replaced, and recursion stays bounded.
    std::vector<std::wstring> errors;
    auto functions = ParseTitleFunctions(
        L"// stars for a 0-100 rating\n$define(stars, n, $repeat(*, $div(%n%, 20)))\n$define(twice, x, %x%%x%)\n"
        L"$define(if, a, b)\n$define(loop, x, $loop(%x%)x)\n$define(open, x", errors);
    Check(functions.size() == 3 && errors.size() == 2 && errors[0].find(L"built-in $if") != std::wstring::npos &&
              errors[1].find(L"closing") != std::wstring::npos,
          L"functions.tf keeps good definitions and names the line of each bad one (%zu kept, %zu refused)", functions.size(),
          errors.size());
    SetTitleFunctions(functions);
    std::wstring stars = Format(L"$stars(%rating%)|[$stars(%none%)]|$twice(ab)", {{L"rating", L"60"}});
    Check(stars == L"***||abab", L"a user function sees its arguments as fields and keeps [ ] truth: %ls", stars.c_str());
    std::wstring loop = Format(L"$loop(a)", {});
    Check(loop.size() <= 65536, L"a function that calls itself forever stops at the depth cap (%zu chars)", loop.size());
    Check(Format(L"$if(%a%,yes,no)", {{L"a", L"1"}}) == L"yes", L"built-in functions still win");
    SetTitleFunctions({});
    Check(Format(L"$twice(ab)", {}) == L"[UNKNOWN FUNCTION]", L"clearing the user functions removes them");

    // The ini reader: UTF-16 with a BOM, comments, sections, spaced keys and quoted values.
    std::wstring wide = L"; comment\r\n[Theme]\r\nPlaying Mark = \"> \"\r\n# also a comment\r\nname=Sea\r\n";
    std::string utf16 = "\xFF\xFE" + std::string((const char*)wide.data(), wide.size() * 2);
    auto ini = ParseIni(utf16);
    Check(ini.size() == 1 && ini[0].name == L"theme" && ini[0].Get(L"playing_mark") && *ini[0].Get(L"playing_mark") == L"> " &&
              ini[0].Get(L"name") && *ini[0].Get(L"name") == L"Sea",
          L"ini files read as UTF-16, keep quoted spaces and lowercase their names");

    // .cordpack: stored and deflated entries read back with their checksums; anything else is refused.
    std::vector<PackEntry> entries;
    std::wstring error;
    Check(ReadPackZip(MakeZip({{"Theme.ini", "[theme]\nname=A\n"}, {"fonts/", ""}}), entries, error) && entries.size() == 1 &&
              entries[0].path == L"theme.ini" && entries[0].bytes == "[theme]\nname=A\n",
          L"a stored .cordpack lists its files, lowercase, without folders");
    entries.clear();
    Check(ReadPackZip(MakeZip({{"pack.ini", "[pack]\nname=Z\n"}}, 8), entries, error) && entries.size() == 1 &&
              entries[0].bytes == "[pack]\nname=Z\n",
          L"a deflated entry, as Windows' Compress to ZIP makes, is unpacked");
    entries.clear();
    Check(!ReadPackZip(MakeZip({{"a.ini", "x"}}, 12), entries, error) && error.find(L"store or deflate") != std::wstring::npos,
          L"another compression method is refused with a reason");
    Check(!ReadPackZip(MakeZip({{"a.ini", "x"}}, 0, true), entries, error) && error.find(L"checksum") != std::wstring::npos,
          L"a damaged entry fails its checksum");
    Check(!ReadPackZip("not a zip at all, just text", entries, error), L"text is not a .cordpack");

    // Theme sounds: named in [sounds], or found at sounds/<name>.wav; a WAV decodes to stereo.
    const std::string wav = MakeSineWav(0.1, 1);
    Pack loud = ParsePack(L"loud", Files({
        {L"theme.ini", "[theme]\nname = Loud\n[sounds]\nrate = Sounds\\Click.wav\nboom = x.wav\nsparkle = gone.wav\n"},
        {L"sounds/click.wav", wav},
        {L"sounds/oops.wav", wav},
        {L"autoplaylists/rainy.txt", "name Rainy day\ncordinal-auto 1\nlimit 20 tracks random 0\ngroup all\nrule genre contains jazz\t\nend\n"},
        {L"autoplaylists/junk.txt", "hello"},
    }));
    auto has = [&](Sfx sfx) {
        return !loud.themes.empty() && std::any_of(loud.themes[0].sounds.begin(), loud.themes[0].sounds.end(),
                                                   [&](auto& x) { return x.first == sfx && x.second == wav; });
    };
    auto loudWarned = [&](const wchar_t* text) {
        return std::any_of(loud.warnings.begin(), loud.warnings.end(), [&](auto& w) { return w.find(text) != std::wstring::npos; });
    };
    Check(has(Sfx::Rate) && has(Sfx::Oops) && loud.themes[0].sounds.size() == 2 && loudWarned(L"no sound effect called boom") &&
              loudWarned(L"gone.wav isn't in the pack"),
          L"a theme's sounds come from [sounds] or sounds/<name>.wav, and mistakes are named");
    unsigned rate = 0;
    std::vector<float> stereo;
    Check(DecodePackWav(wav, rate, stereo) && rate > 0 && stereo.size() == 2 * (size_t)(rate / 10) &&
              std::any_of(stereo.begin(), stereo.end(), [](float v) { return std::fabs(v) > 0.1f; }) &&
              !DecodePackWav("RIFF....WAVEjunk", rate, stereo),
          L"a WAV sound decodes to stereo samples; anything else doesn't");
    AutoPlaylist rules;
    Check(loud.autoPlaylists.size() == 1 && loud.autoPlaylists[0].name == L"Rainy day" &&
              ParseAutoPlaylist(loud.autoPlaylists[0].rules, rules) && rules.limit == 20 && rules.root.rules.size() == 1 &&
              rules.root.rules[0].field == RuleField::Genre && loudWarned(L"junk.txt is not an auto-playlist"),
          L"auto-playlists are read in cordinal's own rule format, under their own name");
    Check(PackSummary(loud) == L"a theme, 2 sounds and an auto-playlist", L"a pack says what it holds: %ls", PackSummary(loud).c_str());

    // Fields, column sets, global keys, the tray tip, and a theme's rating mark and waveform colours.
    Pack extra = ParsePack(L"extra", Files({
        {L"fields.ini", "[fields]\ndecade = $div(%year%,10)0s\nempty =\n"},
        {L"columns.ini", "[set.classical]\nname = Classical\nshow = #, Title, Composer , , Duration\n[set.none]\nshow =\n"},
        {L"keys.ini", "[global keys]\nCtrl+Alt+Right = Next\n"},
        {L"formats.ini", "[formats]\ntray tip = %title%!\n"},
        {L"theme.ini", "[theme]\nname = Hearts\nbase = light\nrating mark = \xE2\x99\xA5\xE2\x99\xA5\n[waveform]\nplayed = #C8303A\n"},
        {L"themes/pale.ini", "[theme]\nname = Pale\nbase = light\n[waveform]\nunplayed = #FAFAFA\n"},
    }));
    auto extraWarned = [&](const wchar_t* text) {
        return std::any_of(extra.warnings.begin(), extra.warnings.end(), [&](auto& w) { return w.find(text) != std::wstring::npos; });
    };
    Check(extra.fields.size() == 1 && extra.fields[0].first == L"decade" && extra.fields[0].second == L"$div(%year%,10)0s",
          L"fields.ini names fields by their format; an empty one is left out");
    Check(extra.columnSets.size() == 1 && extra.columnSets[0].name == L"Classical" &&
              extra.columnSets[0].show == std::vector<std::wstring>{L"#", L"Title", L"Composer", L"Duration"} &&
              extra.columns.empty() && extraWarned(L"[set.none] shows no columns"),
          L"a column set lists column names, and isn't a column itself");
    Check(extra.globalKeys.size() == 1 && extra.keys.empty() && extra.globalKeys[0].command == L"Next" &&
              extra.formats[kPackTray] == L"%title%!",
          L"global keys and the tray tip are read apart from the window's keys and formats");
    Check(extra.themes.size() == 2 && extra.themes[0].ratingMark == L"\x2665" && extra.themes[0].wavePlayed == 0xC8303A &&
              extra.themes[0].waveRest == kNoRgb && extra.themes[0].legibility.empty() &&
              extra.themes[1].legibility.find(L"waveform unplayed on surface") == 0,
          L"a theme draws ratings with its own mark and the waveform in its own colours, held to 3:1");

    // Every kind of pack file, read leniently.
    Pack pack = ParsePack(L"sea", Files({
        {L"pack.ini", "[pack]\nformat = 1\nname = Sea\nversion = 1.0\n"},
        {L"theme.ini", "[theme]\nname = Sea\nbase = dark\nfont = Fira Sans\nsize = 13\n[palette]\nbg = #0B1D2A\nsurface = 10283A\n"
                       "accent = #5CC8FF\nshade = #123456\nborder = blue\n"},
        {L"themes/fog.ini", "[theme]\nname = Fog\n[palette]\ntext = #EEEEEE\nbg = #FFFFFF\n"},
        {L"columns.ini", "[bitrate]\ntitle = kbps\nformat = [%bitrate%]\nalign = right\nwidth = 60\n[broken]\ntitle = x\n"},
        {L"groups.ini", "[groups]\nby = %genre%\ntitle = %genre%\ncover = no\n"},
        {L"formats.ini", "[formats]\nwindow title = %title% - Sea\nstatus bar = \" %codec% \"\n"},
        {L"keys.ini", "[keys]\nCtrl+Alt+N = Playback > Next\n"},
        {L"eq/warm.ini", "[warm]\nbands = 3, 2, 1, 0, 0, 0, 0, -1, -2, 40\n"},
        {L"layout.ini", "[layout]\nname = Sea side\nplayer = left\n"},
        {L"later.ini", "[pack]\nformat = 9\n"},
        {L"notes.ini", "[x]\ny = 1\n"},
        {L"readme.txt", "hello"},
    }));
    auto warned = [&](const wchar_t* text) {
        return std::any_of(pack.warnings.begin(), pack.warnings.end(), [&](auto& w) { return w.find(text) != std::wstring::npos; });
    };
    Check(pack.error.empty() && pack.name == L"Sea" && pack.version == L"1.0", L"pack.ini names the pack");
    Check(pack.themes.size() == 2 && pack.themes[0].name == L"Sea" && pack.themes[0].palette.dark &&
              pack.themes[0].palette.bg == 0x0B1D2A && pack.themes[0].palette.surface == 0x10283A &&
              pack.themes[0].palette.text == DarkPalette().text && pack.themes[0].font == L"Fira Sans" && pack.themes[0].fontPx == 13,
          L"a theme takes its colours from the pack and the rest from its base palette");
    Check(warned(L"no palette colour is called shade") && warned(L"border is not a #RRGGBB"),
          L"an unknown colour role and a bad colour are named in the warnings");
    Check(pack.themes[1].legibility.find(L"text on bg") == 0,
          L"a theme that breaks the contrast rules names the pair: %ls", pack.themes[1].legibility.c_str());
    Check(pack.columns.size() == 1 && pack.columns[0].key == L"bitrate" && pack.columns[0].title == L"kbps" &&
              pack.columns[0].sort == L"[%bitrate%]" && pack.columns[0].right && pack.columns[0].width == 60 &&
              warned(L"[broken] has no format"),
          L"columns are format strings; one without a format is left out");
    Check(pack.groups.any && pack.groups.by == L"%genre%" && !pack.groups.cover, L"group headers are format strings");
    Check(pack.formats[kPackWindow] == L"%title% - Sea" && pack.formats[kPackStatus] == L" %codec% " &&
              pack.formats[kPackNowPlaying].empty(),
          L"formats set the window title and status bar, and leave the rest alone");
    Check(pack.keys.size() == 1 && pack.keys[0].keys == L"ctrl+alt+n" && pack.keys[0].command == L"Playback > Next",
          L"a key map pairs keys with command names");
    Check(pack.presets.size() == 1 && pack.presets[0].name == L"warm" && pack.presets[0].gains[0] == 3 &&
              pack.presets[0].gains[7] == -1 && pack.presets[0].gains[9] == 12,
          L"EQ presets are clamped to the equalizer's range");
    Check(pack.layouts.size() == 1 && pack.layouts[0].name == L"Sea side" && pack.layouts[0].values.Get(L"player"),
          L"a layout preset keeps its values for Customize layout");
    Check(warned(L"later.ini needs a later cordinal") && warned(L"notes.ini is not a file packs use") && !warned(L"readme"),
          L"a file for a later pack format and an unknown ini are named; other files are ignored");
    Pack future = ParsePack(L"future", Files({{L"pack.ini", "[pack]\nformat = 2\n"}, {L"theme.ini", "[theme]\nname=x\n"}}));
    Check(!future.error.empty() && future.themes.empty(), L"a pack for a later pack format is refused whole");
    Pack empty = ParsePack(L"empty", Files({{L"readme.txt", "hi"}}));
    Check(!empty.error.empty(), L"a pack with nothing to use says so");
}

// In the window: a pack found in the folder stays off until turned on; on, it adds its theme, column,
// formats, key and functions; off again, everything it changed goes back.
void TestPackIntegration(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"packs in the window");
    const std::wstring dataDir = g.dataDir, dir = scratch + L"\\packs\\sea";
    const ThemeMode original = g.themeMode, originalDefault = g.defaultTheme;
    g.dataDir = scratch;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    WriteTestBytes(dir + L"\\pack.ini", "[pack]\nname = Sea\n");
    WriteTestBytes(dir + L"\\theme.ini", "[theme]\nname = Sea\nbase = dark\nrating mark = \xE2\x99\xA5\n"
                                          "[palette]\nbg = #0B1D2A\nsurface = #10283A\n[waveform]\nplayed = #5CC8FF\n");
    WriteTestBytes(dir + L"\\columns.ini", "[mood]\ntitle = Mood\nformat = $twice(%title%)\n"
                                            "[set.short]\nname = Short\nshow = Title, mood, Nonsense\n");
    WriteTestBytes(dir + L"\\formats.ini", "[formats]\nwindow title = Sea: %title%\n");

    WriteTestBytes(dir + L"\\functions.tf", "$define(twice, x, %x%%x%)\n");
    WriteTestBytes(dir + L"\\fields.ini", "[fields]\nshout = $upper(%title%)\ntitle = never used\n");
    WriteTestBytes(dir + L"\\keys.ini", "[keys]\nCtrl+Alt+N = Playback > Next\nK = Playback > Next\n"
                                         "[global keys]\nCtrl+Alt+Shift+F24 = Playback > Shuffle\n");
    CreateDirectoryW((dir + L"\\sounds").c_str(), nullptr);
    WriteTestBytes(dir + L"\\sounds\\rate.wav", MakeSineWav(0.05, 1));
    CreateDirectoryW((dir + L"\\autoplaylists").c_str(), nullptr);
    WriteTestBytes(dir + L"\\autoplaylists\\jazz.txt", "name Jazz\ncordinal-auto 1\nlimit 0 tracks random 0\ngroup all\nrule genre contains jazz\t\nend\n");

    LoadPacks(false);
    const std::wstring ownWindow = g.fmtWindow;  // settings.ini's own, which packs may cover but never change
    Check(ThemeTotal() == (int)ThemeMode::Count && ColumnTotal() == kColumnCount && ownWindow != L"Sea: %title%" &&
              IniRead(L"Packs", L"On", L"").empty() && IniRead(L"Packs", L"Known", L"") == L"sea",
          L"a new pack changes nothing until it is turned on");
    OnCommand(IDM_PACK_BASE, 0, nullptr);
    Check(ThemeTotal() == (int)ThemeMode::Count + 1 && ColumnTotal() == kColumnCount + 1 && g.fmtWindow == L"Sea: %title%" &&
              IniRead(L"Packs", L"On", L"") == L"sea",
          L"turning a pack on adds its theme, its column and its window title, and is remembered");
    Check(Format(L"$twice(ab)", {}) == L"abab", L"its $define functions reach every title format");
    ACCEL keys[256];
    int count = CopyAcceleratorTableW(g.accel, keys, (int)std::size(keys));
    bool bound = false, typed = false;
    for (int i = 0; i < count; ++i) {
        bound |= keys[i].key == 'N' && (keys[i].fVirt & (FCONTROL | FALT)) == (FCONTROL | FALT) && keys[i].cmd == IDM_NEXT;
        typed |= keys[i].key == 'K' && !(keys[i].fVirt & (FCONTROL | FALT));
    }
    Check(bound && !typed, L"its keys reach the menu command they name, and a bare letter is refused");

    OnCommand(IDM_PACK_THEME_BASE, 0, nullptr);
    g.defaultTheme = g.themeMode;  // as if the startup-default review had kept it
    SavePreferences();
    Check(PackThemeMode(g.themeMode) && P().bg == 0x0B1D2A && IniRead(L"Window", L"PackTheme", L"") == L"sea/Sea" &&
              IniInt(L"Window", L"Theme", -1) == (int)ThemeMode::Dark,
          L"a pack theme applies, and is saved by name beside its built-in fallback");
    Check(PackThemeHasSound(Sfx::Rate) && !PackThemeHasSound(Sfx::Dice), L"the pack theme brings its own sound for the effects it names");
    Check(std::wstring(ThemeInfo(g.themeMode).ratingMark) == L"\x2665" && WaveformPlayedColor() == Ref(0x5CC8FF),
          L"the pack theme's rating mark and waveform colour are the ones in use");
    TrackId first = g_lib.Count() ? 0 : kNone;
    std::wstring title;
    if (first != kNone) LookupField(first, L"title", title);
    Check(first != kNone && FormatTitle(L"%shout%|%title%", Lookup(first, false)) == UpperCopy(title) + L"|" + title,
          L"a pack's field works in every format, and can't replace one of cordinal's");
    const int shuffle = g.shuffle;
    Check(PackHotkey(0x5000) && g.shuffle != shuffle, L"a pack's global key runs the command it names");
    OnCommand(IDM_SHUFFLE, 0, nullptr);
    OnCommand(IDM_PACK_COLSET_BASE, 0, nullptr);
    Check(g.columns == std::vector<int>{0, 2, kColumnCount}, L"a column set shows its columns by name, in its order");
    OnCommand(IDM_COLUMN_BASE + kColumnCount, 0, nullptr);  // hidden again, for the check below
    const size_t lists = g.playlists.size();
    OnCommand(IDM_PACK_AUTO_BASE, 0, nullptr);
    Check(g.playlists.size() == lists + 1 && g.playlists.back().kind == PlaylistKind::Auto && g.playlists.back().name == L"Jazz",
          L"a pack's auto-playlist is added from the Playlist menu");
    RemovePlaylist((int)g.playlists.size() - 1);
    OnCommand(IDM_COLUMN_BASE + kColumnCount, 0, nullptr);
    Check(SubitemOf(kColumnCount) >= 0 && IniRead(L"Columns", L"PackColumns", L"") == L"sea/mood",
          L"a pack column can be shown, and is saved by name");

    OnCommand(IDM_PACK_BASE, 0, nullptr);
    Check(g.themeMode == ThemeMode::Dark && ThemeTotal() == (int)ThemeMode::Count && SubitemOf(kColumnCount) < 0 &&
              g.fmtWindow == ownWindow && Format(L"$twice(ab)", {}) == L"[UNKNOWN FUNCTION]",
          L"turning it off leaves the built-in theme under it and takes its column, formats and functions away");
    Check(g.defaultTheme == ThemeMode::Dark, L"a default pack theme whose pack went off falls back the same way");
    OnCommand(IDM_PACK_BASE, 0, nullptr);
    OnCommand(IDM_PACK_THEME_BASE, 0, nullptr);
    g.defaultTheme = g.themeMode;
    SavePreferences();
    LoadPacks(true);
    Check(ThemeTotal() == (int)ThemeMode::Count && ReadSavedTheme() == ThemeMode::Dark,
          L"safe mode starts without packs, on the pack theme's built-in fallback");

    // Dropping a pack installs it (here without the question) and turns it on; music isn't taken.
    std::filesystem::remove_all(scratch + L"\\packs");
    IniWrite(L"Packs", L"On", L"");
    LoadPacks(false);
    const std::wstring outside = scratch + L"\\downloads\\Night";
    SHCreateDirectoryExW(nullptr, outside.c_str(), nullptr);
    WriteTestBytes(outside + L"\\pack.ini", "[pack]\nname = Night\n");
    WriteTestBytes(outside + L"\\theme.ini", "[theme]\nname = Night\nbase = dark\n");
    WriteTestBytes(scratch + L"\\downloads\\song.wav", MakeSineWav(0.05, 1));
    Check(!InstallPack(scratch + L"\\downloads\\song.wav", false) && !InstallPack(scratch + L"\\downloads", false),
          L"music, and a folder without a pack.ini, are not packs");
    Check(InstallPack(outside, false) && GetFileAttributesW((scratch + L"\\packs\\Night\\theme.ini").c_str()) != INVALID_FILE_ATTRIBUTES &&
              IniRead(L"Packs", L"On", L"").find(L"Night") != std::wstring::npos && ThemeTotal() == (int)ThemeMode::Count + 1,
          L"a dropped pack folder is copied into the packs folder and turned on");
    {
        const std::wstring blocked = outside + L"\\one\\two\\three\\locked.bin";
        if (Ready(app::packfiles::CreateDirectories(DirectoryOf(blocked)) == 0 && WriteTestBytes(blocked, "held"),
                  L"make a blocked ancillary pack file")) {
            HANDLE held = CreateFileW(blocked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (Ready(held != INVALID_HANDLE_VALUE, L"hold a pack copy source")) {
                const std::string installed = ReadTestBytes(scratch + L"\\packs\\Night\\theme.ini");
                bool handled = InstallPack(outside, false);
                CloseHandle(held);
                Check(handled && ReadTestBytes(scratch + L"\\packs\\Night\\theme.ini") == installed &&
                          !app::packfiles::Exists(scratch + L"\\packs\\~install-Night"),
                      L"a failed recursive pack copy leaves the installed pack byte-identical and removes its staging tree");
            }
            app::packfiles::Remove(outside + L"\\one");
        }
    }
    WriteTestBytes(scratch + L"\\downloads\\Night.zip", MakeZip({{"pack.ini", "[pack]\nname = Night 2\n"}, {"theme.ini", "[theme]\nname = Night\n"}}, 8));
    Check(InstallPack(scratch + L"\\downloads\\Night.zip", false) &&
              GetFileAttributesW((scratch + L"\\packs\\Night.cordpack").c_str()) != INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW((scratch + L"\\packs\\Night").c_str()) == INVALID_FILE_ATTRIBUTES && ThemeTotal() == (int)ThemeMode::Count + 1,
          L"a zipped pack of the same name replaces the installed one, which is gone");
    {
        const std::wstring installed = scratch + L"\\packs\\Night", archive = installed + L".cordpack";
        if (Ready(app::packfiles::CreateDirectories(installed) == 0 && WriteTestBytes(installed + L"\\keep.bin", "previous"),
                  L"make both installed pack forms for rollback")) {
            const std::string bytes = ReadTestBytes(archive);
            HANDLE held = CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (Ready(held != INVALID_HANDLE_VALUE, L"hold the second pack form against renaming")) {
                bool handled = InstallPack(outside, false);
                CloseHandle(held);
                Check(handled && ReadTestBytes(installed + L"\\keep.bin") == "previous" && ReadTestBytes(archive) == bytes &&
                          !app::packfiles::Exists(scratch + L"\\packs\\~old-Night") &&
                          !app::packfiles::Exists(scratch + L"\\packs\\~install-Night"),
                      L"a failed second pack rename restores the first form and preserves the locked archive");
            }
        }
    }

    // Make a pack: the current look, written as files that read back to the same look.
    const std::wstring made = MakeStarterPack(false);
    Pack mine;
    {
        std::vector<PackEntry> files;
        for (const auto& e : std::filesystem::recursive_directory_iterator(made)) {
            if (!e.is_regular_file()) continue;
            std::wstring rel = std::filesystem::relative(e.path(), made).wstring();
            for (auto& c : rel) c = c == L'\\' ? L'/' : (wchar_t)towlower(c);
            files.push_back({rel, ReadTestBytes(e.path().wstring())});
        }
        mine = ParsePack(L"mine", files);
    }
    const Palette& now = P();
    Check(!made.empty() && mine.error.empty() && mine.themes.size() == 1 && mine.themes[0].palette.bg == now.bg &&
              mine.themes[0].palette.selectText == now.selectText && mine.themes[0].palette.dark == now.dark &&
              mine.formats[kPackWindow] == g.fmtWindow && mine.layouts.size() == 1 && mine.presets.size() == 1 &&
              mine.columns.size() == 1 && mine.columnSets.size() == 1 && mine.fields.size() == 1 && !mine.groups.any &&
              mine.keys.empty() && mine.globalKeys.empty() && !mine.functions.empty(),
          L"Make a pack writes the current theme, formats, layout and EQ as a pack that reads back the same");

    std::filesystem::remove_all(scratch + L"\\packs");
    std::filesystem::remove_all(scratch + L"\\downloads");
    IniWrite(L"Packs", L"On", L"");
    LoadPacks(false);
    g.dataDir = dataDir;
    g.themeMode = original;
    g.defaultTheme = originalDefault;
    ApplyTheme();
    ApplyPackFormats();
}
