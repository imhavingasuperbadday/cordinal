// Profile backup folders and staged restore. A restore retains the entire previous profile beside it.
#pragma once
#include "migration.h"
#include <atomic>

namespace backup {
bool Create(const std::wstring& folder, const std::wstring& profile, const std::string& settings,
            const LibraryArchive& data, const std::atomic<bool>& cancel, std::wstring& error);
bool Read(const std::wstring& folder, LibraryArchive& data, std::string& settings, std::wstring& error);
bool Stage(const std::wstring& folder, const std::wstring& profile, const std::string& settings,
           const LibraryArchive& data, const std::atomic<bool>& cancel, std::wstring& stage,
           std::wstring& error);
bool Commit(const std::wstring& profile, const std::wstring& stage, std::wstring& previous,
            std::wstring& error);
// Completes rollback after an interrupted directory swap, before the profile is opened.
bool Recover(const std::wstring& profile, std::wstring& error);
bool ReadBytes(const std::wstring& file, std::string& bytes, uint64_t max = 256 * 1024 * 1024);
bool WriteBytes(const std::wstring& file, std::string_view bytes);
std::string RemapSettings(const std::string& bytes, const std::vector<migration::PathMap>& maps);
} // namespace backup
