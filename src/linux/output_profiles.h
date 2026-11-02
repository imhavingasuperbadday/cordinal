// Per-output sound settings. The UI owns this model; the native worker only supplies copied ids.
#pragma once
#include "dsp.h"
#include <filesystem>
#include <map>

namespace lx {
struct OutputSoundProfile {
    EqSettings eq;
    bool remember = false;
    float headroom = 0;
};

class OutputProfiles {
public:
    void Load(const std::filesystem::path& path, const EqSettings& global);
    bool Save(const EqSettings& active);
    EqSettings Switch(const std::string& id, const EqSettings& active);
    EqSettings Remember(bool on, const EqSettings& active);
    bool Remembered() const;
    float Headroom() const;
    bool SetHeadroom(float db);
    const EqSettings& Global() const { return global_; }
    const std::string& Output() const { return output_; }
private:
    void Capture(const EqSettings& active);
    std::filesystem::path path_;
    std::string output_;
    EqSettings global_;
    std::map<std::string, OutputSoundProfile> profiles_;
};
}
