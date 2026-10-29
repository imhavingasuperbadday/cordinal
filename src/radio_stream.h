// An internet-radio source: bounded network buffering, ICY metadata and cancellable reads.
#pragma once

#include "audio_live.h"

#include <memory>
#include <string>

namespace radio {
class Stream : public LiveAudioSource {
public:
    // Starts a request on a worker. The source never reads the network on the UI thread.
    static std::shared_ptr<Stream> Open(const std::string& url);
    ~Stream() override;
    size_t Read(uint8_t* out, size_t bytes) override;
    void Abort() override;
    std::wstring Problem() const override;
    std::string Title() const;
    bool Connected() const;
private:
    struct State;
    explicit Stream(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};
}  // namespace radio
