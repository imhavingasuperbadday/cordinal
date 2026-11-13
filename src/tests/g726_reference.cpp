// Independent test oracle for G.726 (see g726_reference.h). Each function below is one sub-block of
// section 4.2 of the recommendation, written as the recommendation states it: unsigned words of
// the stated width, two's complement (TC), signed magnitude (SM) and the 11-bit floating format
// (FL: sign, 4-bit exponent, 6-bit mantissa), with explicit masks and sign extension. Nothing here
// is shared with the player's decoder (audio_g726.cpp), which uses ordinary signed arithmetic.
// DQ is kept as 16-bit signed magnitude at every rate, which the recommendation requires at
// 40 kbit/s and permits at the others.
#include "g726_reference.h"

namespace g726ref {
namespace {

// The position of the highest set bit plus one (0 for 0): every EXP table of the recommendation.
uint32_t Exp(uint32_t mag) {
    uint32_t e = 0;
    while (mag >> e) ++e;
    return e;
}

// 4.2.6 FMULT: a 16-bit TC coefficient times an 11-bit FL signal, giving 16-bit TC.
uint32_t Fmult(uint32_t an, uint32_t srn) {
    const uint32_t ans = an >> 15;
    const uint32_t anmag = ans == 0 ? an >> 2 : (16384 - (an >> 2)) & 8191;
    const uint32_t anexp = Exp(anmag);
    const uint32_t anmant = anmag == 0 ? 1 << 5 : (anmag << 6) >> anexp;
    const uint32_t srns = srn >> 10, srnexp = (srn >> 6) & 15, srnmant = srn & 63;
    const uint32_t wans = srns ^ ans;
    const uint32_t wanexp = srnexp + anexp;
    const uint32_t wanmant = ((srnmant * anmant) + 48) >> 4;
    const uint32_t wanmag = wanexp <= 26 ? (wanmant << 7) >> (26 - wanexp) : ((wanmant << 7) << (wanexp - 26)) & 32767;
    return wans == 0 ? wanmag : (65536 - wanmag) & 65535;
}

// FLOATA: 16-bit SM DQ to FL.
uint32_t FloatA(uint32_t dq) {
    const uint32_t dqs = dq >> 15, mag = dq & 32767, exp = Exp(mag);
    const uint32_t mant = mag == 0 ? 1 << 5 : (mag << 6) >> exp;
    return (dqs << 10) + (exp << 6) + mant;
}

// FLOATB: 16-bit TC SR to FL.
uint32_t FloatB(uint32_t sr) {
    const uint32_t srs = sr >> 15, mag = srs == 0 ? sr : (65536 - sr) & 32767, exp = Exp(mag);
    const uint32_t mant = mag == 0 ? 1 << 5 : (mag << 6) >> exp;
    return (srs << 10) + (exp << 6) + mant;
}

// 16-bit SM DQ to 16-bit TC (the conversion in ADDB and ADDC).
uint32_t DqToTc(uint32_t dq) { return (dq >> 15) == 0 ? dq : (65536 - (dq & 32767)) & 65535; }

// The rate tables, indexed by |I| (IM): RECONST's DQLN (12-bit TC), FUNCTW's WI (12-bit TC) and
// FUNCTF's FI, from Tables 11-14 and the FUNCTW and FUNCTF blocks.
struct Rate {
    unsigned levels;          // magnitudes |I| = 0 .. levels - 1
    const uint32_t* dqln;     // by |I|
    const uint32_t* wi;
    const uint32_t* fi;
    const int* decision;      // QUAN: lowest DLN of |I| = 1, 2, ... (signed); below the first is |I| = 0
};
const uint32_t kDqln2[] = {116, 365}, kWi2[] = {4074, 439}, kFi2[] = {0, 7};
const uint32_t kDqln3[] = {2048, 135, 273, 373}, kWi3[] = {4092, 30, 137, 582}, kFi3[] = {0, 1, 2, 7};
const uint32_t kDqln4[] = {2048, 4, 135, 213, 273, 323, 373, 425}, kWi4[] = {4084, 18, 41, 64, 112, 198, 355, 1122},
               kFi4[] = {0, 0, 0, 1, 1, 1, 3, 7};
const uint32_t kDqln5[] = {2048, 4030, 28, 104, 169, 224, 274, 318, 358, 395, 429, 459, 488, 514, 539, 566},
               kWi5[] = {14, 14, 24, 39, 40, 41, 58, 100, 141, 179, 219, 280, 358, 440, 529, 696},
               kFi5[] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 3, 4, 5, 6, 6};
// Tables 7-10, as signed DLN thresholds: |I| = n where decision[n - 1] <= DLN < decision[n].
const int kDecision2[] = {261};
const int kDecision3[] = {8, 218, 331};
const int kDecision4[] = {-124, 80, 178, 246, 300, 349, 400};
const int kDecision5[] = {-122, -16, 68, 139, 198, 250, 298, 339, 378, 413, 445, 475, 502, 528, 553};
const Rate kRates[4] = {{2, kDqln2, kWi2, kFi2, kDecision2}, {4, kDqln3, kWi3, kFi3, kDecision3},
                        {8, kDqln4, kWi4, kFi4, kDecision4}, {16, kDqln5, kWi5, kFi5, kDecision5}};
}  // namespace

