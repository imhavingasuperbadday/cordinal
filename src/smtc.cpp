// Windows System Media Transport Controls: the media flyout with cover art, the lock screen,
// headset buttons and the Game Bar widget. Owns the WinRT objects behind them; every public call
// runs on the UI thread. Button presses arrive on a system thread and are only posted to the
// window as messages.
#include "smtc.h"

#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <shcore.h>
#include <systemmediatransportcontrolsinterop.h>
#include <windows.media.h>
#include <windows.storage.streams.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <atomic>
#include <cmath>

namespace smtc {
namespace {

namespace Media = ABI::Windows::Media;
namespace Streams = ABI::Windows::Storage::Streams;
namespace Foundation = ABI::Windows::Foundation;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
using ButtonHandler =
    Foundation::__FITypedEventHandler_2_Windows__CMedia__CSystemMediaTransportControls_Windows__CMedia__CSystemMediaTransportControlsButtonPressedEventArgs_t;

// Only the UI thread writes these. The button handler reads g_hwnd from a system thread.
ComPtr<Media::ISystemMediaTransportControls> g_controls;
ComPtr<Media::ISystemMediaTransportControls2> g_controls2;  // the timeline lives on the second interface
ComPtr<Media::ISystemMediaTransportControlsDisplayUpdater> g_updater;
ComPtr<Media::ISystemMediaTransportControlsTimelineProperties> g_timeline;
EventRegistrationToken g_buttonToken{};
std::atomic<HWND> g_hwnd{nullptr};
UINT g_msg = 0;
bool g_roInitialized = false;

// Runs on a system thread: it must not touch UI state, so it only posts.
HRESULT OnButtonPressed(Media::ISystemMediaTransportControls*,
                        Media::ISystemMediaTransportControlsButtonPressedEventArgs* args) {
    Media::SystemMediaTransportControlsButton button{};
    if (FAILED(args->get_Button(&button))) return S_OK;
    Button b = Button::Play;
    switch (button) {
        case Media::SystemMediaTransportControlsButton_Play: b = Button::Play; break;
        case Media::SystemMediaTransportControlsButton_Pause: b = Button::Pause; break;
        case Media::SystemMediaTransportControlsButton_Stop: b = Button::Stop; break;
        case Media::SystemMediaTransportControlsButton_Next: b = Button::Next; break;
        case Media::SystemMediaTransportControlsButton_Previous: b = Button::Previous; break;
        default: return S_OK;
    }
    if (HWND hwnd = g_hwnd.load()) PostMessageW(hwnd, g_msg, (WPARAM)b, 0);
    return S_OK;
}

// Ticks are 100 ns, the unit of TimeSpan.
Foundation::TimeSpan SpanOf(double seconds) {
    Foundation::TimeSpan span{};
    span.Duration = (INT64)std::llround(seconds * 10000000.0);
    return span;
}

// The flyout takes a stream, so the bytes go into an in-memory stream first.
bool SetThumbnail(Media::ISystemMediaTransportControlsDisplayUpdater* updater, const std::string& bytes) {
    ComPtr<IInspectable> insp;
    ComPtr<Streams::IRandomAccessStream> stream;
    if (FAILED(RoActivateInstance(
            HStringReference(RuntimeClass_Windows_Storage_Streams_InMemoryRandomAccessStream).Get(), &insp)) ||
        FAILED(insp.As(&stream)))
        return false;
    {
        ComPtr<IStream> view;
        if (FAILED(CreateStreamOverRandomAccessStream(stream.Get(), IID_PPV_ARGS(&view)))) return false;
        ULONG written = 0;
        if (FAILED(view->Write(bytes.data(), (ULONG)bytes.size(), &written)) || written != bytes.size()) return false;
    }
    if (FAILED(stream->Seek(0))) return false;

    ComPtr<Streams::IRandomAccessStreamReferenceStatics> statics;
    ComPtr<Streams::IRandomAccessStreamReference> reference;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference).Get(), IID_PPV_ARGS(&statics))) ||
        FAILED(statics->CreateFromStream(stream.Get(), &reference)))
        return false;
    return SUCCEEDED(updater->put_Thumbnail(reference.Get()));
}

}  // namespace

