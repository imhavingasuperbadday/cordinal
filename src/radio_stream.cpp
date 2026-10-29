// Radio audio is fetched on a bounded worker and delivered to the decoder without blocking the UI.
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#endif
#include "radio_stream.h"
#include "radio_http.h"
#include "radio_hls.h"
#include "web_internal.h"
#include "library.h"

#include <array>
#include <charconv>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace radio {
struct Stream::State {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> cancel{false};
    std::array<uint8_t, 256 * 1024> ring{};
    size_t read = 0, used = 0;
    bool done = false, connected = false;
    std::string title;
    std::wstring problem;
};

// Detached network requests own only this counter and their copied state. A cancelled request can
// take until its OS timeout to return; limiting them keeps rapid station changes bounded.
static std::atomic<unsigned>& Workers() { static auto* value = new std::atomic<unsigned>(0); return *value; }

std::shared_ptr<Stream> Stream::Open(const std::string& url) {
    auto state = std::make_shared<State>();
    auto source = std::shared_ptr<Stream>(new Stream(state));
    if ((!url.starts_with("https://") && !url.starts_with("http://")) || url.size() > 8192 ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) {
        state->done = true; state->problem = L"The station's stream address is invalid";
        return source;
    }
    unsigned active = Workers().fetch_add(1);
    if (active >= 8) {
        --Workers(); state->done = true; state->problem = L"Too many station connections are still closing";
        return source;
    }
    try {
        std::thread([state, url] {
            try {
                size_t interval = 0, audioLeft = 0, metadataLeft = 0;
                bool needLength = false;
                std::string metadata;
                auto push = [&](std::string_view bytes) {
                    while (!bytes.empty()) {
                        std::unique_lock lock(state->mutex);
                        state->changed.wait(lock, [&] { return state->cancel || state->used < state->ring.size(); });
                        if (state->cancel) return false;
                        size_t write = (state->read + state->used) % state->ring.size();
                        size_t n = std::min({bytes.size(), state->ring.size() - state->used, state->ring.size() - write});
                        memcpy(state->ring.data() + write, bytes.data(), n);
                        state->used += n;
                        bytes.remove_prefix(n);
                        lock.unlock(); state->changed.notify_all();
                    }
                    return true;
                };
                bool playlist = false;
                std::string playlistBytes, prefix;
                auto headers = [&](unsigned status, const Headers& fields) {
                    if (status != 200) {
                        std::lock_guard lock(state->mutex);
                        state->problem = L"The station returned HTTP " + std::to_wstring(status);
                        return false;
                    }
                    auto type = fields.find("content-type");
                    playlist = type != fields.end() && (web::Lower(type->second).find("mpegurl") != std::string::npos ||
                                                         web::Lower(type->second).find("scpls") != std::string::npos);
                    if (auto it = fields.find("icy-metaint"); it != fields.end()) {
                        auto parsed = std::from_chars(it->second.data(), it->second.data() + it->second.size(), interval);
                        if (parsed.ec != std::errc() || interval > (16u << 20)) interval = 0;
                    }
                    audioLeft = interval;
                    { std::lock_guard lock(state->mutex); state->connected = true; }
                    state->changed.notify_all();
                    return !state->cancel;
                };
                auto data = [&](std::string_view bytes) {
                    if (!interval) return push(bytes);
                    while (!bytes.empty() && !state->cancel) {
                        if (audioLeft) {
                            size_t n = std::min(audioLeft, bytes.size());
                            if (!push(bytes.substr(0, n))) return false;
                            bytes.remove_prefix(n); audioLeft -= n;
                            if (!audioLeft) needLength = true;
                        } else if (needLength) {
                            metadataLeft = (unsigned char)bytes.front() * 16u;
                            bytes.remove_prefix(1); needLength = false; metadata.clear();
                            if (!metadataLeft) audioLeft = interval;
                        } else {
                            size_t n = std::min(metadataLeft, bytes.size());
                            metadata.append(bytes.substr(0, n)); bytes.remove_prefix(n); metadataLeft -= n;
                            if (!metadataLeft) {
                                constexpr std::string_view key = "StreamTitle='";
                                size_t begin = metadata.find(key);
                                if (begin != metadata.npos) {
                                    begin += key.size(); size_t end = metadata.find("';", begin);
                                    if (end != metadata.npos) {
                                        std::lock_guard lock(state->mutex);
                                        state->title = metadata.substr(begin, end - begin);
                                    }
                                }
                                audioLeft = interval;
                            }
                        }
                    }
                    return !state->cancel;
                };
                bool ok = false;
                std::string current = url;
                for (unsigned depth = 0; depth < 6 && !state->cancel; ++depth) {
                    playlist = false; playlistBytes.clear(); prefix.clear(); interval = 0;
                    bool classified = false, bounded = true;
                    ok = HttpStream(current, state->cancel, headers, [&](std::string_view bytes) {
                        if (!classified) {
                            prefix.append(bytes);
                            if (prefix.size() < 10 && !playlist) return true;
                            std::string_view head = prefix;
                            if (head.starts_with("\xEF\xBB\xBF")) head.remove_prefix(3);
                            playlist = playlist || head.starts_with("#EXTM3U") || web::Lower(std::string(head.substr(0, 10))).starts_with("[playlist]");
                            classified = true; bytes = prefix;
                        }
                        if (playlist) {
                            if (bytes.size() > (1u << 20) - playlistBytes.size()) { bounded = false; return false; }
                            playlistBytes.append(bytes); return !state->cancel;
                        }
                        return data(bytes);
                    }, {}, &current);
                    if (ok && !classified && !prefix.empty()) ok = data(prefix);
                    if (!ok || !bounded || !playlist || state->cancel) break;
                    if (playlistBytes.find("#EXT-X-") != std::string::npos) {
                        std::wstring problem;
                        auto wait = [&](unsigned ms) {
                            std::unique_lock lock(state->mutex);
                            return !state->changed.wait_for(lock, std::chrono::milliseconds(ms), [&] { return state->cancel.load(); });
                        };
                        ok = PlayHls(current, std::move(playlistBytes), state->cancel, push, wait, problem);
                        if (!problem.empty()) { std::lock_guard lock(state->mutex); state->problem = std::move(problem); }
                        break;
                    }
                    // PLS and ordinary M3U files are station links, not audio segments.
                    std::string next;
                    for (size_t at = 0; at < playlistBytes.size();) {
                        size_t end = playlistBytes.find('\n', at); if (end == playlistBytes.npos) end = playlistBytes.size();
                        std::string line = playlistBytes.substr(at, end - at); at = end + 1;
                        while (!line.empty() && (unsigned char)line.back() <= ' ') line.pop_back();
                        while (!line.empty() && (unsigned char)line.front() <= ' ') line.erase(0, 1);
                        if (web::Lower(line.substr(0, 6)) == "file1=") line.erase(0, 6);
                        else if (!line.starts_with("http://") && !line.starts_with("https://") && line.find('=') != line.npos) continue;
                        if (!line.empty() && line[0] != '#' && line[0] != '[') {
                            next = ResolveRadioUrl(current, line); if (!next.empty()) break;
                        }
                    }
                    if (next.empty() || next == current || depth == 5) {
                        std::lock_guard lock(state->mutex); state->problem = L"The station playlist has no usable stream link."; ok = false; break;
                    }
                    current = std::move(next);
                }
                {
                    std::lock_guard lock(state->mutex);
                    if (!state->cancel && state->problem.empty())
                        state->problem = ok ? L"The station stopped streaming" : L"Couldn't connect to the station or the stream ended";
                    state->done = true;
                }
            } catch (...) {
                std::lock_guard lock(state->mutex);
                state->problem = L"The station connection failed";
                state->done = true;
            }
            state->changed.notify_all();
            --Workers();
        }).detach();
    } catch (...) {
        --Workers(); state->done = true; state->problem = L"Couldn't start the station connection";
    }
    return source;
}

