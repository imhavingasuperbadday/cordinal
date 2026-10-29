// Native MP3/AAC encoding and M4A muxing. Each caller owns its output or sibling temporary file.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// Produces a complete gapless M4A, with optional prebuilt ilst items. Failed writes remove `out`.
bool EncodeM4a(const std::wstring& source, const std::wstring& out, unsigned kbps, const std::atomic<bool>& cancel,
               std::wstring& error, const std::string& metadata = {});
// Emits ADTS or raw MP3 frames, without playback processing. `frames` excludes codec priming/padding.
bool EncodeLossyAudio(const std::wstring& source, unsigned kbps, bool mp3, const std::atomic<bool>& cancel,
                      const std::function<bool(const char*, size_t)>& send, uint64_t& frames, std::wstring& error);
