// Cordinal Ogg Opus adapter: native rate, header gain, preskip and final granule trimming.
// Owns the packet-level Opus driver: parses each packet's TOC, selects the SILK
// speech layer (configs < 16) and/or the CELT music layer, applies CELT redundancy
// and loss concealment, and exposes 48 kHz interleaved floats to PacketDecoder.
// Key invariant: captures/restores the full SILK + CELT state so seeks are exact.
#include "native_audio.h"
#include "audio_packets.h"
#include "audio_opus_packet.h"
#include "audio_opus_celt.h"
#include "audio_opus_silk.h"
#include <numbers>
#ifdef CORDINAL_CODEC_TRACE
#include <cstdio>
#endif

namespace {
using namespace nativeaudio;
class OpusDecoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path) {
        if (!media_.OpenOggOpus(path)) return false;
        // Header output gain is stored as Q7.8 dB; convert to a linear multiplier.
        gain_=(float)std::pow(10.0,(int16_t)U16(media_.config.data()+16)/(256.0*20));
        return Ready(48000);
    }
private:
    // Snapshot of both layer decoders plus the mode-transition flags. Opus has no
    // per-packet keyframes, so seeking must resume from such a captured state.
    struct History : State {
        std::shared_ptr<const OpusCelt::State> celt;
        std::shared_ptr<const OpusSilk::State> silk;
        int previous;
        bool redundancy;
    };
    std::shared_ptr<const State> Capture() const override {
        auto history=std::make_shared<History>();
        history->celt=celt_.Save();history->silk=silk_.Save();
        history->previous=previous_;history->redundancy=previousRedundancy_;return history;
    }
    bool Restore(const State& state) override {
        const auto& history=static_cast<const History&>(state);
        celt_.Load(*history.celt);silk_.Load(*history.silk);
        previous_=history.previous;previousRedundancy_=history.redundancy;return true;
    }
    // No preroll distance guarantees a clean start: a packet may depend on
    // arbitrarily old history, so the caller must supply a captured state.
    size_t Preroll() const override { return SIZE_MAX; }
    void Reset() override { celt_.Reset(); silk_.Reset(); previous_=-1;previousRedundancy_=false; }
    bool Decode(const uint8_t* data,size_t bytes,std::vector<float>& out) override {
        OpusPacket packet;
        if (!packet.Parse({data,bytes})) return false;
        unsigned config=packet.toc>>3;
        // TOC: config selects mode and frame duration; its low two bits are the
        // duration code, bit 2 is the stereo flag. `end` is the last CELT band
        // present for this configuration.
        unsigned lm=config&3,channels=(packet.toc&4)?2:1,end=config>=28?21:config>=24?19:config>=20?17:13;
        std::vector<float>& block=block_;
        for (auto payload:packet.frames) {
            OpusRange range(payload);
            // 0 = SILK-only, 1 = hybrid (SILK + CELT high band), 2 = CELT-only.
            unsigned mode=config<12?0:config<16?1:2;
            bool redundant=false,atBeginning=false;
            size_t redundantBytes=0;
            uint32_t redundantRange=0;
            std::vector<float> redundancy;
            // SILK layer (configs 0-15). A switch away from CELT-only invalidates
            // SILK's prediction history.
            if (config<16) {
                if (previous_==2) silk_.Reset();
                // config<12 is SILK-only (8/12/16 kHz, 10/20/40/60 ms); configs
                // 12-15 are 10/20 ms hybrid, so SILK carries the low band.
                unsigned ms=config<12?((config&3)==3?60:10u<<(config&3)):(10u<<(config&1));
                unsigned rate=config<4?8000:config<8?12000:16000;
                // A payload of <=1 byte is a lost/empty frame: run concealment.
                if (payload.size()<=1) {
                    if (!silk_.Conceal(channels,rate,ms,block)) return false;
                } else if (!silk_.Decode(range,channels,rate,ms,block)) return false;
                // Optional CELT redundancy: a low-band copy of a previous or
                // upcoming frame, signalled inside the SILK payload, used to hide
                // packet loss. Decoded first if it precedes the SILK data.
                if (range.Tell()+17+(mode==1?20:0)<=(int)payload.size()*8) {
                    redundant=mode==0 || range.Bit(12);
                    if (redundant) {
                        atBeginning=range.Bit(1);
                        redundantBytes=mode==1?range.Uniform(256)+2:payload.size()-(range.Tell()+7)/8;
                        if (redundantBytes>payload.size() || (int)(payload.size()-redundantBytes)*8<range.Tell() ||
                            !range.Restrict(payload.size()-redundantBytes)) return false;
                    }
                }
                auto readRedundancy=[&] {
                    OpusRange red(payload.last(redundantBytes));
                    if (!celt_.Decode(red,channels,1,0,config<4?13:config<12?17:config<14?19:21,redundancy)) return false;
                    redundantRange=red.FinalRange();return true;
                };
                if (redundant && atBeginning && !readRedundancy()) return false;
                if (config>=12) {
                    if (previous_!=1) celt_.Reset();
                    std::vector<float> high;
                    if (!celt_.Decode(range,channels,ms==10?2:3,17,config>=14?21:19,high) || high.size()!=block.size()) return false;
                    for (size_t i=0;i<block.size();++i) block[i]+=high[i];
                } else if (previous_==1) {
                    const uint8_t silence[]{255,255};OpusRange fade(silence);std::vector<float> tail;
                    if (!celt_.Decode(fade,channels,0,0,config<4?13:17,tail)) return false;
                    for (size_t i=0;i<tail.size();++i) block[i]+=tail[i];
                }
                if (redundant) {
                    if (!atBeginning) { celt_.Reset();if (!readRedundancy()) return false; }
                    // Overlap-add the redundancy copy into the frame using the
                    // CELT overlap window (240 samples at 48 kHz).
                    size_t start=atBeginning?120:block.size()/2-120,redStart=120;
                    if (atBeginning) std::copy_n(redundancy.begin(),240,block.begin());
                    for (unsigned i=0;i<120;++i) {
                        double a=std::sin((i+0.5)*(std::numbers::pi/240));
                        float weight=(float)std::pow(std::sin(a*a*(std::numbers::pi/2)),2);
                        for (unsigned ch=0;ch<2;++ch) {
                            float red=redundancy[(redStart+i)*2+ch];
                            float& dst=block[(start+i)*2+ch];
                            dst=atBeginning?red*(1-weight)+dst*weight:dst*(1-weight)+red*weight;
                        }
                    }
                }
            } else {
                // CELT-only layer (configs 16-31).
                if (previous_!=2 && !previousRedundancy_) celt_.Reset();
                if (!celt_.Decode(range,channels,lm,0,end,block)) return false;
            }
#ifdef CORDINAL_CODEC_TRACE
            fprintf(stderr,"range %08x\n",payload.size()<=1?0:range.FinalRange()^redundantRange);
#endif
            if (gain_==1 && out.empty()) out.swap(block);
            else if (gain_==1) out.insert(out.end(),block.begin(),block.end());
            else {
                size_t at=out.size();out.resize(at+block.size());
                for (size_t i=0;i<block.size();++i) out[at+i]=block[i]*gain_;
            }
            // Record mode/redundancy so the next packet knows whether each layer
            // needs to be reset (discontinuous history) or may continue.
            previous_=mode;previousRedundancy_=redundant && !atBeginning;
        }
        return true;
    }
    OpusCelt celt_;
    OpusSilk silk_;
    // Per-frame scratch, kept to reuse its storage; a lone unscaled frame is swapped out whole.
    std::vector<float> block_;
    // Linear multiplier from the Ogg Opus header output gain.
    float gain_=1;
    // Previous packet's mode (-1 = none) and whether it trailed with CELT
    // redundancy; together they decide which layer state must be reset.
    int previous_=-1;
    bool previousRedundancy_=false;
};
}
std::unique_ptr<Decoder> OpenOpusDecoder(const std::wstring& path) {
    auto d=std::make_unique<OpusDecoder>();if (!d->Open(path)) return nullptr;return d;
}
