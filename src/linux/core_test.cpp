// A command-line check of the portable core on Linux, without the UI:
//   cordinal-core-test scan DIR               list the audio files under DIR with their tags
//   cordinal-core-test decode FILE            decode FILE completely and report it
//   cordinal-core-test play FILE [SECONDS]    play FILE through the engine and PipeWire
//   cordinal-core-test replaygain FILE...     measure the files' ReplayGain as one album
//   cordinal-core-test tags FILE DIR          edit FILE's tags with an undo journal in DIR, then undo
//   cordinal-core-test convert FILE DIR       convert FILE to .cord with its log in DIR, then undo the conversion
//   cordinal-core-test organize FILE ROOT     organize FILE (and its folder's cover) under ROOT, then undo it
//   cordinal-core-test copy FILE DEST         copy FILE to DEST, find it again, then replace it with AAC
//   cordinal-core-test eq                     check the equalizer's boost through the renderer
//   cordinal-core-test waveform               check live notes and whole-track peak previews
//   cordinal-core-test weighted-shuffle       check that skip-aware shuffle keeps every occurrence
//   cordinal-core-test chapters FILE          list FILE's chapters: the first, the last and the next start
//   cordinal-core-test library FILE           list a saved library.cdb: files, titles and ReplayGain
//   cordinal-core-test history FILE           list a saved library.cdb's listens and points
//   cordinal-core-test playlists FILE         list a saved library.cdb: playlists, items, inbox and ratings
//   cordinal-core-test listenbrainz DIR       queue a listen and keep a token in DIR, check a bogus token
//   cordinal-core-test devices                list the PipeWire output devices
//   cordinal-core-test helpers DIR            read, trust and run a helper made of two shell scripts
//   cordinal-core-test sfx                    play a clip and an effect on the sound effect stream
//   cordinal-core-test steam [cancel]         list discovered Steam soundtrack folders; "cancel" lists none
//   cordinal-core-test away DIR               the away-from-home pieces: JSON, STUN, keys, certificates and HTTPS
//   cordinal-core-test web DIR CONTROL        serve DIR over HTTP; CONTROL gets the port and pairing code
#include "audio_output.h"
#include "convert.h"
#include "decoder.h"
#include "devicecopy.h"
#include "engine.h"
#include "helpers.h"
#include "library.h"
#include "listenbrainz.h"
#include "organize.h"
#include "peaks.h"
#include "playlog.h"
#include "playlist_shuffle.h"
#include "points.h"
#include "replaygain.h"
#include "scanner.h"
#include "sfx.h"
#include "steam.h"
#include "tags.h"
#include "tagwriter.h"
#include "tagundo.h"
#include "web_internal.h"
#include "web_remote.h"
#include "web_tls.h"
#include "qr.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <algorithm>

