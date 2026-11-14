// The sfnt reader behind the text stack (typeface.h): tables, cmap, glyf and CFF/CFF2 outlines,
// CBDT/sbix color bitmaps and the kern table.
#include "typeface.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lx::type {

int32_t MulFix(int32_t a, int32_t b) {
    int64_t p = (int64_t)a * b;
    return (int32_t)(p < 0 ? -((-p + 0x8000) >> 16) : (p + 0x8000) >> 16);
}

int32_t DivFix(int32_t a, int32_t b) {
    if (b == 0) return 0x7FFFFFFF;
    bool neg = (a < 0) != (b < 0);
    uint64_t ua = (uint64_t)std::llabs(a), ub = (uint64_t)std::llabs(b);
    uint64_t q = ((ua << 16) + (ub >> 1)) / ub;
    if (q > 0x7FFFFFFF) q = 0x7FFFFFFF;
    return neg ? -(int32_t)q : (int32_t)q;
}

bool Outline::Box(int32_t* xMin, int32_t* yMin, int32_t* xMax, int32_t* yMax) const {
    if (x.empty()) return false;
    *xMin = *xMax = x[0];
    *yMin = *yMax = y[0];
    for (size_t i = 1; i < x.size(); ++i) {
        *xMin = std::min(*xMin, x[i]), *xMax = std::max(*xMax, x[i]);
        *yMin = std::min(*yMin, y[i]), *yMax = std::max(*yMax, y[i]);
    }
    return true;
}

namespace {

bool TableDirectory(Bytes file, size_t at, std::vector<std::pair<uint32_t, Bytes>>* out) {
    uint32_t version = file.U32(at);
    if (version != 0x00010000 && version != Tag("OTTO") && version != Tag("true")) return false;
    uint16_t count = file.U16(at + 4);
    if (!file.Has(at + 12, (size_t)count * 16)) return false;
    for (uint16_t i = 0; i < count; ++i) {
        size_t rec = at + 12 + (size_t)i * 16;
        uint32_t tag = file.U32(rec), off = file.U32(rec + 8), len = file.U32(rec + 12);
        if (file.Has(off, len)) out->push_back({tag, file.Sub(off, len)});
    }
    return true;
}

void AppendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
    else if (c < 0x10000) s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
    else s += (char)(0xF0 | c >> 18), s += (char)(0x80 | (c >> 12 & 0x3F)), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
}

// A name as FreeType picks it: a Windows English record first, then any Windows Unicode one, then
// the Macintosh English one, then a Unicode-platform one.
std::string Name(Bytes name, uint16_t id) {
    uint16_t count = name.U16(2), strings = name.U16(4);
    int win = -1, winAny = -1, mac = -1, uni = -1;
    for (uint16_t i = 0; i < count; ++i) {
        size_t r = 6 + (size_t)i * 12;
        if (!name.Has(r, 12) || name.U16(r + 6) != id || name.U16(r + 8) == 0) continue;
        uint16_t platform = name.U16(r), encoding = name.U16(r + 2), lang = name.U16(r + 4);
        if (platform == 3 && (encoding <= 1 || encoding == 10)) {
            if ((lang & 0x3FF) == 0x009 && win < 0) win = i;
            if (winAny < 0) winAny = i;
        } else if (platform == 1 && encoding == 0 && lang == 0 && mac < 0) {
            mac = i;
        } else if (platform == 0 && uni < 0) {
            uni = i;
        }
    }
    int pick = win >= 0 ? win : winAny >= 0 ? winAny : mac >= 0 ? mac : uni;
    if (pick < 0) return {};
    size_t r = 6 + (size_t)pick * 12;
    Bytes s = name.Sub((size_t)strings + name.U16(r + 10), name.U16(r + 8));
    std::string out;
    if (name.U16(r) == 1) {
        for (size_t i = 0; i < s.n; ++i) AppendUtf8(out, s.p[i] < 0x80 ? s.p[i] : '?');
        return out;
    }
    for (size_t i = 0; i + 1 < s.n; i += 2) {
        uint32_t c = s.U16(i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < s.n) {
            uint32_t lo = s.U16(i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), i += 2;
        }
        if (c) AppendUtf8(out, c);
    }
    return out;
}

// ---- CFF INDEX and DICT ------------------------------------------------------------------------

struct Index {
    Bytes data;          // the whole file
    size_t offsets = 0;  // where the offset array starts
    size_t base = 0;     // the byte before the first object (offsets are 1-based)
    uint32_t count = 0;
    uint8_t offSize = 0;
    size_t end = 0;      // just past the INDEX
    Bytes Get(uint32_t i) const {
        if (i >= count) return {};
        auto off = [&](uint32_t k) {
            size_t at = offsets + (size_t)k * offSize;
            uint32_t v = 0;
            for (int b = 0; b < offSize; ++b) v = v << 8 | data.U8(at + b);
            return v;
        };
        uint32_t a = off(i), b = off(i + 1);
        if (a == 0 || b < a) return {};
        if (!data.Has(base + a, b - a)) return {};
        return data.Sub(base + a, b - a);
    }
};

bool ReadIndex(Bytes data, size_t at, bool cff2, Index* out) {
    *out = {};
    out->data = data;
    size_t head = cff2 ? 4 : 2;
    if (!data.Has(at, head)) return false;
    out->count = cff2 ? data.U32(at) : data.U16(at);
    if (out->count == 0) {
        out->end = at + head;
        return true;
    }
    out->offSize = data.U8(at + head);
    if (out->offSize < 1 || out->offSize > 4 || out->count > 0x1000000) return false;
    out->offsets = at + head + 1;
    size_t table = (size_t)(out->count + 1) * out->offSize;
    if (!data.Has(out->offsets, table)) return false;
    out->base = out->offsets + table - 1;
    uint32_t last = 0;
    for (int b = 0; b < out->offSize; ++b) last = last << 8 | data.U8(out->offsets + (size_t)out->count * out->offSize + b);
    out->end = out->base + last;
    return data.Has(out->base, last);
}

