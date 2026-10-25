#include <JSystem/JAudio2/JASCriticalSection.h>

namespace {
struct AudioLock {
    OSMutex mutex;
    AudioLock() { OSInitMutex(&mutex); }
};
OSMutex* audioMutex() {
    // Same process-wide recursive exclusion as the desktop JAS adapter.
    // The native OS mutex uses libctru primitives and retains thread ownership.
    static AudioLock lock;
    return &lock.mutex;
}
}
JASCriticalSection::JASCriticalSection() { OSLockMutex(audioMutex()); }
JASCriticalSection::~JASCriticalSection() { OSUnlockMutex(audioMutex()); }
