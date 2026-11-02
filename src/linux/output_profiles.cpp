// Atomic Linux output profiles. Full node names are hex encoded as section names, so two devices
// cannot collide and punctuation cannot escape the format. Global EQ stays in settings.ini.
#include "output_profiles.h"
#include "packs.h"
#include "library.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace lx {
namespace {
std::string Hex(const std::string& text) {
    std::string out;
    for (unsigned char c : text) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    return out;
}
std::string Unhex(std::wstring_view text) {
    if (text.empty() || text.size() % 2) return {};
    std::string out;
    auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1; };
    for (size_t i = 0; i < text.size(); i += 2) {
        int hi = digit(text[i]), lo = digit(text[i + 1]);
        if (hi < 0 || lo < 0 || (!hi && !lo)) return {};
        out += (char)(hi * 16 + lo);
    }
    return out;
}
float Number(const IniSection& section, const wchar_t* key, float def, float low, float high) {
    const auto* text = section.Get(key);
    if (!text) return def;
    wchar_t* end = nullptr;
    float value = wcstof(text->c_str(), &end);
    return end == text->c_str() || *end || !std::isfinite(value) ? def : std::clamp(value, low, high);
}
bool Flag(const IniSection& section, const wchar_t* key) {
    const auto* text = section.Get(key);
    return text && *text == L"1";
}
std::string Float(float v) { char text[32]; snprintf(text, sizeof(text), "%.9g", v); return text; }
}

void OutputProfiles::Load(const std::filesystem::path& path, const EqSettings& global) {
    path_ = path;
    global_ = global;
    output_.clear();
    profiles_.clear();
    std::error_code error;
    auto size = std::filesystem::file_size(path, error);
    if (error || size > (1u << 20)) return;
    std::ifstream file(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    for (const auto& section : ParseIni(bytes)) {
        if (!section.name.starts_with(L"output-") || profiles_.size() >= 128) continue;
        std::string id = Unhex(std::wstring_view(section.name).substr(7));
        if (id.empty()) continue;
        OutputSoundProfile profile;
        profile.remember = Flag(section, L"remember");
        profile.headroom = Number(section, L"headroom", 0, 0, 24);
        profile.eq.enabled = Flag(section, L"enabled");
        profile.eq.preamp = Number(section, L"preamp", 0, -60, 24);
        if (const auto* bands = section.Get(L"bands")) {
            std::wistringstream stream(*bands);
            for (float& band : profile.eq.gains) {
                float value = 0;
                if (!(stream >> value) || !std::isfinite(value)) break;
                band = std::clamp(value, -12.0f, 12.0f);
                if (stream.peek() == L',') stream.get();
            }
        }
        int count = (int)Number(section, L"filter_count", 0, 0, kParametricBands);
        if (count) {
            std::string text = "Preamp: " + Float(profile.eq.preamp) + " dB\n";
            for (int i = 0; i < count; ++i)
                if (const auto* filter = section.Get((L"filter" + std::to_wstring(i + 1)).c_str())) text += ToUtf8(*filter) + "\n";
            EqSettings parsed = profile.eq;
            std::wstring error;
            if (ParseAutoEq(text, parsed, error) && parsed.filterCount == count) {
                parsed.enabled = profile.eq.enabled;
                parsed.parametric = Flag(section, L"parametric");
                profile.eq = parsed;
            }
        }
        profile.eq.crossfeed = Flag(section, L"crossfeed");
        profiles_[std::move(id)] = profile;
    }
}

void OutputProfiles::Capture(const EqSettings& active) {
    if (Remembered()) profiles_[output_].eq = active;
    else global_ = active;
}

bool OutputProfiles::Save(const EqSettings& active) {
    Capture(active);
    std::string text = "; cordinal output profiles, format 1\n";
    for (const auto& [id, profile] : profiles_) {
        text += "[output-" + Hex(id) + "]\nremember=" + std::to_string(profile.remember) +
            "\nheadroom=" + Float(profile.headroom) + "\nenabled=" + std::to_string(profile.eq.enabled) +
            "\npreamp=" + Float(profile.eq.preamp) + "\nbands=";
        for (size_t i = 0; i < profile.eq.gains.size(); ++i) text += (i ? "," : "") + Float(profile.eq.gains[i]);
        text += "\ncrossfeed=" + std::to_string(profile.eq.crossfeed) + "\nparametric=" +
            std::to_string(profile.eq.parametric) + "\nfilter_count=" + std::to_string(profile.eq.filterCount) + "\n";
        std::istringstream lines(FormatAutoEq(profile.eq));
        std::string line;
        std::getline(lines, line);  // the preamp is already stored above
        for (int i = 0; i < profile.eq.filterCount && std::getline(lines, line); ++i) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            text += "filter" + std::to_string(i + 1) + "=" + line + "\n";
        }
        text += "\n";
    }
    if (profiles_.empty()) return true;
    if (path_.empty()) return false;
    // A sibling temporary file and rename leave the previous bytes intact on any failed write.
    std::string temporary = path_.string() + ".tmp-XXXXXX";
    int fd = mkstemp(temporary.data());
    if (fd < 0) return false;
    FILE* file = fdopen(fd, "wb");
    if (!file) { close(fd); remove(temporary.c_str()); return false; }
    bool ok = fwrite(text.data(), 1, text.size(), file) == text.size() && fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file)) ok = false;
    if (ok) ok = rename(temporary.c_str(), path_.c_str()) == 0;
    if (!ok) remove(temporary.c_str());
    return ok;
}

EqSettings OutputProfiles::Switch(const std::string& id, const EqSettings& active) {
    Capture(active);
    output_ = id;
    return Remembered() ? profiles_.at(output_).eq : global_;
}
EqSettings OutputProfiles::Remember(bool on, const EqSettings& active) {
    if (output_.empty()) return active;
    Capture(active);
    auto& profile = profiles_[output_];
    profile.remember = on;
    if (on) profile.eq = active;
    return on ? active : global_;
}
bool OutputProfiles::Remembered() const {
    auto it = profiles_.find(output_);
    return it != profiles_.end() && it->second.remember;
}
float OutputProfiles::Headroom() const {
    auto it = profiles_.find(output_);
    return it == profiles_.end() ? 0 : it->second.headroom;
}
bool OutputProfiles::SetHeadroom(float db) {
    if (output_.empty() || !std::isfinite(db) || db < 0 || db > 24) return false;
    profiles_[output_].headroom = db;
    return true;
}
}
