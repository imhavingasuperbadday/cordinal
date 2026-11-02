#include "gpod/model.hpp"

#include <utility>

namespace gpod {

Chapter& ChapterData::add_chapter(std::uint32_t startpos, std::string title) {
    // The first chapter at position 0 is normalized to 1, matching the
    // behavior of the original itdb_chapterdata_add_chapter().
    if (startpos == 0) {
        startpos = 1;
    }
    chapters.push_back(Chapter{startpos, std::move(title)});
    return chapters.back();
}

void ChapterData::remove_chapter(std::size_t index) {
    if (index < chapters.size()) {
        chapters.erase(chapters.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

}  // namespace gpod
