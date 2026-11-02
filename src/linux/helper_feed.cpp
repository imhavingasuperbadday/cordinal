// Linux visual feed: anonymous shared memory, a private writable mapping in cordinal and a fresh
// read-only fd for each visualizer. No persistent files, names or writable descriptors are inherited.
#include "helpers.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstddef>
#include <cstring>

static_assert(sizeof(LONG) == 4 && offsetof(HelperFeed, sequence) == 20 &&
    offsetof(HelperFeed, clock) == 32 && offsetof(HelperFeed, wave) == 232 && sizeof(HelperFeed) == 16616,
    "the version-1 CDVF layout is shared with Windows and external readers");

bool HelperFeedWriter::Open() {
    if (view_) return true;
    mapping_ = memfd_create("cordinal-visual-feed", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (mapping_ < 0 || ftruncate(mapping_, sizeof(HelperFeed)) < 0) { Close(); return false; }
    void* memory = mmap(nullptr, sizeof(HelperFeed), PROT_READ | PROT_WRITE, MAP_SHARED, mapping_, 0);
    if (memory == MAP_FAILED) { Close(); return false; }
    view_ = static_cast<HelperFeed*>(memory);
    if (fcntl(mapping_, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) < 0) { Close(); return false; }
    memcpy(view_->magic, "CDVF", 4);
    view_->version = 1;
    view_->size = sizeof(HelperFeed);
    view_->frames = HelperFeed::kFrames;
    view_->bands = HelperFeed::kBands;
    view_->clock = -1;
    return true;
}

HANDLE HelperFeedWriter::ReaderHandle() const {
    if (mapping_ < 0) return nullptr;
    std::string path = "/proc/self/fd/" + std::to_string(mapping_);
    int reader = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (reader >= 0 && reader < 3) {
        int aboveStdio = fcntl(reader, F_DUPFD_CLOEXEC, 3);
        close(reader);
        reader = aboveStdio;
    }
    return reader < 0 ? nullptr : reinterpret_cast<HANDLE>((intptr_t)reader);
}

void HelperFeedWriter::Publish(const std::function<void(HelperFeed&)>& fill) {
    if (!view_) return;
    auto next = std::make_unique<HelperFeed>(*view_);
    fill(*next);
    __atomic_add_fetch(&view_->sequence, 1, __ATOMIC_SEQ_CST);
    const size_t from = offsetof(HelperFeed, state);
    memcpy((char*)view_ + from, (const char*)next.get() + from, sizeof(HelperFeed) - from);
    __atomic_add_fetch(&view_->sequence, 1, __ATOMIC_SEQ_CST);
}

void HelperFeedWriter::Close() {
    if (view_) munmap(view_, sizeof(HelperFeed));
    if (mapping_ >= 0) close(mapping_);
    view_ = nullptr;
    mapping_ = -1;
}

bool ReadHelperFeed(const HelperFeed* shared, HelperFeed& copy) {
    if (!shared) return false;
    for (int attempt = 0; attempt < 64; ++attempt) {
        LONG before = __atomic_load_n(&shared->sequence, __ATOMIC_ACQUIRE);
        if (before & 1) continue;
        memcpy(&copy, shared, sizeof(copy));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&shared->sequence, __ATOMIC_ACQUIRE) == before) return true;
    }
    return false;
}