// ACCUM over the FMULT products, and MIX with LIMA for the scale factor.
void Codec::Estimate() {
    uint32_t sezi = 0;
    for (int i = 0; i < 6; ++i) sezi = (sezi + Fmult(b_[i], dq_[i])) & 65535;
    const uint32_t sei = (((sezi + Fmult(a_[1], sr_[1])) & 65535) + Fmult(a_[0], sr_[0])) & 65535;
    sez_ = sezi >> 1;
    se_ = sei >> 1;
    // LIMA, then MIX.
    const uint32_t al = ap_ >= 256 ? 64 : ap_ >> 2;
    const uint32_t dif = (yu_ + 16384 - (yl_ >> 6)) & 16383, difs = dif >> 13;
    const uint32_t difm = difs == 0 ? dif : (16384 - dif) & 8191;
    const uint32_t prodm = (difm * al) >> 6;
    const uint32_t prod = difs == 0 ? prodm : (16384 - prodm) & 16383;
    y_ = ((yl_ >> 6) + prod) & 8191;
}

unsigned Codec::Encode(int sl) {
    Estimate();
    const Rate& rate = kRates[bits_ - 2];
    // SUBTA (SL is 14-bit TC; SE is 15-bit TC).
    const uint32_t slw = (uint32_t)sl & 16383, sls = slw >> 13, sli = sls == 0 ? slw : 49152 + slw;
    const uint32_t ses = se_ >> 14, sei = ses == 0 ? se_ : 32768 + se_;
    const uint32_t d = (sli + 65536 - sei) & 65535;
    // LOG
    const uint32_t ds = d >> 15, dqm = ds == 0 ? d : (65536 - d) & 32767;
    const uint32_t exp = dqm ? Exp(dqm) - 1 : 0;  // 0 for 0 and 1, 1 for 2-3, ...
    const uint32_t mant = ((dqm << 7) >> exp) & 127;
    const uint32_t dl = (exp << 7) + mant;
    // SUBTB, then QUAN on the signed DLN.
    const uint32_t dln = (dl + 4096 - (y_ >> 2)) & 4095;
    const int signedDln = dln >= 2048 ? (int)dln - 4096 : (int)dln;
    unsigned m = 0;
    while (m + 1 < rate.levels && signedDln >= rate.decision[m]) ++m;
    // Negative codes count down from the top; |I| = 0 is the all-ones code except at 16 kbit/s,
    // whose even-level quantizer has no zero level.
    const unsigned top = (1u << bits_) - 1;
    unsigned code;
    if (bits_ == 2) code = ds ? top - m : m;
    else code = m == 0 ? top : ds ? top - m : m;
    Reconstruct(code);
    return code;
}

int Codec::Decode(unsigned code) {
    Estimate();
    const uint32_t sr = (uint32_t)Reconstruct(code) & 65535;
    return sr >= 32768 ? (int)sr - 65536 : (int)sr;
}

