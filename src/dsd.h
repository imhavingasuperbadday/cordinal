// The DSD bitstream contract shared by the DSD decoder, the engine's DSD path and the DAC outputs:
// which transport carries the bits, how one float payload sample encodes them, and the payload's
// sample rate. DSD travels through the engine's ordinary float ring buffer, so the values here are
// carried unchanged. Nothing here needs <windows.h>, so the Linux port can use it too.
#pragma once
#include <cstdint>

enum class DsdTransport : uint8_t { None, Dop, AsioNative };

constexpr uint8_t kDsdIdle = 0x69;  // equal ones and zeros: DSD's silence
// DoP's marker bytes: the top 8 bits of each 24-bit sample, alternating from one frame to the next.
constexpr uint8_t kDopMarkerA = 0x05, kDopMarkerB = 0xFA;

// DoP: one float sample is 16 DSD bits of one channel, the first bit in time in the word's MSB, so
// the value is word / 65536 and lies in [0, 1). Both directions are exact; junk maps to idle bits.
inline float DopPayload(uint16_t w) { return (float)w * (1.0f / 65536.0f); }
inline uint16_t DopWord(float f) {
    if (!(f >= 0.0f && f < 1.0f)) return 0x6969;  // outside the payload range, or NaN
    return (uint16_t)(unsigned)(f * 65536.0f);
}

// ASIO native DSD: one float sample is 8 DSD bits, the first bit in time in the MSB.
inline float DsdBytePayload(uint8_t b) { return (float)b * (1.0f / 256.0f); }
inline uint8_t DsdByte(float f) {
    if (!(f >= 0.0f && f < 1.0f)) return kDsdIdle;
    return (uint8_t)(unsigned)(f * 256.0f);
}

// The payload's sample rate; 0 for None, where DSD plays as PCM.
inline unsigned DsdFrameRate(unsigned dsdRate, DsdTransport t) {
    return t == DsdTransport::Dop ? dsdRate / 16 : t == DsdTransport::AsioNative ? dsdRate / 8 : 0;
}

// The usual name of a DSD rate, in the 44.1 kHz family (2.8224 MHz is DSD64) and the 48 kHz one
// (3.072 MHz is DSD64). Any other rate is reported as plain DSD.
inline const wchar_t* DsdRateName(unsigned dsdRate) {
    switch (dsdRate) {
        case 2822400: case 3072000: return L"DSD64";
        case 5644800: case 6144000: return L"DSD128";
        case 11289600: case 12288000: return L"DSD256";
        case 22579200: case 24576000: return L"DSD512";
        case 45158400: case 49152000: return L"DSD1024";
        default: return L"DSD";
    }
}