// A DICT's operators with their operands; escaped operators are 1200 + the second byte.
struct Dict {
    std::vector<std::pair<int, std::vector<double>>> ops;
    const std::vector<double>* Find(int op) const {
        for (auto& [o, v] : ops)
            if (o == op) return &v;
        return nullptr;
    }
    double Number(int op, double def, size_t i = 0) const {
        auto* v = Find(op);
        return v && v->size() > i ? (*v)[i] : def;
    }
};

Dict ReadDict(Bytes d) {
    Dict out;
    std::vector<double> stack;
    size_t i = 0;
    while (i < d.n) {
        uint8_t b = d.U8(i);
        if (b <= 21 || b == 22 || b == 23 || b == 24) {
            int op = b;
            ++i;
            if (b == 12) op = 1200 + d.U8(i++);
            if (b == 23) {  // CFF2 blend: keep the default values
                if (stack.empty()) break;
                size_t n = (size_t)stack.back();
                stack.pop_back();
                stack.resize(std::min(stack.size(), n));
                continue;
            }
            out.ops.push_back({op, stack});
            stack.clear();
        } else if (b == 28) {
            stack.push_back(d.S16(i + 1)), i += 3;
        } else if (b == 29) {
            stack.push_back((int32_t)d.U32(i + 1)), i += 5;
        } else if (b == 30) {
            std::string s;
            ++i;
            bool done = false;
            while (i < d.n && !done) {
                uint8_t v = d.U8(i++);
                for (int nib : {v >> 4, v & 15}) {
                    if (nib <= 9) s += (char)('0' + nib);
                    else if (nib == 0xA) s += '.';
                    else if (nib == 0xB) s += 'E';
                    else if (nib == 0xC) s += "E-";
                    else if (nib == 0xE) s += '-';
                    else if (nib == 0xF) {
                        done = true;
                        break;
                    }
                }
            }
            stack.push_back(std::strtod(s.c_str(), nullptr));
        } else if (b >= 32 && b <= 246) {
            stack.push_back(b - 139), ++i;
        } else if (b >= 247 && b <= 250) {
            stack.push_back((b - 247) * 256 + d.U8(i + 1) + 108), i += 2;
        } else if (b >= 251 && b <= 254) {
            stack.push_back(-(b - 251) * 256 - d.U8(i + 1) - 108), i += 2;
        } else {
            ++i;  // reserved
        }
        if (stack.size() > 513) break;
    }
    return out;
}

int Bias(uint32_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

}  // namespace

// ---- CFF and CFF2 ------------------------------------------------------------------------------

struct Face::Cff {
    Bytes data;
    bool cff2 = false;
    Index charStrings, globalSubrs;
    std::vector<Index> localSubrs;  // one per font dict (one for a non-CID CFF)
    std::vector<int> regionCounts;  // CFF2: regions per ItemVariationData, for blend
    std::vector<int> fdVsindex;
    Bytes fdSelect;
    int fdFormat = -1;
    uint32_t FontDict(uint16_t glyph) const {
        if (fdFormat < 0) return 0;
        if (fdFormat == 0) return fdSelect.U8(1 + glyph);
        if (fdFormat == 3) {
            uint16_t n = fdSelect.U16(1);
            for (uint16_t r = 0; r < n; ++r) {
                size_t at = 3 + (size_t)r * 3;
                uint16_t first = fdSelect.U16(at), next = fdSelect.U16(at + 3);
                if (glyph >= first && glyph < next) return fdSelect.U8(at + 2);
            }
            return 0;
        }
        if (fdFormat == 4) {
            uint32_t n = fdSelect.U32(1);
            for (uint32_t r = 0; r < n && r < 65536; ++r) {
                size_t at = 5 + (size_t)r * 6;
                uint32_t first = fdSelect.U32(at), next = fdSelect.U32(at + 6);
                if (glyph >= first && glyph < next) return fdSelect.U16(at + 4);
            }
        }
        return 0;
    }
};

namespace {

bool ReadPrivate(Bytes data, const Dict& fd, bool cff2, Index* subrs, int* vsindex) {
    auto* p = fd.Find(18);
    *subrs = {};
    if (!p || p->size() < 2) return true;
    size_t size = (size_t)(*p)[0], at = (size_t)(*p)[1];
    if (!data.Has(at, size)) return false;
    Dict priv = ReadDict(data.Sub(at, size));
    if (vsindex) *vsindex = (int)priv.Number(22, 0);
    if (auto* s = priv.Find(19); s && !s->empty() && (*s)[0] > 0) ReadIndex(data, at + (size_t)(*s)[0], cff2, subrs);
    return true;
}

std::shared_ptr<Face::Cff> LoadCff(Bytes data, bool cff2) {
    auto cff = std::make_shared<Face::Cff>();
    cff->data = data;
    cff->cff2 = cff2;
    Dict top;
    size_t afterTop;
    if (!cff2) {
        if (data.U8(0) != 1) return nullptr;
        Index names, tops, strings;
        if (!ReadIndex(data, data.U8(2), false, &names) || !ReadIndex(data, names.end, false, &tops) || tops.count < 1 ||
            !ReadIndex(data, tops.end, false, &strings))
            return nullptr;
        top = ReadDict(tops.Get(0));
        afterTop = strings.end;
        if (top.Number(1206, 2) != 2) return nullptr;  // Type 1 charstrings
    } else {
        if (data.U8(0) != 2) return nullptr;
        uint8_t headerSize = data.U8(2);
        uint16_t topSize = data.U16(3);
        if (!data.Has(headerSize, topSize)) return nullptr;
        top = ReadDict(data.Sub(headerSize, topSize));
        afterTop = (size_t)headerSize + topSize;
    }
    if (!ReadIndex(data, afterTop, cff2, &cff->globalSubrs)) return nullptr;
    double cs = top.Number(17, 0);
    if (cs <= 0 || !ReadIndex(data, (size_t)cs, cff2, &cff->charStrings) || cff->charStrings.count == 0) return nullptr;
    if (auto* fda = top.Find(1236); fda && !fda->empty()) {
        Index fds;
        if (!ReadIndex(data, (size_t)(*fda)[0], cff2, &fds)) return nullptr;
        for (uint32_t i = 0; i < fds.count && i < 256; ++i) {
            Index subrs;
            int vs = 0;
            ReadPrivate(data, ReadDict(fds.Get(i)), cff2, &subrs, &vs);
            cff->localSubrs.push_back(subrs);
            cff->fdVsindex.push_back(vs);
        }
        if (auto* sel = top.Find(1237); sel && !sel->empty()) {
            cff->fdSelect = data.Sub((size_t)(*sel)[0]);
            cff->fdFormat = cff->fdSelect.U8(0);
        } else if (fds.count > 1) {
            cff->fdFormat = -1;
        }
    } else {
        Index subrs;
        int vs = 0;
        ReadPrivate(data, top, cff2, &subrs, &vs);
        cff->localSubrs.push_back(subrs);
        cff->fdVsindex.push_back(vs);
    }
    if (cff2) {
        if (double vstore = top.Number(24, 0); vstore > 0) {
            // The VariationStore: a length, then format, region list, and ItemVariationData offsets.
            Bytes vs = data.Sub((size_t)vstore + 2);
            uint16_t count = vs.U16(6);
            for (uint16_t i = 0; i < count; ++i) {
                Bytes ivd = vs.Sub(vs.U32(8 + (size_t)i * 4));
                cff->regionCounts.push_back(ivd.U16(4));
            }
        }
    }
    return cff;
}

// The Type 2 charstring interpreter, collecting the outline in font units.
struct CharString {
    const Face::Cff& cff;
    const Index& local;
    int vsindex;
    std::vector<double> xs, ys;
    std::vector<uint8_t> tags;
    std::vector<uint16_t> ends;
    double stack[520];
    int sp = 0;
    double x = 0, y = 0;
    int stems = 0;
    bool widthDone = false, open = false, ended = false;
    double transient[32] = {};
    int depth = 0;

