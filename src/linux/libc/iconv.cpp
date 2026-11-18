// cordinal's own iconv (stage 5b of replacing glibc): the conversions the program can ask for. No
// call in cordinal itself uses iconv; the static libstdc++ does, through std::__narrow_multibyte_chars
// (src/c++11/locale-inst.cc), which narrows the C locale's wide separators through
// iconv_open("ASCII//TRANSLIT", nl_langinfo(CODESET)) and iconv_open("ASCII", ...). That codeset is
// cordinal's own ("UTF-8" or "ANSI_X3.4-1968"), so those two pairs are the ones the program needs.
// The rest are the encodings cordinal's metadata and tag work read: UTF-8, ASCII, ISO-8859-1, CP1252
// and the UTF-16 and UTF-32 forms with an explicit byte order.
//
// glibc's answers are what the libc check compares: E2BIG when the output has no room for a whole
// character (the input stays on it), EILSEQ on a byte or value the encoding does not allow, EINVAL
// when the input ends inside a character (glibc keeps the partial bytes nowhere the next call can
// use: a call after it starts afresh, which is what this does), and iconv(cd, NULL, NULL, ...)
// resetting. //TRANSLIT replaces what the target cannot hold and //IGNORE drops it, each counted as
// an irreversible conversion. A name it does not know, or one of the names that need byte-order
// detection ("UTF-16", "UTF-32") or that stand for other scripts (Shift_JIS, EUC-JP and the like),
// is refused with EINVAL; nothing in the program names them.
//
// //TRANSLIT has the substitutions glibc makes for spaces, quotes, dashes and the few symbols and
// ligatures below, and '?' for everything else, which is also glibc's answer for every accented
// letter (glibc's table has no combining-mark decomposition here). glibc's own //IGNORE answers
// EILSEQ for a character it drops; this one drops it as the name promises, and nothing names it.
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern "C" void* malloc(size_t);
extern "C" void free(void*);

namespace {

enum Encoding {
    kUtf8,
    kAscii,
    kLatin1,
    kCp1252,
    kUtf16LE,
    kUtf16BE,
    kUtf32LE,
    kUtf32BE,
    kWcharT,  // glibc's WCHAR_T: UCS-4 in the machine's byte order, with no byte-order mark
};

constexpr bool LittleEndian() {
    union {
        unsigned int value;
        unsigned char bytes[4];
    } probe = {1};
    return probe.bytes[0] == 1;
}

struct Descriptor {
    int from;
    int to;
    bool translit;
    bool ignore;
};

// The names the table knows, lowercased, as glibc's own alias lists have them.
const struct {
    const char* name;
    int encoding;
} kNames[] = {
    {"utf-8", kUtf8},          {"utf8", kUtf8},
    {"ascii", kAscii},         {"us-ascii", kAscii},     {"ansi_x3.4-1968", kAscii},
    {"ansi_x3.4-1986", kAscii}, {"iso-ir-6", kAscii},
    {"iso-8859-1", kLatin1},   {"iso8859-1", kLatin1},   {"iso_8859-1", kLatin1},
    {"latin1", kLatin1},       {"latin-1", kLatin1},     {"l1", kLatin1},
    {"cp819", kLatin1},
    {"cp1252", kCp1252},       {"windows-1252", kCp1252}, {"ms-ansi", kCp1252},
    {"utf-16le", kUtf16LE},    {"utf16le", kUtf16LE},    {"utf-16be", kUtf16BE},
    {"utf16be", kUtf16BE},     {"utf-32le", kUtf32LE},   {"utf32le", kUtf32LE},
    {"utf-32be", kUtf32BE},    {"utf32be", kUtf32BE},    {"wchar_t", kWcharT},
    {"ucs-4", kWcharT},        {"ucs4", kWcharT},
};

// A name's encoding, or -1. A "//TRANSLIT" or "//IGNORE" suffix sets its flag.
int Lookup(const char* name, bool& translit, bool& ignore) {
    char base[40];
    size_t length = 0;
    for (const char* p = name; *p && *p != '/'; ++p) {
        if (length + 1 >= sizeof base) return -1;
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        base[length++] = c;
    }
    base[length] = 0;
    for (const char* p = name + length; *p == '/';) {
        if (p[1] != '/') return -1;
        p += 2;
        const char* end = p;
        while (*end && *end != '/') ++end;
        const size_t suffix = (size_t)(end - p);
        if (suffix == 8 && !strncmp(p, "TRANSLIT", 8)) translit = true;
        else if (suffix == 6 && !strncmp(p, "IGNORE", 6)) ignore = true;
        else return -1;
        p = end;
    }
    for (const auto& entry : kNames)
        if (!strcmp(base, entry.name)) return entry.encoding;
    return -1;
}

// The Unicode value of a CP1252 byte, or 0xFFFFFFFF for the five it leaves undefined.
uint32_t Cp1252ToUnicode(unsigned char byte) {
    static const uint32_t kHigh[32] = {0x20AC, 0xFFFFFFFF, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                       0x02C6, 0x2030,     0x0160, 0x2039, 0x0152, 0xFFFFFFFF, 0x017D, 0xFFFFFFFF,
                                       0xFFFFFFFF, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                       0x02DC, 0x2122,     0x0161, 0x203A, 0x0153, 0xFFFFFFFF, 0x017E, 0x0178};
    if (byte >= 0x80 && byte < 0xA0) return kHigh[byte - 0x80];
    return byte;
}

// The CP1252 byte for a Unicode value, or -1 when it has none.
int UnicodeToCp1252(uint32_t cp) {
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) return (int)cp;
    switch (cp) {
        case 0x20AC: return 0x80;
        case 0x201A: return 0x82;
        case 0x0192: return 0x83;
        case 0x201E: return 0x84;
        case 0x2026: return 0x85;
        case 0x2020: return 0x86;
        case 0x2021: return 0x87;
        case 0x02C6: return 0x88;
        case 0x2030: return 0x89;
        case 0x0160: return 0x8A;
        case 0x2039: return 0x8B;
        case 0x0152: return 0x8C;
        case 0x017D: return 0x8E;
        case 0x2018: return 0x91;
        case 0x2019: return 0x92;
        case 0x201C: return 0x93;
        case 0x201D: return 0x94;
        case 0x2022: return 0x95;
        case 0x2013: return 0x96;
        case 0x2014: return 0x97;
        case 0x02DC: return 0x98;
        case 0x2122: return 0x99;
        case 0x0161: return 0x9A;
        case 0x203A: return 0x9B;
        case 0x0153: return 0x9C;
        case 0x017E: return 0x9E;
        case 0x0178: return 0x9F;
        default: return -1;
    }
}

