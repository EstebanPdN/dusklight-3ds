// Synthetic host fixture: extracted production controller/UI methods and real CARD storage.
#include "global.h"
#include "m_Do/m_Do_MemCard.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace dusk { bool IsShuttingDown = false; }
namespace {
std::map<OSMutex*, std::recursive_mutex> locks;
OSCond* workerCondition;
OSMutex* workerMutex;
unsigned signals, idleStops;
std::optional<s32> mountResult;
struct PermissionStillBusy {};
constexpr char containerPath[] = "sdmc:/3ds/dusklight/saves/native-card0.bin";

s32 fixtureMount(s32 channel, void* work, CARDCallback callback) {
    if (mountResult) return *mountResult;
    return CARDMount(channel, work, callback);
}
std::vector<char> storedBytes() {
    std::ifstream input(containerPath, std::ios::binary);
    assert(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

extern "C" {
void OSInitMutex(OSMutex* mutex) { assert(locks.try_emplace(mutex).second); }
void OSLockMutex(OSMutex* mutex) { locks.at(mutex).lock(); }
void OSUnlockMutex(OSMutex* mutex) { locks.at(mutex).unlock(); }
BOOL OSTryLockMutex(OSMutex* mutex) { return locks.at(mutex).try_lock(); }
void OSInitCond(OSCond* condition) { workerCondition = condition; }
void OSSignalCond(OSCond* condition) { assert(condition == workerCondition); ++signals; }
void OSWaitCond(OSCond* condition, OSMutex* mutex) {
    assert(condition == workerCondition && mutex == workerMutex);
    assert(g_mDoMemCd_control.isCardCommNone());
    // ponytail: one synchronous worker command; no host thread scheduling claim.
    ++idleStops;
    dusk::IsShuttingDown = true;
}
}

// Unexercised worker commands must never silently succeed.
void mDoMemCd_Ctrl_c::restore() { throw std::runtime_error("unexpected restore dispatch"); }
void mDoMemCd_Ctrl_c::store() { throw std::runtime_error("unexpected store dispatch"); }
void mDoMemCd_Ctrl_c::format() { throw std::runtime_error("unexpected format dispatch"); }

// Only the optional outer-mount negative control substitutes a CARD result.
// All ownership decisions and persistent I/O otherwise use runtime/card.cpp.
#define CARDMount fixtureMount
#include "memcard_methods.inc"
#undef CARDMount
mDoMemCd_Ctrl_c g_mDoMemCd_control;

struct dFile_select_c {
#include "memcard_select_enum.inc"
    using DispFunc = void (dFile_select_c::*)();
    u8 mCardCheckProc = MEMCARDCHECKPROC_STAT_CHECK;
    u8 mNextCardCheckProc = MEMCARDCHECKPROC_STAT_CHECK;
    u8 mKeyWaitCardCheckProc = MEMCARDCHECKPROC_STAT_CHECK;
    bool field_0x0280 = true;
    DispFunc mWindowCloseMsgDispCb = &dFile_select_c::noFileSpaceDispInit;
    DispFunc mKeyWaitMsgDispCb = &dFile_select_c::noFileSpaceDispInit;
    bool allowErrorPresentation = false;
    void errDispInitSet(int, int) {
        if (!allowErrorPresentation) throw std::runtime_error("unexpected error presentation");
    }
    void noSaveSelDispInit() { throw std::runtime_error("unexpected UI callback execution"); }
    void noFileSpaceDispInit() { throw std::runtime_error("unexpected UI callback execution"); }
    void MemCardStatCheck();
};
#include "memcard_select_method.inc"

namespace {
using Controller = mDoMemCd_Ctrl_c;
void queueAttach() {
    auto& control = g_mDoMemCd_control;
    const auto before = signals;
    control.command_attach();
    assert(signals == before + 1 && control.mCardCommand == Controller::COMM_ATTACH_e);
    assert(control.mCardState == Controller::CARD_STATE_13_e && control.getStatus(0) == 14);
    dFile_select_c waiting;
    waiting.MemCardStatCheck();
    assert(waiting.mCardCheckProc == dFile_select_c::MEMCARDCHECKPROC_STAT_CHECK);
    assert(control.mCardCommand == Controller::COMM_ATTACH_e);
}
void runWorker() {
    const auto before = idleStops;
    dusk::IsShuttingDown = false;
    g_mDoMemCd_control.main();
    assert(idleStops == before + 1 && g_mDoMemCd_control.isCardCommNone());
}
void denied() {
    auto& control = g_mDoMemCd_control;
    if (control.getStatus(0) == 14) throw PermissionStillBusy{};
    assert(control.mCardState == Controller::CARD_STATE_FATAL_ERROR_e && control.getStatus(0) == 8);
    dFile_select_c select;
    select.allowErrorPresentation = true;
    select.MemCardStatCheck();
    assert(!select.field_0x0280 && select.mWindowCloseMsgDispCb == nullptr);
    assert(select.mNextCardCheckProc == dFile_select_c::MEMCARDCHECKPROC_ERRMSG_WAIT_KEY);
    assert(select.mKeyWaitMsgDispCb == &dFile_select_c::noSaveSelDispInit);
    assert(select.mKeyWaitCardCheckProc == dFile_select_c::MEMCARDCHECKPROC_NO_SAVE_SEL_DISP);
    assert(control.mCardCommand != Controller::COMM_FORMAT_e && control.mCardCommand != Controller::COMM_RESTORE_e);
}
void accepted() {
    auto& control = g_mDoMemCd_control;
    CARDInit("GZ2E", "01");
    queueAttach();
    runWorker();
    assert(control.mCardState == Controller::CARD_STATE_READY_e && control.getStatus(0) == 2);
    dFile_select_c select;
    select.MemCardStatCheck();
    assert(select.mCardCheckProc == dFile_select_c::MEMCARDCHECKPROC_LOAD_WAIT);
    assert(control.mCardCommand == Controller::COMM_RESTORE_e && control.field_0x1fc8 == 0);
}
}

int exercise() {
    // The runner gives each invocation a fresh ignored Build directory.
    assert(std::filesystem::create_directory("sdmc:"));
    assert(std::filesystem::create_directory("sdmc:/3ds"));
    CARDInit("GZ2E", "01");
    assert(CARDMount(0, nullptr, nullptr) == CARD_RESULT_READY);
    CARDFileInfo file{};
    assert(CARDCreate(0, "gczelda2", CARD_FILE_SIZE, &file) == CARD_RESULT_READY);
    std::array<u8, SECTOR_SIZE> payload;
    for (std::size_t at = 0; at < payload.size(); ++at) payload[at] = static_cast<u8>(at * 17 + 3);
    assert(CARDWrite(&file, payload.data(), payload.size(), SECTOR_SIZE) == CARD_RESULT_READY);
    assert(CARDClose(&file) == CARD_RESULT_READY);
    const auto original = storedBytes();

    auto& control = g_mDoMemCd_control;
    control.mChannel = 0;
    control.mInitialized = true;
    OSInitMutex(&control.mMutex);
    OSInitCond(&control.mCond);
    workerMutex = &control.mMutex;
    for (bool foreignMaker : {false, true}) {
        CARDInit(foreignMaker ? "GZ2E" : "GZ2P", foreignMaker ? "02" : "01");
        queueAttach();
        runWorker(); // Real CARDCheck -> NOPERM -> terminal non-formatting UI flow.
        denied();
        assert(storedBytes() == original);

        queueAttach();
        const auto result = CARDOpen(0, "gczelda2", &file);
        assert(result == CARD_RESULT_NOPERM);
        control.setCardState(result); // Same shared recovery used by restore/store/checkspace.
        denied();
        assert(storedBytes() == original);

        accepted(); // The original owner still reaches the real load dispatch.
        assert(storedBytes() == original);
    }
    CARDInit("GZ2E", "01");
    queueAttach();
    mountResult = CARD_RESULT_NOPERM;
    runWorker(); // Also handle a provider denying CARDMount itself.
    mountResult.reset();
    denied();
    assert(storedBytes() == original);
    accepted();
    assert(storedBytes() == original);
    std::printf("PASS production attach/main/mount/setCardState/getStatus/file-select: "
                "busy state=%d status=14 -> denied state=%d status=8, owner state=%d status=2; "
                "foreign GID/maker bytes preserved\n", Controller::CARD_STATE_13_e,
                Controller::CARD_STATE_FATAL_ERROR_e, Controller::CARD_STATE_READY_e);
    return 0;
}

int main() {
    try {
        return exercise();
    } catch (const PermissionStillBusy&) {
        std::fputs("FAIL permission denial left the consumer busy\n", stderr);
        return 4;
    }
}
