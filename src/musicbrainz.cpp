// Optional MusicBrainz lookup worker. One request at a time, a one-second minimum interval, an
// on-disk cache and a short failure backoff keep the service usable without making the library
// depend on it. The worker only produces ids; the UI thread applies them to tracks and groups.
#include "musicbrainz.h"

#include "album_editions.h"
#include "backup.h"
#include "web_remote.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace musicbrainz {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kFoundTtl = std::chrono::hours(24 * 30);
constexpr auto kMissTtl = std::chrono::minutes(10);
constexpr auto kInterval = std::chrono::milliseconds(1100);

struct Entry {
    std::string releaseGroup;
    Clock::time_point checked{};
    bool found = false;
};

struct Candidate {
    std::string artist, title;
};

std::string Key(const Candidate& candidate) {
    return NormalizeAlbumText(candidate.artist) + '\x1f' + NormalizeAlbumText(candidate.title);
}

bool Fresh(const Entry& entry) {
    if (entry.checked == Clock::time_point{}) return false;
    auto age = Clock::now() - entry.checked;
    return age < (entry.found ? kFoundTtl : kMissTtl);
}

std::string Encode(std::string_view text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string CreditName(const web::JsonValue& credit) {
    std::string name = credit.String("name");
    if (name.empty()) name = credit["artist"].String("name");
    return name;
}

std::string ChooseReleaseGroup(std::string_view json, const Candidate& candidate) {
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return {};
    const web::JsonValue& groups = root["release-groups"];
    if (groups.kind != web::JsonValue::Kind::Array) return {};
    std::string wantArtist = NormalizeAlbumText(candidate.artist), wantTitle = NormalizeAlbumText(candidate.title);
    std::map<std::string, int> scores;
    for (const auto& item : groups.items) {
        int score = atoi(item["score"].text.c_str());
        if (score < 90) continue;
        if (NormalizeAlbumText(item["title"].text) != wantTitle) continue;
        bool artistOk = true;
        const web::JsonValue& credits = item["artist-credit"];
        if (credits.kind == web::JsonValue::Kind::Array && !credits.items.empty()) {
            artistOk = false;
            for (const auto& credit : credits.items)
                if (NormalizeAlbumText(CreditName(credit)) == wantArtist) { artistOk = true; break; }
        }
        if (!artistOk) continue;
        std::string id = item["id"].text;
        if (id.empty()) continue;
        int& saved = scores[id];
        saved = std::max(saved, score);
    }
    std::string best;
    int bestScore = 0, second = 0;
    for (const auto& [id, score] : scores) {
        if (score > bestScore) {
            second = bestScore;
            bestScore = score;
            best = id;
        } else if (score > second) second = score;
    }
    // Two plausible release groups are ambiguous; leave the local albums ungrouped.
    if (best.empty() || (second && bestScore - second <= 5)) return {};
    return best;
}

struct State {
    std::wstring cachePath;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Candidate> queue;
    std::set<std::string> queued;
    std::map<std::string, Entry> cache;
    std::vector<LookupResult> results;
    std::atomic<bool> stop{false}, enabled{false}, started{false};
    bool cacheLoaded = false;
    Clock::time_point nextRequest{};
    Clock::time_point lastQueueCheck{};
};

State s;

void SaveCache() {
    if (!s.cacheLoaded) return;
    std::string bytes = "1\n";
    {
        std::lock_guard lock(s.mutex);
        auto cutoff = Clock::now() - std::chrono::hours(24 * 180);
        for (const auto& [key, entry] : s.cache) {
            if (entry.checked < cutoff) continue;
            if (key.find('\t') != std::string::npos || key.find('\n') != std::string::npos) continue;
            bytes += key + '\t' + entry.releaseGroup + '\t' +
                     std::to_string((long long)std::chrono::duration_cast<std::chrono::seconds>(
                         entry.checked.time_since_epoch()).count()) + '\t' + (entry.found ? "1" : "0") + '\n';
        }
    }
    if (s.cachePath.empty()) return;
    std::wstring temp = s.cachePath + L".tmp";
    DeleteFileW(temp.c_str());
    if (!backup::WriteBytes(temp, bytes)) return;
    if (!MoveFileExW(temp.c_str(), s.cachePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(temp.c_str());
}

void LoadCache() {
    if (s.cachePath.empty()) {
        s.cacheLoaded = true;
        return;
    }
    std::string bytes;
    if (!backup::ReadBytes(s.cachePath, bytes, 4u << 20)) {
        s.cacheLoaded = true;
        return;
    }
    size_t at = bytes.find('\n');
    if (at == std::string::npos || bytes.substr(0, at) != "1") {
        s.cacheLoaded = true;
        return;
    }
    ++at;
    while (at < bytes.size()) {
        size_t end = bytes.find('\n', at);
        if (end == std::string::npos) end = bytes.size();
        std::string line = bytes.substr(at, end - at);
        at = end + 1;
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        size_t c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
        Entry entry;
        entry.releaseGroup = line.substr(a + 1, b - a - 1);
        long long seconds = atoll(line.substr(b + 1, c - b - 1).c_str());
        entry.checked = Clock::time_point(std::chrono::seconds(seconds));
        entry.found = line.substr(c + 1, 1) == "1";
        if (!line.substr(0, a).empty()) s.cache[line.substr(0, a)] = std::move(entry);
    }
    s.cacheLoaded = true;
}

void PushResult(const Candidate& candidate, const std::string& releaseGroup) {
    LookupResult result;
    result.artist = candidate.artist;
    result.title = candidate.title;
    result.releaseGroup = releaseGroup;
    result.found = !releaseGroup.empty();
    s.results.push_back(std::move(result));
}

// One lookup. Returns the release-group id, or empty when MusicBrainz has no confident match.
std::string Fetch(const Candidate& candidate, bool& offline) {
    offline = false;
    std::string query = "releasegroup:\"" + candidate.title + "\" AND artist:\"" + candidate.artist + "\"";
    std::string url = "https://musicbrainz.org/ws/2/release-group?query=" + Encode(query) + "&fmt=json&limit=25";
    std::string headers = "User-Agent: cordinal/0.1.1 (https://github.com/imhavingasuperbadday/cordinal)\r\n"
                          "Accept: application/json\r\n";
#ifdef _WIN32
    void* session = nullptr;
#else
    void* session = (void*)&s.stop;
#endif
    web::HttpReply reply = web::HttpFetch(session, url, "GET", {}, headers, 8000);
    if (reply.status == 0) {
        offline = true;
        return {};
    }
    auto retry = reply.headers.find("retry-after");
    if (reply.status == 429 || reply.status == 503) {
        long seconds = retry == reply.headers.end() ? 10 : atol(retry->second.c_str());
        s.nextRequest = Clock::now() + std::chrono::seconds(std::clamp(seconds, 1L, 300L));
        return {};
    }
    if (reply.status != 200) {
        s.nextRequest = Clock::now() + std::chrono::seconds(30);
        return {};
    }
    s.nextRequest = Clock::now() + kInterval;
    return ChooseReleaseGroup(reply.body, candidate);
}

void Worker() {
    {
        std::lock_guard lock(s.mutex);
        LoadCache();
    }
    for (;;) {
        Candidate candidate;
        {
            std::unique_lock lock(s.mutex);
            s.wake.wait(lock, [] { return s.stop || (s.enabled && !s.queue.empty()); });
            if (s.stop) break;
            if (!s.enabled || s.queue.empty()) continue;
            candidate = std::move(s.queue.front());
            s.queue.pop_front();
            s.queued.erase(Key(candidate));
            std::string key = Key(candidate);
            auto cached = s.cache.find(key);
            if (cached != s.cache.end() && Fresh(cached->second)) {
                if (cached->second.found) PushResult(candidate, cached->second.releaseGroup);
                continue;
            }
        }
        {
            std::unique_lock lock(s.mutex);
            if (s.nextRequest > Clock::now())
                s.wake.wait_until(lock, s.nextRequest, [] { return s.stop || !s.enabled; });
            if (s.stop || !s.enabled) continue;
        }
        bool offline = false;
        std::string group = Fetch(candidate, offline);
        {
            std::lock_guard lock(s.mutex);
            Entry entry;
            entry.releaseGroup = group;
            entry.checked = Clock::now();
            entry.found = !group.empty();
            s.cache[Key(candidate)] = std::move(entry);
            if (offline) s.nextRequest = Clock::now() + std::chrono::seconds(60);
            PushResult(candidate, group);
            s.wake.notify_all();
        }
        SaveCache();
    }
}

}  // namespace

std::string ParseReleaseGroupSearch(std::string_view json, std::string_view artist, std::string_view title) {
    return ChooseReleaseGroup(json, Candidate{std::string(artist), std::string(title)});
}

void Start(const std::wstring& dataDir, bool enabled) {
    std::lock_guard lock(s.mutex);
    s.cachePath = dataDir.empty() ? std::wstring{} : dataDir + kPathSep + L"musicbrainz-cache.txt";
    s.enabled = enabled;
    if (s.started) {
        s.wake.notify_all();
        return;
    }
    s.stop = false;
    s.started = true;
    s.cacheLoaded = false;
    s.queue.clear();
    s.queued.clear();
    s.results.clear();
    s.nextRequest = Clock::time_point{};
    s.lastQueueCheck = Clock::time_point{};
    s.thread = std::thread(Worker);
}

void Stop() {
    {
        std::lock_guard lock(s.mutex);
        if (!s.started) return;
        s.stop = true;
    }
    s.wake.notify_all();
    if (s.thread.joinable()) s.thread.join();
    SaveCache();
    std::lock_guard lock(s.mutex);
    s.started = false;
}

void SetEnabled(bool enabled) {
    s.enabled = enabled;
    s.wake.notify_all();
}

bool Enabled() { return s.enabled; }

bool Busy() {
    std::lock_guard lock(s.mutex);
    return !s.queue.empty();
}

bool Tick() {
    if (!s.enabled) return false;
    auto now = Clock::now();
    static uint32_t lastRevision = 0;
    if (s.lastQueueCheck == Clock::time_point{} || now - s.lastQueueCheck > std::chrono::seconds(5)) {
        s.lastQueueCheck = now;
        // Album candidates only change with track metadata; do not rescan a large library every tick.
        if (g_lib.Revision() != lastRevision) {
            lastRevision = g_lib.Revision();
            auto candidates = AlbumLookupCandidates();
            std::lock_guard lock(s.mutex);
            for (const auto& candidate : candidates) {
                std::string key = Key({candidate.artist, candidate.title});
                if (key.empty() || s.queued.count(key)) continue;
                auto cached = s.cache.find(key);
                if (cached != s.cache.end() && Fresh(cached->second)) continue;
                s.queue.push_back({candidate.artist, candidate.title});
                s.queued.insert(std::move(key));
            }
            s.wake.notify_all();
        }
    }
    std::vector<LookupResult> results;
    {
        std::lock_guard lock(s.mutex);
        results.swap(s.results);
    }
    bool changed = false;
    for (const auto& result : results)
        if (result.found) changed |= ApplyMusicBrainzReleaseGroup(result.artist, result.title, result.releaseGroup);
    if (changed) AutoGroupAlbums();
    return changed;
}

}  // namespace musicbrainz
