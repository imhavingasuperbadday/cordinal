// Native headset matching and descriptor validation, kept pure enough to check without a USB device.
#pragma once
#include "headsets.h"
#include <filesystem>
#include <span>

namespace lx {
bool BlackSharkDescriptor(std::span<const uint8_t> descriptor);
std::filesystem::path UsbAncestor(std::filesystem::path path);
bool MatchingHeadsetUsb(const std::filesystem::path& hid, const std::filesystem::path& audio);
}
