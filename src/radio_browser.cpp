// Worker-backed directory searches and transactional favorites, shared by both front ends.
#include "radio_browser.h"
#include "library.h"
#include "web_remote.h"

#include <atomic>
#include <algorithm>
#include <mutex>
#include <thread>

namespace radio {
struct Browser::Query {
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::vector<Station> stations;
    bool done = false, ok = false;
};
struct Browser::Image {
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::string bytes;
    bool done = false, published = false;
    unsigned long long lastUsed = 0;  // UI thread only
};
struct Browser::Click { std::atomic<bool> cancel{false}, done{false}; };

static std::atomic<unsigned>& Workers() { static auto* count = new std::atomic<unsigned>(0); return *count; }
static std::atomic<unsigned>& ImageWorkers() { static auto* count = new std::atomic<unsigned>(0); return *count; }

Browser::Browser(std::wstring dataDir) {
    folder_ = std::move(dataDir) + kPathSep + L"radio";
    path_ = folder_ + kPathSep + L"stations.json";
    readable_ = LoadFavorites(path_, favorites_);
    if (!readable_) problem_ = L"Couldn't read saved stations. The original file was kept.";
}
Browser::~Browser() {
    Cancel();
    for (auto& [url, image] : images_) image->cancel = true;
    for (auto& click : clicks_) click->cancel = true;
}
void Browser::Cancel() { if (query_) query_->cancel = true; query_.reset(); }
void Browser::SearchDirectory(const Search& search) {
    Cancel();
    auto query = std::make_shared<Query>();
    query_ = query;
    problem_.clear();
    if (Workers().fetch_add(1) >= 8) {
        --Workers(); query_.reset();
        problem_ = L"Too many directory requests are still closing. Try again shortly.";
        return;
    }
    try {
        std::thread([query, search] {
            std::vector<Station> stations;
            bool ok = false;
            try { ok = Discover(search, stations, query->cancel); } catch (...) {}
            {
                std::lock_guard lock(query->mutex);
                query->stations = std::move(stations);
                query->ok = ok; query->done = true;
            }
            --Workers();
        }).detach();
    } catch (...) {
        --Workers(); query_.reset(); problem_ = L"Couldn't start the station search.";
    }
}
bool Browser::Poll() {
    bool changed = false;
    for (const auto& [url, image] : images_) {
        std::lock_guard lock(image->mutex);
        if (image->done && !image->published) { image->published = true; changed = true; }
    }
    if (!query_) return changed;
    bool done, ok;
    {
        std::lock_guard lock(query_->mutex);
        done = query_->done; ok = query_->ok;
        if (done && ok) results_ = std::move(query_->stations);
    }
    if (!done) return changed;
    query_.reset();
    if (!ok) problem_ = L"Couldn't reach the station directory. Check your connection and try again.";
    return true;
}
void Browser::RequestIcon(const std::string& url) {
    if (url.empty()) return;
    if (auto found = images_.find(url); found != images_.end()) {
        found->second->lastUsed = ++imageUse_;
        return;
    }
    if ((!url.starts_with("http://") && !url.starts_with("https://")) || url.size() > 8192) return;
    if (images_.size() >= 100) {
        auto oldest = images_.end();
        for (auto it = images_.begin(); it != images_.end(); ++it) {
            std::lock_guard lock(it->second->mutex);
            if (it->second->done && (oldest == images_.end() || it->second->lastUsed < oldest->second->lastUsed))
                oldest = it;
        }
        if (oldest == images_.end()) return;
        images_.erase(oldest);
    }
    if (ImageWorkers().fetch_add(1) >= 4) { --ImageWorkers(); return; }
    auto image = std::make_shared<Image>();
    image->lastUsed = ++imageUse_;
    images_[url] = image;
    try {
        std::thread([image, url] {
            std::string bytes;
            try {
#ifdef _WIN32
                void* session = nullptr;
#else
                void* session = &image->cancel;
#endif
                auto reply = web::HttpFetch(session, url, "GET", {}, "Accept: image/*\r\n", 6000);
                if (!image->cancel && reply.status == 200 && reply.body.size() <= 256 * 1024) bytes = std::move(reply.body);
            } catch (...) {}
            { std::lock_guard lock(image->mutex); image->bytes = std::move(bytes); image->done = true; }
            --ImageWorkers();
        }).detach();
    } catch (...) {
        --ImageWorkers();
        std::lock_guard lock(image->mutex); image->done = true;
    }
}
const std::string* Browser::Icon(const std::string& url) const {
    auto found = images_.find(url);
    if (found == images_.end()) return nullptr;
    std::lock_guard lock(found->second->mutex);
    return found->second->done && !found->second->bytes.empty() ? &found->second->bytes : nullptr;
}
void Browser::RecordPlay(const std::string& stationId) {
    std::erase_if(clicks_, [](const auto& click) { return click->done.load(); });
    if (Workers().fetch_add(1) >= 8) { --Workers(); return; }
    auto click = std::make_shared<Click>(); clicks_.push_back(click);
    try {
        std::thread([click, stationId] {
            try { CountPlay(stationId, click->cancel); } catch (...) {}
            click->done = true; --Workers();
        }).detach();
    } catch (...) { --Workers(); click->done = true; }
}
bool Browser::Favorite(const Station& station) const {
    return std::any_of(favorites_.begin(), favorites_.end(), [&](const auto& s) { return s.id == station.id || s.url == station.url; });
}
bool Browser::ToggleFavorite(const Station& station) {
    if (!readable_) { problem_ = L"Couldn't read saved stations. The original file was kept."; return false; }
    auto next = favorites_;
    auto found = std::find_if(next.begin(), next.end(), [&](const auto& s) { return s.id == station.id || s.url == station.url; });
    if (found == next.end()) next.push_back(station); else next.erase(found);
    if (GetFileAttributesW(folder_.c_str()) == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(folder_.c_str(), nullptr)) {
        problem_ = L"Couldn't create the saved-stations folder.";
        return false;
    }
    if (!SaveFavorites(path_, next)) { problem_ = L"Couldn't save stations. The previous favorites were kept."; return false; }
    favorites_ = std::move(next); problem_.clear();
    return true;
}
}  // namespace radio