namespace {

std::wstring Wide(const char* s) { return compat::Widen(s); }
std::string Narrow(const std::wstring& s) { return compat::Narrow(s.c_str()); }

int Serve(const char* directory, const char* control) {
    std::vector<ScanFile> files;
    if (!ListAudioFiles(Wide(directory), files)) return 1;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    auto snapshot = std::make_shared<web::Snapshot>(); snapshot->revision = 1;
    for (const auto& file : files) {
        TagData tags; if (!ReadTags(file.path, tags)) continue;
        Library library;
        auto id = library.Add(file.path);
        if (id == kInvalidTrack || library.Path(id) != file.path) return 2;
        auto alternate = file.path;
        size_t at = alternate.find_last_of('/') + 1;
        for (size_t i = at; i < alternate.size(); ++i) if (alternate[i] >= 'a' && alternate[i] <= 'z') {
            alternate[i] -= 'a' - 'A';
            if (library.Add(alternate) == id) return 3;
            break;
        }
        web::Track t; t.key = "fixture-" + std::to_string(snapshot->tracks.size()); t.path = file.path;
        t.title = tags.title; t.artist = tags.artist; t.album = tags.album; t.albumArtist = tags.albumArtist;
        t.duration = tags.duration; t.number = tags.track; t.library = true; t.codec = ToUtf8(CodecName(tags.codec));
        t.mime = web::Mime(file.path, t.codec); t.original = !IsSubtrack(file.path) && !t.mime.empty();
        snapshot->tracks.push_back(std::move(t));
    }
    web::Server server; web::Config config; config.port = 0;
    if (!server.Start(config) || !server.Pair()) return 4;
    server.Publish(snapshot);
    auto status = server.GetStatus();
    FILE* output = fopen(control, "w"); if (!output) return 5;
    fprintf(output, "{\"port\":%u,\"code\":%s}\n", status.port, web::Json(status.pairingCode).c_str()); fclose(output);
    std::string stop = std::string(control) + ".stop";
    for (unsigned i = 0; i < 3000 && GetFileAttributesW(Wide(stop.c_str()).c_str()) == INVALID_FILE_ATTRIBUTES; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.Stop(); remove(control); remove(stop.c_str()); return 0;
}

int Scan(const char* dir) {
    std::vector<ScanFile> files;
    if (!ListAudioFiles(Wide(dir), files)) {
        fprintf(stderr, "can't list %s\n", dir);
        return 1;
    }
    for (const ScanFile& f : files) {
        TagData t;
        bool ok = ReadTags(f.path, t);
        printf("%s\t%s\t%s\t%s\t%.2f s\t%u Hz\n", Narrow(f.path).c_str(), ok ? t.artist.c_str() : "?",
               ok ? t.title.c_str() : "?", ok ? t.album.c_str() : "?", t.duration, t.sampleRate);
    }
    printf("%zu files\n", files.size());
    return 0;
}

int Decode(const char* file) {
    std::wstring error;
    auto d = OpenDecoder(Wide(file), 48000, error);
    if (!d) {
        fprintf(stderr, "%s\n", Narrow(error).c_str());
        return 1;
    }
    std::vector<float> buf(4096 * 2);
    size_t total = 0;
    float peak = 0;
    while (size_t n = d->Read(buf.data(), 4096)) {
        total += n;
        for (size_t i = 0; i < n * 2; ++i) peak = std::max(peak, std::fabs(buf[i]));
    }
    printf("%zu frames at %u Hz (%.3f s, reported %.3f s), peak %.4f\n", total, d->Rate(), (double)total / d->Rate(),
           d->Duration(), peak);
    return total ? 0 : 1;
}

int Play(const char* file, double seconds) {
    std::mutex mx;
    std::condition_variable cv;
    bool started = false, ended = false;
    SetPostMessageHandler([&](HWND, UINT msg, WPARAM wp, LPARAM) {
        if (msg != WM_ENGINE) return;
        std::lock_guard lock(mx);
        if ((EngineEvent)wp == EngineEvent::TrackStarted) started = true;
        if ((EngineEvent)wp == EngineEvent::Ended || (EngineEvent)wp == EngineEvent::Error) ended = true;
        cv.notify_all();
    });
    Engine engine;
    std::wstring error;
    if (!engine.Init(reinterpret_cast<HWND>(1), error)) {
        fprintf(stderr, "engine: %s\n", Narrow(error).c_str());
        return 1;
    }
    if (getenv("CORDINAL_EXCLUSIVE")) engine.SetExclusive(true);
    PlayItem item;
    item.token = 1;
    item.path = Wide(file);
    engine.Play(item);
    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        {
            std::unique_lock lock(mx);
            cv.wait_for(lock, std::chrono::milliseconds(500));
            if (ended) break;
        }
        Engine::OutputInfo o = engine.Output();
        printf("%s %.2f / %.2f s  %s %u Hz%s %s\n", started ? "playing" : "starting", engine.Position(), engine.Duration(),
               o.exclusive ? "exclusive" : "shared", o.rate, o.bitPerfect ? " bit-perfect" : "", Narrow(o.problem).c_str());
        fflush(stdout);
    }
    std::wstring err = engine.TakeError();
    if (!err.empty()) fprintf(stderr, "error: %s\n", Narrow(err).c_str());
    engine.Shutdown();
    return started ? 0 : 1;
}

// Measures each file as the player's scanner does and prints the gain it stores; fails if any can't
// be measured or the album gain doesn't follow from the tracks.
int MeasureReplayGain(int count, char** files) {
    std::atomic<bool> cancel{false};
    std::atomic<std::shared_ptr<Decoder>> current;
    std::vector<TrackId> ids;
    for (int i = 0; i < count; ++i) {
        TrackId id = g_lib.Add(Wide(files[i]));
        TagData tags;
        if (ReadTags(Wide(files[i]), tags)) g_lib.ApplyTags(id, tags);
        RgResult r = replaygain::Measure(g_lib.SnapshotPath(id), 0, 0, cancel, current, nullptr);
        if (!r.ok) {
            fprintf(stderr, "%s: couldn't be measured\n", files[i]);
            return 1;
        }
        replaygain::Apply(r);
        ids.push_back(id);
        printf("%s\t%+.2f dB\tpeak %.4f\n", files[i], g_lib[id].rgTrackGain / 100.0, g_lib[id].rgTrackPeak / 20000.0);
    }
    replaygain::UpdateAlbumGains(ids);
    const Track& t = g_lib[ids[0]];
    float gain = replaygain::Gain(t, ReplayGainOptions{}, 0);
    printf("album %+.2f dB, plays at %+.2f dB\n", t.rgAlbumGain / 100.0, 20 * std::log10(gain));
    return t.rgAlbumGain == kNoGain || !std::isfinite(gain) ? 1 : 0;
}

// Edits a file's tags with an undo journal, then undoes the edit, as the tag editor does. The
// file must come back with its old title and no ReplayGain tags. Works on FILE itself: pass a copy.
int TagRoundTrip(const char* file, const char* journalDir) {
    std::wstring path = Wide(file), error;
    TagData before, after, undone;
    if (!ReadTags(path, before)) return 1;
    TagEdit edit;
    edit.title = "Edited on Linux \xC3\x84";
    edit.rgTrackGain = "-3.25 dB";
    edit.rgTrackPeak = "0.500000";
    TagJournal journal;
    TagUndoFile undo;
    if (!CaptureTagUndo(path, edit, undo)) return 2;
    journal.files.push_back(std::move(undo));
    if (!SaveTagJournal(Wide(journalDir), journal, error) || !WriteTags(path, edit, error)) {
        fprintf(stderr, "%s\n", Narrow(error).c_str());
        return 3;
    }
    if (!ReadTags(path, after) || after.title != *edit.title || std::fabs(after.rgTrackGain + 3.25f) > 0.01f) return 4;
    std::wstring newest;
    uint32_t files = 0;
    TagJournal saved;
    if (!FindNewestTagJournal(Wide(journalDir), newest, files) || files != 1 || !LoadTagJournal(newest, saved)) {
        fprintf(stderr, "no undo journal in %s (newest \"%s\", %u files)\n", journalDir, Narrow(newest).c_str(), files);
        return 5;
    }
    if (!WriteTags(path, saved.files[0].old, error)) {
        fprintf(stderr, "%s\n", Narrow(error).c_str());
        return 6;
    }
    DeleteTagJournal(newest);
    if (!ReadTags(path, undone) || undone.title != before.title || !std::isnan(undone.rgTrackGain)) return 7;
    printf("edited \"%s\" and put back \"%s\"\n", after.title.c_str(), undone.title.c_str());
    return 0;
}

// Organizes FILE under ROOT with the default pattern (its tags must give an album artist, album, track
// number and title), with the undo log in ROOT/log. The folder's cover image must follow it and the
// emptied folder must go; the undo must put both files back and leave no organized folders behind.
int OrganizeRoundTrip(const char* file, const char* rootArg) {
    const std::wstring path = Wide(file), root = Wide(rootArg), logDir = root + L"/log", folder = DirectoryOf(path);
    const std::wstring cover = folder + L"/cover.jpg";
    TagData tags;
    if (!ReadTags(path, tags)) return 2;
    const TrackId id = g_lib.Add(path);
    g_lib.ApplyTags(id, tags);
    const auto plan = PlanOrganize({id}, root, kDefaultOrganizePattern);
    const std::wstring expected = root + L"/" + Wide(tags.albumArtist.c_str()) + L"/" + Wide(tags.album.c_str()) + L"/" +
                                  (tags.track < 10 ? L"0" : L"") + std::to_wstring(tags.track) + L" " + Wide(tags.title.c_str()) +
                                  L"." + ExtOf(path);
    if (plan.size() != 1 || !plan[0].skip.empty() || plan[0].to != expected) {
        fprintf(stderr, "planned %s, expected %s\n", plan.empty() ? "nothing" : Narrow(plan[0].to + L" " + plan[0].skip).c_str(),
                Narrow(expected).c_str());
        return 3;
    }
    const OrganizeResult result = ExecuteOrganize(plan, logDir, {root});
    for (const auto& [from, why] : result.failed) fprintf(stderr, "%s: %s\n", Narrow(from).c_str(), Narrow(why).c_str());
    if (result.moved.size() != 1 || result.moved[0].second != expected) return 4;
    auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    if (!exists(expected) || exists(path) || !exists(DirectoryOf(expected) + L"/cover.jpg")) return 5;
    if (exists(folder)) return 6;  // the emptied source folder is removed
    std::wstring error;
    const auto undone = UndoLastOrganize(logDir, {root}, error);
    if (!error.empty()) {
        fprintf(stderr, "%s\n", Narrow(error).c_str());
        return 7;
    }
    if (undone.size() != 2 || !exists(path) || !exists(cover) || exists(expected)) return 8;
    if (exists(root + L"/" + Wide(tags.albumArtist.c_str()))) return 9;  // the organized folders are gone again
    printf("organized \"%s\" and moved it back\n", Narrow(path).c_str());
    return 0;
}

// Copies a tagged FLAC FILE (with a cover.jpg beside it) into DEST as the playlist "Mix": the copy
// and its cover land under the pattern's folders with a manifest, and a second plan finds the copy
// already there. Copying the playlist again as AAC makes an .m4a and, with stale removal, removes the
// FLAC copy the playlist no longer wants. The original is never touched.
int CopyRoundTrip(const char* file, const char* destArg) {
    const std::wstring path = Wide(file), dest = Wide(destArg);
    auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    TagData tags;
    if (!ReadTags(path, tags)) return 2;
    const TrackId id = g_lib.Add(path);
    g_lib.ApplyTags(id, tags);
    std::string before, after;
    if (!ReadFileBytes(path, before)) return 2;
    auto run = [&](CopyFormat format, bool removeStale, CopyResult& result, std::vector<CopyItem>& plan) {
        std::vector<CopyRecord> manifest;
        if (!ReadCopyManifest(dest, false, manifest)) return false;
        plan = PlanCopy({id}, dest, false, kDefaultOrganizePattern, format, manifest);
        CopySettings settings;
        settings.dest = dest;
        settings.staging = dest + L".staging";
        settings.set = L"Mix";
        settings.removeStale = removeStale;
        settings.kbps = CopyKbps(format);
        std::atomic<bool> cancel{false};
        result = ExecuteCopy(plan, settings, manifest, cancel, [](size_t) {});
        for (const auto& [from, why] : result.failed) fprintf(stderr, "%s: %s\n", Narrow(from).c_str(), Narrow(why).c_str());
        return result.failed.empty();
    };
    const std::wstring folder = dest + L"/" + Wide(tags.albumArtist.c_str()) + L"/" + Wide(tags.album.c_str()) + L"/";
    const std::wstring name = (tags.track < 10 ? L"0" : L"") + std::to_wstring(tags.track) + L" " + Wide(tags.title.c_str());
    CopyResult result;
    std::vector<CopyItem> plan;
    if (!run(CopyFormat::Original, false, result, plan) || result.copied != 1) return 3;
    if (!exists(folder + name + L".flac") || !exists(folder + L"cover.jpg") || !exists(dest + L"/" + kCopyManifest)) return 4;
    if (!run(CopyFormat::Original, false, result, plan) || plan.size() != 1 || !plan[0].done || result.copied) return 5;
    if (!run(CopyFormat::Aac128, true, result, plan) || result.converted != 1 || result.removed != 1) return 6;
    if (!exists(folder + name + L".m4a") || exists(folder + name + L".flac") || exists(dest + L".staging")) return 7;
    TagData copied;
    if (!ReadTags(folder + name + L".m4a", copied) || copied.title != tags.title) return 8;
    if (!ReadFileBytes(path, after) || after != before) return 9;  // the original is only ever read
    printf("copied \"%s\", found it again and replaced it with AAC\n", Narrow(path).c_str());
    return 0;
}

// Converts FILE (FLAC, WAV or AIFF) to .cord with its log in LOGDIR, verified before the original
// goes to the Recycle Bin, then undoes the conversion: the original must come back and the .cord
// must go. Works on FILE itself: pass a copy, and an XDG_DATA_HOME on the same drive.
int ConvertRoundTrip(const char* file, const char* logDir) {
    std::wstring path = Wide(file), error;
    const auto plan = PlanConvert({{0, path}});
    if (plan.size() != 1 || !plan[0].skip.empty()) {
        fprintf(stderr, "won't convert: %s\n", plan.empty() ? "no plan" : Narrow(plan[0].skip).c_str());
        return 2;
    }
    std::atomic<bool> cancel{false};
    ConvertSettings settings;
    const ConvertResult result = ExecuteConvert(plan, settings, Wide(logDir), cancel, [](size_t) {});
    for (const auto& [from, why] : result.failed) fprintf(stderr, "%s: %s\n", Narrow(from).c_str(), Narrow(why).c_str());
    if (result.done.size() != 1) return 3;
    const std::wstring cord = result.done[0].to;
    if (!result.done[0].recycled) {
        fprintf(stderr, "the original wasn't sent to the Recycle Bin\n");
        return 4;
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW(cord.c_str()) == INVALID_FILE_ATTRIBUTES) return 5;
    const auto undone = UndoLastConvert(Wide(logDir), true, error);
    if (!error.empty()) {
        fprintf(stderr, "%s\n", Narrow(error).c_str());
        return 6;
    }
    if (undone.size() != 1 || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return 7;
    if (GetFileAttributesW(cord.c_str()) != INVALID_FILE_ATTRIBUTES) return 8;
    if (CanUndoConvert(Wide(logDir))) return 9;
    printf("converted \"%s\" to .cord and undid it\n", Narrow(path).c_str());
    return 0;
}

// A 62 Hz tone through the player's renderer with the Bass boost preset: it must come out about
// as much louder as the preset's 62 Hz slider says (within the limiter's reach).
int CheckEq() {
    const unsigned rate = 48000;
    std::vector<float> tone(rate * 2);
    for (size_t i = 0; i < rate; ++i) tone[2 * i] = tone[2 * i + 1] = 0.1f * (float)std::sin(2 * M_PI * 62 * i / rate);
    EqSettings eq;
    auto rms = [](const std::vector<float>& v) {
        double sum = 0;
        for (size_t i = v.size() / 2; i < v.size(); ++i) sum += (double)v[i] * v[i];
        return std::sqrt(sum / (v.size() - v.size() / 2));
    };
    double flat = rms(RenderPlayerPcm(tone.data(), rate, rate, eq, false));
    for (const EqPreset& preset : EqPresets())
        if (std::wstring(preset.name) == L"Bass boost") eq.gains = preset.gains;
    eq.enabled = true;
    double boosted = rms(RenderPlayerPcm(tone.data(), rate, rate, eq, false));
    double db = 20 * std::log10(boosted / flat), want = eq.gains[1];
    printf("62 Hz: %+.2f dB with the Bass boost preset (slider %+.1f dB)\n", db, want);
    return std::fabs(db - want) < 1.0 ? 0 : 1;
}

int CheckWaveform(const char* flac, const char* vorbis, const char* opus) {
    for (unsigned rate : {44100u, 48000u, 96000u}) {
        std::vector<float> note(rate / 50 * 2), silence(rate / 10 * 2);
        for (size_t i = 0; i < note.size() / 2; ++i) {
            note[i * 2] = 0.5f * (float)std::sin(2 * M_PI * 440 * i / rate);
            note[i * 2 + 1] = -note[i * 2];
        }
        WaveformMotion motion;
        motion.Add(note.data(), note.size() / 2, rate);
        float hit = motion.Level();
        std::array<uint8_t, 3> shape{0, 128, 255}, held = shape;
        motion.Shape(shape.data(), shape.size()); motion.Shape(held.data(), held.size());
        if (hit < 0.65f || shape != held || shape[0] < 50 || shape[0] >= shape[1] || shape[1] >= shape[2]) return 1;
        motion.Add(silence.data(), silence.size() / 2, rate);
        float tail = motion.Level();
        motion.Add(silence.data(), silence.size() / 2, rate);
        if (!(tail > 0.2f && tail < hit && motion.Level() < tail)) return 2;
        motion = {};
        motion.Shape(shape.data(), shape.size());
        if (std::any_of(shape.begin(), shape.end(), [](uint8_t v) { return v != 0; })) return 3;
    }
    PeakAccumulator accumulator;
    Peaks preview{};
    accumulator.Begin(4 * 48000);
    if (accumulator.Preview(preview)) return 4;
    std::vector<float> first(48000 * 2, 0.5f), quiet(48000 * 2);
    accumulator.Add(first.data(), 48000);
    if (!accumulator.Preview(preview)) return 5;
    auto positioned = [&] {
        return std::all_of(preview.v, preview.v + kPeakBuckets / 4, [](uint8_t v) { return v == CompandPeak(0.5f); }) &&
               std::all_of(preview.v + kPeakBuckets / 4, preview.v + kPeakBuckets, [](uint8_t v) { return v == 0; });
    };
    if (!positioned()) return 6;
    accumulator.Add(quiet.data(), 48000);  // merge blocks while the undecoded half stays empty
    if (!accumulator.Preview(preview) || !positioned()) return 7;
    accumulator.Begin(4);
    accumulator.Add(first.data(), 1);
    if (!accumulator.Preview(preview) || !positioned()) return 8;
    struct SampledTrack final : Decoder {
        double position = 0;
        bool Seek(double seconds) override { position = seconds; return true; }
        bool FastSeek() const override { return true; }
        double Duration() const override { return 4; }
        unsigned Rate() const override { return 1000; }
        size_t Read(float* out, size_t frames) override {
            const float levels[] = {0.1f, 0.3f, 0.6f, 0.9f};
            for (size_t i = 0; i < frames; ++i) {
                out[i * 2] = out[i * 2 + 1] = levels[std::clamp((int)position, 0, 3)];
                position += 0.001;
            }
            return frames;
        }
    } track;
    if (!PreviewPeaks(track, 0, 4, preview, nullptr) ||
        std::abs((int)preview.v[75] - CompandPeak(0.1f)) > 2 ||
        std::abs((int)preview.v[525] - CompandPeak(0.9f)) > 2 ||
        !PreviewPeaks(track, 1, 2, preview, nullptr) ||
        std::abs((int)preview.v[150] - CompandPeak(0.3f)) > 2 ||
        std::abs((int)preview.v[450] - CompandPeak(0.6f)) > 2) return 9;
    if (flac) {
        std::wstring error;
        auto decoder = OpenDecoder(Wide(flac), 48000, error);
        if (!decoder || decoder->FastSeek() || !decoder->CanPreview() ||
            !PreviewPeaks(*decoder, 0, decoder->Duration(), preview, nullptr) ||
            std::none_of(preview.v, preview.v + kPeakBuckets, [](uint8_t v) { return v != 0; })) return 10;
        double position = 12;
        auto reference = OpenDecoder(Wide(flac), 48000, error);
        float actual[1024]{}, expected[1024]{};
        if (!decoder->PreviewSeek(position) || !reference || !reference->Seek(position + 0.5 / decoder->Rate()) ||
            decoder->Read(actual, 512) != 512 || reference->Read(expected, 512) != 512 ||
            std::memcmp(actual, expected, sizeof(actual)) != 0) return 11;
    }
    for (const char* file : {vorbis, opus}) if (file) {
        std::wstring error;
        auto decoder = OpenDecoder(Wide(file), 48000, error);
        if (!decoder || !decoder->CanPreview() || !PreviewPeaks(*decoder, 0, decoder->Duration(), preview, nullptr) ||
            std::none_of(preview.v, preview.v + kPeakBuckets, [](uint8_t v) { return v != 0; })) return 12;
    }
    puts("live notes lift the whole shape within 20 ms, release smoothly and clear on reset");
    puts("whole-track peaks appear in their correct positions as the file is decoded");
    return 0;
}

// Lists a saved library: each track's file, title and the ReplayGain the player stored for it.
int ListLibrary(const char* file) {
    std::vector<Playlist> playlists;
    if (!LoadLibrary(Wide(file), playlists)) return 1;
    auto gain = [](int16_t g) { return g == kNoGain ? std::string("-") : std::to_string(g / 100.0); };
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& t = g_lib[id];
        printf("%s\t%s\ttrack %s\talbum %s\n", Narrow(g_lib.Path(id)).c_str(), ToUtf8(g_lib.Str(t.title)).c_str(),
               gain(t.rgTrackGain).c_str(), gain(t.rgAlbumGain).c_str());
    }
    return 0;
}

int ListHistory(const char* file) {
    std::vector<Playlist> playlists;
    if (!LoadLibrary(Wide(file), playlists)) return 1;
    for (const auto& event : PlayLog())
        if (g_lib.Contains(event.id))
            printf("listen\t%s\t%s\t%u\n", Narrow(g_lib.Path(event.id)).c_str(),
                   event.flags & PlayEvent::kCounted ? "counted" : "skip", event.seconds);
    for (const auto& event : PointsRecent())
        if (g_lib.Contains(event.id) && (event.kind == PointsKind::Play || event.kind == PointsKind::Collection))
            printf("award\t%s\t%s\n", Narrow(g_lib.Path(event.id)).c_str(),
                   event.kind == PointsKind::Play ? "play" : "collection");
    return PointsInitialized() ? 0 : 2;
}

int ListPlaylists(const char* file) {
    std::vector<Playlist> playlists;
    if (!LoadLibrary(Wide(file), playlists)) return 1;
    for (size_t i = 0; i < playlists.size(); ++i) {
        printf("%zu\t%s\t%d\t%zu\n", i, Narrow(playlists[i].name).c_str(), (int)playlists[i].kind, playlists[i].items.size());
        printf("sort\t%s\t%d\t%d\n", Narrow(playlists[i].name).c_str(), playlists[i].sortColumn, playlists[i].sortAscending);
        for (size_t j = 0; j < playlists[i].items.size(); ++j)
            if (g_lib.Contains(playlists[i].items[j]))
                printf("item\t%s\t%zu\t%s\n", Narrow(playlists[i].name).c_str(), j, Narrow(g_lib.Path(playlists[i].items[j])).c_str());
    }
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        if (g_lib[id].InLibrary()) printf("library\t%s\n", Narrow(g_lib.Path(id)).c_str());
        if (g_lib[id].flags & Track::kInbox) printf("inbox\t%s\n", Narrow(g_lib.Path(id)).c_str());
        if (g_lib[id].flags & Track::kExcluded) printf("excluded\t%s\n", Narrow(g_lib.Path(id)).c_str());
        printf("rating\t%s\t%u\n", Narrow(g_lib.Path(id)).c_str(), (unsigned)g_lib[id].rating);
        printf("stat\t%s\t%llu\t%llu\n", Narrow(g_lib.Path(id)).c_str(),
               (unsigned long long)g_lib[id].fileSize, (unsigned long long)g_lib[id].fileTime);
        printf("tags\t%s\t%d\t%d\n", Narrow(g_lib.Path(id)).c_str(), g_lib[id].Loaded(),
               (g_lib[id].flags & Track::kTagFailed) != 0);
    }
    return 0;
}