int Codec::Reconstruct(unsigned code) {
    const Rate& rate = kRates[bits_ - 2];
    const uint32_t i = code & ((1u << bits_) - 1);
    // RECONST: the sign bit, and |I| counted down from the top for negative codes.
    const uint32_t dqs = i >> (bits_ - 1);
    const uint32_t im = dqs == 0 ? i : ((1u << bits_) - 1 - i) & ((1u << (bits_ - 1)) - 1);
    const uint32_t dqln = rate.dqln[im];
    // ADDA, ANTILOG (16-bit SM DQ)
    const uint32_t dql = (dqln + (y_ >> 2)) & 4095;
    const uint32_t dqls = dql >> 11, dex = (dql >> 7) & 15, dmn = dql & 127, dqt = (1 << 7) + dmn;
    const uint32_t dqmag = dqls == 0 ? (dqt << 7) >> (14 - dex) : 0;
    const uint32_t dq = (dqs << 15) + dqmag;
    // ADDB
    const uint32_t dqi = DqToTc(dq);
    const uint32_t ses = se_ >> 14, sei = ses == 0 ? se_ : (1 << 15) + se_;
    const uint32_t sr = (dqi + sei) & 65535;
    // ADDC
    const uint32_t sezs = sez_ >> 14, sezi = sezs == 0 ? sez_ : (1 << 15) + sez_;
    const uint32_t dqsez = (dqi + sezi) & 65535;
    const uint32_t pk0 = dqsez >> 15, sigpk = dqsez == 0 ? 1 : 0;
    // FUNCTW, FILTD, LIMB, FILTE
    const uint32_t wi = rate.wi[im];
    uint32_t dif = ((wi << 5) + 131072 - y_) & 131071;
    uint32_t difsx = (dif >> 16) == 0 ? dif >> 5 : (dif >> 5) + 4096;
    const uint32_t yut = (y_ + difsx) & 8191;
    const uint32_t geul = ((yut + 11264) & 16383) >> 13, gell = ((yut + 15840) & 16383) >> 13;
    const uint32_t yup = gell == 1 ? 544 : geul == 0 ? 5120 : yut;
    dif = (yup + ((1048576 - yl_) >> 6)) & 16383;
    difsx = (dif >> 13) == 0 ? dif : dif + 507904;
    const uint32_t ylp = (yl_ + difsx) & 524287;
    // FUNCTF, FILTA, FILTB
    const uint32_t fi = rate.fi[im];
    dif = ((fi << 9) + 8192 - dms_) & 8191;
    difsx = (dif >> 12) == 0 ? dif >> 5 : (dif >> 5) + 3840;
    const uint32_t dmsp = (difsx + dms_) & 4095;
    dif = ((fi << 11) + 32768 - dml_) & 32767;
    difsx = (dif >> 14) == 0 ? dif >> 7 : (dif >> 7) + 16128;
    const uint32_t dmlp = (difsx + dml_) & 16383;
    // UPA2, LIMC
    const uint32_t pks1 = pk0 ^ pk1_, pks2 = pk0 ^ pk2_;
    const uint32_t uga2a = pks2 == 0 ? 16384 : 114688;
    const uint32_t a1 = a_[0], a2 = a_[1];
    uint32_t fa1;
    if ((a1 >> 15) == 0) fa1 = a1 <= 8191 ? a1 << 2 : 8191 << 2;
    else fa1 = a1 >= 57345 ? (a1 << 2) & 131071 : 24577 << 2;
    const uint32_t fa = pks1 == 1 ? fa1 : (131072 - fa1) & 131071;
    const uint32_t uga2b = (uga2a + fa) & 131071, uga2s = uga2b >> 16;
    const uint32_t uga2 = sigpk == 1 ? 0 : uga2s == 0 ? uga2b >> 7 : (uga2b >> 7) + 64512;
    const uint32_t ula2 = (a2 >> 15) == 0 ? (65536 - (a2 >> 7)) & 65535 : (65536 - ((a2 >> 7) + 65024)) & 65535;
    const uint32_t a2t = (a2 + ((uga2 + ula2) & 65535)) & 65535;
    uint32_t a2p = a2t;
    if (a2t >= 32768 && a2t <= 53248) a2p = 53248;
    else if (a2t >= 12288 && a2t <= 32767) a2p = 12288;
    // UPA1, LIMD
    const uint32_t uga1 = sigpk == 1 ? 0 : (pk0 ^ pk1_) == 0 ? 192 : 65344;
    const uint32_t ula1 = (a1 >> 15) == 0 ? (65536 - (a1 >> 8)) & 65535 : (65536 - ((a1 >> 8) + 65280)) & 65535;
    const uint32_t a1t = (a1 + ((uga1 + ula1) & 65535)) & 65535;
    const uint32_t a1ul = (15360 + 65536 - a2p) & 65535, a1ll = (a2p + 65536 - 15360) & 65535;
    uint32_t a1p = a1t;
    if (a1t >= 32768 && a1t <= a1ll) a1p = a1ll;
    else if (a1t >= a1ul && a1t <= 32767) a1p = a1ul;
    // XOR, UPB
    uint32_t bp[6];
    const uint32_t dqmag15 = dq & 32767;
    for (int n = 0; n < 6; ++n) {
        const uint32_t u = (dq >> 15) ^ (dq_[n] >> 10);
        const uint32_t ugb = dqmag15 == 0 ? 0 : u == 0 ? 128 : 65408;
        const uint32_t bn = b_[n];
        const uint32_t ulb = bits_ == 5 ? ((bn >> 15) == 0 ? (65536 - (bn >> 9)) & 65535 : (65536 - ((bn >> 9) + 65408)) & 65535)
                                        : ((bn >> 15) == 0 ? (65536 - (bn >> 8)) & 65535 : (65536 - ((bn >> 8) + 65280)) & 65535);
        bp[n] = (bn + ((ugb + ulb) & 65535)) & 65535;
    }
    // FLOATA, FLOATB
    const uint32_t dq0 = FloatA(dq), sr0 = FloatB(sr);
    // TONE, TRANS (with the delayed TD and YL)
    uint32_t tdp = a2p >= 32768 && a2p < 53760 ? 1 : 0;
    const uint32_t ylint = yl_ >> 15, ylfrac = (yl_ >> 10) & 31;
    const uint32_t thr1 = (32 + ylfrac) << ylint, thr2 = ylint > 9 ? 31 << 10 : thr1;
    const uint32_t dqthr = (thr2 + (thr2 >> 1)) >> 1;
    const uint32_t tr = dqmag15 > dqthr && td_ == 1 ? 1 : 0;
    tones += tdp;
    transitions += tr;
    // SUBTC, FILTC, TRIGA
    dif = ((dmsp << 2) + 32768 - dmlp) & 32767;
    const uint32_t difm = (dif >> 14) == 0 ? dif : (32768 - dif) & 16383;
    const uint32_t dthr = dmlp >> 3;
    const uint32_t ax = y_ >= 1536 && difm < dthr && tdp == 0 ? 0 : 1;
    dif = ((ax << 9) + 2048 - ap_) & 2047;
    difsx = (dif >> 10) == 0 ? dif >> 4 : (dif >> 4) + 896;
    const uint32_t app = (difsx + ap_) & 1023;
    const uint32_t apr = tr == 1 ? 256 : app;
    // TRIGB, then the delays.
    if (tr == 1) {
        a1p = a2p = tdp = 0;
        for (uint32_t& v : bp) v = 0;
    }
    yu_ = yup; yl_ = ylp; dms_ = dmsp; dml_ = dmlp; ap_ = apr;
    a_[0] = a1p; a_[1] = a2p;
    for (int n = 0; n < 6; ++n) b_[n] = bp[n];
    for (int n = 5; n > 0; --n) dq_[n] = dq_[n - 1];
    dq_[0] = dq0;
    sr_[1] = sr_[0]; sr_[0] = sr0;
    pk2_ = pk1_; pk1_ = pk0;
    td_ = tdp;
    return (int)sr;
}

std::vector<unsigned> Encode(const std::vector<int>& sl, unsigned bits, Codec* codec) {
    Codec c(bits);
    std::vector<unsigned> codes;
    codes.reserve(sl.size());
    for (int s : sl) codes.push_back(c.Encode(s));
    if (codec) *codec = c;
    return codes;
}

std::vector<int> Decode(const std::vector<unsigned>& codes, unsigned bits) {
    Codec c(bits);
    std::vector<int> out;
    out.reserve(codes.size());
    for (unsigned code : codes) out.push_back(c.Decode(code));
    return out;
}
}  // namespace g726ref