Stream::~Stream() { Abort(); }
size_t Stream::Read(uint8_t* out, size_t bytes) {
    if (!bytes) return 0;
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, [&] { return state_->cancel || state_->used || state_->done; });
    if (state_->cancel || !state_->used) return 0;
    size_t n = std::min({bytes, state_->used, state_->ring.size() - state_->read});
    memcpy(out, state_->ring.data() + state_->read, n);
    state_->read = (state_->read + n) % state_->ring.size(); state_->used -= n;
    lock.unlock(); state_->changed.notify_all();
    return n;
}
void Stream::Abort() { state_->cancel = true; state_->changed.notify_all(); }
std::wstring Stream::Problem() const { std::lock_guard lock(state_->mutex); return state_->problem; }
std::string Stream::Title() const { std::lock_guard lock(state_->mutex); return state_->title; }
bool Stream::Connected() const { std::lock_guard lock(state_->mutex); return state_->connected; }

#ifdef _WIN32
bool HttpStream(const std::string& url, const std::atomic<bool>& cancel,
                const std::function<bool(unsigned, const Headers&)>& headers,
                const std::function<bool(std::string_view)>& data,
                const std::string& requestHeaders, std::string* effectiveUrl) {
    std::wstring wide = FromUtf8(url);
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || !parts.lpszHostName) return false;
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target = std::wstring(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength) +
                          std::wstring(parts.lpszExtraInfo ? parts.lpszExtraInfo : L"", parts.dwExtraInfoLength);
    if (target.empty()) target = L"/";
    HINTERNET session = WinHttpOpen(L"cordinal/2.3", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session ? WinHttpConnect(session, host.c_str(), parts.nPort, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (request && !cancel) {
        DWORD disabled = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
        WinHttpSetTimeouts(request, 8000, 8000, 8000, 8000);
        std::wstring extra = L"Icy-MetaData: 1\r\nAccept-Encoding: identity\r\n" + FromUtf8(requestHeaders);
        if (WinHttpSendRequest(request, extra.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr) && !cancel) {
            if (effectiveUrl) {
                DWORD length = 0; WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &length);
                std::wstring final(length / sizeof(wchar_t), 0);
                if (length && WinHttpQueryOption(request, WINHTTP_OPTION_URL, final.data(), &length)) {
                    while (!final.empty() && !final.back()) final.pop_back(); *effectiveUrl = ToUtf8(final);
                }
            }
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
            size = 0; WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, nullptr, &size, nullptr);
            std::wstring raw(size / sizeof(wchar_t), L'\0'); Headers fields;
            if (size && WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, raw.data(), &size, nullptr)) {
                std::string text = ToUtf8(raw);
                for (size_t at = text.find("\r\n"); at != text.npos && at + 2 < text.size();) {
                    size_t end = text.find("\r\n", at + 2), colon = text.find(':', at + 2);
                    if (end == text.npos) break;
                    if (colon < end) {
                        std::string value = text.substr(colon + 1, end - colon - 1);
                        while (!value.empty() && (unsigned char)value.front() <= ' ') value.erase(0, 1);
                        while (!value.empty() && (unsigned char)value.back() <= ' ') value.pop_back();
                        fields[web::Lower(text.substr(at + 2, colon - at - 2))] = std::move(value);
                    }
                    at = end;
                }
            }
            if (headers(status, fields)) {
                ok = true;
                while (!cancel) {
                    char buffer[16384]; DWORD got = 0;
                    if (!WinHttpReadData(request, buffer, sizeof(buffer), &got)) { ok = false; break; }
                    if (!got || !data(std::string_view(buffer, got))) break;
                }
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return ok;
}
bool DecryptRadioAes(std::string& bytes, const std::string& key, std::array<uint8_t, 16> iv) {
    if (key.size() != 16 || bytes.empty() || bytes.size() % 16) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                   sizeof(BCRYPT_CHAIN_MODE_CBC), 0) >= 0;
    if (ok) ok = BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, (PUCHAR)key.data(), 16, 0) >= 0;
    std::string decoded(bytes.size(), 0); ULONG count = 0;
    if (ok) ok = BCryptDecrypt(handle, (PUCHAR)bytes.data(), (ULONG)bytes.size(), nullptr, iv.data(), 16,
                              (PUCHAR)decoded.data(), (ULONG)decoded.size(), &count, BCRYPT_BLOCK_PADDING) >= 0;
    if (handle) BCryptDestroyKey(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (ok) { decoded.resize(count); bytes = std::move(decoded); }
    return ok;
}
#endif
}  // namespace radio
