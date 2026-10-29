// A bounded, read-only window into an audio file. Mapping keeps the file cache shared
// with Windows, avoids a song-sized private allocation, and removes stdio copies.
#pragma once
#include <windows.h>
#include "archive.h"
#include <algorithm>
#include <memory>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <emmintrin.h>

namespace nativeaudio {
// File owns a mapping, owns an archive member inflated into memory, or borrows a caller-owned buffer. View() returns a pointer
// that stays valid only until the next View() call; borrowed bytes must outlive File.
class File {
public:
    ~File() {
        Unmap();
        if (map_) CloseHandle(map_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept { Swap(other); }
    File& operator=(File&& other) noexcept { File moved(std::move(other));Swap(moved);return *this; }
    bool Open(const std::wstring& path) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {  // perhaps a song inside a ZIP (archive.h)
            auto member = ReadArchiveMember(path);
            if (!member || member->empty()) return false;
            Borrow((const uint8_t*)member->data(), member->size());
            owned_ = std::move(member);
            return true;
        }
        LARGE_INTEGER size{};
        if (file_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(file_, &size) || size.QuadPart <= 0) return false;
        size_ = (uint64_t)size.QuadPart;
        map_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        return map_ != nullptr;
    }
    // Reads bytes the caller keeps alive, such as an embedded resource: one view covers them all,
    // so View() never maps or unmaps anything.
    void Borrow(const uint8_t* data, size_t size) {
        File borrowed;
        borrowed.view_ = borrowed.window_ = data;
        borrowed.size_ = borrowed.length_ = data ? size : 0;
        Swap(borrowed);
    }
    uint64_t Size() const { return size_; }
    // For decoders that read on through the file once their container is parsed. Later windows
    // are copied into one reused buffer, 64 KiB after a jump and doubling to 1 MiB while reads
    // continue forward. Copying from the file cache costs a fraction of the soft fault that
    // touching each page of a fresh mapping takes. Larger requests still map.
    void Sequential() { sequential_ = map_ != nullptr; }
    // Return a pointer to [at, at+bytes), mapping or reading a fresh window only when the range
    // is not already covered. Returns nullptr if the range lies outside the file.
    const uint8_t* View(uint64_t at, size_t bytes) {
        if (at > size_ || bytes > size_ - at) return nullptr;
        if (window_ && at >= base_ && at - base_ <= length_ && bytes <= length_ - (size_t)(at - base_))
            return window_ + (size_t)(at - base_);
        if (sequential_ && bytes <= kRead) return Read(at, bytes);
        Unmap();
        // Windows allocation granularity is 64 KiB on supported x64 Windows systems.
        base_ = at & ~uint64_t(65535);
        length_ = (size_t)std::min<uint64_t>(size_ - base_, std::max<uint64_t>(1048576, at - base_ + bytes));
        view_ = (const uint8_t*)MapViewOfFile(map_, FILE_MAP_READ, (DWORD)(base_ >> 32), (DWORD)base_, length_);
        window_ = view_;
        return view_ ? view_ + (size_t)(at - base_) : nullptr;
    }
private:
    static constexpr size_t kRead = 1048576;
    // A borrowed buffer has no mapping, and unmapping a pointer into the image would unmap the exe.
    void Unmap() {
        if (view_ && map_) UnmapViewOfFile(view_);
        view_ = window_ = nullptr;
    }
    const uint8_t* Read(uint64_t at, size_t bytes) {
        const bool forward = window_ && at >= base_ && at <= base_ + length_;
        chunk_ = forward ? std::min(chunk_ * 2, kRead) : 65536;
        const uint64_t start = at & ~uint64_t(4095);
        const size_t length = (size_t)std::min<uint64_t>(size_ - start, std::max<uint64_t>(chunk_, at - start + bytes));
        Unmap();
        if (!buffer_) buffer_ = std::make_unique_for_overwrite<uint8_t[]>(kRead + 4096);
        LARGE_INTEGER position{};
        position.QuadPart = (LONGLONG)start;
        DWORD got = 0;
        if (!SetFilePointerEx(file_, position, nullptr, FILE_BEGIN) || !ReadFile(file_, buffer_.get(), (DWORD)length, &got, nullptr) ||
            got != length) return nullptr;
        window_ = buffer_.get(); base_ = start; length_ = length;
        return window_ + (size_t)(at - start);
    }
    void Swap(File& other) noexcept {
        std::swap(file_,other.file_);std::swap(map_,other.map_);std::swap(view_,other.view_);std::swap(window_,other.window_);
        std::swap(size_,other.size_);std::swap(base_,other.base_);std::swap(length_,other.length_);
        std::swap(owned_,other.owned_);std::swap(buffer_,other.buffer_);std::swap(chunk_,other.chunk_);
        std::swap(sequential_,other.sequential_);
    }
    HANDLE file_ = INVALID_HANDLE_VALUE, map_ = nullptr;
    // view_ is the mapped view; window_ is what View() reads: view_, the read buffer or borrowed bytes.
    const uint8_t* view_ = nullptr;
    const uint8_t* window_ = nullptr;
    std::unique_ptr<uint8_t[]> buffer_;
    size_t chunk_ = 65536;
    bool sequential_ = false;
    uint64_t size_ = 0, base_ = 0;
    size_t length_ = 0;
    std::shared_ptr<const std::string> owned_;  // an archive member's bytes, which view_ points into
};

// Little-endian unless be is set. U32/U64 take the memcpy fast path, which assumes
// a little-endian host (x64), and byte-swap only for big-endian input; U16 always
// assembles byte by byte. Forced inline: size-optimized units would otherwise call
// them, which costs more than the load itself in a per-sample loop.
__forceinline uint16_t U16(const uint8_t* p, bool be = false) {
    return be ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8));
}
__forceinline uint32_t U32(const uint8_t* p, bool be = false) {
    uint32_t n;
    memcpy(&n, p, 4);
#ifdef _MSC_VER
    return be ? _byteswap_ulong(n) : n;
#else
    return be ? __builtin_bswap32(n) : n;
#endif
}
__forceinline uint64_t U64(const uint8_t* p, bool be = false) {
    uint64_t n;
    memcpy(&n, p, 8);
#ifdef _MSC_VER
    return be ? _byteswap_uint64(n) : n;
#else
    return be ? __builtin_bswap64(n) : n;
#endif
}
inline bool Is(const uint8_t* p, const char* id) { return memcmp(p, id, 4) == 0; }

