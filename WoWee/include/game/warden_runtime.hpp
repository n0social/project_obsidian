#pragma once

#include <atomic>
#include <mutex>

namespace wowee::game {

/** Shared by GameHandler packet enqueue/drain and the Warden watchdog thread. */
inline std::mutex& wardenPacketQueueMutex() {
    static std::mutex mutex;
    return mutex;
}

/** Held for extract + handle of SMSG_WARDEN_DATA so RC4 stays in packet order. */
inline std::recursive_mutex& wardenDispatchMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

inline std::atomic<bool>& wardenPumpGuard() {
    static std::atomic<bool> busy{false};
    return busy;
}

} // namespace wowee::game