    void Close() {
        if (!open) return;
        size_t first = ends.empty() ? 0 : ends.back() + 1;
        // A closing point on top of the start is dropped, as FreeType does.
        if (xs.size() > first + 1 && tags.back() == 1 && xs.back() == xs[first] && ys.back() == ys[first]) {
            xs.pop_back(), ys.pop_back(), tags.pop_back();
        }
        ends.push_back((uint16_t)(xs.size() - 1));
        open = false;
    }
    void Move(double dx, double dy) {
        Close();
        x += dx, y += dy;
        xs.push_back(x), ys.push_back(y), tags.push_back(1);
        open = true;
    }
    void Line(double dx, double dy) {
        if (!open) Move(0, 0);
        x += dx, y += dy;
        xs.push_back(x), ys.push_back(y), tags.push_back(1);
    }
    void Curve(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
        if (!open) Move(0, 0);
        double x1 = x + dx1, y1 = y + dy1, x2 = x1 + dx2, y2 = y1 + dy2;
        x = x2 + dx3, y = y2 + dy3;
        xs.push_back(x1), ys.push_back(y1), tags.push_back(2);
        xs.push_back(x2), ys.push_back(y2), tags.push_back(2);
        xs.push_back(x), ys.push_back(y), tags.push_back(1);
    }
    // CFF1: a stack-clearing operator's optional leading width.
    void Width(bool odd) {
        if (!widthDone && !cff.cff2 && odd && sp > 0) {
            std::memmove(stack, stack + 1, sizeof(double) * (sp - 1));
            --sp;
        }
        widthDone = true;
    }
    void Stems() {
        Width(sp % 2 == 1);
        stems += sp / 2;
        sp = 0;
    }
    bool Run(Bytes cs) {
        if (++depth > 10) return false;
        size_t i = 0;
        while (i < cs.n && !ended) {
            if (xs.size() > 100000) return false;
            uint8_t b = cs.U8(i);
            if (b >= 32 || b == 28) {
                double v;
                if (b == 28) v = cs.S16(i + 1), i += 3;
                else if (b <= 246) v = b - 139, i += 1;
                else if (b <= 250) v = (b - 247) * 256 + cs.U8(i + 1) + 108, i += 2;
                else if (b <= 254) v = -(b - 251) * 256 - cs.U8(i + 1) - 108, i += 2;
                else v = (int32_t)cs.U32(i + 1) / 65536.0, i += 5;
                if (sp >= 513) return false;
                stack[sp++] = v;
                continue;
            }
            ++i;
            switch (b) {
            case 1: case 3: case 18: case 23:  // hstem vstem hstemhm vstemhm
                Stems();
                break;
            case 19: case 20:  // hintmask cntrmask (with implied vstems)
                Stems();
                i += (stems + 7) / 8;
                break;
            case 21:  // rmoveto
                Width(sp > 2);
                if (sp >= 2) Move(stack[sp - 2], stack[sp - 1]);
                sp = 0;
                break;
            case 22:  // hmoveto
                Width(sp > 1);
                if (sp >= 1) Move(stack[sp - 1], 0);
                sp = 0;
                break;
            case 4:  // vmoveto
                Width(sp > 1);
                if (sp >= 1) Move(0, stack[sp - 1]);
                sp = 0;
                break;
            case 5:  // rlineto
                for (int k = 0; k + 1 < sp; k += 2) Line(stack[k], stack[k + 1]);
                sp = 0;
                break;
            case 6: case 7: {  // hlineto vlineto
                bool h = b == 6;
                for (int k = 0; k < sp; ++k, h = !h) h ? Line(stack[k], 0) : Line(0, stack[k]);
                sp = 0;
                break;
            }
            case 8:  // rrcurveto
                for (int k = 0; k + 5 < sp; k += 6) Curve(stack[k], stack[k + 1], stack[k + 2], stack[k + 3], stack[k + 4], stack[k + 5]);
                sp = 0;
                break;
            case 24: {  // rcurveline
                int k = 0;
                for (; k + 5 < sp - 2; k += 6) Curve(stack[k], stack[k + 1], stack[k + 2], stack[k + 3], stack[k + 4], stack[k + 5]);
                if (k + 1 < sp) Line(stack[k], stack[k + 1]);
                sp = 0;
                break;
            }
            case 25: {  // rlinecurve
                int k = 0;
                for (; k + 1 < sp - 6; k += 2) Line(stack[k], stack[k + 1]);
                if (k + 5 < sp) Curve(stack[k], stack[k + 1], stack[k + 2], stack[k + 3], stack[k + 4], stack[k + 5]);
                sp = 0;
                break;
            }
            case 26: {  // vvcurveto
                int k = 0;
                double dx1 = 0;
                if (sp % 2) dx1 = stack[k++];
                for (; k + 3 < sp; k += 4, dx1 = 0) Curve(dx1, stack[k], stack[k + 1], stack[k + 2], 0, stack[k + 3]);
                sp = 0;
                break;
            }
            case 27: {  // hhcurveto
                int k = 0;
                double dy1 = 0;
                if (sp % 2) dy1 = stack[k++];
                for (; k + 3 < sp; k += 4, dy1 = 0) Curve(stack[k], dy1, stack[k + 1], stack[k + 2], stack[k + 3], 0);
                sp = 0;
                break;
            }
            case 30: case 31: {  // vhcurveto hvcurveto
                bool h = b == 31;
                int k = 0;
                while (k + 3 < sp) {
                    bool last = sp - k == 5;
                    double e = last ? stack[k + 4] : 0;
                    if (h) Curve(stack[k], 0, stack[k + 1], stack[k + 2], e, stack[k + 3]);
                    else Curve(0, stack[k], stack[k + 1], stack[k + 2], stack[k + 3], e);
                    k += last ? 5 : 4;
                    h = !h;
                }
                sp = 0;
                break;
            }
            case 10: case 29: {  // callsubr callgsubr
                if (sp < 1) return false;
                const Index& subrs = b == 10 ? local : cff.globalSubrs;
                int n = (int)stack[--sp] + Bias(subrs.count);
                if (n < 0 || (uint32_t)n >= subrs.count) return false;
                if (!Run(subrs.Get((uint32_t)n))) return false;
                break;
            }
            case 11:  // return
                --depth;
                return true;
            case 14:  // endchar
                Width(sp == 1 || sp == 5);
                Close();
                ended = true;
                break;
            case 15:  // CFF2 vsindex
                if (sp >= 1) vsindex = (int)stack[sp - 1];
                sp = 0;
                break;
            case 16: {  // CFF2 blend: the default master's values stay
                if (sp < 1) return false;
                int n = (int)stack[--sp];
                int k = vsindex >= 0 && vsindex < (int)cff.regionCounts.size() ? cff.regionCounts[vsindex] : 0;
                if (n < 0 || (int64_t)n * (k + 1) > sp) return false;
                sp -= n * k;
                break;
            }
            case 12: {
                uint8_t e = cs.U8(i++);
                double* s = stack;
                switch (e) {
                case 35:  // flex
                    if (sp >= 12) Curve(s[0], s[1], s[2], s[3], s[4], s[5]), Curve(s[6], s[7], s[8], s[9], s[10], s[11]);
                    break;
                case 34:  // hflex
                    if (sp >= 7) Curve(s[0], 0, s[1], s[2], s[3], 0), Curve(s[4], 0, s[5], -s[2], s[6], 0);
                    break;
                case 36:  // hflex1
                    if (sp >= 9)
                        Curve(s[0], s[1], s[2], s[3], s[4], 0), Curve(s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
                    break;
                case 37:  // flex1
                    if (sp >= 11) {
                        double dx = s[0] + s[2] + s[4] + s[6] + s[8], dy = s[1] + s[3] + s[5] + s[7] + s[9];
                        bool horizontal = std::fabs(dx) > std::fabs(dy);
                        Curve(s[0], s[1], s[2], s[3], s[4], s[5]);
                        Curve(s[6], s[7], s[8], s[9], horizontal ? s[10] : -dx, horizontal ? -dy : s[10]);
                    }
                    break;
                case 3: if (sp >= 2) s[sp - 2] = s[sp - 2] && s[sp - 1], --sp; continue;
                case 4: if (sp >= 2) s[sp - 2] = s[sp - 2] || s[sp - 1], --sp; continue;
                case 5: if (sp >= 1) s[sp - 1] = !s[sp - 1]; continue;
                case 9: if (sp >= 1) s[sp - 1] = std::fabs(s[sp - 1]); continue;
                case 10: if (sp >= 2) s[sp - 2] += s[sp - 1], --sp; continue;
                case 11: if (sp >= 2) s[sp - 2] -= s[sp - 1], --sp; continue;
                case 12: if (sp >= 2) s[sp - 2] = s[sp - 1] ? s[sp - 2] / s[sp - 1] : 0, --sp; continue;
                case 14: if (sp >= 1) s[sp - 1] = -s[sp - 1]; continue;
                case 15: if (sp >= 2) s[sp - 2] = s[sp - 2] == s[sp - 1], --sp; continue;
                case 18: if (sp >= 1) --sp; continue;
                case 20:
                    if (sp >= 2) {
                        int at = (int)s[sp - 1];
                        if (at >= 0 && at < 32) transient[at] = s[sp - 2];
                        sp -= 2;
                    }
                    continue;
                case 21:
                    if (sp >= 1) {
                        int at = (int)s[sp - 1];
                        s[sp - 1] = at >= 0 && at < 32 ? transient[at] : 0;
                    }
                    continue;
                case 22:
                    if (sp >= 4) s[sp - 4] = s[sp - 2] <= s[sp - 1] ? s[sp - 4] : s[sp - 3], sp -= 3;
                    continue;
                case 23: if (sp < 513) s[sp++] = 0.5; continue;
                case 24: if (sp >= 2) s[sp - 2] *= s[sp - 1], --sp; continue;
                case 26: if (sp >= 1) s[sp - 1] = std::sqrt(std::fabs(s[sp - 1])); continue;
                case 27: if (sp >= 1 && sp < 513) s[sp] = s[sp - 1], ++sp; continue;
                case 28: if (sp >= 2) std::swap(s[sp - 1], s[sp - 2]); continue;
                case 29:
                    if (sp >= 1) {
                        int k = (int)s[sp - 1];
                        if (k < 0) k = 0;
                        s[sp - 1] = k < sp - 1 ? s[sp - 2 - k] : 0;
                    }
                    continue;
                case 30:
                    if (sp >= 2) {
                        int n = (int)s[sp - 2], j = (int)s[sp - 1];
                        sp -= 2;
                        if (n > 0 && n <= sp) {
                            j = ((j % n) + n) % n;
                            std::rotate(s + sp - n, s + sp - j, s + sp);
                        }
                    }
                    continue;
                default:
                    break;
                }
                sp = 0;
                break;
            }
            default:
                sp = 0;  // reserved operators clear the stack
                break;
            }
        }
        --depth;
        return true;
    }
};

}  // namespace

// ---- Loading -----------------------------------------------------------------------------------

FontData MapFile(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st;
    void* map = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size < ((uint64_t)1 << 32))
        map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return nullptr;
    size_t size = (size_t)st.st_size;
    return FontData(new std::string_view((const char*)map, size), [map, size](const std::string_view* v) {
        munmap(map, size);
        delete v;
    });
}

