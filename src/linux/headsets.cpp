// Linux headset worker. PipeWire identifies the selected output; sysfs ties its ALSA card to the
// same USB device as hidraw. Only the exact BlackShark interface/protocol is opened, never a mouse
// or a second headset. Discovery reads status; persistent writes require an explicit UI command.
#include "headsets_native.h"
#include "audio_output.h"
#include "library.h"

#include <linux/hidraw.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

namespace fs = std::filesystem;
namespace {
std::string ReadText(const fs::path& path) {
    std::ifstream file(path);
    std::string text;
    std::getline(file, text);
    return text;
}
}

namespace lx {
fs::path UsbAncestor(fs::path path) {
    std::error_code error;
    path = fs::canonical(path, error);
    if (error) return {};
    while (!path.empty() && path != path.root_path()) {
        if (fs::exists(path / "idVendor", error) && fs::exists(path / "idProduct", error)) return path;
        path = path.parent_path();
    }
    return {};
}

bool MatchingHeadsetUsb(const fs::path& hid, const fs::path& audio) {
    const auto usb = UsbAncestor(hid);
    if (usb.empty() || usb != UsbAncestor(audio) || ReadText(usb / "idVendor") != "1532" ||
        ReadText(usb / "idProduct") != "0555") return false;
    std::error_code error;
    auto path = fs::canonical(hid, error);
    while (!error && path != usb && !path.empty()) {
        if (fs::exists(path / "bInterfaceNumber", error)) return ReadText(path / "bInterfaceNumber") == "03";
        path = path.parent_path();
    }
    return false;
}

bool BlackSharkDescriptor(std::span<const uint8_t> bytes) {
    uint32_t page = 0, usage = 0, size = 0, count = 0, report = 0, input = 0, output = 0;
    bool collection = false;
    for (size_t at = 0; at < bytes.size();) {
        uint8_t tag = bytes[at++];
        if (tag == 0xfe) return false;  // this interface needs no long or stacked global items
        unsigned length = tag & 3;
        if (length == 3) length = 4;
        if (length > bytes.size() - at) return false;
        uint32_t value = 0;
        for (unsigned i = 0; i < length; ++i) value |= (uint32_t)bytes[at++] << (8 * i);
        switch (tag & 0xfc) {
            case 0x04: page = value; break;
            case 0x08: usage = value; break;
            case 0x74: size = value; break;
            case 0x94: count = value; break;
            case 0x84: report = value; break;
            case 0xa4: case 0xb4: return false;
            case 0xa0: collection |= page == 0xff00 && usage == 1; usage = 0; break;
            case 0x80: if (report == 2 && size <= 32 && count <= 1024) input += size * count; break;
            case 0x90: if (report == 2 && size <= 32 && count <= 1024) output += size * count; break;
        }
    }
    return collection && input == 63 * 8 && output == 63 * 8;
}
}

