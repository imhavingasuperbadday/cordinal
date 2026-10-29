// Strict, bounded AutoEq / Equalizer APO text import, shared by the editor and saved preferences.
#include "dsp.h"

#include <charconv>
#include <cmath>

namespace {
std::string_view Token(std::string_view& line) {
    size_t first = line.find_first_not_of(" \t\r");
    if (first == line.npos) { line = {}; return {}; }
    line.remove_prefix(first);
    size_t end = line.find_first_of(" \t\r");
    auto token = line.substr(0, end);
    line.remove_prefix(token.size());
    return token;
}
bool Number(std::string_view text, float& value, float low, float high) {
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return false;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
           std::isfinite(value) && value >= low && value <= high;
}
std::string Decimal(float value) {
    char text[32];
    auto result = std::to_chars(text, text + sizeof(text), value);
    return std::string(text, result.ptr);
}
}

bool ParseAutoEq(std::string_view text, EqSettings& settings, std::wstring& error) {
    EqSettings parsed = settings;
    parsed.preamp = 0;
    parsed.filterCount = 0;
    parsed.parametric = parsed.enabled = true;
    error.clear();
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    bool preampSeen = false;
    size_t number = 0;
    while (!text.empty()) {
        ++number;
        size_t end = text.find('\n');
        auto line = text.substr(0, end);
        text.remove_prefix(end == text.npos ? text.size() : end + 1);
        line = line.substr(0, line.find('#'));
        auto word = Token(line);
        if (word.empty()) continue;
        bool ok = false;
        if (word == "Preamp:" && !preampSeen) {
            ok = Number(Token(line), parsed.preamp, -60, 24) && Token(line) == "dB";
            preampSeen = true;
        } else if (word == "Filter" && parsed.filterCount < kParametricBands) {
            auto index = Token(line);
            unsigned id = 0;
            if (index.ends_with(':')) {
                index.remove_suffix(1);
                auto result = std::from_chars(index.data(), index.data() + index.size(), id);
                ok = result.ec == std::errc{} && result.ptr == index.data() + index.size() && id > 0;
            }
            EqFilter f;
            auto on = Token(line), type = Token(line);
            f.enabled = on == "ON";
            ok = ok && (on == "ON" || on == "OFF");
            if (type == "PK") f.type = EqFilterType::Peak;
            else if (type == "LSC" || type == "LS") f.type = EqFilterType::LowShelf;
            else if (type == "HSC" || type == "HS") f.type = EqFilterType::HighShelf;
            else ok = false;
            ok = ok && Token(line) == "Fc" && Number(Token(line), f.frequency, 1, 96000) &&
                 Token(line) == "Hz" && Token(line) == "Gain" && Number(Token(line), f.gain, -40, 40) &&
                 Token(line) == "dB" && Token(line) == "Q" && Number(Token(line), f.q, 0.05f, 100);
            parsed.filters[parsed.filterCount++] = f;
        }
        if (!ok || !Token(line).empty()) {
            error = L"Invalid or unsupported profile at line " + std::to_wstring(number) +
                    L". Use up to 20 PK, LSC or HSC filters with Fc, Gain and Q.";
            return false;
        }
    }
    if (!parsed.filterCount) {
        error = L"The profile contains no parametric filters. Choose an AutoEq ParametricEQ.txt file.";
        return false;
    }
    settings = parsed;
    return true;
}

std::string FormatAutoEq(const EqSettings& settings) {
    std::string text = "Preamp: " + Decimal(settings.preamp) + " dB\r\n";
    for (int i = 0; i < settings.filterCount && i < kParametricBands; ++i) {
        const auto& f = settings.filters[i];
        const char* type = f.type == EqFilterType::Peak ? "PK" : f.type == EqFilterType::LowShelf ? "LSC" : "HSC";
        text += "Filter " + std::to_string(i + 1) + ": " + (f.enabled ? "ON " : "OFF ") + type +
                " Fc " + Decimal(f.frequency) + " Hz Gain " + Decimal(f.gain) + " dB Q " + Decimal(f.q) + "\r\n";
    }
    return text;
}