int CheckWeightedShuffle() {
    SetPlayLog({});
    for (unsigned i = 0; i < 20; ++i) LogPlay(1, 1000 + i * 100, 3, false, true);
    std::vector<TrackId> tracks{0, 1, 2, 2};
    std::mt19937 rng(12345);
    int favored = 0;
    bool complete = true;
    for (int i = 0; i < 300; ++i) {
        std::vector<uint32_t> pair{0, 1};
        ShuffleByListening(pair, tracks, rng);
        favored += pair.back() == 0;
        std::vector<uint32_t> all{0, 1, 2, 3};
        ShuffleByListening(all, tracks, rng);
        std::sort(all.begin(), all.end());
        complete &= all == std::vector<uint32_t>({0, 1, 2, 3});
    }
    SetPlayLog({});
    if (!complete || favored <= 250) return 1;
    printf("favored %d of 300; all occurrences retained\n", favored);
    return 0;
}

int ListChapters(const char* file) {
    std::vector<Chapter> chapters;
    if (!ReadChapters(Wide(file), chapters)) return 1;
    printf("first\t%s\n", Narrow(ChapterTextAt(chapters, 0)).c_str());
    printf("last\t%s\n", Narrow(ChapterTextAt(chapters, chapters.back().start)).c_str());
    printf("next\t%.3f\n", ChapterStartAfter(chapters, 0));
    return 0;
}

