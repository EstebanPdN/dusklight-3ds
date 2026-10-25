#include "dvd_provider.hpp"
#include "bootstrap.hpp"
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

namespace {
std::atomic_flag providerClaim = ATOMIC_FLAG_INIT;
thread_local dusk::native::DvdProvider* activeProvider = nullptr;
std::atomic<dusk::native::DvdProvider*> sharedProvider{};
struct ServiceMutex {OSMutex value;ServiceMutex(){OSInitMutex(&value);}};
OSMutex* serviceMutex(){static ServiceMutex value;return &value.value;}
struct DvdCall {bool shared;DvdCall():shared(sharedProvider.load(std::memory_order_acquire)!=nullptr){if(shared)OSLockMutex(serviceMutex());}~DvdCall(){if(shared)OSUnlockMutex(serviceMutex());}};
dusk::native::DvdProvider* currentProvider(){auto* shared=sharedProvider.load(std::memory_order_acquire);return shared?shared:activeProvider;}
std::atomic<s32> driveStatus{DVD_STATE_NO_DISK};
void publishDrive(dusk::native::DiscStatus status){
    using dusk::native::DiscStatus;
    const s32 state=status==DiscStatus::ready?DVD_STATE_END:
                    status==DiscStatus::missing?DVD_STATE_NO_DISK:
                    status==DiscStatus::unsupported?DVD_STATE_WRONG_DISK:DVD_STATE_FATAL_ERROR;
    driveStatus.store(state,std::memory_order_release);
}
}
namespace dusk::native {
DvdProvider::DvdProvider(Disc& input) : disc(input) {
    if(disc.currentStatus()!=DiscStatus::ready)return;
    if (activeProvider || providerClaim.test_and_set(std::memory_order_acquire)) return;
    if (!found(disc.findDirectory("/", currentDirectory))) {
        providerClaim.clear(std::memory_order_release); return;
    }
    currentPath[0] = '/';
    owner = OSGetCurrentThread(); activeProvider = this; result = DvdStatus::ready;
    disc.observeState(publishDrive);
}
bool DvdProvider::bound() const { return owner && ((owner == OSGetCurrentThread() && activeProvider == this)||sharedProvider.load(std::memory_order_acquire)==this); }
bool DvdProvider::shareWithGameWorkers(){if(!bound())return false;dusk::native::DvdProvider* empty=nullptr;(void)serviceMutex();return sharedProvider.compare_exchange_strong(empty,this,std::memory_order_release);}
bool DvdProvider::found(DiscFileStatus status) {
    result = status == DiscFileStatus::ready ? DvdStatus::ready :
             status == DiscFileStatus::notFound ? DvdStatus::notFound :
             status == DiscFileStatus::invalidPath ? DvdStatus::invalidArgument : DvdStatus::ioError;
    return status == DiscFileStatus::ready;
}
bool DvdProvider::absolutePath(const char* path, std::array<char, 1024>& output, bool directory) {
    output.fill(0); result = DvdStatus::invalidArgument;
    if (!path) return false;
    std::size_t size = 0;
    while (size < output.size() && path[size]) ++size;
    if (!size || size == output.size()) return false;
    if (directory && size > 1 && path[size - 1] == '/') {
        if (path[size - 2] == '/') return false;
        --size;
    }
    std::size_t prefix = 0;
    if (path[0] != '/') {
        if (!disc.directoryValid(currentDirectory)) { result = DvdStatus::ioError; return false; }
        prefix = std::strlen(currentPath.data());
        std::memcpy(output.data(), currentPath.data(), prefix);
        if (prefix > 1) output[prefix++] = '/';
    }
    if (size >= output.size() - prefix) return false;
    std::memcpy(output.data() + prefix, path, size);
    return true;
}
DvdProvider::~DvdProvider() {
    if (!owner) return;
    if (!bound()) fatal("DVD provider destroyed outside its loading worker\n");
    // Do not dereference caller-owned records during teardown; every outstanding
    // record becomes invalid because the scope-local registry disappears.
    disc.observeState(nullptr);driveStatus.store(DVD_STATE_NO_DISK,std::memory_order_release);
    activeProvider = nullptr; owner = nullptr; providerClaim.clear(std::memory_order_release);
}
bool DvdProvider::open(const char* path, DVDFileInfo* info) {
    if (!bound()) return false;
    result = DvdStatus::invalidArgument;
    if (!path || !info) return false;
    for (const auto& slot : slots) if (slot.info == info) return false;
    std::array<char, 1024> resolved{};
    if (!absolutePath(path, resolved, false)) return false;
    DiscFile file;
    if (!found(disc.findFile(resolved.data(), file))) return false;
    if (file.size() > std::numeric_limits<std::int32_t>::max()) { result = DvdStatus::unsupported; return false; }
    auto slot = std::find_if(slots.begin(), slots.end(), [](const auto& value) { return !value.info; });
    if (slot == slots.end()) {
        result = DvdStatus::exhausted;
        OSReport("Native DVD open exhausted: %u retained file records\n",fileCapacity);
        return false;
    }
    slot->file = file; slot->info = info;
    std::strcpy(slot->path.data(), resolved.data());
    *info = {}; info->length = file.size(); info->startAddr=file.discOffset(); info->cb.state = DVD_STATE_END;
    result = DvdStatus::ready;
    return true;
}
bool DvdProvider::close(DVDFileInfo* info) {
    if (!bound()) return false;
    result = DvdStatus::invalidArgument;
    if (!info) return false;
    for (auto& slot : slots) if (slot.info == info) {
        slot.info = nullptr; *info = {}; info->cb.state = DVD_STATE_END;
        result = DvdStatus::ready; return true;
    }
    return false;
}
bool DvdProvider::openEntry(std::int32_t entry,DVDFileInfo* info) {
    if(!bound())return false;
    result=DvdStatus::invalidArgument;
    if(entry<0 || !info)return false;
    for(const auto& slot:slots)if(slot.info==info)return false;
    DiscFile file;
    const auto found=disc.openFile(static_cast<std::uint32_t>(entry),file);
    if(found!=DiscFileStatus::ready) {
        result=found==DiscFileStatus::notFound?DvdStatus::notFound:DvdStatus::ioError;
        return false;
    }
    if(file.size()>INT32_MAX){result=DvdStatus::unsupported;return false;}
    auto slot=std::find_if(slots.begin(),slots.end(),[](const auto& value){return !value.info;});
    if(slot==slots.end()){
        result=DvdStatus::exhausted;
        OSReport("Native DVD entry %ld exhausted: %u retained file records\n",static_cast<long>(entry),fileCapacity);
        return false;
    }
    slot->file=file;slot->info=info;slot->path.fill(0);
    *info={};info->length=file.size();info->startAddr=file.discOffset();info->cb.state=DVD_STATE_END;
    result=DvdStatus::ready;return true;
}
std::int32_t DvdProvider::fileStatus(const DVDFileInfo* info) const {
    if(bound() && info)for(const auto& slot:slots)if(slot.info==info && disc.readFile(slot.file,0,nullptr,0))return info->cb.state;
    return DVD_STATE_FATAL_ERROR;
}
std::int32_t DvdProvider::commandStatus(const DVDCommandBlock* block) const {
    if(bound() && block)for(const auto& slot:slots)if(slot.info && &slot.info->cb==block)return fileStatus(slot.info);
    return DVD_STATE_FATAL_ERROR;
}
std::int32_t DvdProvider::read(DVDFileInfo* info, void* output, std::int32_t length, std::int32_t offset) {
    if (!bound()) return DVD_RESULT_FATAL_ERROR;
    result = DvdStatus::invalidArgument;
    if (!info || length < 0 || offset < 0 || (length && !output)) return DVD_RESULT_FATAL_ERROR;
    for (auto& slot : slots) if (slot.info == info) {
        if (std::uint32_t(offset) > slot.file.size()) return DVD_RESULT_FATAL_ERROR;
        const auto count = std::min(std::uint32_t(length), slot.file.size() - std::uint32_t(offset));
        info->cb.state = DVD_STATE_BUSY;
        info->cb.command=DVD_COMMAND_READ;
        info->cb.offset = offset; info->cb.length = length; info->cb.addr = output;
        info->cb.currTransferSize = info->cb.transferredSize = 0;
        if (!disc.readFile(slot.file, offset, output, count)) {
            info->cb.state = DVD_STATE_FATAL_ERROR; result = DvdStatus::ioError;
            return DVD_RESULT_FATAL_ERROR;
        }
        // A synchronous read reports only logical bytes, as original archive
        // overlay callers require for their aligned request lengths. No callback.
        info->cb.currTransferSize = info->cb.transferredSize = count;
        info->cb.state = DVD_STATE_END; result = DvdStatus::ready;
        return static_cast<std::int32_t>(count);
    }
    return DVD_RESULT_FATAL_ERROR;
}
bool DvdProvider::entryPath(std::int32_t entry, char* output, std::uint32_t capacity) {
    if (!bound()) return false;
    result = DvdStatus::invalidArgument;
    if (entry < 0 || !output || !capacity) return false;
    output[0] = '\0';
    for (const auto& slot : slots) if (slot.path[0] && slot.file.entryIndex() == std::uint32_t(entry)) {
        if (!disc.readFile(slot.file, 0, nullptr, 0)) { result = DvdStatus::ioError; return false; }
        const auto bytes = std::strlen(slot.path.data()) + 1;
        if (bytes > capacity) return false;
        std::memcpy(output, slot.path.data(), bytes); result = DvdStatus::ready; return true;
    }
    result = DvdStatus::unsupported;
    return false;
}
std::int32_t DvdProvider::pathEntry(const char* path) {
    if(!bound())return -1;
    std::array<char, 1024> resolved{};
    if (!absolutePath(path, resolved, false)) return -1;
    std::uint32_t index = 0;
    if (!found(disc.findEntry(resolved.data(), index))) return -1;
    result=DvdStatus::ready;return static_cast<std::int32_t>(index);
}
bool DvdProvider::changeDirectory(const char* path) {
    if (!bound()) return false;
    std::array<char, 1024> resolved{};
    if (!absolutePath(path, resolved, true)) return false;
    DiscDirectory directory;
    if (!found(disc.findDirectory(resolved.data(), directory))) return false;
    currentDirectory = directory; currentPath = resolved; return true;
}
bool DvdProvider::currentDirectoryPath(char* output, std::uint32_t capacity) {
    if (!bound()) return false;
    result = DvdStatus::invalidArgument;
    if (!output || !capacity) return false;
    output[0] = 0;
    if (!disc.directoryValid(currentDirectory)) { result = DvdStatus::ioError; return false; }
    const auto bytes = std::min<std::size_t>(std::strlen(currentPath.data()), capacity - 1);
    std::memcpy(output, currentPath.data(), bytes); output[bytes] = 0;
    result = DvdStatus::ready; return true;
}
bool DvdProvider::directoryMatches(const DirectorySlot& slot, const DVDDir* info) const {
    return slot.info == info && info && disc.directoryValid(slot.directory) &&
           info->entryNum == slot.directory.entryIndex() && info->location == slot.directory.location() &&
           info->next == slot.directory.next();
}
bool DvdProvider::openDirectory(const char* path, DVDDir* info) {
    if (!bound()) return false;
    std::array<char, 1024> resolved{};
    if (!absolutePath(path, resolved, true)) return false;
    std::uint32_t index = 0;
    if (!found(disc.findEntry(resolved.data(), index))) return false;
    return openDirectoryEntry(index, info);
}
bool DvdProvider::openDirectoryEntry(std::int32_t index, DVDDir* info) {
    if (!bound()) return false;
    result = DvdStatus::invalidArgument;
    if (index < 0 || !info) return false;
    for (const auto& slot : directories) if (slot.info == info) return false;
    DiscDirectory directory;
    if (!found(disc.openDirectory(index, directory))) return false;
    auto slot = std::find_if(directories.begin(), directories.end(), [](const auto& value) { return !value.info; });
    if (slot == directories.end()) { result = DvdStatus::exhausted; return false; }
    slot->info = info; slot->directory = directory; slot->entry = {};
    *info = {directory.entryIndex(), directory.location(), directory.next()};
    result = DvdStatus::ready; return true;
}
bool DvdProvider::readDirectory(DVDDir* info, DVDDirEntry* output) {
    if (!bound()) return false;
    result = DvdStatus::invalidArgument;
    if (!info || !output) return false;
    *output = {};
    for (auto& slot : directories) if (slot.info == info) {
        if (!directoryMatches(slot, info)) { result = DvdStatus::ioError; return false; }
        if (!found(disc.readDirectory(slot.directory, slot.entry))) return false;
        info->location = slot.directory.location();
        *output = {slot.entry.index, slot.entry.directory, slot.entry.name.data()};
        return true;
    }
    return false;
}
bool DvdProvider::rewindDirectory(DVDDir* info) {
    if (!bound()) return false;
    result = DvdStatus::invalidArgument;
    for (auto& slot : directories) if (directoryMatches(slot, info)) {
        if (!disc.rewindDirectory(slot.directory)) { result = DvdStatus::ioError; return false; }
        slot.entry = {}; info->location = slot.directory.location();
        result = DvdStatus::ready; return true;
    }
    return false;
}
bool DvdProvider::closeDirectory(DVDDir* info) {
    if (!bound()) return false;
    result = DvdStatus::invalidArgument;
    if (!info) return false;
    for (auto& slot : directories) if (slot.info == info) {
        slot = {}; *info = {}; result = DvdStatus::ready; return true;
    }
    return false;
}
bool DvdProvider::diskID(DVDDiskID* output) {
    static_assert(sizeof(DVDDiskID)==32);
    if(!bound())return false;
    DVDDiskID actual{};
    if(!disc.read(0,&actual,sizeof(actual))){result=DvdStatus::ioError;return false;}
    if(output)*output=actual;
    result=DvdStatus::ready;return true;
}
}
extern "C" s32 DVDGetDriveStatus(){ DvdCall call;return driveStatus.load(std::memory_order_acquire);}
extern "C" BOOL DVDCheckDisk(){ DvdCall call;return DVDGetDriveStatus()==DVD_STATE_END;}
extern "C" BOOL DVDLowReadDiskID(DVDDiskID* output,DVDLowCallback callback) { DvdCall call;
    if(!currentProvider() || !currentProvider()->diskID(output))return false;
    if(callback)callback(0); // Same successful completion code as pinned Aurora.
    return true;
}
extern "C" s32 DVDConvertPathToEntrynum(const char* path) { DvdCall call;
    return currentProvider()?currentProvider()->pathEntry(path):-1;
}
extern "C" BOOL DVDOpen(const char* path, DVDFileInfo* info) { DvdCall call;
    return currentProvider() && currentProvider()->open(path, info);
}
extern "C" BOOL DVDChangeDir(const char* path) { DvdCall call;
    return currentProvider() && currentProvider()->changeDirectory(path);
}
extern "C" BOOL DVDGetCurrentDir(char* output, u32 capacity) { DvdCall call;
    return currentProvider() && currentProvider()->currentDirectoryPath(output, capacity);
}
extern "C" BOOL DVDFastOpenDir(s32 entry, DVDDir* info) { DvdCall call;
    return currentProvider() && entry >= 0 && std::uint64_t(entry) <= INT32_MAX &&
           currentProvider()->openDirectoryEntry(static_cast<std::int32_t>(entry), info);
}
extern "C" int DVDOpenDir(const char* path, DVDDir* info) { DvdCall call;
    return currentProvider() && currentProvider()->openDirectory(path, info);
}
extern "C" int DVDReadDir(DVDDir* info, DVDDirEntry* output) { DvdCall call;
    return currentProvider() && currentProvider()->readDirectory(info, output);
}
extern "C" int DVDCloseDir(DVDDir* info) { DvdCall call;
    return currentProvider() && currentProvider()->closeDirectory(info);
}
extern "C" void DVDRewindDir(DVDDir* info) { DvdCall call;
    if (currentProvider()) currentProvider()->rewindDirectory(info);
}
extern "C" BOOL DVDFastOpen(s32 entry,DVDFileInfo* info) { DvdCall call;
    if(!currentProvider() || entry<0 || std::uint64_t(entry)>INT32_MAX)return false;
    return currentProvider()->openEntry(static_cast<std::int32_t>(entry),info);
}
extern "C" s32 DVDGetFileInfoStatus(const DVDFileInfo* info) { DvdCall call;
    return currentProvider()?currentProvider()->fileStatus(info):DVD_STATE_FATAL_ERROR;
}
extern "C" s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { DvdCall call;
    return currentProvider()?currentProvider()->commandStatus(block):DVD_STATE_FATAL_ERROR;
}
extern "C" s32 DVDCancel(volatile DVDCommandBlock* block){ DvdCall call;
    // This provider finishes reads before returning/invoking callbacks. A
    // registered completed command has nothing in flight to cancel. Invalid
    // or another thread's records must not be reported as cancelled.
    if(!currentProvider()||currentProvider()->commandStatus(const_cast<const DVDCommandBlock*>(block))!=DVD_STATE_END)return DVD_RESULT_FATAL_ERROR;
    return DVD_RESULT_GOOD;
}
extern "C" BOOL DVDClose(DVDFileInfo* info) { DvdCall call;
    return currentProvider() && currentProvider()->close(info);
}
extern "C" s32 DVDReadPrio(DVDFileInfo* info, void* output, s32 length, s32 offset, s32) { DvdCall call;
    if (!currentProvider() || length < 0 || offset < 0 ||
        std::uint64_t(length) > INT32_MAX || std::uint64_t(offset) > INT32_MAX) return DVD_RESULT_FATAL_ERROR;
    return currentProvider()->read(info, output, static_cast<std::int32_t>(length), static_cast<std::int32_t>(offset));
}
extern "C" BOOL DVDReadAsyncPrio(DVDFileInfo* info,void* output,s32 length,s32 offset,DVDCallback callback,s32 priority) { DvdCall call;
    // Finish registry/state updates before invoking user code. The callback may
    // read another file or close this record; nothing touches info afterward.
    const auto count=DVDReadPrio(info,output,length,offset,priority);
    if(count<0)return false;
    info->callback=callback;
    if(callback)callback(count,info);
    return true;
}
extern "C" BOOL DVDConvertEntrynumToPath(s32 entry, char* output, u32 capacity) { DvdCall call;
    if (!currentProvider() || entry < 0 || std::uint64_t(entry) > INT32_MAX) return false;
    return currentProvider()->entryPath(static_cast<std::int32_t>(entry), output, capacity);
}

namespace dusk::native {
bool initializeOriginalDisc(){
    // Retained through process exit: no FILE/FST destruction can race a worker.
    auto* disc=new Disc;
    if(disc->open(gameImagePath)!=DiscStatus::ready){delete disc;return false;}
    auto* provider=new DvdProvider(*disc);
    if(!provider->bound()||!provider->shareWithGameWorkers()){delete provider;delete disc;return false;}
    return true;
}
}
extern "C" void DVDInit(){DvdCall call;if(!currentProvider()||!currentProvider()->bound())OSPanic(__FILE__,__LINE__,"Original DVD startup requires validated native disc service");}
