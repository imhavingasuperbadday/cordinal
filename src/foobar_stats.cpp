// Reads foo_playcount's XML export with the system XML parser. Imports data, never external entities.
#include "migration.h"
#include <shlwapi.h>
#include <wrl/client.h>
#include <xmllite.h>
#include <cwchar>

#pragma comment(lib, "xmllite.lib")

namespace migration {
namespace {
using Microsoft::WRL::ComPtr;

bool Integer(const std::wstring& text, uint64_t& value) {
    if (text.empty()) return false;
    value = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9' || value > (UINT64_MAX - (c - L'0')) / 10) return false;
        value = value * 10 + c - L'0';
    }
    return true;
}

bool Attribute(IXmlReader* reader, const wchar_t* name, std::wstring& value) {
    if (reader->MoveToAttributeByName(name, nullptr) != S_OK) return false;
    const wchar_t* text;
    UINT n;
    bool ok = SUCCEEDED(reader->GetValue(&text, &n));
    if (ok) value.assign(text, n);
    reader->MoveToElement();
    return ok;
}

bool DateAttribute(IXmlReader* reader, const wchar_t* name, uint32_t& time) {
    std::wstring text;
    if (!Attribute(reader, name, text)) return true;
    uint64_t ticks;
    if (!Integer(text, ticks)) return false;
    if (!ticks) return true;
    uint64_t seconds = ticks / 10000000ull;
    if (seconds <= 11644473600ull || seconds - 11644473600ull > UINT32_MAX) return false;
    time = (uint32_t)(seconds - 11644473600ull);
    return true;
}
} // namespace

bool ReadFoobarStatistics(const std::wstring& file, std::vector<Statistics>& out, size_t& unlocated,
                          std::wstring& error) {
    ComPtr<IStream> stream;
    ComPtr<IXmlReader> reader;
    if (FAILED(SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr,
                                      &stream)) ||
        FAILED(CreateXmlReader(__uuidof(IXmlReader), &reader, nullptr))) {
        error = L"Couldn't open the foobar2000 XML export.";
        return false;
    }
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 256 * 1024 * 1024) {
        error = L"The XML export exceeds the 256 MB import limit.";
        return false;
    }
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    reader->SetProperty(XmlReaderProperty_MaxElementDepth, 16);
    reader->SetInput(stream.Get());
    std::vector<Statistics> rows;
    Statistics current;
    size_t missing = 0;
    bool root = false, inEntry = false, ended = false;
    UINT entryDepth = 0;
    auto finish = [&] {
        if (!current.path.empty())
            rows.push_back(current);
        else
            ++missing;
        inEntry = false;
    };
    XmlNodeType type;
    HRESULT result;
    bool valid = true;
    std::wstring lastNode;
    while ((result = reader->Read(&type)) == S_OK && valid) {
        const wchar_t* raw;
        UINT length, depth;
        reader->GetDepth(&depth);
        if (type != XmlNodeType_Element && type != XmlNodeType_EndElement) continue;
        if (FAILED(reader->GetLocalName(&raw, &length))) {
            valid = false;
            break;
        }
        std::wstring name(raw, length);
        lastNode = name;
        if (type == XmlNodeType_Element) {
            if (depth == 0) {
                if (root || name != L"PlaybackStatistics") {
                    valid = false;
                    break;
                }
                root = true;
                ended = reader->IsEmptyElement() != FALSE;
            } else if (name == L"Entry") {
                if (depth != 1 || inEntry || ended) {
                    valid = false;
                    break;
                }
                current = {};
                inEntry = true;
                entryDepth = depth;
                std::wstring text;
                if (Attribute(reader.Get(), L"Count", text)) {
                    uint64_t n = 0;
                    valid = Integer(text, n) && n <= UINT32_MAX;
                    current.count = (uint32_t)n;
                    current.hasCount = true;
                }
                if (Attribute(reader.Get(), L"RatingFriendly", text)) {
                    // Friendly ratings use stars, including half stars in newer exports.
                    if (text == L"0" || text == L"1" || text == L"2" || text == L"3" || text == L"4" ||
                        text == L"5" || text == L"0.5" || text == L"1.5" || text == L"2.5" ||
                        text == L"3.5" || text == L"4.5") {
                        current.rating = (uint8_t)((text[0] - L'0') * 20 + (text.size() > 1 ? 10 : 0));
                        current.hasRating = true;
                    } else
                        valid = false;
                } else if (Attribute(reader.Get(), L"Rating", text)) {
                    error = L"This XML has no readable ratings. Export again with Playback Statistics 3.1.2 "
                            L"or newer.";
                    return false;
                }
                valid = valid && DateAttribute(reader.Get(), L"Added", current.added) &&
                        DateAttribute(reader.Get(), L"FirstPlayed", current.first) &&
                        DateAttribute(reader.Get(), L"LastPlayed", current.last) &&
                        !(current.first && current.last && current.first > current.last);
                if (reader->IsEmptyElement()) finish();
            } else if (name == L"Item" && inEntry) {
                std::wstring path, sub;
                if (depth != entryDepth + 1 || !Attribute(reader.Get(), L"Path", path)) {
                    valid = false;
                    break;
                }
                Statistics item = current;
                item.path = path;
                if (item.path.starts_with(L"file://")) {
                    wchar_t decoded[32768];
                    DWORD n = (DWORD)std::size(decoded);
                    if (FAILED(PathCreateFromUrlW(item.path.c_str(), decoded, &n, 0))) {
                        valid = false;
                        break;
                    }
                    item.path = decoded;
                }
                if (Attribute(reader.Get(), L"SubSong", sub) || Attribute(reader.Get(), L"Subsong", sub)) {
                    uint64_t index;
                    if (!Integer(sub, index) || index >= INT_MAX) {
                        valid = false;
                        break;
                    }
                    if (index) item.path = SubtrackPath(item.path, (int)index + 1);
                }
                rows.push_back(std::move(item));
                // One foobar identity may name several copies of the same song.
                current.path = L"#located";
            }
        } else if (name == L"Entry" && inEntry) {
            if (current.path == L"#located")
                inEntry = false;
            else
                finish();
        } else if (name == L"PlaybackStatistics" && !inEntry)
            ended = true;
    }
    if (FAILED(result) || !valid || !root || !ended || inEntry) {
        UINT line = 0;
        reader->GetLineNumber(&line);
        error = L"Invalid foobar2000 XML near " + lastNode + L" on line " + std::to_wstring(line) +
                L". Nothing was imported.";
        return false;
    }
    if (rows.empty()) {
        error = L"No file locations were found. Export from foobar2000's Library > Playback Statistics menu "
                L"with file locations included, using Playback Statistics 3.1.2 or newer.";
        return false;
    }
    out = std::move(rows);
    unlocated = missing;
    return true;
}
} // namespace migration