// What //TRANSLIT makes of a character the target cannot hold: glibc's substitutions for the spaces,
// quotes, dashes and symbols a locale's separators use, and '?' for everything else.
const char* Transliteration(uint32_t cp) {
    switch (cp) {
        case 0x00A0:
        case 0x2000:
        case 0x2001:
        case 0x2002:
        case 0x2003:
        case 0x2004:
        case 0x2005:
        case 0x2006:
        case 0x2007:
        case 0x2008:
        case 0x2009:
        case 0x200A:
        case 0x202F:
        case 0x205F:
        case 0x3000: return " ";
        case 0x2018:
        case 0x2019:
        case 0x201A:
        case 0x201B:
        case 0x2032: return "'";
        case 0x201C:
        case 0x201D:
        case 0x201E:
        case 0x2033: return "\"";
        case 0x00AB: return "<<";
        case 0x00BB: return ">>";
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2013:
        case 0x2014:
        case 0x2015:
        case 0x2212:
        case 0x2027: return "-";
        case 0x2020:
        case 0x2021: return "+";
        case 0x2026: return "...";
        case 0x00A9: return "(C)";
        case 0x00AE: return "(R)";
        case 0x2122: return "(TM)";
        case 0x20AC: return "EUR";
        case 0x00B5: return "u";
        case 0x00C6: return "AE";
        case 0x00E6: return "ae";
        case 0x0152: return "OE";
        case 0x0153: return "oe";
        case 0x00DF: return "ss";
        default: return "?";
    }
}

enum class Step { kOk, kNeedMore, kInvalid };