// The ListenBrainz module: the token is stored for its owner alone, a listen is queued on disk and survives a
// restart, and a refused token is told apart from an unreachable server.
int CheckListenBrainz(const char* dirArg) {
    const std::wstring dir = Wide(dirArg);
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    std::mutex mu;
    std::condition_variable cv;
    int answer = -1;
    SetPostMessageHandler([&](HWND, UINT, WPARAM wp, LPARAM lp) {
        if (wp > 2) return;
        delete reinterpret_cast<std::wstring*>(lp);
        std::lock_guard lock(mu);
        answer = (int)wp;
        cv.notify_all();
    });
    listenbrainz::Start(dir, nullptr, WM_APP);
    listenbrainz::SetSendingDisabledForTest(true);
    listenbrainz::SetToken(L"  test-token-0000  ");
    check(listenbrainz::Enabled(), "a token turns scrobbling on");
    struct stat st {};
    check(stat((std::string(dirArg) + "/listenbrainz.token").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
          "the token file is readable by its owner alone");
    listenbrainz::Listen l;
    l.listenedAt = 1700000000;
    l.artist = "Artist";
    l.title = "Title \xE2\x82\xAC";
    l.durationMs = 1000;
    listenbrainz::Submit(l);
    check(listenbrainz::GetStatus().queued == 1, "a finished listen is queued");
    listenbrainz::Stop();
    listenbrainz::Start(dir, nullptr, WM_APP);
    listenbrainz::SetSendingDisabledForTest(true);
    check(listenbrainz::Enabled() && listenbrainz::GetStatus().queued == 1, "the token and the queue survive a restart");
    listenbrainz::Validate(L"not-a-real-token");
    {
        std::unique_lock lock(mu);
        cv.wait_for(lock, std::chrono::seconds(60), [&] { return answer >= 0; });
    }
    printf("note the bogus token's check answered %d (0 refused, 2 unreachable)\n", answer);
    check(answer == 0 || answer == 2, "a bogus token is refused, or the server is unreachable");
    listenbrainz::SetToken(L"");
    check(!listenbrainz::Enabled() && stat((std::string(dirArg) + "/listenbrainz.token").c_str(), &st) != 0,
          "clearing the token removes the file");
    listenbrainz::Stop();
    return failures;
}

// The sound effect stream: a clip plays through PipeWire (its position moves, then it ends) and an
// effect renders and plays beside it. Without a PipeWire server the stream can't open; that is a skip.
int CheckSfx() {
    std::vector<float> clip(2 * 48000 / 2);  // half a second of a quiet tone, stereo
    for (size_t f = 0; f < clip.size() / 2; ++f) clip[f * 2] = clip[f * 2 + 1] = 0.05f * std::sin(f * 0.05f);
    sfx::PlayClip(clip, 48000, 1.0f);
    sfx::Play(Sfx::Rate, 6, true, 0.2f);
    double top = 0;
    bool advanced = false;
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const double position = sfx::ClipPosition();
        advanced |= position > 0;
        top = std::max(top, position);
        if (advanced && position < 0) break;  // the clip has ended; no need to wait out the rest
    }
    int failures = 0;
    bool moved = top > 0.1;
    // Position -1 once it has finished; the stream may still be draining effects.
    bool ended = sfx::ClipPosition() < 0;
    if (!moved && !getenv("CORDINAL_E2E")) {  // the end-to-end run has a PipeWire server and needs the sound
        printf("skip no sound effect stream (is PipeWire running?)\n");
        sfx::Shutdown();
        return 0;
    }
    printf("%s the clip played through (position reached %.2f s)\n", moved && ended ? "ok  " : "FAIL", top);
    failures += !(moved && ended);
    sfx::SetSuppressed(true);
    sfx::PlayClip(clip, 48000, 1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    printf("%s nothing plays while suppressed\n", sfx::ClipPosition() < 0 ? "ok  " : "FAIL");
    failures += sfx::ClipPosition() >= 0;
    sfx::Shutdown();
    return failures;
}

// The output devices PipeWire offers (the Preferences > Output device list). The end-to-end run has a null
// sink and needs it listed.
int ListDevices() {
    auto devices = ListOutputDevices();
    for (const auto& d : devices) printf("%s\t%s\n", d.node.c_str(), d.description.c_str());
    if (devices.empty() && getenv("CORDINAL_E2E")) {
        fprintf(stderr, "no output device listed\n");
        return 1;
    }
    return 0;
}

// Helpers: a helper folder with two shell scripts for programs is read, trusted, changed (and no longer
// trusted), and plays and tags a file type cordinal doesn't know.
int CheckHelpers(const char* dirArg) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(dirArg) / "helpers", dir = root / "fake";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(dir, ec);
    auto write = [&](const char* name, const char* text, bool exec) {
        std::ofstream(dir / name, std::ios::binary) << text;
        if (exec) fs::permissions(dir / name, fs::perms::owner_all, ec);
    };
    // 1 s of 8 kHz mono silence as a WAV stream, whatever it is asked to play.
    write("dec", "#!/bin/sh\nprintf 'RIFF\\244>\\0\\0WAVEfmt \\020\\0\\0\\0\\001\\0\\001\\0@\\037\\0\\0\\200>\\0\\0\\002\\0\\020\\0data\\200>\\0\\0'\nhead -c 16000 /dev/zero\n", true);
    write("inf", "#!/bin/sh\necho 'title=Fake title'\necho 'artist=Fake artist'\n", true);
    write("helper.ini", "[helper]\nformat = 1\nname = Fake\n[decoder]\nextensions = fake\ndecode = dec.exe {start} {file}\ninfo = inf {file}\n", false);
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    auto found = ReadHelpers(root.wstring());
    check(found.size() == 1 && found[0].error.empty() && found[0].extensions == std::vector<std::wstring>{L"fake"},
          "the helper is read, and a program named .exe is the file without it here");
    if (found.size() != 1 || !found[0].error.empty()) {
        if (!found.empty()) printf("     %ls\n", found[0].error.c_str());
        return failures + 1;
    }
    std::wstring record = MakeHelperTrust(dir.wstring());
    check(!record.empty() && CheckHelperTrust(dir.wstring(), record), "turning it on records its files, and they still match");
    SetActiveHelpers({std::make_shared<const Helper>(found[0])});
    check(HelperForExtension(L"fake") != nullptr && !HelperForExtension(L"flac"), "it plays its own file type, not cordinal's");
    std::wstring error;
    auto decoder = OpenHelperDecoder(L"/nowhere/song.fake", error);
    check(decoder && decoder->Rate() == 8000, "its WAV stream is the decoder");
    if (decoder) {
        std::vector<float> samples(4096 * 2);
        size_t total = 0;
        for (size_t n; (n = decoder->Read(samples.data(), 4096)) > 0;) total += n;
        check(total == 8000, "the whole second arrives");
    } else {
        printf("     %ls\n", error.c_str());
    }
    TagData tags;
    check(ReadHelperTags(L"/nowhere/song.fake", tags) && tags.title == "Fake title" && tags.artist == "Fake artist", "its info command gives the tags");
    write("dec", "#!/bin/sh\nexit 1\n", true);
    check(!CheckHelperTrust(dir.wstring(), record), "a changed program is no longer trusted");
    SetActiveHelpers({});
    StopHelperPrograms();
    return failures;
}

}  // namespace

