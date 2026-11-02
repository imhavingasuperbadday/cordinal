// iPhone / iPod Touch sync handshake, ported from itdb_iphone.c.
//
// Upstream called libimobiledevice inline from the iTunesDB sync path and
// returned negative errno values.  This version keeps the *protocol sequence*
// (notifications, AFC lock-file locking, post-process cleanup) in IphoneSync
// and pushes the device calls behind IphoneTransport.  The sequence is then
// testable with a fake transport, and the default build stays free of the
// optional libimobiledevice dependency.
//
// GPOD_HAVE_IMOBILEDEVICE records whether the real backend exists so that
// iphone.cpp (the fallback) and iphone_imobiledevice.cpp (the backend) agree on
// exactly one definition of the factory and of iphone_backend_available().

#ifndef GPOD_IPHONE_HPP
#define GPOD_IPHONE_HPP

#include <chrono>
#include <memory>
#include <string>

#if defined(GPOD_WITH_IPHONE)
#if defined(__has_include)
#if __has_include(<libimobiledevice/libimobiledevice.h>)
#define GPOD_HAVE_IMOBILEDEVICE 1
#endif
#endif
#endif
#ifndef GPOD_HAVE_IMOBILEDEVICE
#define GPOD_HAVE_IMOBILEDEVICE 0
#endif

namespace gpod {

class Device;

namespace iphone {

// Notification names posted through com.apple.mobile.notification_proxy.
// libimobiledevice spells these NP_SYNC_WILL_START and friends.
inline constexpr char sync_will_start[] = "com.apple.itunes-mobdev.syncWillStart";
inline constexpr char sync_lock_request[] = "com.apple.itunes-mobdev.syncLockRequest";
inline constexpr char sync_did_start[] = "com.apple.itunes-mobdev.syncDidStart";
inline constexpr char sync_failed_to_start[] = "com.apple.itunes-mobdev.syncFailedToStart";
inline constexpr char sync_did_finish[] = "com.apple.itunes-mobdev.syncDidFinish";

// Absolute AFC paths.  The lock file is rooted at the AFC share, while the
// post-process leftovers live under iTunes_Control on newer devices.
inline constexpr char lock_file_path[] = "/com.apple.itunes.lock_sync";
inline constexpr char post_process_status_path[] =
    "/iTunes_Control/iTunes/iTunes Library.itlp/DBTemp/"
    ".status-com.apple.itdprep.command.runPostProcess";
inline constexpr char ddd_itdbm_path[] =
    "/iTunes_Control/iTunes/iTunes Library.itlp/DBTemp/ddd.itdbm";

}  // namespace iphone

// Result of one attempt at the exclusive AFC lock.  Upstream compared the raw
// afc_error_t against AFC_E_SUCCESS and AFC_E_OP_WOULD_BLOCK; everything else
// aborted the sync.
enum class LockResult {
    Acquired,
    WouldBlock,
    Error,
};

struct IphoneSyncOptions {
    // Upstream tried LOCK_ATTEMPTS times, sleeping LOCK_WAIT microseconds
    // between attempts.
    int lock_attempts = 50;
    std::chrono::milliseconds lock_wait{200};
};

// Every operation IphoneSync needs from the device.  The backend keeps the
// libimobiledevice handles; the state machine never sees them.  Implementations
// report failures by throwing gpod::Error (ErrorCode::Io).
class IphoneTransport {
public:
    IphoneTransport() = default;
    IphoneTransport(const IphoneTransport&) = delete;
    IphoneTransport& operator=(const IphoneTransport&) = delete;
    virtual ~IphoneTransport() = default;

    // idevice_new() for the device UUID.
    virtual void connect(const std::string& uuid) = 0;
    // lockdownd_client_new_with_handshake().  Also used by stop() to open the
    // fresh session needed for the closing notification, which is why it must
    // replace any current session rather than nest one.
    virtual void lockdown_handshake() = 0;
    // lockdownd_client_free().  Upstream released the client once syncDidStart
    // had been posted.
    virtual void close_lockdown() = 0;
    // lockdownd_start_service("com.apple.afc") plus afc_client_new().
    virtual void start_afc() = 0;
    // afc_client_free().  Upstream closed AFC before reconnecting for
    // syncDidFinish.
    virtual void close_afc() = 0;
    // Start the notification proxy service and post one notification.
    virtual void post_notification(const std::string& name) = 0;
    // afc_file_open(path, AFC_FOPEN_RW).
    virtual void open_lock_file(const std::string& path) = 0;
    // One afc_file_lock(AFC_LOCK_EX) attempt on the open lock file.
    [[nodiscard]] virtual LockResult lock_exclusive() = 0;
    // afc_file_lock(AFC_LOCK_UN) plus afc_file_close().  Must tolerate a lock
    // that was opened but never acquired, like upstream's cleanup did.
    virtual void unlock_and_close() = 0;
    // afc_remove_path().
    virtual void remove_path(const std::string& path) = 0;
    // idevice_free().  Must be idempotent.
    virtual void disconnect() = 0;
};

// RAII owner of one iPhone sync session.  start() runs the handshake and
// returns an object that holds the AFC lock; stop() (or the destructor) runs
// the closing sequence.  Nothing here throws out of the destructor.
class IphoneSync {
public:
    // Throws Error(InvalidArgument) when the device has no UUID,
    // Error(Unsupported) when no transport is available, and Error(Io) when the
    // device rejects any step.  A failure unwinds like upstream's
    // leave_with_err label: the lock is released, AFC and lockdownd are closed,
    // and syncFailedToStart is posted if syncWillStart already went out.
    [[nodiscard]] static std::unique_ptr<IphoneSync> start(
        Device& device, std::unique_ptr<IphoneTransport> transport = {},
        IphoneSyncOptions options = {});

    ~IphoneSync();

    IphoneSync(const IphoneSync&) = delete;
    IphoneSync& operator=(const IphoneSync&) = delete;
    IphoneSync(IphoneSync&& other) noexcept;
    IphoneSync& operator=(IphoneSync&& other) noexcept;

    // Idempotent and non-throwing: removes the post-process leftovers, unlocks
    // and closes the lock file, posts syncDidFinish on a fresh lockdownd
    // session, then disconnects.
    void stop() noexcept;

private:
    explicit IphoneSync(std::unique_ptr<IphoneTransport> transport);

    std::unique_ptr<IphoneTransport> transport_;
    bool lock_file_open_ = false;
    bool started_ = false;
};

// True when the real libimobiledevice transport was compiled in.
[[nodiscard]] bool iphone_backend_available() noexcept;

// Builds the real transport, or throws Error(Unsupported) when the backend is
// not compiled in.  Declared unconditionally so callers do not need the
// GPOD_WITH_IPHONE define.
[[nodiscard]] std::unique_ptr<IphoneTransport> create_imobiledevice_transport();

}  // namespace gpod

#endif  // GPOD_IPHONE_HPP
