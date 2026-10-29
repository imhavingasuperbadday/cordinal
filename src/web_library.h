// UI-thread snapshot capture. Copies small batches; indexing and auto-playlists run on workers.
#pragma once
#include "library.h"
#include "web_server.h"

namespace web {
class LibraryBuilder {
public:
    LibraryBuilder();
    ~LibraryBuilder();
    std::shared_ptr<Snapshot> Tick(const Library& library, const std::vector<::Playlist>& playlists,
                                   bool showOriginals = false);
    void Reset();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}  // namespace web