namespace {
class HidLink {
public:
    ~HidLink() { if (fd_ >= 0) close(fd_); }
    bool Open(const std::string& card, std::wstring& problem) {
        if (card.empty() || card.find_first_not_of("0123456789") != card.npos) return false;
        fs::path audio = fs::path("/sys/class/sound") / ("card" + card) / "device";
        std::error_code error;
        for (const auto& entry : fs::directory_iterator("/sys/class/hidraw", error)) {
            if (!lx::MatchingHeadsetUsb(entry.path() / "device", audio)) continue;
            fs::path node = fs::path("/dev") / entry.path().filename();
            int fd = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                problem = L"Cannot access this headset's hidraw interface. Allow your user read/write access to USB 1532:0555 interface 03, then Refresh.";
                continue;
            }
            hidraw_devinfo info{};
            int length = 0;
            hidraw_report_descriptor descriptor{};
            bool valid = ioctl(fd, HIDIOCGRAWINFO, &info) >= 0 && info.bustype == BUS_USB &&
                info.vendor == 0x1532 && info.product == 0x0555 &&
                ioctl(fd, HIDIOCGRDESCSIZE, &length) >= 0 && length > 0 && length <= HID_MAX_DESCRIPTOR_SIZE;
            if (valid) {
                descriptor.size = length;
                valid = ioctl(fd, HIDIOCGRDESC, &descriptor) >= 0 &&
                    lx::BlackSharkDescriptor({descriptor.value, descriptor.size});
            }
            if (valid) { fd_ = fd; return true; }
            close(fd);
        }
        return false;
    }
    bool Send(const std::array<uint8_t, 64>& report) {
        pollfd descriptor{fd_, POLLOUT, 0};
        return poll(&descriptor, 1, 300) > 0 && (descriptor.revents & POLLOUT) &&
            write(fd_, report.data(), report.size()) == (ssize_t)report.size();
    }
    bool Remote(bool on) {
        bool ok = Send(BlackSharkReport(2, 0xe1, on));
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        return ok;
    }
    bool Query(uint8_t command, uint8_t& value) { return Query(command, &value, 1); }
    bool Query(uint8_t command, uint8_t* values, size_t count) {
        bool ok = false;
        Remote(true);
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (!Remote(true)) break;
            std::array<uint8_t, 64> report{};
            for (int i = 0; i < 64 && read(fd_, report.data(), report.size()) > 0; ++i) {}
            if (!Send(BlackSharkReport(3, command))) break;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(180);
            for (;;) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) break;
                pollfd descriptor{fd_, POLLIN, 0};
                if (poll(&descriptor, 1, (int)left) <= 0 || !(descriptor.revents & POLLIN)) break;
                ssize_t size = read(fd_, report.data(), report.size());
                if (size > 0 && BlackSharkReply(report.data(), size, command, values, count)) { ok = true; break; }
            }
        }
        Remote(false);
        return ok;
    }
    bool Set(uint8_t command, uint8_t value) {
        bool ok = Remote(true) && Remote(true) && Send(BlackSharkReport(4, command, value));
        Remote(false);
        return ok;
    }
    bool Preset(int preset) {
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (!Remote(true) || !Remote(true) || !Send(BlackSharkReport(3, 0x1e)) ||
                !Remote(true) || !Send(BlackSharkReport(4, 0x93, preset)) ||
                !Remote(true) || !Send(BlackSharkReport(4, 0x9d, preset >= 250 && preset < 255 ? 2 : 1)) ||
                !Remote(true)) { Remote(false); return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            uint8_t current = 0;
            if (Query(0x13, current) && current == preset) return true;
        }
        return false;
    }
private:
    int fd_ = -1;
};

void ReadHardware(HidLink& link, HeadsetInfo& info) {
    uint8_t value = 0;
    if (!link.Query(0x21, value) || value > 100) {
        info.status = L"Headset did not reply. Check its power and wireless connection, then Refresh.";
        return;
    }
    info.battery = value;
    if (link.Query(0x2a, value)) info.charging = value != 0;
    if (link.Query(0x18, value)) {
        if (!value) info.sidetone = 0;
        else if (link.Query(0x19, value) && value <= 10) info.sidetone = value;
    }
    if (link.Query(0x2c, value) && (value == 0 || (value >= 15 && value <= 60))) info.idleMinutes = value;
    if (link.Query(0x27, value) && value <= 1) info.dnd = value;
    if (link.Query(0x13, value)) info.preset = value;
    std::array<uint8_t, 10> bands{};
    info.eqKnown = link.Query(0x15, bands.data(), bands.size());
    if (info.eqKnown) for (size_t i = 0; i < bands.size(); ++i) info.eqBands[i] = (int8_t)bands[i];
    info.status = L"USB hardware connected. Changes affect the headset in every application.";
}

bool ApplyHardware(HidLink& link, const HeadsetChanges& c) {
    if (c.sidetone >= 0) {
        if (c.sidetone && !link.Set(0x99, c.sidetone)) return false;
        if (!link.Set(0x98, c.sidetone != 0)) return false;
    }
    if (c.idleMinutes >= 0 && !link.Set(0xac, c.idleMinutes)) return false;
    if (c.dnd >= 0 && !link.Set(0xa7, c.dnd)) return false;
    return c.preset < 0 || link.Preset(c.preset);
}
}