// The away-from-home pieces that are Linux's own (OpenSSL keys, certificates, TLS and HttpFetch), and the
// shared readers they feed. Keys and certificates are made in DIR; the check serves HTTPS on loopback.
int CheckAway(const char* directory) {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    web::JsonValue v;
    check(web::ParseJson("{\"a\":[1,true,null,{\"b\":\"\\u00e9\"}],\"c\":-2.5e3}", v) && v["a"].items.size() == 4 &&
          v["a"].items[3].String("b") == "\xc3\xa9" && !web::ParseJson("{\"a\":1,}", v), "the JSON reader");
    // RFC 5769 section 2.2: a binding response carrying 192.0.2.1 for this transaction.
    const uint8_t id[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
    const uint8_t stun[] = {0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
        0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
        0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43, 0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99,
        0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04,
        0xc0, 0x7d, 0x4c, 0x96};
    std::string address;
    check(web::ParseStun({(const char*)stun, sizeof(stun)}, id, address) && address == "192.0.2.1" &&
          web::PublicIPv4("73.12.200.41") && !web::PublicIPv4("192.168.1.1") && !web::PublicIPv4("100.72.1.2") &&
          web::SsdpLocation("HTTP/1.1 200 OK\r\nlocation: http://10.0.0.1:49152/rootDesc.xml\r\n\r\n") == "http://10.0.0.1:49152/rootDesc.xml",
          "STUN replies, internet addresses and router discovery are read");
    const qr::Code code = qr::Encode("https://203.0.113.7/#pair=0123456789abcdef0123456789abcdef");
    check(code.size >= 21 && code.modules.size() == (size_t)code.size * code.size, "a pairing link becomes a QR code");

    // The account key: saved and read back, and its signature checks out against its own JWK.
    web::AccountKey key, again;
    std::string blob = key.Create() ? key.Export() : std::string{};
    std::string jwk = key.Jwk(), signature = key.Sign("header.payload");
    check(!blob.empty() && again.Import(blob) && again.Jwk() == jwk && signature.size() == 64 && !again.Import("not a key"),
          "the account key is saved and read back");
    bool verified = false;
    web::JsonValue j;
    if (web::ParseJson(jwk, j)) {
        auto decode = [](const std::string& text) {  // base64url, as the JWK holds the coordinates
            std::string out;
            uint32_t bits = 0; int count = 0;
            for (char c : text) {
                const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
                const char* at = strchr(table, c);
                if (!at || !c) return std::string{};
                bits = bits << 6 | (uint32_t)(at - table); count += 6;
                if (count >= 8) { count -= 8; out += (char)(bits >> count & 0xFF); }
            }
            return out;
        };
        std::string point = "\x04" + decode(j.String("x")) + decode(j.String("y"));
        OSSL_PARAM_BLD* build = OSSL_PARAM_BLD_new();
        OSSL_PARAM_BLD_push_utf8_string(build, OSSL_PKEY_PARAM_GROUP_NAME, "P-256", 0);
        OSSL_PARAM_BLD_push_octet_string(build, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
        OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(build);
        EVP_PKEY_CTX* make = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        EVP_PKEY* pub = nullptr;
        if (point.size() == 65 && make && EVP_PKEY_fromdata_init(make) == 1) EVP_PKEY_fromdata(make, &pub, EVP_PKEY_PUBLIC_KEY, params);
        ECDSA_SIG* sig = ECDSA_SIG_new();
        ECDSA_SIG_set0(sig, BN_bin2bn((const unsigned char*)signature.data(), 32, nullptr),
                       BN_bin2bn((const unsigned char*)signature.data() + 32, 32, nullptr));
        unsigned char* der = nullptr;
        int size = i2d_ECDSA_SIG(sig, &der);
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        verified = pub && size > 0 && EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, pub) == 1 &&
                   EVP_DigestVerify(md, der, (size_t)size, (const unsigned char*)"header.payload", 14) == 1;
        EVP_MD_CTX_free(md); OPENSSL_free(der); ECDSA_SIG_free(sig); EVP_PKEY_free(pub); EVP_PKEY_CTX_free(make);
        OSSL_PARAM_free(params); OSSL_PARAM_BLD_free(build);
    }
    check(verified, "an ES256 signature verifies with the account key's JWK");

    // The server's key, a certificate request for Let's Encrypt, and a certificate to serve.
    const std::wstring keyName = Wide(directory) + L"/web-key.bin";
    unlink(Narrow(keyName).c_str());
    std::string error;
    struct stat st{};
    check(web::EnsureTlsKey(keyName, error) && !stat(Narrow(keyName).c_str(), &st) && !(st.st_mode & 077) &&
          web::EnsureTlsKey(keyName, error), "the server's key is made once, readable only by its owner");
    std::string csr = web::CertificateRequest(keyName, {"203.0.113.7", "music.example"}, error);
    bool requestOk = false;
    {
        const unsigned char* p = (const unsigned char*)csr.data();
        X509_REQ* request = d2i_X509_REQ(nullptr, &p, (long)csr.size());
        EVP_PKEY* pub = request ? X509_REQ_get0_pubkey(request) : nullptr;
        STACK_OF(X509_EXTENSION)* extensions = request ? X509_REQ_get_extensions(request) : nullptr;
        std::string names;
        for (int i = 0; extensions && i < sk_X509_EXTENSION_num(extensions); ++i) {
            BIO* text = BIO_new(BIO_s_mem());
            X509V3_EXT_print(text, sk_X509_EXTENSION_value(extensions, i), 0, 0);
            char* data = nullptr;
            long n = BIO_get_mem_data(text, &data);
            names.append(data, (size_t)n);
            BIO_free(text);
        }
        requestOk = pub && X509_REQ_verify(request, pub) == 1 && names.find("IP Address:203.0.113.7") != std::string::npos &&
                    names.find("DNS:music.example") != std::string::npos;
        sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
        X509_REQ_free(request);
    }
    check(requestOk, "the certificate request is signed by the server's key and names its addresses");
    std::string der = web::SelfSignedCertificate(keyName, {"127.0.0.1"}, 2);
    std::vector<std::string> names;
    uint64_t from = 0, until = 0;
    std::string pem;
    {
        const unsigned char* p = (const unsigned char*)der.data();
        X509* cert = d2i_X509(nullptr, &p, (long)der.size());
        BIO* out = BIO_new(BIO_s_mem());
        char* data = nullptr;
        long n = cert && PEM_write_bio_X509(out, cert) ? BIO_get_mem_data(out, &data) : 0;
        if (n > 0) pem.assign(data, (size_t)n);
        BIO_free(out);
        X509_free(cert);
    }
    auto chain = web::PemCertificates(pem + pem);
    check(web::CertificateInfo(der, from, until, names) && names == std::vector<std::string>{"127.0.0.1"} && until > from &&
          until - from > 86400 && chain.size() == 2 && chain[0] == der, "certificates are read for their names, dates and PEM");

    // HTTPS on the server's own port, checked by HttpFetch against that certificate.
    auto identity = web::TlsIdentity::Create({der}, keyName, error);
    web::Server server;
    web::Config config;
    config.port = 0;
    check(identity && server.Start(config), "the certificate becomes the server's identity");
    if (!identity || !server.GetStatus().running) return failures;
    const std::string port = std::to_string(server.GetStatus().port);
    web::Away away;
    away.identity = identity;
    away.url = "https://127.0.0.1:" + port + "/";
    away.challenges = {{"tok-1", "tok-1.thumbprint"}};
    server.SetAway(away);
    const std::string trust = std::string(directory) + "/trusted.pem";
    if (FILE* f = fopen(trust.c_str(), "w")) { fputs(pem.c_str(), f); fclose(f); }
    setenv("SSL_CERT_FILE", trust.c_str(), 1);  // read when HttpFetch first makes its TLS context
    auto secure = web::HttpFetch(nullptr, "https://127.0.0.1:" + port + "/manifest.webmanifest", "GET");
    auto plain = web::HttpFetch(nullptr, "http://127.0.0.1:" + port + "/manifest.webmanifest", "GET");
    auto challenge = web::HttpFetch(nullptr, "http://127.0.0.1:" + port + "/.well-known/acme-challenge/tok-1", "GET");
    check(secure.status == 200 && secure.body.find("cordinal") != std::string::npos && plain.status == 200 && plain.body == secure.body,
          "the same port serves HTTPS and HTTP");
    check(challenge.status == 200 && challenge.body == "tok-1.thumbprint", "the http-01 answer is served for Let's Encrypt");
    auto wrongName = web::HttpFetch(nullptr, "https://localhost:" + port + "/manifest.webmanifest", "GET", {}, {}, 3000);
    check(wrongName.status == 0, "a certificate that doesn't name the host is refused");
    std::atomic<bool> cancel{true};
    auto t0 = std::chrono::steady_clock::now();
    auto cancelled = web::HttpFetch(&cancel, "http://192.0.2.1/", "GET", {}, {}, 10000);
    check(cancelled.status == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2), "a cancelled request stops at once");
    server.Stop();
    return failures;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "weighted-shuffle")) return CheckWeightedShuffle();
    if (argc >= 3 && !strcmp(argv[1], "chapters")) return ListChapters(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "steam")) {
        std::atomic<bool> cancelled{argc >= 3 && !strcmp(argv[2], "cancel")};
        for (const auto& folder : FindSteamMusicFolders(&cancelled)) puts(Narrow(folder).c_str());
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "library")) return ListLibrary(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "history")) return ListHistory(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "playlists")) return ListPlaylists(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "replaygain")) return MeasureReplayGain(argc - 2, argv + 2);
    if (argc >= 4 && !strcmp(argv[1], "tags")) return TagRoundTrip(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "convert")) return ConvertRoundTrip(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "organize")) return OrganizeRoundTrip(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "copy")) return CopyRoundTrip(argv[2], argv[3]);
    if (argc >= 2 && !strcmp(argv[1], "eq")) return CheckEq();
    if (argc >= 2 && !strcmp(argv[1], "waveform"))
        return CheckWaveform(argc >= 3 ? argv[2] : nullptr, argc >= 4 ? argv[3] : nullptr, argc >= 5 ? argv[4] : nullptr);
    if (argc >= 3 && !strcmp(argv[1], "helpers")) return CheckHelpers(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "devices")) return ListDevices();
    if (argc >= 2 && !strcmp(argv[1], "sfx")) return CheckSfx();
    if (argc >= 3 && !strcmp(argv[1], "away")) return CheckAway(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "listenbrainz")) return CheckListenBrainz(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "web")) return Serve(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "scan")) return Scan(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "decode")) return Decode(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "play")) return Play(argv[2], argc >= 4 ? atof(argv[3]) : 5);
    fprintf(stderr, "usage: cordinal-core-test scan DIR | decode FILE | play FILE [SECONDS] | replaygain FILE... | "
                    "tags FILE DIR | convert FILE DIR | organize FILE ROOT | copy FILE DEST | eq | waveform | helpers DIR | devices | sfx | listenbrainz DIR | weighted-shuffle | chapters FILE | library FILE | history FILE | playlists FILE | steam [cancel] | away DIR | web DIR CONTROL\n");
    return 2;
}
