// Cordinal packet/container reader and sample-accurate decoder adapter. PacketFile
// flattens MP4/ADTS/ASF/Ogg-Opus/Matroska into a packet list; PacketDecoder
// decodes those packets on demand with bounded preroll and periodic state
// checkpoints so seeks stay cheap.
#pragma once
#include "audio_benchmark.h"
#include "audio_file.h"
#include "decoder.h"
#include <cmath>
#include <map>
#include <vector>

namespace nativeaudio {
// Codec IDs are the four-character ISO/Matroska tags, so a value doubles as the
// container's codec identifier.
enum AudioCodec : uint32_t { AudioAac=0x6d703461,AudioAlac=0x616c6163,AudioOpus=0x4f707573,
    AudioFlac=0x664c6143,AudioVorbis=0x566f7262,AudioMpeg=0x6d706567,AudioAc3=0x61632d33,
    AudioPcmLE=0x70636d6c,AudioPcmBE=0x70636d62,AudioPcmFloat=0x70636d66 };
// CRC-32 of an Ogg page, reading its stored checksum (bytes 22..25) as zero; defined in
// audio_ogg.cpp.
uint32_t OggCrc(const uint8_t* page,size_t bytes);
// A packet is either one contiguous file range (pieces == 0) or a run of `pieces`
// segments starting at segments[first]. start/frames are absolute sample counts.
struct Packet { uint64_t offset = 0, start = 0; uint32_t bytes = 0, frames = 0, first = 0, pieces = 0; };
struct Segment { uint64_t offset; uint32_t bytes; };
// Parsed container: the packet list, codec config, and the trim amounts
// (encoder delay/padding and container start/end) needed for an exact timeline.
class PacketFile {
public:
    bool OpenMp4(const std::wstring& path);
    bool OpenAdts(const std::wstring& path);
    bool OpenAsf(const std::wstring& path);
    bool OpenOggOpus(const std::wstring& path);
    bool OpenMatroska(const std::wstring& path,bool index=true);
    bool IsMatroska() {
        const uint8_t* p=file.View(0,4);return p && U32(p,true)==0x1a45dfa3;
    }
    bool Trim(unsigned sampleRate) {
        if (!matroska) return true;
        if (!timestamps.empty()) {
            // A codec frame timeline is exact; container timestamps may be rounded
            // to TimestampScale. Reject discontinuities rather than hide a gap.
            const auto& first=timestamps.front();
            for (const auto& timestamp:timestamps) {
                if (timestamp.first>=packets.size()) return false;
                uint64_t frame=packets[timestamp.first].start;
                if (codec==AudioVorbis && timestamp.first) frame+=consumedDelay;
                double expected=first.second+frame*(1000000000.0/sampleRate);
                if (std::abs(timestamp.second-expected)>timestampScale+2000000000.0/sampleRate) return false;
            }
        }
        auto samples=[&](uint64_t ns) { return (uint64_t)std::llround(ns*(double)sampleRate/1000000000); };
        uint64_t delay=samples(delayNs),end=samples(trimEndNs);
        uint64_t start=delay-std::min(delay,consumedDelay)+samples(trimStartNs);
        if (start>frames || skip>frames-start || end>frames-skip-start) return false;
        skip+=start;valid=frames-skip-end;return valid!=0;
    }
    // Assemble a packet's bytes. Contiguous packets are viewed directly; packets
    // with a prefix or multiple segments (Matroska block groups in a lace) are
    // gathered into scratch_, so the returned pointer is valid only until the next
    // Data() call. Returns nullptr if a segment cannot be mapped.
    const uint8_t* Data(size_t packet) {
        const auto& p=packets[packet];
        static constexpr uint8_t empty=0;
        if (!p.bytes) return &empty;
        if (!p.pieces && prefix.empty()) return file.View(p.offset,p.bytes);
        scratch_=prefix; scratch_.reserve(p.bytes);
        if (!p.pieces) {
            if (p.bytes<prefix.size()) return nullptr;
            const uint8_t* data=file.View(p.offset,p.bytes-prefix.size());if (!data) return nullptr;
            scratch_.insert(scratch_.end(),data,data+p.bytes-prefix.size());
        }
        for (size_t i=0;i<p.pieces;++i) {
            const auto& s=segments[p.first+i]; const uint8_t* data=file.View(s.offset,s.bytes);
            if (!data) return nullptr;
            scratch_.insert(scratch_.end(),data,data+s.bytes);
        }
        return scratch_.size()==p.bytes?scratch_.data():nullptr;
    }
    File file;
    std::vector<Packet> packets;
    std::vector<Segment> segments;
    std::vector<uint8_t> config;
    std::vector<uint8_t> prefix;
    uint32_t codec = 0, rate = 0, channels = 0;
    uint32_t sampleBits=0;
    uint64_t skip = 0, valid = 0, frames = 0;
    uint64_t delayNs=0,trimStartNs=0,trimEndNs=0;
    uint64_t consumedDelay=0; // Priming samples already suppressed by the codec core.
    std::vector<std::pair<size_t,int64_t>> timestamps;
    uint64_t timestampScale=1000000;
    bool matroska=false;
private:
    std::vector<uint8_t> scratch_;
};

// Decode packets only as needed. Codec state is reconstructed from a bounded preroll on seeks.
class PacketDecoder : public Decoder {
public:
    // Fill `count` frames, decoding packets until the request is met or the stream
    // ends. The returned sample range is clipped to the codec's reported start so
    // encoder priming is never emitted.
    size_t Read(float* out, size_t count) override {
        if (failed_) return 0;
        size_t done = 0;
        while (done < count && pos_ < total_) {
            if (used_ == pcm_.size()/2) {
                if (next_ == media_.packets.size()) { failed_=true;break; }
                Remember(next_);
                const auto& packet = media_.packets[next_];
                const uint8_t* bytes = media_.Data(next_++);
                if (!overwritePcm_) pcm_.clear();
                used_ = 0;
                bool decoded=false;
                if (bytes) { CORDINAL_CORE_SCOPE;decoded=Decode(bytes,packet.bytes,pcm_); }
                if (!decoded || pcm_.size()%2 || pcm_.size()/2 > 65536) {
                    pcm_.clear();failed_=true;break;
                }
                const uint64_t target = media_.skip + pos_;
                if (target < packet.start) { pcm_.clear();failed_=true;break; }
                used_ = (size_t)std::min<uint64_t>(pcm_.size()/2,target-packet.start);
                if (used_ == pcm_.size()/2) continue;
            }
            size_t n = (size_t)std::min<uint64_t>({count-done,pcm_.size()/2-used_,total_-pos_});
            memcpy(out+done*2,pcm_.data()+used_*2,n*2*sizeof(float));
            used_ += n; pos_ += n; done += n;
        }
        return done;
    }
    // Seek by seconds. Prefer restoring a captured checkpoint at or before the
    // target; failing that, rewind by Preroll() packets and Reset() the codec.
    bool Seek(double seconds) override {
        if (!std::isfinite(seconds)) return false;
        pos_ = (uint64_t)std::min<double>((double)total_,std::max(0.0,std::floor(seconds*rate_)));
        uint64_t raw = media_.skip+pos_;
        auto at = std::upper_bound(media_.packets.begin(),media_.packets.end(),raw,
            [](uint64_t v,const Packet& p){ return v < p.start; });
        next_ = at == media_.packets.begin() ? 0 : (size_t)(at-media_.packets.begin()-1);
        pcm_.clear(); used_ = 0;failed_=false;
        if (pos_ == total_) { next_ = media_.packets.size();return true; }
        if (Preroll()==SIZE_MAX && !checkpoints_.empty()) {
            auto found=checkpoints_.upper_bound(next_);
            if (found!=checkpoints_.begin()) {
                --found;
                if (Restore(*found->second)) { next_=found->first;return true; }
            }
        }
        next_ -= std::min(next_,Preroll());Reset();
        return true;
    }
    unsigned Rate() const override { return rate_; }
    bool Failed() const override { return failed_; }
    double Duration() const override { return (double)total_/rate_; }
protected:
    // Shared setup: apply container trimming, compute the playable length, and
    // seek to zero. overwritePcm lets a codec resize/reuse the PCM vector across
    // packets instead of clearing it each time.
    bool Ready(unsigned rate,bool overwritePcm=false) {
        // The container is parsed; from here packets are read in order.
        media_.file.Sequential();
        overwritePcm_=overwritePcm;
        rate_ = rate;
        if (!media_.Trim(rate_)) return false;
        if (!rate_ || media_.skip >= media_.frames) return false;
        total_ = media_.frames-media_.skip;
        if (media_.valid) total_ = std::min(total_,media_.valid);
        return Seek(0);
    }
    virtual bool Decode(const uint8_t*,size_t,std::vector<float>&) = 0;
    virtual void Reset() {}
    virtual size_t Preroll() const { return 0; }
    struct State { virtual ~State()=default; };
    virtual std::shared_ptr<const State> Capture() const { return {}; }
    virtual bool Restore(const State&) { return false; }
    PacketFile media_;
    unsigned rate_ = 0;
private:
    // Capture codec state at most every checkpointSeconds_ of playback; a codec
    // that opts out of preroll (Preroll() == SIZE_MAX) is not checkpointed.
    void Remember(size_t packet) {
        if (Preroll()!=SIZE_MAX || checkpoints_.contains(packet)) return;
        uint64_t frame=media_.packets[packet].start,interval=uint64_t(rate_)*checkpointSeconds_;
        auto previous=checkpoints_.upper_bound(packet);
        if (previous!=checkpoints_.begin() && frame-media_.packets[std::prev(previous)->first].start<interval) return;
        auto state=Capture();if (!state) return;
        checkpoints_.emplace(packet,std::move(state));
        if (checkpoints_.size()>128) {
            // Coarsen the sparse history as a long stream is played. Memory stays
            // bounded, and old positions retain coverage for subsequent seeks.
            bool remove=false;
            for (auto it=checkpoints_.begin();it!=checkpoints_.end();) {
                if (remove) it=checkpoints_.erase(it);else ++it;
                remove=!remove;
            }
            checkpointSeconds_*=2;
        }
    }
    uint64_t total_ = 0, pos_ = 0;
    size_t next_ = 0, used_ = 0;
    std::vector<float> pcm_;
    std::map<size_t,std::shared_ptr<const State>> checkpoints_;
    unsigned checkpointSeconds_=5;
    bool failed_=false;
    bool overwritePcm_=false; // The codec overwrites/resizes every sample on success.
};
} // namespace nativeaudio
