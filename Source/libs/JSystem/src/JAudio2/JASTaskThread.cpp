#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/JAudio2/JASTaskThread.h"
#include "JSystem/JAudio2/JASCalc.h"
#include "JSystem/JAudio2/JASCriticalSection.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#if defined(DUSK_TARGET_3DS)
#include "helpers/native_thread.h"
#include <atomic>
#endif

JASTaskThread::JASTaskThread(int priority, int msgCount, u32 stackSize) :
    JKRThread(JASDram, stackSize, msgCount, priority)
{
    field_0x84 = false;
    OSInitThreadQueue(&threadQueue_);
}

JASTaskThread::~JASTaskThread() {
#if defined(DUSK_TARGET_3DS)
    // Stop/join before draining pending commands or destroying derived state.
    OSCancelThread(getThreadRecord());
#endif
    OSMessage msg;
    BOOL received;
    while (true) {
        msg = waitMessage(&received);
        if (!received) {
            return;
        }
        JASKernel::getCommandHeap()->free(msg);
    }
}

void* JASTaskThread::allocCallStack(JASThreadCallback callback, const void* msg, u32 msgSize) {
    ThreadMemPool* heap;
    u32 size = msgSize + offsetof(JASThreadCallStack, msg);
    JASThreadCallStack *callStack = (JASThreadCallStack*) JASKernel::getCommandHeap()->alloc(size);
    if (callStack == NULL) {
        return NULL;
    }

    callStack->msgType_ = 1;
#if TARGET_ANDROID
    JASCalc::_bcopy(msg, callStack->msg.buffer, msgSize);
#else
    JASCalc::bcopy(msg, callStack->msg.buffer, msgSize);
#endif
    callStack->callback_ = callback;
    return callStack;
}

void* JASTaskThread::allocCallStack(JASThreadCallback callback, void* msg) {
    JASThreadCallStack *callStack;
    callStack = (JASThreadCallStack*)JASKernel::getCommandHeap()->alloc(offsetof(JASThreadCallStack, msg) + sizeof(void*));
    if (callStack == NULL) {
        return NULL;
    }

    callStack->msgType_ = 0;
    callStack->msg.bufferPtr = msg;
    callStack->callback_ = callback;
    return callStack;
}

int JASTaskThread::sendCmdMsg(JASThreadCallback callback, const void* msg, u32 msgSize) {
    void* callstack;

    callstack = allocCallStack(callback, msg, msgSize);
    if (callstack == NULL) {
        return 0;
    }

    BOOL iVar2 = sendMessage(callstack);
    if (!iVar2) {
        JASKernel::getCommandHeap()->free(callstack);
    }
    return iVar2;
}

int JASTaskThread::sendCmdMsg(JASThreadCallback callback, void* msg) {
    void* callstack;

    callstack = allocCallStack(callback, msg);
    if (callstack == NULL) {
        return 0;
    }

    BOOL iVar2 = sendMessage(callstack);
    if (!iVar2) {
        JASKernel::getCommandHeap()->free(callstack);
    }
    return iVar2;
}

void* JASTaskThread::run() {
    JASThreadCallStack* callstack;
    OSInitFastCast();
    do {
#ifdef TARGET_PC
        BOOL received = FALSE;
        callstack = static_cast<JASThreadCallStack*>(waitMessageBlock(&received));
        if (!received) {
            break;
        }
#else
        callstack = static_cast<JASThreadCallStack*>(waitMessageBlock());
#endif
#if defined(DUSK_TARGET_3DS)
        struct PendingCommand {
            void* data;
            ~PendingCommand() { JASKernel::getCommandHeap()->free(data); }
        } pending{callstack};
#endif
#if defined(DUSK_TARGET_3DS)
        dusk::native::sleepThreadWhile(&threadQueue_, &field_0x84);
#else
        if (field_0x84) {
            OSSleepThread(&threadQueue_);
        }
#endif

        if (callstack->msgType_) {
            callstack->callback_(callstack->msg.buffer);
        } else {
            callstack->callback_(callstack->msg.bufferPtr);
        }

#if !defined(DUSK_TARGET_3DS)
        JASKernel::getCommandHeap()->free(callstack);
#endif
    } while (true);
#ifdef TARGET_PC
    return NULL;
#endif
}

void JASTaskThread::pause(bool param_0) {
    JASCriticalSection aJStack_14;
#if defined(DUSK_TARGET_3DS)
    std::atomic_ref<bool>(field_0x84).store(param_0, std::memory_order_release);
    if (!param_0) OSWakeupThread(&threadQueue_);
#else
    if (param_0) {
        field_0x84 = 1;
    } else {
        if (field_0x84) {
            OSWakeupThread(&threadQueue_);
        }
        field_0x84 = 0;
    }
#endif
}