FontData FromBytes(std::shared_ptr<const std::string> bytes) {
    if (!bytes) return nullptr;
    return FontData(new std::string_view(*bytes), [bytes](const std::string_view* v) { delete v; });
}

int Face::Count(std::string_view data) {
    Bytes file{(const unsigned char*)data.data(), data.size()};
    uint32_t v = file.U32(0);
    if (v == Tag("ttcf")) return (int)std::min<uint32_t>(file.U32(8), 0xFFFF);
    return v == 0x00010000 || v == Tag("OTTO") || v == Tag("true") ? 1 : 0;
}

Bytes Face::Table(uint32_t tag) const {
    for (auto& [t, b] : tables_)
        if (t == tag) return b;
    return {};
}

std::shared_ptr<Face> Face::Load(FontData data, int index) {
    if (!data) return nullptr;
    auto face = std::shared_ptr<Face>(new Face());
    face->data_ = data;
    face->file_ = {(const unsigned char*)data->data(), data->size()};
    Bytes file = face->file_;
    size_t at = 0;
    index &= 0xFFFF;
    if (file.U32(0) == Tag("ttcf")) {
        if ((uint32_t)index >= file.U32(8)) return nullptr;
        at = file.U32(12 + (size_t)index * 4);
    } else if (index != 0) {
        return nullptr;
    }
    if (!TableDirectory(file, at, &face->tables_)) return nullptr;
    Bytes head = face->Table(Tag("head")), hhea = face->Table(Tag("hhea")), os2 = face->Table(Tag("OS/2"));
    Bytes maxp = face->Table(Tag("maxp")), name = face->Table(Tag("name"));
    face->glyphCount = maxp.U16(4);
    face->unitsPerEm = head.U16(18);
    if (face->unitsPerEm < 16 || face->unitsPerEm > 16384) face->unitsPerEm = 1000;
    face->integerPpem = head.U16(16) & 8;
    face->longLoca_ = head.S16(50) == 1;
    face->ascender = hhea.S16(4);
    face->descender = hhea.S16(6);
    face->lineGap = hhea.S16(8);
    face->numHMetrics_ = hhea.U16(34);
    if (face->ascender == 0 && face->descender == 0 && os2.n >= 78) {
        face->ascender = os2.S16(68), face->descender = os2.S16(70), face->lineGap = os2.S16(72);
        if (face->ascender == 0 && face->descender == 0)
            face->ascender = os2.U16(74), face->descender = -(int)os2.U16(76), face->lineGap = 0;
    }
    if (face->descender > 0) face->descender = -face->descender;
    Bytes post = face->Table(Tag("post"));
    face->underlinePosition = post.S16(8), face->underlineThickness = post.S16(10);
    if (os2.n >= 64) {
        uint16_t sel = os2.U16(62);
        face->italic = sel & (1 | 512);
        face->bold = sel & 32;
    } else {
        uint16_t mac = head.U16(44);
        face->bold = mac & 1, face->italic = mac & 2;
    }
    bool wws = os2.n >= 64 && (os2.U16(62) & 256);
    for (uint16_t id : wws ? std::initializer_list<uint16_t>{21, 16, 1} : std::initializer_list<uint16_t>{16, 1})
        if (face->family.empty()) face->family = Name(name, id);
    for (uint16_t id : wws ? std::initializer_list<uint16_t>{22, 17, 2} : std::initializer_list<uint16_t>{17, 2})
        if (face->style.empty()) face->style = Name(name, id);

    face->hmtx_ = face->Table(Tag("hmtx"));
    face->kern_ = face->Table(Tag("kern"));
    face->gsub = face->Table(Tag("GSUB"));
    face->gpos = face->Table(Tag("GPOS"));
    face->gdef = face->Table(Tag("GDEF"));
    face->cbdt_ = face->Table(Tag("CBDT"));
    face->cblc_ = face->Table(Tag("CBLC"));
    if (face->cbdt_.Empty() || face->cblc_.Empty()) face->cbdt_ = face->cblc_ = {};
    face->sbix_ = face->Table(Tag("sbix"));
    face->glyf_ = face->Table(Tag("glyf"));
    face->loca_ = face->Table(Tag("loca"));
    if (!face->glyf_.Empty() && !face->loca_.Empty()) {
        face->outlineKind_ = 1;
    } else if (Bytes cff = face->Table(Tag("CFF ")); !cff.Empty()) {
        if ((face->cff_ = LoadCff(cff, false))) face->outlineKind_ = 2;
    } else if (Bytes cff2 = face->Table(Tag("CFF2")); !cff2.Empty()) {
        if ((face->cff_ = LoadCff(cff2, true))) face->outlineKind_ = 3;
    }
    if (face->cff_ && face->glyphCount == 0) face->glyphCount = (int)std::min<uint32_t>(face->cff_->charStrings.count, 65535);
    if (!face->ReadCmap() || face->glyphCount == 0) return nullptr;
    return face;
}

