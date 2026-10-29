// Cordinal's in-process codecs. Implementations read format specifications directly;
// no third-party decoder implementation is used by these entry points. Each function returns null
// when the file isn't that format or can't be opened; later damage is reported by Decoder::Failed().
// See OpenDecoder for dispatch and the user-facing open error text.
#pragma once
#include "decoder.h"

// One entry point per container or codec family; OpenDecoder selects one by file extension.
std::unique_ptr<Decoder> OpenPcmDecoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenFlacDecoder(const std::wstring& path);
// A FLAC stream in memory the caller keeps alive, such as an embedded resource. Never Matroska.
std::unique_ptr<Decoder> OpenFlacDecoder(const uint8_t* data, size_t size);
std::unique_ptr<Decoder> OpenVorbisDecoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenMpegDecoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenAlacDecoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenAacDecoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenWmaDecoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenOpusDecoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenWavPackDecoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenApeDecoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenAc3Decoder(const std::wstring& path);
std::unique_ptr<Decoder> OpenMatroskaDecoder(const std::wstring& path);
// cordinal's own format (cord.h), version 1.
std::unique_ptr<Decoder> OpenCordDecoder(const std::wstring& path);
// DSD in DSF or uncompressed DSDIFF, as PCM near 88.2 kHz.
std::unique_ptr<Decoder> OpenDsdDecoder(const std::wstring& path);
// Tracker modules (MOD, S3M, XM, IT), played once through to where they would repeat.
std::unique_ptr<Decoder> OpenModuleDecoder(const std::wstring& path);
// A module's name, format and length.
struct TagData;
bool ReadModuleTags(const std::wstring& path, TagData& out);
// Standard MIDI files through a SoundFont or DLS bank (audio_midi.cpp says which bank is used).
// Null when the file can't be read or no bank can be found.
std::unique_ptr<Decoder> OpenMidiDecoder(const std::wstring& path);
bool ReadMidiTags(const std::wstring& path, TagData& out);
// Where the player's own SoundFonts live (<data>\soundfonts), looked in after the song's folder.
void SetMidiSoundFontFolder(const std::wstring& folder);

// Exact integer readers (PcmReader in decoder.h) sharing those decoders' parsing; OpenPcmReader
// selects one by file extension. OpenPcmExact covers the containers OpenPcmDecoder reads, integer
// PCM only.
std::unique_ptr<PcmReader> OpenFlacExact(const std::wstring& path);
std::unique_ptr<PcmReader> OpenPcmExact(const std::wstring& path);
std::unique_ptr<PcmReader> OpenCordExact(const std::wstring& path);