// Convert interleaved samples to float: 16/32-bit integers scale into [-1,1),
// float32 passes through with non-finite values forced to 0. `samples` counts both
// channels (left/right pairs).
inline void Stereo16(const uint8_t* p, float* out, size_t samples) {
    const __m128 scale = _mm_set1_ps(1.0f / 32768.0f);
    size_t i = 0;
    for (; i + 8 <= samples; i += 8) {
        __m128i v = _mm_loadu_si128((const __m128i*)(p + i * 2));
        __m128i sign = _mm_srai_epi16(v, 15);
        _mm_storeu_ps(out + i, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(v, sign)), scale));
        _mm_storeu_ps(out + i + 4, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi16(v, sign)), scale));
    }
    for (; i < samples; ++i) out[i] = (int16_t)U16(p + i * 2) * (1.0f / 32768.0f);
}
inline void Stereo32(const uint8_t* p, float* out, size_t samples) {
    const __m128 scale = _mm_set1_ps(1.0f / 2147483648.0f);
    size_t i = 0;
    for (; i + 4 <= samples; i += 4)
        _mm_storeu_ps(out + i, _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i*)(p + i * 4))), scale));
    for (; i < samples; ++i) out[i] = (int32_t)U32(p + i * 4) * (1.0f / 2147483648.0f);
}
inline void StereoFloat32(const uint8_t* p,float* out,size_t samples) {
    const __m128i magnitude = _mm_set1_epi32(0x7FFFFFFF), infinity = _mm_set1_epi32(0x7F800000);
    size_t i = 0;
    for (; i+4 <= samples; i += 4) {
        __m128i v = _mm_loadu_si128((const __m128i*)(p+i*4));
        __m128i finite = _mm_cmplt_epi32(_mm_and_si128(v,magnitude),infinity);
        _mm_storeu_ps(out+i,_mm_castsi128_ps(_mm_and_si128(v,finite)));
    }
    for (; i < samples; ++i) {
        uint32_t u = U32(p+i*4);
        if ((u & 0x7FFFFFFF) >= 0x7F800000) out[i] = 0;
        else memcpy(out+i,&u,4);
    }
}

// Convert planar mono/stereo to the player's interleaved layout four frames at
// a time, checking finite samples in the same pass before publishing the block.
inline bool InterleaveStereo(const float* left,const float* right,float* out,size_t frames) {
    const __m128i magnitude=_mm_set1_epi32(0x7FFFFFFF), infinity=_mm_set1_epi32(0x7F800000);
    size_t i=0;
    for (;i+4<=frames;i+=4) {
        __m128 l=_mm_loadu_ps(left+i),r=_mm_loadu_ps(right+i);
        __m128i valid=_mm_and_si128(_mm_cmplt_epi32(_mm_and_si128(_mm_castps_si128(l),magnitude),infinity),
                                  _mm_cmplt_epi32(_mm_and_si128(_mm_castps_si128(r),magnitude),infinity));
        if (_mm_movemask_epi8(valid)!=65535) return false;
        _mm_storeu_ps(out+i*2,_mm_unpacklo_ps(l,r));
        _mm_storeu_ps(out+i*2+4,_mm_unpackhi_ps(l,r));
    }
    for (;i<frames;++i) {
        uint32_t l,r;memcpy(&l,left+i,4);memcpy(&r,right+i,4);
        if ((l&0x7FFFFFFF)>=0x7F800000 || (r&0x7FFFFFFF)>=0x7F800000) return false;
        out[i*2]=left[i];out[i*2+1]=right[i];
    }
    return true;
}

// Match Cordinal's existing downmix: mono duplicates, 2..5 channels use the front
// pair, 6..8 fold center and rear into the front. LFE is intentionally omitted.
inline void Mix(const float* channels, unsigned count, float* out) {
    if (count == 1) { out[0] = out[1] = channels[0]; }
    else if (count < 6) { out[0] = channels[0]; out[1] = channels[1]; }
    else {
        constexpr float c = 0.7071f, norm = 1.0f / (1.0f + 2 * c);
        out[0] = (channels[0] + c * channels[2] + c * channels[4]) * norm;
        out[1] = (channels[1] + c * channels[2] + c * channels[5]) * norm;
    }
}
} // namespace nativeaudio