bool Face::ReadCmap() {
    Bytes cmap = Table(Tag("cmap"));
    uint16_t count = cmap.U16(2);
    int bestScore = 0;
    for (uint16_t i = 0; i < count; ++i) {
        size_t r = 4 + (size_t)i * 8;
        uint16_t platform = cmap.U16(r), encoding = cmap.U16(r + 2);
        Bytes sub = cmap.Sub(cmap.U32(r + 4));
        uint16_t format = sub.U16(0);
        if (platform == 0 && encoding == 5 && format == 14) {
            cmap14_ = sub;
            continue;
        }
        int score = 0;
        bool full = format == 12 || format == 13;
        if (format != 0 && format != 4 && format != 6 && !full) continue;
        if (platform == 3 && encoding == 10) score = full ? 6 : 3;
        else if (platform == 0 && (encoding == 4 || encoding == 6)) score = full ? 5 : 3;
        else if (platform == 3 && encoding == 1) score = 4;
        else if (platform == 0) score = full ? 5 : 3;
        else if (platform == 3 && encoding == 0) score = 2;
        else if (platform == 1 && encoding == 0 && format == 0) score = 1;
        if (score > bestScore) {
            bestScore = score;
            cmap_ = sub;
            cmapFormat_ = format;
            symbolCmap_ = platform == 3 && encoding == 0;
        }
    }
    return bestScore > 0;
}

