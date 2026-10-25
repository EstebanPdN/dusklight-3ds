#pragma once
#include <dolphin/os/OSMessage.h>

namespace dusk::native {
// The owner closes before joining waiters and releasing queue storage.
// Pending messages can be drained; all blocked operations are woken.
void closeMessageQueue(OSMessageQueue* queue);
// Called by the native thread trampoline on its real owner after user unwind.
void releaseCurrentThreadMutexes();
}