// Creates the controls for the window and registers the button handler; false when the controls
// are unavailable (Windows 8.1 or older, or no shell).
bool Init(HWND hwnd, UINT msg) {
    if (g_controls) {
        g_hwnd = hwnd;
        g_msg = msg;
        return true;
    }
    // The UI thread is already an STA. RPC_E_CHANGED_MODE means another apartment model, which
    // activation still accepts, so it is not an error here.
    g_roInitialized = SUCCEEDED(RoInitialize(RO_INIT_SINGLETHREADED));

    ComPtr<ISystemMediaTransportControlsInterop> interop;
    ComPtr<Media::ISystemMediaTransportControls> controls;
    ComPtr<Media::ISystemMediaTransportControlsDisplayUpdater> updater;
    ComPtr<Media::ISystemMediaTransportControls2> controls2;
    ComPtr<Media::ISystemMediaTransportControlsTimelineProperties> timeline;
    ComPtr<IInspectable> timelineObject;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControls).Get(),
                                      IID_PPV_ARGS(&interop))) ||
        FAILED(interop->GetForWindow(hwnd, IID_PPV_ARGS(&controls))) ||
        FAILED(controls->get_DisplayUpdater(&updater)) ||
        FAILED(RoActivateInstance(
            HStringReference(RuntimeClass_Windows_Media_SystemMediaTransportControlsTimelineProperties).Get(),
            &timelineObject)) ||
        FAILED(controls.As(&controls2)) || FAILED(timelineObject.As(&timeline))) {
        if (g_roInitialized) RoUninitialize();
        g_roInitialized = false;
        return false;
    }

    controls->put_IsEnabled(TRUE);
    controls->put_IsPlayEnabled(TRUE);
    controls->put_IsPauseEnabled(TRUE);
    controls->put_IsStopEnabled(TRUE);
    controls->put_IsNextEnabled(TRUE);
    controls->put_IsPreviousEnabled(TRUE);
    updater->put_Type(Media::MediaPlaybackType_Music);
    updater->Update();

    auto handler = Callback<ButtonHandler>(
        [](Media::ISystemMediaTransportControls* sender,
           Media::ISystemMediaTransportControlsButtonPressedEventArgs* args) -> HRESULT {
            return OnButtonPressed(sender, args);
        });
    EventRegistrationToken token{};
    if (!handler.Get() || FAILED(controls->add_ButtonPressed(handler.Get(), &token))) {
        if (g_roInitialized) RoUninitialize();
        g_roInitialized = false;
        return false;
    }
    g_controls = controls;
    g_controls2 = controls2;
    g_updater = updater;
    g_timeline = timeline;
    g_buttonToken = token;
    g_msg = msg;
    g_hwnd = hwnd;
    return true;
}

// Maps our playback state onto the WinRT enum.
void SetState(State s) {
    if (!g_controls) return;
    Media::MediaPlaybackStatus status = Media::MediaPlaybackStatus_Closed;
    switch (s) {
        case State::Closed: status = Media::MediaPlaybackStatus_Closed; break;
        case State::Stopped: status = Media::MediaPlaybackStatus_Stopped; break;
        case State::Playing: status = Media::MediaPlaybackStatus_Playing; break;
        case State::Paused: status = Media::MediaPlaybackStatus_Paused; break;
    }
    g_controls->put_PlaybackStatus(status);
}

// Publishes title, artist and album, then commits the change with Update().
void SetMetadata(const std::wstring& title, const std::wstring& artist, const std::wstring& album,
                 const std::string& image) {
    if (!g_updater) return;
    ComPtr<Media::IMusicDisplayProperties> music;
    if (SUCCEEDED(g_updater->get_MusicProperties(&music))) {
        HString text;
        text.Set(title.c_str(), (UINT32)title.size());
        music->put_Title(text.Get());
        text.Set(artist.c_str(), (UINT32)artist.size());
        music->put_Artist(text.Get());
        text.Set(album.c_str(), (UINT32)album.size());
        ComPtr<Media::IMusicDisplayProperties2> music2;
        if (SUCCEEDED(music.As(&music2))) music2->put_AlbumTitle(text.Get());
    }
    if (image.empty() || !SetThumbnail(g_updater.Get(), image)) g_updater->put_Thumbnail(nullptr);
    g_updater->Update();
}

// Clamps to a valid range, since the control rejects an out-of-range or negative timeline.
void SetTimeline(double position, double duration) {
    if (!g_timeline || !g_controls2) return;
    if (!(duration > 0)) duration = 0;
    if (!(position > 0)) position = 0;
    if (position > duration) position = duration;
    g_timeline->put_StartTime(SpanOf(0));
    g_timeline->put_EndTime(SpanOf(duration));
    g_timeline->put_MinSeekTime(SpanOf(0));
    g_timeline->put_MaxSeekTime(SpanOf(duration));
    g_timeline->put_Position(SpanOf(position));
    g_controls2->UpdateTimelineProperties(g_timeline.Get());
}

// Unhooks the handler, hides the flyout and releases the WinRT objects; mirrors Init.
void Shutdown() {
    g_hwnd = nullptr;
    if (g_controls) {
        g_controls->remove_ButtonPressed(g_buttonToken);
        g_controls->put_IsEnabled(FALSE);  // takes the flyout down at once
    }
    g_timeline.Reset();
    g_updater.Reset();
    g_controls2.Reset();
    g_controls.Reset();
    if (g_roInitialized) RoUninitialize();
    g_roInitialized = false;
}

}  // namespace smtc