// One character of `from`, which needs at most six bytes.
Step Decode(int from, const unsigned char* bytes, size_t count, size_t& length, uint32_t& cp) {
    length = 0;
    switch (from) {
        case kUtf8: {
            if (!count) return Step::kNeedMore;
            const unsigned char lead = bytes[0];
            if (lead < 0x80) {
                length = 1;
                cp = lead;
                return Step::kOk;
            }
            unsigned low = 0x80, high = 0xBF;
            int need;
            if (lead < 0xC2) return Step::kInvalid;
            if (lead <= 0xDF) {
                cp = lead & 0x1F;
                need = 1;
            } else if (lead <= 0xEF) {
                cp = lead & 0x0F;
                need = 2;
                if (lead == 0xE0) low = 0xA0;
                else if (lead == 0xED) high = 0x9F;
            } else if (lead <= 0xF7) {
                cp = lead & 0x07;
                need = 3;
                if (lead == 0xF0) low = 0x90;
            } else if (lead <= 0xFB) {  // the longer forms of the old encoding, which glibc takes
                cp = lead & 0x03;
                need = 4;
            } else if (lead <= 0xFD) {
                cp = lead & 0x01;
                need = 5;
            } else {
                return Step::kInvalid;
            }
            for (int i = 1; i <= need; ++i) {
                if ((size_t)i >= count) return Step::kNeedMore;
                const unsigned char byte = bytes[i];
                if (i == 1) {
                    if (byte < low || byte > high) return Step::kInvalid;
                } else if ((byte & 0xC0) != 0x80) {
                    return Step::kInvalid;
                }
                cp = (cp << 6) | (byte & 0x3F);
            }
            length = (size_t)need + 1;
            return Step::kOk;
        }
        case kAscii:
            if (!count) return Step::kNeedMore;
            if (bytes[0] >= 0x80) return Step::kInvalid;
            cp = bytes[0];
            length = 1;
            return Step::kOk;
        case kLatin1:
            if (!count) return Step::kNeedMore;
            cp = bytes[0];
            length = 1;
            return Step::kOk;
        case kCp1252:
            if (!count) return Step::kNeedMore;
            cp = Cp1252ToUnicode(bytes[0]);
            if (cp == 0xFFFFFFFF) return Step::kInvalid;
            length = 1;
            return Step::kOk;
        case kUtf16LE:
        case kUtf16BE: {
            if (count < 2) return Step::kNeedMore;
            const bool little = from == kUtf16LE;
            const uint32_t unit = little ? bytes[0] | (bytes[1] << 8) : ((uint32_t)bytes[0] << 8) | bytes[1];
            if (unit >= 0xDC00 && unit <= 0xDFFF) return Step::kInvalid;  // a lone low half
            if (unit < 0xD800 || unit > 0xDBFF) {
                cp = unit;
                length = 2;
                return Step::kOk;
            }
            if (count < 4) return Step::kNeedMore;
            const uint32_t second = little ? bytes[2] | ((uint32_t)bytes[3] << 8) : ((uint32_t)bytes[2] << 8) | bytes[3];
            if (second < 0xDC00 || second > 0xDFFF) return Step::kInvalid;
            cp = 0x10000 + ((unit - 0xD800) << 10) + (second - 0xDC00);
            length = 4;
            return Step::kOk;
        }
        case kUtf32LE:
        case kUtf32BE: {
            if (count < 4) return Step::kNeedMore;
            const bool little = from == kUtf32LE;
            cp = little ? (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
                              ((uint32_t)bytes[3] << 24)
                        : ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
                              (uint32_t)bytes[3];
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return Step::kInvalid;
            length = 4;
            return Step::kOk;
        }
        case kWcharT: {
            if (count < 4) return Step::kNeedMore;
            const bool little = LittleEndian();
            cp = little ? (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
                              ((uint32_t)bytes[3] << 24)
                        : ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
                              (uint32_t)bytes[3];
            if (cp > 0x7FFFFFFF) return Step::kInvalid;  // the internal form holds any wchar_t
            length = 4;
            return Step::kOk;
        }
        default: return Step::kInvalid;
    }
}

// The bytes of one character in `to`, or 0 when the target has no representation for it.
size_t Encode(int to, uint32_t cp, char* out) {
    switch (to) {
        case kUtf8: {
            if (cp > 0x7FFFFFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
            if (cp < 0x80) {
                out[0] = (char)cp;
                return 1;
            }
            size_t length;
            unsigned lead;
            if (cp < 0x800) {
                length = 2;
                lead = 0xC0;
            } else if (cp < 0x10000) {
                length = 3;
                lead = 0xE0;
            } else if (cp < 0x200000) {
                length = 4;
                lead = 0xF0;
            } else if (cp < 0x4000000) {  // the longer forms of the old encoding
                length = 5;
                lead = 0xF8;
            } else {
                length = 6;
                lead = 0xFC;
            }
            for (size_t i = length; i-- > 0;) {
                if (i == 0) {
                    out[0] = (char)(lead | cp);
                    break;
                }
                out[i] = (char)(0x80 | (cp & 0x3F));
                cp >>= 6;
            }
            return length;
        }
        case kAscii:
            if (cp >= 0x80) return 0;
            out[0] = (char)cp;
            return 1;
        case kLatin1:
            if (cp > 0xFF) return 0;
            out[0] = (char)cp;
            return 1;
        case kCp1252: {
            const int byte = UnicodeToCp1252(cp);
            if (byte < 0) return 0;
            out[0] = (char)byte;
            return 1;
        }
        case kUtf16LE:
        case kUtf16BE: {
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
            const bool little = to == kUtf16LE;
            uint32_t units[2];
            size_t count = 1;
            if (cp < 0x10000) {
                units[0] = cp;
            } else {
                const uint32_t value = cp - 0x10000;
                units[0] = 0xD800 + (value >> 10);
                units[1] = 0xDC00 + (value & 0x3FF);
                count = 2;
            }
            for (size_t i = 0; i < count; ++i) {
                if (little) {
                    out[i * 2] = (char)(units[i] & 0xFF);
                    out[i * 2 + 1] = (char)(units[i] >> 8);
                } else {
                    out[i * 2] = (char)(units[i] >> 8);
                    out[i * 2 + 1] = (char)(units[i] & 0xFF);
                }
            }
            return count * 2;
        }
        case kUtf32LE:
        case kUtf32BE: {
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
            const bool little = to == kUtf32LE;
            for (int i = 0; i < 4; ++i) out[i] = (char)(cp >> (little ? i * 8 : 24 - i * 8));
            return 4;
        }
        case kWcharT: {
            if (cp > 0x7FFFFFFF) return 0;
            const bool little = LittleEndian();
            for (int i = 0; i < 4; ++i) out[i] = (char)(cp >> (little ? i * 8 : 24 - i * 8));
            return 4;
        }
        default: return 0;
    }
}

}  // namespace

#define HIDE(name) __asm__(".hidden " #name);
HIDE(iconv) HIDE(iconv_open) HIDE(iconv_close)
#undef HIDE

extern "C" {
void* iconv_open(const char* tocode, const char* fromcode) {
    bool toTranslit = false, toIgnore = false, fromTranslit = false, fromIgnore = false;
    const int to = Lookup(tocode, toTranslit, toIgnore);
    const int from = Lookup(fromcode, fromTranslit, fromIgnore);
    if (to < 0 || from < 0) {
        errno = EINVAL;
        return (void*)-1;
    }
    Descriptor* descriptor = (Descriptor*)malloc(sizeof(Descriptor));
    if (!descriptor) return (void*)-1;
    descriptor->from = from;
    descriptor->to = to;
    descriptor->translit = toTranslit;
    descriptor->ignore = toIgnore;
    return descriptor;
}

int iconv_close(void* cd) {
    if (!cd || cd == (void*)-1) {
        errno = EBADF;
        return -1;
    }
    free(cd);
    return 0;
}

size_t iconv(void* cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft) {
    if (!cd || cd == (void*)-1) {
        errno = EBADF;
        return (size_t)-1;
    }
    if (!inbuf || !*inbuf) return 0;  // the reset form: these encodings carry no shift state
    const Descriptor& descriptor = *(Descriptor*)cd;
    size_t irreversible = 0;
    while (*inbytesleft) {
        // glibc's WCHAR_T conversion checks that there is room for a whole wide character before it
        // reads one; the gconv modules decode first and answer for the character they find.
        if (descriptor.to == kWcharT && *outbytesleft < 4) {
            errno = E2BIG;
            return (size_t)-1;
        }
        unsigned char bytes[8] = {};
        const size_t available = *inbytesleft < 6 ? *inbytesleft : 6;
        memcpy(bytes, *inbuf, available);
        size_t length = 0;
        uint32_t cp = 0;
        const Step step = Decode(descriptor.from, bytes, available, length, cp);
        if (step == Step::kNeedMore) {
            errno = EINVAL;  // the input ends inside a character; the pointer stays on it
            return (size_t)-1;
        }
        if (step == Step::kInvalid) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        char encoded[16];
        size_t written = Encode(descriptor.to, cp, encoded);
        if (!written) {
            // glibc's modules ask whether there is room for this target's own unit before they
            // decide that a character has no representation.
            size_t unit = 1;
            if (descriptor.to == kUtf16LE || descriptor.to == kUtf16BE) unit = 2;
            else if (descriptor.to == kUtf32LE || descriptor.to == kUtf32BE || descriptor.to == kWcharT) unit = 4;
            if (*outbytesleft < unit) {
                errno = E2BIG;
                return (size_t)-1;
            }
        }
        if (!written && descriptor.translit) {
            const char* replacement = Transliteration(cp);
            const size_t replacementLength = strlen(replacement);
            if (*outbytesleft < replacementLength) {
                errno = E2BIG;
                return (size_t)-1;
            }
            memcpy(*outbuf, replacement, replacementLength);
            *outbuf += replacementLength;
            *outbytesleft -= replacementLength;
            ++irreversible;
            *inbuf += length;
            *inbytesleft -= length;
            continue;
        }
        if (!written && descriptor.ignore) {
            ++irreversible;
            *inbuf += length;
            *inbytesleft -= length;
            continue;
        }
        if (!written) {
            errno = EILSEQ;  // the target cannot hold this character
            return (size_t)-1;
        }
        if (*outbytesleft < written) {
            errno = E2BIG;  // the input stays on this character, for the output to catch up
            return (size_t)-1;
        }
        memcpy(*outbuf, encoded, written);
        *outbuf += written;
        *outbytesleft -= written;
        *inbuf += length;
        *inbytesleft -= length;
    }
    return irreversible;
}
}  // extern "C"