struct HeadsetController::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool quit = false, refresh = false, selectedChanged = false;
    std::string selected;
    HeadsetInfo info;
    std::optional<std::pair<std::wstring, HeadsetChanges>> pending;
    void Run() {
        for (;;) {
            bool detailed;
            std::string node;
            std::optional<std::pair<std::wstring, HeadsetChanges>> command;
            {
                std::lock_guard lock(mutex);
                if (quit) break;
                detailed = std::exchange(refresh, false);
                selectedChanged = false;
                node = selected;
                command = std::move(pending);
                pending.reset();
            }
            auto outputs = ListOutputDevices();
            const OutputDevice* output = nullptr;
            for (const auto& candidate : outputs)
                if (node.empty() ? candidate.systemDefault : candidate.node == node) output = &candidate;
            if (!output) for (const auto& candidate : outputs) if (candidate.systemDefault) output = &candidate;
            HeadsetInfo next;
            if (output) {
                next.id = FromUtf8(output->node);
                next.name = FromUtf8(output->description);
                next.model = RecognizeHeadphones(next.name);
            }
            {
                std::lock_guard lock(mutex);
                if (selectedChanged) continue;  // the UI changed output while enumeration was in flight
                if (next.id == info.id) { next = info; next.busy = false; }
                else detailed = true;
            }
            if ((detailed || command) && output) {
                ++next.revision;
                next.battery = next.charging = next.sidetone = next.idleMinutes = next.dnd = next.preset = -1;
                next.eqKnown = false;
                HidLink link;
                next.status.clear();
                next.hardware = link.Open(output->alsaCard, next.status);
                if (next.hardware) {
                    next.model = L"Razer BlackShark V2 Pro 2.4 (USB)";
                    bool applied = !command || (command->first == next.id && ApplyHardware(link, command->second));
                    ReadHardware(link, next);
                    if (command) {
                        const auto& c = command->second;
                        bool verified = applied && (c.sidetone < 0 || c.sidetone == next.sidetone) &&
                            (c.idleMinutes < 0 || c.idleMinutes == next.idleMinutes) && (c.dnd < 0 || c.dnd == next.dnd) &&
                            (c.preset < 0 || (c.preset == next.preset && next.eqKnown));
                        next.status = verified ? L"Hardware settings applied and read back from the headset." :
                            L"Couldn't verify every setting. Refresh before retrying; some changes may have applied.";
                    }
                } else if (next.status.empty()) next.status = L"Native controls aren't available for this output. Cordinal EQ and crossfeed are available.";
            }
            {
                std::unique_lock lock(mutex);
                if (!selectedChanged) { next.busy = pending.has_value() || refresh; info = std::move(next); }
                wake.wait_for(lock, std::chrono::seconds(2), [&] { return quit || refresh || pending || selectedChanged; });
            }
        }
    }
};

HeadsetController::HeadsetController() : impl_(std::make_unique<Impl>()) {}
HeadsetController::~HeadsetController() { Stop(); }
void HeadsetController::Start() {
    if (impl_->worker.joinable()) return;
    impl_->quit = false;
    impl_->worker = std::thread([this] { impl_->Run(); });
}
void HeadsetController::Stop() {
    { std::lock_guard lock(impl_->mutex); impl_->quit = true; }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}
void HeadsetController::SelectOutput(std::string node) {
    std::lock_guard lock(impl_->mutex);
    if (node == impl_->selected) return;
    impl_->selected = std::move(node);
    impl_->selectedChanged = true;
    impl_->info.busy = true;
    impl_->wake.notify_all();
}
void HeadsetController::Refresh() {
    { std::lock_guard lock(impl_->mutex); impl_->refresh = true; impl_->info.busy = true; }
    impl_->wake.notify_all();
}
HeadsetInfo HeadsetController::Snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->info;
}
bool HeadsetController::Apply(const std::wstring& id, const HeadsetChanges& changes) {
    std::lock_guard lock(impl_->mutex);
    if (id.empty() || id != impl_->info.id || !impl_->info.hardware || impl_->info.busy || !ValidHeadsetChanges(changes)) return false;
    impl_->pending = std::make_pair(id, changes);
    impl_->info.busy = true;
    impl_->wake.notify_all();
    return true;
}