uint16_t Face::GlyphIndex(uint32_t ch) const {
    auto lookup = [&](uint32_t c) -> uint32_t {
        const Bytes& t = cmap_;
        switch (cmapFormat_) {
        case 0:
            return c < 256 ? t.U8(6 + c) : 0;
        case 4: {
            if (c > 0xFFFF) return 0;
            uint16_t segX2 = t.U16(6);
            size_t ends = 14, starts = 16 + (size_t)segX2, deltas = starts + segX2, ranges = deltas + segX2;
            size_t lo = 0, hi = segX2 / 2;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (t.U16(ends + mid * 2) < c) lo = mid + 1;
                else hi = mid;
            }
            if (lo >= segX2 / 2u) return 0;
            uint16_t start = t.U16(starts + lo * 2);
            if (c < start) return 0;
            uint16_t delta = t.U16(deltas + lo * 2), ro = t.U16(ranges + lo * 2);
            if (ro == 0) return (c + delta) & 0xFFFF;
            uint16_t g = t.U16(ranges + lo * 2 + ro + (c - start) * 2);
            return g ? (g + delta) & 0xFFFF : 0;
        }
        case 6: {
            uint16_t first = t.U16(6), n = t.U16(8);
            return c >= first && c - first < n ? t.U16(10 + (size_t)(c - first) * 2) : 0;
        }
        case 12: case 13: {
            uint32_t n = t.U32(12);
            size_t lo = 0, hi = std::min<size_t>(n, (t.n - std::min<size_t>(t.n, 16)) / 12);
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                size_t g = 16 + mid * 12;
                if (t.U32(g + 4) < c) lo = mid + 1;
                else hi = mid;
            }
            size_t g = 16 + lo * 12;
            if (!t.Has(g, 12) || t.U32(g) > c || t.U32(g + 4) < c) return 0;
            return cmapFormat_ == 12 ? t.U32(g + 8) + (c - t.U32(g)) : t.U32(g + 8);
        }
        }
        return 0;
    };
    uint32_t g = lookup(ch);
    if (!g && symbolCmap_ && ch < 0x100) g = lookup(ch + 0xF000);
    return g < (uint32_t)glyphCount ? (uint16_t)g : 0;
}

uint16_t Face::VariantGlyph(uint32_t ch, uint32_t selector) const {
    uint32_t n = cmap14_.U32(6);
    for (uint32_t i = 0; i < n && i < 256; ++i) {
        size_t r = 10 + (size_t)i * 11;
        if (cmap14_.U24(r) != selector) continue;
        uint32_t nonDefault = cmap14_.U32(r + 7);
        if (!nonDefault) return 0;
        Bytes m = cmap14_.Sub(nonDefault);
        uint32_t count = m.U32(0);
        size_t lo = 0, hi = std::min<size_t>(count, m.n / 5);
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            uint32_t v = m.U24(4 + mid * 5);
            if (v == ch) {
                uint16_t g = m.U16(4 + mid * 5 + 3);
                return g < glyphCount ? g : 0;
            }
            if (v < ch) lo = mid + 1;
            else hi = mid;
        }
        return 0;
    }
    return 0;
}

int Face::Advance(uint16_t glyph) const {
    if (numHMetrics_ == 0) return 0;
    size_t i = std::min<size_t>(glyph, (size_t)numHMetrics_ - 1);
    return hmtx_.U16(i * 4);
}

int Face::LeftBearing(uint16_t glyph) const {
    if (glyph < numHMetrics_) return hmtx_.S16((size_t)glyph * 4 + 2);
    return hmtx_.S16((size_t)numHMetrics_ * 4 + (size_t)(glyph - numHMetrics_) * 2);
}

int Face::KernTable(uint16_t left, uint16_t right) const {
    // Version 0 (Microsoft) subtables of format 0 with horizontal kerning values.
    if (kern_.U16(0) != 0) return 0;
    uint16_t count = kern_.U16(2);
    size_t at = 4;
    int value = 0;
    for (uint16_t i = 0; i < count && kern_.Has(at, 6); ++i) {
        uint16_t length = kern_.U16(at + 2), coverage = kern_.U16(at + 4);
        if ((coverage >> 8) == 0 && (coverage & 1) && !(coverage & 4)) {
            Bytes sub = kern_.Sub(at + 6);
            uint16_t pairs = sub.U16(0);
            uint32_t key = (uint32_t)left << 16 | right;
            size_t lo = 0, hi = std::min<size_t>(pairs, sub.n >= 8 ? (sub.n - 8) / 6 : 0);
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                uint32_t k = sub.U32(8 + mid * 6);
                if (k == key) {
                    int v = sub.S16(8 + mid * 6 + 4);
                    value = (coverage & 8) ? v : value + v;
                    break;
                }
                if (k < key) lo = mid + 1;
                else hi = mid;
            }
        }
        if (length < 6) break;
        at += length;
    }
    return value;
}

// ---- Outlines ----------------------------------------------------------------------------------

bool Face::GetOutline(uint16_t glyph, int32_t xScale, int32_t yScale, bool roundComposites, Outline* out) const {
    out->Clear();
    if (glyph >= glyphCount) return false;
    if (outlineKind_ == 1) return GlyfOutline(glyph, xScale, yScale, roundComposites, 0, out);
    if (outlineKind_ >= 2) return CffOutline(glyph, xScale, yScale, out);
    return false;
}

