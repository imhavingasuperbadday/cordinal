// RFC 6716 section 4.1 arithmetic decoder, including backwards raw bits.
// OpusRange owns the bit-exact range-coder state shared by SILK and CELT: it
// decodes entropy symbols from the front and raw bits from the back of the same
// buffer, tracking the consumed-bit count each layer uses for budget accounting.
#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace nativeaudio {
class OpusRange {
public:
    // RFC 4.1.1 init: stride 128 and a value seeded from the first byte; the
    // initial Normalize() then consumes the first byte pair.
    explicit OpusRange(std::span<const uint8_t> bytes):bytes_(bytes) {
        previous_=Byte(); range_=128; value_=127-(previous_>>1); Normalize();
    }
    // Maps the current value into [0,total) and caches unit_=range_/total for the
    // following Update(), which narrows the interval and renormalizes.
    __forceinline uint32_t Frequency(uint32_t total) {
        if (!total || total>65536 || range_<total) { good_=false; return 0; }
        unit_=range_/total;
        return total-std::min(value_/unit_+1,total);
    }
    __forceinline void Update(uint32_t low,uint32_t high,uint32_t total) {
        if (low>=high || high>total) { good_=false; return; }
        value_-=unit_*(total-high);
        range_=low?unit_*(high-low):range_-unit_*(total-high);
        Normalize();
    }
    // Decodes a symbol from an absolute-frequency table (pdf); the table must sum
    // to at most 65536.
    unsigned Symbol(std::span<const uint16_t> pdf) {
        unsigned total=0; for (auto p:pdf) total+=p;
        unsigned target=Frequency(total),low=0;
        for (unsigned i=0;i<pdf.size();++i) {
            unsigned high=low+pdf[i]; if (target<high) { Update(low,high,total); return i; } low=high;
        }
        good_=false; return 0;
    }
    // Fast path for 2-entry binary iCDFs {prob, 0}.
    __forceinline unsigned Icdf2(uint8_t prob) {
        const uint32_t total=256;
        if (range_<total) { good_=false; return 0; }
        const uint32_t unit=range_>>8, threshold=unit*prob;
        if (value_>=threshold) {
            value_-=threshold; range_-=threshold; Normalize(); return 0;
        }
        range_=threshold; Normalize(); return 1;
    }
    // Decodes from a cumulative iCDF (counts ending in 0) at the given precision;
    // the default 8-bit form is what the SILK/CELT model tables use.
    // Checking value_ >= unit * cumulative[i] avoids both integer divisions per call.
    __forceinline unsigned Icdf(std::span<const uint8_t> cumulative,unsigned precision=8) {
        const uint32_t total=1u<<precision;
        if (range_<total) { good_=false; return 0; }
        const uint32_t unit=range_>>precision;
        for (unsigned i=0;i<cumulative.size();++i) {
            const uint32_t c=cumulative[i];
            if (value_>=unit*c) {
                value_-=unit*c;
                range_=i?unit*(cumulative[i-1]-c):range_-unit*c;
                Normalize(); return i;
            }
        }
        good_=false; return 0;
    }
    // Decodes one bit that is 1 with probability 1/2^logp.
    __forceinline bool Bit(unsigned logp) {
        uint32_t unit=range_>>logp; bool bit=value_<unit;
        if (bit) range_=unit;
        else { value_-=unit; range_-=unit; }
        Normalize(); return bit;
    }
    // Reads `count` raw bits from the end of the buffer, least-significant first
    // (RFC 4.1.2); they count toward the bit budget but not the range state.
    __forceinline uint32_t Raw(unsigned count) {
        if (count==1) {
            if (raw_>=bytes_.size()*8) { good_=false; return 0; }
            uint32_t bit=(bytes_[bytes_.size()-1-(raw_>>3)]>>(raw_&7))&1u;
            ++raw_; ++total_; return bit;
        }
        if (count>32 || count>bytes_.size()*8-std::min(raw_,bytes_.size()*8)) { good_=false; return 0; }
        if (count<=25 && bytes_.size()-(raw_>>3)>=4) {
            uint32_t w;
            std::memcpy(&w,bytes_.data()+bytes_.size()-4-(raw_>>3),4);
#ifdef _MSC_VER
            w=_byteswap_ulong(w);
#else
            w=__builtin_bswap32(w);
#endif
            uint32_t value=(w>>(raw_&7))&((1u<<count)-1);
            raw_+=count; total_+=count; return value;
        }
        // Take the rest of the current byte at a time rather than single bits.
        uint32_t value=0;
        for (unsigned got=0;got<count;) {
            unsigned bit=raw_&7,take=std::min(8-bit,count-got);
            value|=uint32_t((bytes_[bytes_.size()-1-(raw_>>3)]>>bit)&((1u<<take)-1))<<got;
            got+=take; raw_+=take;
        }
        total_+=count; return value;
    }
    // Decodes a uniform value in [0,count): an arithmetic-coded prefix then raw low bits.
    __forceinline uint32_t Uniform(uint32_t count) {
        if (count<2) return 0;
        unsigned width=32-std::countl_zero(count-1),extra=width>8?width-8:0;
        uint32_t high=extra?((count-1)>>extra)+1:count;
        uint32_t value=Frequency(high); Update(value,value+1,high);
        value=(value<<extra)|Raw(extra);
        if (value>=count) { good_=false; return count-1; }
        return value;
    }
    // Bits consumed so far (whole bits; conservative for the terminating SILK
    // symbol, see Good()).
    __forceinline int Tell() const { return total_-(32-std::countl_zero(range_)); }
    // Consumed position in 1/8-bit units, for CELT's fractional budget accounting.
    // RFC 6716 derives the three fractional bits of log2(range) by repeated squaring; they are
    // equally the number of thresholds 2^15*2^(j/8), rounded up, that the top 16 bits reach.
    __forceinline int TellFraction() const {
        unsigned l=32-std::countl_zero(range_),r=range_>>(l-16);
        unsigned fraction=r>=46341?(r>=55110?(r>=60098?7:6):(r>=50536?5:4))
                                  :(r>=38968?(r>=42496?3:2):(r>=35734?1:0));
        return total_*8-(int)(l*8+fraction);
    }
    // Final range-coder state; deterministic, so usable as a noise seed and as a
    // packet-identity value for tracing.
    uint32_t FinalRange() const { return range_; }
    size_t Bytes() const { return bytes_.size(); }
    // Truncates the buffer, e.g. to carve off a trailing CELT redundancy copy.
    bool Restrict(size_t bytes) {
        if (bytes>bytes_.size() || raw_>bytes*8) { good_=false;return false; }
        bytes_=bytes_.first(bytes);return true;
    }
    // A terminating SILK arithmetic code can consume the conservative extra
    // bit counted by Tell(). CELT's explicit allocation uses the strict budget.
    bool Good(unsigned terminating=0) const { return good_ && Tell()<=(int)bytes_.size()*8+(int)terminating; }
    // Reports only the internal validity flag, without Good()'s bit-budget check.
    bool Healthy() const { return good_; }
    void Fail() { good_=false; }
private:
    // Returns the next byte; past the end it yields zero, so the coder may read a
    // little beyond the payload (Good() catches overruns afterwards).
    __forceinline uint8_t Byte() { return front_<bytes_.size()?bytes_[front_++]:0; }
    // Renormalizes while range_ <= 2^23, pulling a new byte and folding in the
    // second-order bit shuffle of RFC 4.1.1.
    __forceinline void Normalize() {
        while (range_<=0x800000) {
            range_<<=8; uint8_t next=Byte();
            unsigned symbol=((previous_&1)<<7)|(next>>1); previous_=next;
            value_=((value_<<8)+(255-symbol))&0x7FFFFFFF; total_+=8;
        }
    }
    // front_ indexes bytes read from the front for entropy decoding; raw_ counts
    // raw bits read backwards from the end. previous_ retains the last byte for
    // the byte-pair shuffle; total_ is bits consumed including initialisation.
    std::span<const uint8_t> bytes_;
    size_t front_=0,raw_=0;
    uint32_t range_=0,value_=0,unit_=0;
    uint8_t previous_=0;
    int total_=9;
    bool good_=true;
};
} // namespace nativeaudio
