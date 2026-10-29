// UI-owned radio directory state. Searches run on workers and publish copied results; favorites
// change only after their atomic profile write succeeds.
#pragma once
#include "radio.h"

#include <memory>
#include <map>

namespace radio {
class Browser {
public:
    explicit Browser(std::wstring dataDir);
    ~Browser();
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;
    void SearchDirectory(const Search& search);
    bool Poll();
    void Cancel();
    bool Searching() const { return (bool)query_; }
    const std::vector<Station>& Results() const { return results_; }
    const std::vector<Station>& Favorites() const { return favorites_; }
    bool Favorite(const Station& station) const;
    bool ToggleFavorite(const Station& station);
    void RecordPlay(const std::string& stationId);
    void RequestIcon(const std::string& url);
    const std::string* Icon(const std::string& url) const;
    const std::wstring& Problem() const { return problem_; }
private:
    struct Query;
    struct Image;
    struct Click;
    std::wstring folder_, path_, problem_;
    std::vector<Station> favorites_, results_;
    std::shared_ptr<Query> query_;
    std::map<std::string, std::shared_ptr<Image>> images_;
    std::vector<std::shared_ptr<Click>> clicks_;
    unsigned long long imageUse_ = 0;
    bool readable_ = true;
};
}  // namespace radio