bool Face::GlyfOutline(uint16_t glyph, int32_t xScale, int32_t yScale, bool roundComposites, int depth, Outline* out) const {
    if (depth > 8 || glyph >= glyphCount) return false;
    size_t start, end;
    if (longLoca_) start = loca_.U32((size_t)glyph * 4), end = loca_.U32((size_t)glyph * 4 + 4);
    else start = (size_t)loca_.U16((size_t)glyph * 2) * 2, end = (size_t)loca_.U16((size_t)glyph * 2 + 2) * 2;
    if (end <= start || !glyf_.Has(start, end - start)) return true;  // an empty glyph (a space)
    Bytes g = glyf_.Sub(start, end - start);
    int16_t contours = g.S16(0);
    if (contours >= 0) {
        size_t first = out->x.size();
        size_t base = out->ends.empty() ? 0 : out->ends.back() + 1;
        if (first != base) return false;
        if (!g.Has(10, (size_t)contours * 2 + 2)) return false;
        uint16_t points = contours ? g.U16(10 + (size_t)(contours - 1) * 2) + 1 : 0;
        if (first + points > 65535) return false;
        uint16_t prev = 0;
        for (int c = 0; c < contours; ++c) {
            uint16_t e = g.U16(10 + (size_t)c * 2);
            if ((c && e < prev) || e >= points) return false;
            prev = e;
            out->ends.push_back((uint16_t)(first + e));
        }
        size_t at = 10 + (size_t)contours * 2;
        at += 2 + g.U16(at);
        std::vector<uint8_t> flags(points);
        for (uint16_t i = 0; i < points;) {
            if (!g.Has(at, 1)) return false;
            uint8_t f = g.U8(at++);
            flags[i++] = f;
            if (f & 8) {
                uint8_t repeat = g.U8(at++);
                while (repeat-- && i < points) flags[i++] = f;
            }
        }
        int32_t v = 0;
        std::vector<int32_t> xs(points), ys(points);
        for (uint16_t i = 0; i < points; ++i) {
            uint8_t f = flags[i];
            if (f & 2) v += (f & 16) ? g.U8(at) : -(int)g.U8(at), at += 1;
            else if (!(f & 16)) v += g.S16(at), at += 2;
            xs[i] = v;
        }
        v = 0;
        for (uint16_t i = 0; i < points; ++i) {
            uint8_t f = flags[i];
            if (f & 4) v += (f & 32) ? g.U8(at) : -(int)g.U8(at), at += 1;
            else if (!(f & 32)) v += g.S16(at), at += 2;
            ys[i] = v;
        }
        if (at > g.n) return false;
        for (uint16_t i = 0; i < points; ++i) out->Add(MulFix(xs[i], xScale), MulFix(ys[i], yScale), flags[i] & 1);
        return true;
    }
    // A composite: each component's outline, moved and transformed.
    size_t at = 10;
    for (int components = 0; components < 256; ++components) {
        if (!g.Has(at, 4)) return false;
        uint16_t flags = g.U16(at), sub = g.U16(at + 2);
        at += 4;
        int32_t a1, a2;
        if (flags & 1) a1 = g.S16(at), a2 = g.S16(at + 2), at += 4;
        else if (flags & 2) a1 = g.S8(at), a2 = g.S8(at + 1), at += 2;
        else a1 = g.U8(at), a2 = g.U8(at + 1), at += 2;
        int32_t xx = 0x10000, xy = 0, yx = 0, yy = 0x10000;  // 16.16
        bool transform = false;
        if (flags & 8) {
            xx = yy = (int32_t)g.S16(at) << 2, at += 2, transform = true;
        } else if (flags & 0x40) {
            xx = (int32_t)g.S16(at) << 2, yy = (int32_t)g.S16(at + 2) << 2, at += 4, transform = true;
        } else if (flags & 0x80) {
            xx = (int32_t)g.S16(at) << 2, yx = (int32_t)g.S16(at + 2) << 2, xy = (int32_t)g.S16(at + 4) << 2,
            yy = (int32_t)g.S16(at + 6) << 2, at += 8, transform = true;
        }
        size_t from = out->x.size();
        size_t contourFrom = out->ends.size();
        if (!GlyfOutline(sub, xScale, yScale, roundComposites, depth + 1, out)) return false;
        size_t to = out->x.size();
        if (transform) {
            for (size_t i = from; i < to; ++i) {
                int32_t px = out->x[i], py = out->y[i];
                out->x[i] = MulFix(px, xx) + MulFix(py, xy);
                out->y[i] = MulFix(px, yx) + MulFix(py, yy);
            }
        }
        int32_t dx, dy;
        if (flags & 2) {
            dx = MulFix(a1, xScale), dy = MulFix(a2, yScale);
            if ((flags & 4) && roundComposites) dy = (dy + 32) & ~63;  // vertical only, as FreeType's light hinting
        } else {
            // Point matching: the parent's point a1 meets the component's point a2.
            size_t p1 = (size_t)(uint32_t)a1, p2 = from + (size_t)(uint32_t)a2;
            if (p1 >= from || p2 >= to) return false;
            dx = out->x[p1] - out->x[p2], dy = out->y[p1] - out->y[p2];
        }
        for (size_t i = from; i < to; ++i) out->x[i] += dx, out->y[i] += dy;
        (void)contourFrom;
        if (!(flags & 0x20)) break;
    }
    return true;
}

bool Face::CffOutline(uint16_t glyph, int32_t xScale, int32_t yScale, Outline* out) const {
    const Cff& cff = *cff_;
    uint32_t fd = cff.FontDict(glyph);
    static const Index kNone;
    const Index& local = fd < cff.localSubrs.size() ? cff.localSubrs[fd] : kNone;
    CharString cs{cff, local, fd < cff.fdVsindex.size() ? cff.fdVsindex[fd] : 0};
    if (!cs.Run(cff.charStrings.Get(glyph))) return false;
    cs.Close();
    if (cs.xs.size() > 65535) return false;
    double sx = xScale / 65536.0, sy = yScale / 65536.0;
    for (size_t i = 0; i < cs.xs.size(); ++i)
        out->Add((int32_t)std::lround(cs.xs[i] * sx), (int32_t)std::lround(cs.ys[i] * sy), cs.tags[i]);
    out->ends = std::move(cs.ends);
    return true;
}

