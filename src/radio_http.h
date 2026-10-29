// Platform HTTP stream reader. Callbacks and the cancellation flag belong to the caller's worker;
// requests follow a bounded number of redirects and never buffer a whole radio response.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace radio {
using Headers = std::map<std::string, std::string>;
bool HttpStream(const std::string& url, const std::atomic<bool>& cancel,
                const std::function<bool(unsigned, const Headers&)>& headers,
                const std::function<bool(std::string_view)>& data,
                const std::string& requestHeaders = {}, std::string* effectiveUrl = nullptr);
}  // namespace radio
