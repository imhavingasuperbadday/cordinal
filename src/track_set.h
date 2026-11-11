// Containers keyed by track id, for data that grows with the library: TrackIdSet, a bit per
// track for "which tracks", and TrackIdMap, a sorted flat array for records only some tracks have.
// Track ids are dense (0 to Count() - 1), so these replace a hash node, its bucket and a heap
// header per track (40 to 60 bytes in a std::unordered_set or map) with a bit, or with the
// record and a 4-byte key. Like the library, they belong to one thread.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

using TrackId = uint32_t;  // as in library.h

class TrackIdSet {
public:
    // Adds `id`; false when it was already there. UINT32_MAX (kInvalidTrack) is never a member:
    // its bit would need half a gigabyte.
    bool insert(TrackId id) {
        if (id == UINT32_MAX) return false;
        const size_t word = id / 64;
        if (word >= bits_.size()) bits_.resize(std::max(word + 1, bits_.size() * 2));
        const uint64_t bit = 1ull << (id % 64);
        if (bits_[word] & bit) return false;
        bits_[word] |= bit;
        ++count_;
        return true;
    }
    bool erase(TrackId id) {
        const size_t word = id / 64;
        const uint64_t bit = 1ull << (id % 64);
        if (word >= bits_.size() || !(bits_[word] & bit)) return false;
        bits_[word] &= ~bit;
        --count_;
        return true;
    }
    bool contains(TrackId id) const { return id / 64 < bits_.size() && (bits_[id / 64] >> (id % 64) & 1); }
    size_t count(TrackId id) const { return contains(id) ? 1 : 0; }
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    // Releases the bits too: a finished scan should not keep a library-sized array.
    void clear() {
        std::vector<uint64_t>().swap(bits_);
        count_ = 0;
    }

private:
    std::vector<uint64_t> bits_;
    size_t count_ = 0;
};

// Sorted by id, so lookups are a binary search and iteration runs in id order. Inserting ahead of
// the last id moves the records after it; loading and remapping arrive in id order and append.
template <class T>
class TrackIdMap {
public:
    struct Entry {
        TrackId id;
        T value;
    };
    T* find(TrackId id) {
        auto it = LowerBound(id);
        return it != entries_.end() && it->id == id ? &it->value : nullptr;
    }
    const T* find(TrackId id) const { return const_cast<TrackIdMap*>(this)->find(id); }
    // The record for `id`, added value-initialized when missing.
    T& operator[](TrackId id) {
        if (entries_.empty() || entries_.back().id < id) {
            entries_.push_back({id, T{}});
            return entries_.back().value;
        }
        auto it = LowerBound(id);
        if (it == entries_.end() || it->id != id) it = entries_.insert(it, {id, T{}});
        return it->value;
    }
    bool erase(TrackId id) {
        auto it = LowerBound(id);
        if (it == entries_.end() || it->id != id) return false;
        entries_.erase(it);
        return true;
    }
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    void reserve(size_t n) { entries_.reserve(n); }
    void clear() { std::vector<Entry>().swap(entries_); }
    auto begin() const { return entries_.cbegin(); }
    auto end() const { return entries_.cend(); }

private:
    std::vector<Entry> entries_;
    typename std::vector<Entry>::iterator LowerBound(TrackId id) {
        return std::lower_bound(entries_.begin(), entries_.end(), id,
                                [](const Entry& e, TrackId key) { return e.id < key; });
    }
};