// ---- Color bitmaps -----------------------------------------------------------------------------

bool Face::GetColorImage(uint16_t glyph, int ppem, ColorImage* out) const {
    *out = {};
    if (glyph >= glyphCount) return false;
    if (!cbdt_.Empty() && CbdtImage(glyph, ppem, out)) return true;
    return !sbix_.Empty() && SbixImage(glyph, ppem, out);
}

bool Face::CbdtImage(uint16_t glyph, int ppem, ColorImage* out) const {
    uint32_t sizes = cblc_.U32(4);
    // The smallest strike at least ppem, else the largest.
    int best = -1, bestPpem = 0;
    for (uint32_t i = 0; i < sizes && i < 256; ++i) {
        size_t r = 8 + (size_t)i * 48;
        if (!cblc_.Has(r, 48)) break;
        uint16_t first = cblc_.U16(r + 40), last = cblc_.U16(r + 42);
        if (glyph < first || glyph > last) continue;
        int p = cblc_.U8(r + 45);
        bool better = best < 0 || (p >= ppem ? bestPpem < ppem || p < bestPpem : bestPpem < ppem && p > bestPpem);
        if (better) best = (int)i, bestPpem = p;
    }
    if (best < 0) return false;
    size_t r = 8 + (size_t)best * 48;
    size_t array = cblc_.U32(r);
    uint32_t subtables = cblc_.U32(r + 8);
    for (uint32_t s = 0; s < subtables && s < 4096; ++s) {
        size_t e = array + (size_t)s * 8;
        uint16_t first = cblc_.U16(e), last = cblc_.U16(e + 2);
        if (glyph < first || glyph > last) continue;
        size_t sub = array + cblc_.U32(e + 4);
        uint16_t indexFormat = cblc_.U16(sub), imageFormat = cblc_.U16(sub + 2);
        uint32_t dataOffset = cblc_.U32(sub + 4);
        size_t at = 0, len = 0;
        Bytes big;  // metrics from the index (formats 2 and 5)
        uint32_t k = glyph - first;
        if (indexFormat == 1) {
            uint32_t a = cblc_.U32(sub + 8 + (size_t)k * 4), b = cblc_.U32(sub + 12 + (size_t)k * 4);
            at = dataOffset + a, len = b > a ? b - a : 0;
        } else if (indexFormat == 3) {
            uint16_t a = cblc_.U16(sub + 8 + (size_t)k * 2), b = cblc_.U16(sub + 10 + (size_t)k * 2);
            at = dataOffset + a, len = b > a ? b - a : 0;
        } else if (indexFormat == 2) {
            uint32_t size = cblc_.U32(sub + 8);
            big = cblc_.Sub(sub + 12, 8);
            at = dataOffset + (size_t)size * k, len = size;
        } else if (indexFormat == 4 || indexFormat == 5) {
            bool five = indexFormat == 5;
            size_t list = five ? sub + 24 : sub + 12;
            uint32_t n = cblc_.U32(five ? sub + 20 : sub + 8);
            if (five) big = cblc_.Sub(sub + 12, 8);
            for (uint32_t j = 0; j < n && j < 65536; ++j) {
                if (five) {
                    if (cblc_.U16(list + (size_t)j * 2) == glyph) {
                        uint32_t size = cblc_.U32(sub + 8);
                        at = dataOffset + (size_t)size * j, len = size;
                        break;
                    }
                } else if (cblc_.U16(list + (size_t)j * 4) == glyph) {
                    uint16_t a = cblc_.U16(list + (size_t)j * 4 + 2), b = cblc_.U16(list + (size_t)j * 4 + 6);
                    at = dataOffset + a, len = b > a ? b - a : 0;
                    break;
                }
            }
        }
        if (!len || !cbdt_.Has(at, len)) return false;
        Bytes img = cbdt_.Sub(at, len);
        Bytes metrics;
        size_t png;
        if (imageFormat == 17) metrics = img.Sub(0, 5), png = 5;
        else if (imageFormat == 18) metrics = img.Sub(0, 8), png = 8;
        else if (imageFormat == 19) metrics = big, png = 0;
        else return false;
        if (metrics.n < 5) return false;
        uint32_t pngLen = img.U32(png);
        if (!img.Has(png + 4, pngLen) || pngLen == 0) return false;
        out->png = {(const char*)img.p + png + 4, pngLen};
        out->ppem = bestPpem;
        out->height = metrics.U8(0), out->width = metrics.U8(1);
        out->left = metrics.S8(2), out->top = metrics.S8(3), out->advance = metrics.U8(4);
        return true;
    }
    return false;
}

bool Face::SbixImage(uint16_t glyph, int ppem, ColorImage* out) const {
    uint32_t strikes = sbix_.U32(4);
    int best = -1, bestPpem = 0;
    for (uint32_t i = 0; i < strikes && i < 256; ++i) {
        Bytes s = sbix_.Sub(sbix_.U32(8 + (size_t)i * 4));
        int p = s.U16(0);
        uint32_t a = s.U32(4 + (size_t)glyph * 4), b = s.U32(8 + (size_t)glyph * 4);
        if (b <= a + 8 || !s.Has(a, b - a)) continue;
        bool better = best < 0 || (p >= ppem ? bestPpem < ppem || p < bestPpem : bestPpem < ppem && p > bestPpem);
        if (better) best = (int)i, bestPpem = p;
    }
    if (best < 0) return false;
    Bytes s = sbix_.Sub(sbix_.U32(8 + (size_t)best * 4));
    uint32_t a = s.U32(4 + (size_t)glyph * 4), b = s.U32(8 + (size_t)glyph * 4);
    Bytes g = s.Sub(a, b - a);
    if (g.U32(4) != Tag("png ")) return false;
    out->png = {(const char*)g.p + 8, g.n - 8};
    // The PNG's size from its IHDR chunk.
    Bytes png = g.Sub(8);
    if (png.U32(12) != Tag("IHDR")) return false;
    out->width = (int)std::min<uint32_t>(png.U32(16), 4096), out->height = (int)std::min<uint32_t>(png.U32(20), 4096);
    out->ppem = bestPpem;
    out->left = g.S16(0);
    out->top = g.S16(2) + out->height;
    return true;
}

}  // namespace lx::type
