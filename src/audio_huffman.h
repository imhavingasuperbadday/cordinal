// Bounded prefix-code lookup shared by Cordinal packet codecs.
#pragma once
#include "audio_bits.h"
#include <array>
#include <span>
#include <vector>

namespace nativeaudio {
// Compact binary tries plus a 10-bit first lookup, built once from numeric format data.
// Complete trees store only (symbol << 5) | length, in left-to-right leaf order.
// A complete binary trie built in leaf order from (symbol, length) pairs, plus a
// 10-bit direct lookup. Read() consumes one code; a missing branch fails the bit
// cursor instead of returning a symbol.
class PrefixBook {
public:
    struct Node { int16_t child[2]{}; };
    std::vector<Node> nodes;
    static constexpr unsigned lookupBits=10;
    std::array<uint16_t,1u<<lookupBits> fast{};
    explicit PrefixBook(std::span<const uint16_t> codes) {
        nodes.reserve(codes.size()-1); // A complete binary tree has leaves-1 internal nodes.
        nodes.push_back({});
        uint32_t position=0;
        for (uint16_t leaf:codes) {
            unsigned n=leaf&31, symbol=leaf>>5, code=position>>(32-n), at=0;
            position+=uint32_t(1)<<(32-n);
            for (unsigned i=n;i--;) {
                unsigned bit=(code>>i)&1;
                if (!i) nodes[at].child[bit]=(int16_t)(-int(symbol)-1);
                else {
                    int next=nodes[at].child[bit];
                    if (!next) { next=(int)nodes.size(); nodes[at].child[bit]=(int16_t)next; nodes.push_back({}); }
                    at=next;
                }
            }
        }
        for (unsigned prefix=0;prefix<fast.size();++prefix) {
            int at=0; unsigned n=0;
            while (n<lookupBits && at>=0) at=nodes[at].child[(prefix>>(lookupBits-1-n++))&1];
            // A zero length encodes the continuation node after lookupBits bits.
            fast[prefix]=(uint16_t)(at<0?((-at-1)<<5)|n:at<<5);
        }
    }
    __forceinline int Read(Bits& b) const {
        int at=0;
        if (b.Left()>=lookupBits) {
            unsigned v=fast[(size_t)b.Peek(lookupBits)];
            if (v&31) { b.Skip(v&31); return v>>5; }
            b.Skip(lookupBits); at=v>>5;
        }
        for (int i=0;i<31 && b.Good();++i) {
            at=nodes[at].child[(size_t)b.Get(1)];
            if (at<0) return -at-1;
            if (!at) break;
        }
        b.Fail(); return 0;
    }
};
} // namespace nativeaudio
