#include "../runtime/archive_bootstrap.hpp"
#include "../runtime/bootstrap.hpp"
#include "../runtime/clock.hpp"
#include "../runtime/disc.hpp"
#include "../runtime/dvd_provider.hpp"
#include "JSystem/JKernel/JKRMemArchive.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JKernel/JKRDvdFile.h"
#include "JSystem/JKernel/JKRFileCache.h"
#include "JSystem/JKernel/JKRFileFinder.h"
#include "dusk/version.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace dusk::native {
void logText(const char* text) { std::fputs(text, stderr); }
[[noreturn]] void fatal(const char* text) { logText(text); std::abort(); }
void waitForRetrace() { std::atomic_thread_fence(std::memory_order_seq_cst); }
void invalidateCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
void flushCache(void*, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); }
std::uint64_t counterTicks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() * (nativeTickFrequency / 1000);
}
std::uint64_t wallMilliseconds() { return epoch1900To2000Ms; }
}
using namespace dusk::native;
namespace {
constexpr ArchiveLimits limits{{1024 * 1024, 4096, 8192, 255}, 1024 * 1024};
void word(unsigned char* p, std::uint32_t value) { for (unsigned i = 0; i < 4; ++i) p[i] = value >> (24 - 8 * i); }
void half(unsigned char* p, std::uint16_t value) { p[0] = value >> 8; p[1] = value; }
std::uint16_t hash(const char* s) { std::uint16_t h = 0; while (*s) h = h * 3 + *s++; return h; }
std::array<unsigned char, 256> rarc() {
    std::array<unsigned char, 256> data{};
    auto w = [&](unsigned at, unsigned value) { word(data.data() + at, value); };
    std::memcpy(data.data(), "RARC", 4);
    w(4, 256); w(8, 32); w(12, 160); w(16, 64); w(20, 64);
    w(32, 1); w(36, 32); w(40, 3); w(44, 48); w(48, 32); w(52, 128);
    std::memcpy(data.data() + 64, "ROOT", 4); w(68, 0);
    half(data.data() + 72, hash("root")); half(data.data() + 74, 3); w(76, 0);
    half(data.data() + 80, 0xffff); half(data.data() + 82, hash("."));
    w(84, 0x02000005); w(88, 0);
    half(data.data() + 100, 0xffff); half(data.data() + 102, hash(".."));
    w(104, 0x02000007); w(108, UINT32_MAX);
    half(data.data() + 120, 7); half(data.data() + 122, hash("screen.blo"));
    w(124, 0x1100000a); w(128, 0); w(132, 20);
    const char names[] = "root\0.\0..\0screen.blo\0";
    std::memcpy(data.data() + 160, names, sizeof(names));
    std::memcpy(data.data() + 192, "SCRNblo1", 8); w(200, 20); w(204, 0);
    return data;
}
std::vector<unsigned char> literalYaz0(const std::array<unsigned char, 256>& input) {
    std::vector<unsigned char> data(16);
    std::memcpy(data.data(), "Yaz0", 4); word(data.data() + 4, input.size());
    for (std::size_t i = 0; i < input.size(); i += 8) {
        data.push_back(0xff);
        data.insert(data.end(), input.begin() + i, input.begin() + i + 8);
    }
    return data;
}
void testArchive() {
    const auto before = kernelStats();
    const auto volumes = JKRFileLoader::getVolumeList().getNumLinks();
    auto* currentVolume = JKRFileLoader::getCurrentVolume();
    auto input = rarc(); const auto original = input;
    // Audio BFCA is a borrowed RARC inside a larger BAA allocation. Derive
    // the remaining bound from the live allocation and keep its owner intact.
    auto* containing=static_cast<unsigned char*>(JKRHeap::alloc(320,32,JKRHeap::getSystemHeap()));
    assert(containing);std::memset(containing,0,320);
    std::memcpy(containing+64,input.data(),input.size());
    auto* embedded=JKRArchive::mount(containing+64,JKRHeap::getSystemHeap(),JKRArchive::MOUNT_DIRECTION_TAIL);
    assert(embedded&&embedded->isMounted()&&embedded->countResource()==1);
    auto* embeddedResource=embedded->getResource(u16(7));
    assert(embeddedResource&&!std::memcmp(embeddedResource,"SCRNblo1",8));
    embedded->unmount();
    assert(!std::memcmp(containing+64,"RARC",4));
    assert(JKRArchive::mount(containing+300,JKRHeap::getSystemHeap(),JKRArchive::MOUNT_DIRECTION_TAIL)==nullptr);
    JKRHeap::free(containing,JKRHeap::getSystemHeap());
    ArchiveBootstrap archive; ResourceSpan output;
    assert(archive.findBasename("screen.blo", output) == ArchiveStatus::notReady);
    assert(archive.openCopy({input.data(), input.size()}, limits, ArchiveOverlayPolicy::requested) == ArchiveStatus::overlaysUnsupported);
    assert(archive.openCopy({input.data(), input.size()}, limits) == ArchiveStatus::ready);
    assert(input == original && archive.archiveBytes() == 256);
    input.fill(0xff); // owned copy must outlive and be independent of the source
    assert(archive.findBasename("SCREEN.BLO", output) == ArchiveStatus::ready);
    assert(output.size == 20 && !std::memcmp(output.data, "SCRNblo1", 8));
    auto* jkr = archive.archiveForLoading();
    assert(jkr && jkr->mEntryNum == -1 && jkr->getResource("/screen.blo") == output.data);
    assert(jkr->getResource(u16(7)) == output.data);
    std::array<unsigned char, 32> copied; copied.fill(0xcd);
    assert(jkr->readResource(copied.data() + 4, 10, "/screen.blo") == 10);
    assert(!std::memcmp(copied.data() + 4, output.data, 10) && copied[3] == 0xcd && copied[14] == 0xcd);
    assert(archive.findBasename("missing.blo", output) == ArchiveStatus::notFound && !output.data);
    assert(archive.findBasename("..", output) == ArchiveStatus::unsupported);
    const std::string tooLong(256, 'a');
    assert(archive.findBasename(tooLong.c_str(), output) == ArchiveStatus::unsupported);
    std::thread wrong([&] {
        ResourceSpan none;
        assert(archive.findBasename("screen.blo", none) == ArchiveStatus::wrongThread);
        assert(archive.close() == ArchiveStatus::wrongThread && !archive.archiveForLoading());
    }); wrong.join();
    assert(archive.openCopy({original.data(), original.size()}, limits) == ArchiveStatus::notReady);
    assert(archive.close() == ArchiveStatus::ready);
    assert(kernelStats().free == before.free && kernelStats().healthy);
    assert(JKRFileLoader::getVolumeList().getNumLinks() == volumes);
    assert(JKRFileLoader::getCurrentVolume() == currentVolume);
    auto encoded = literalYaz0(original);
    assert(archive.openCopy({encoded.data(), encoded.size()}, limits) == ArchiveStatus::ready);
    assert(archive.findBasename("screen.blo", output) == ArchiveStatus::ready);
    assert(archive.close() == ArchiveStatus::ready);
    for (std::size_t length = 0; length < original.size(); ++length) {
        assert(archive.openCopy({original.data(), length}, limits) == ArchiveStatus::malformed);
        assert(!archive.archiveForLoading());
    }
    auto bad = original; bad[122] ^= 1;
    assert(archive.openCopy({bad.data(), bad.size()}, limits) == ArchiveStatus::unsupported);
    bad = original; bad[124] |= 4;
    assert(archive.openCopy({bad.data(), bad.size()}, limits) == ArchiveStatus::unsupported);
    bad = original; word(bad.data() + 88, UINT32_MAX);
    assert(archive.openCopy({bad.data(), bad.size()}, limits) == ArchiveStatus::malformed);
    bad = original; word(bad.data() + 108, 1);
    assert(archive.openCopy({bad.data(), bad.size()}, limits) == ArchiveStatus::malformed);
    bad = original; bad[165] = 'x'; half(bad.data() + 82, hash("x"));
    assert(archive.openCopy({bad.data(), bad.size()}, limits) == ArchiveStatus::malformed); // root cycle
    const ArchiveLimits tiny{{255, 1, 3, 255}, 256};
    assert(archive.openCopy({original.data(), original.size()}, tiny) == ArchiveStatus::limitExceeded);
    // Force the ORIGINAL sidecar allocation to fail; the 3DS guard must reject
    // mounting before memset, leave input untouched and restore the volume list.
    auto* heap = JKRHeap::getSystemHeap();
    const auto allBytes = heap->getMaxAllocatableSize(8);
    void* all = heap->alloc(allBytes, 8); assert(all);
    assert(archive.openCopy({original.data(), original.size()}, limits) == ArchiveStatus::noMemory);
    assert(!archive.archiveForLoading()); heap->free(all);
    assert(archive.close() == ArchiveStatus::ready);
    assert(kernelStats().free == before.free && kernelStats().healthy);
    assert(JKRFileLoader::getVolumeList().getNumLinks() == volumes);
    assert(JKRFileLoader::getCurrentVolume() == currentVolume);
    // Every mutation is either checked-refused or remains a safely usable mount.
    for (std::size_t byte = 0; byte < original.size(); ++byte) {
        bad = original; bad[byte] ^= 0xff;
        if (archive.openCopy({bad.data(), bad.size()}, limits) == ArchiveStatus::ready) {
            archive.findBasename("screen.blo", output);
        }
        assert(archive.close() == ArchiveStatus::ready);
    }
    assert(kernelStats().free == before.free && kernelStats().healthy);
    std::puts("PASS archive: original JKR mount/name/path/id/copy, owned bytes, Yaz0, hostile metadata, allocation failure, worker affinity, no leaked JKR blocks");
}
void testDisc(const char* outputDirectory) {
    const auto arc = rarc();
    std::array<unsigned char, 0x1100> image{};
    auto w = [&](unsigned at, unsigned value) { word(image.data() + at, value); };
    std::memcpy(image.data(), "GZ2E01", 6);
    w(0x1c, 0xc2339f3d); w(0x420, 0x500); w(0x424, 0x800); w(0x428, 128);
    w(0x500, 0x100); w(0x590, 16); w(0x800, 0x01000000); w(0x808, 6);
    w(0x80c, 0); w(0x810, 0x1000); w(0x814, arc.size());
    w(0x818, 0x0100000c); w(0x81c, 0); w(0x820, 5);
    w(0x824, 0x01000013); w(0x828, 2); w(0x82c, 5);
    w(0x830, 0); w(0x834, 0x1000); w(0x838, arc.size());
    w(0x83c, 0x0100001a); w(0x840, 0); w(0x844, 6);
    const char fstNames[] = "archive.arc\0folder\0nested\0empty\0";
    std::memcpy(image.data() + 0x848, fstNames, sizeof(fstNames));
    std::memcpy(image.data() + 0x1000, arc.data(), arc.size());
    const auto path = std::string(outputDirectory) + "/synthetic-archive.iso";
    auto* f = std::fopen(path.c_str(), "wb"); assert(f);
    assert(std::fwrite(image.data(), 1, image.size(), f) == image.size()); assert(!std::fclose(f));
    Disc disc; assert(disc.open(path.c_str()) == DiscStatus::ready);
    assert(DVDGetDriveStatus()==DVD_STATE_NO_DISK&&!DVDCheckDisk());
    {
        Disc unopened;DvdProvider rejected(unopened);
        assert(!rejected.bound()&&!DVDCheckDisk());
    }
    DVDFileInfo info{};
    assert(!DVDOpen("/archive.arc", &info));
    assert(DVDConvertPathToEntrynum("/archive.arc")==-1);
    DVDDiskID id{};
    assert(!DVDLowReadDiskID(&id,nullptr));
    {
        DvdProvider provider(disc); assert(provider.bound());
        assert(DVDCheckDisk()&&DVDGetDriveStatus()==DVD_STATE_END);
        DvdProvider nested(disc); assert(!nested.bound());
        assert(DVDLowReadDiskID(&id,nullptr) && !std::memcmp(id.gameName,"GZ2E",4) && !std::memcmp(id.company,"01",2));
        assert(DVDLowReadDiskID(nullptr,[](u32 status){assert(status==0);}));
        dusk::version::init();
        assert(dusk::version::isGcn() && !dusk::version::isWii() && dusk::version::isRegionUsa());
        assert(dusk::version::getGameVersion()==dusk::version::GameVersion::GcnUsa);
        assert(DVDConvertPathToEntrynum("/ARCHIVE.ARC")==1);
        for(unsigned i=0;i<32;++i)assert(DVDConvertPathToEntrynum("archive.arc")==1);
        assert(DVDConvertPathToEntrynum("/absent.arc")==-1);
        assert(DVDConvertPathToEntrynum("../archive.arc")==-1);
        assert(DVDConvertPathToEntrynum(nullptr)==-1);
        assert(DVDConvertPathToEntrynum("/")==0 && DVDConvertPathToEntrynum("/folder")==2);
        char current[32]; assert(DVDGetCurrentDir(current,sizeof(current)) && !std::strcmp(current,"/"));
        DVDDir directory{}; DVDDirEntry entry{};
        assert(DVDOpenDir("/", &directory) && directory.entryNum==0 && directory.next==6);
        assert(!DVDOpenDir("/", &directory));
        for (auto index : {1u,2u,5u}) {
            assert(DVDReadDir(&directory,&entry) && entry.entryNum==index);
            assert((entry.isDir!=0)==(index!=1) && entry.name && entry.name[0]);
        }
        assert(!DVDReadDir(&directory,&entry) && !entry.name);
        DVDRewindDir(&directory);
        assert(DVDReadDir(&directory,&entry) && entry.entryNum==1 && !std::strcmp(entry.name,"archive.arc"));
        DVDDir copiedDirectory=directory;
        assert(!DVDReadDir(&copiedDirectory,&entry) && !DVDCloseDir(&copiedDirectory));
        const auto originalLocation=directory.location;
        directory.location=UINT32_MAX;
        assert(!DVDReadDir(&directory,&entry));
        directory.location=originalLocation;
        assert(DVDReadDir(&directory,&entry) && entry.entryNum==2);
        assert(DVDCloseDir(&directory) && !DVDCloseDir(&directory));
        assert(DVDFastOpenDir(5,&directory) && !DVDReadDir(&directory,&entry) && DVDCloseDir(&directory));
        assert(!DVDFastOpenDir(1,&directory) && !DVDFastOpenDir(-1,&directory));
        assert(!DVDOpenDir("//", &directory) && !DVDOpenDir("/folder//",&directory));
        assert(DVDChangeDir("/folder/"));
        assert(DVDGetCurrentDir(current,sizeof(current)) && !std::strcmp(current,"/folder"));
        assert(!DVDChangeDir("/archive.arc") && !DVDChangeDir("../"));
        assert(DVDOpenDir("nested/",&directory) && directory.entryNum==3);
        assert(DVDReadDir(&directory,&entry) && entry.entryNum==4 && !entry.isDir);
        assert(DVDCloseDir(&directory));
        assert(DVDConvertPathToEntrynum("nested/archive.arc")==4);
        assert(DVDOpen("nested/archive.arc",&info));
        char nestedPath[64]; assert(DVDConvertEntrynumToPath(4,nestedPath,sizeof(nestedPath)) && !std::strcmp(nestedPath,"/folder/nested/archive.arc"));
        assert(DVDClose(&info));
        assert(DVDChangeDir("/"));
        const auto beforeCache=kernelStats().free;
        {
            auto* cache=JKRFileCache::mount("/folder", JKRHeap::getSystemHeap(), "testdvd"); assert(cache);
            assert(cache->countFile("/")==1 && cache->countFile("/nested")==1);
            auto* finder=cache->getFirstFile("/"); assert(finder && finder->isAvailable() && finder->isDirectory());
            assert(!finder->findNextFile()); JKR_DELETE(finder);
            void* resource=cache->getResource(0,"archive.arc");
            assert(resource && cache->getResSize(resource)==256 && !std::memcmp(resource,arc.data(),arc.size()));
            assert(cache->removeResource(resource));
            assert(cache->becomeCurrent("/nested") && DVDConvertPathToEntrynum("archive.arc")==4);
            cache->unmount();
        }
        assert(kernelStats().free==beforeCache && kernelStats().healthy);
        assert(DVDChangeDir("/"));
        std::array<DVDDir,64> heldDirectories{};
        for(auto& handle:heldDirectories)assert(DVDOpenDir("/",&handle));
        assert(!DVDOpenDir("/",&directory) && provider.lastStatus()==DvdStatus::exhausted);
        for(auto& handle:heldDirectories)assert(DVDCloseDir(&handle));
        assert(!DVDFastOpen(-1,&info) && !DVDFastOpen(0,&info) && !DVDFastOpen(99,&info));
        {
            JKRDvdFile originalFile(DVDConvertPathToEntrynum("/archive.arc"));
            assert(originalFile.isAvailable() && originalFile.getFileSize()==256);
            assert(originalFile.getFileID()==0x1000);
            alignas(32) unsigned char bytes[288];std::memset(bytes,0xcd,sizeof(bytes));
            assert(originalFile.readData(bytes,sizeof(bytes),0)==256);
            assert(!std::memcmp(bytes,arc.data(),arc.size()) && bytes[256]==0xcd);
            assert(originalFile.getStatus()==DVD_STATE_END);
        }
        assert(DVDOpen("archive.arc", &info) && info.length == 256);
        assert(!DVDOpen("archive.arc", &info));
        assert(DVDGetFileInfoStatus(&info)==DVD_STATE_END);
        bool completed=false;
        info.cb.userData=&completed;
        std::array<unsigned char,32> asyncBytes{};
        assert(DVDReadAsyncPrio(&info,asyncBytes.data(),asyncBytes.size(),0,
            [](s32 count,DVDFileInfo* file){assert(count==32 && file->cb.state==DVD_STATE_END);*static_cast<bool*>(file->cb.userData)=true;},2));
        assert(completed && !std::memcmp(asyncBytes.data(),arc.data(),asyncBytes.size()));
        DVDFileInfo closeInCallback{};
        assert(DVDFastOpen(1,&closeInCallback));
        assert(DVDReadAsyncPrio(&closeInCallback,asyncBytes.data(),asyncBytes.size(),0,
            [](s32 count,DVDFileInfo* file){assert(count==32 && DVDConvertPathToEntrynum("archive.arc")==1);assert(DVDClose(file));},2));
        assert(DVDGetFileInfoStatus(&closeInCallback)==DVD_STATE_FATAL_ERROR);
        completed=false;
        assert(!DVDReadAsyncPrio(&info,asyncBytes.data(),-1,0,
            [](s32,DVDFileInfo* file){*static_cast<bool*>(file->cb.userData)=true;},2));
        assert(!completed);
        char name[32]; assert(DVDConvertEntrynumToPath(1, name, sizeof(name)) && !std::strcmp(name, "/archive.arc"));
        assert(!DVDConvertEntrynumToPath(99, name, sizeof(name)) && name[0] == '\0');
        assert(!DVDConvertEntrynumToPath(1, name, 2));
        std::array<unsigned char, 300> data; data.fill(0xcd);
        assert(DVDReadPrio(&info, data.data(), data.size(), 0, 2) == 256);
        assert(!std::memcmp(data.data(), arc.data(), arc.size()) && data[256] == 0xcd);
        assert(info.cb.transferredSize == 256 && info.cb.state == DVD_STATE_END);
        assert(DVDReadPrio(&info, data.data(), -1, 0, 2) == DVD_RESULT_FATAL_ERROR);
        assert(DVDReadPrio(&info, data.data(), 1, 257, 2) == DVD_RESULT_FATAL_ERROR);
        DVDFileInfo copied = info; assert(DVDReadPrio(&copied, data.data(), 4, 0, 2) == DVD_RESULT_FATAL_ERROR);
        std::thread wrong([&] { assert(DVDCheckDisk()&&DVDGetDriveStatus()==DVD_STATE_END); assert(!DVDOpen("archive.arc", &copied)); assert(DVDConvertPathToEntrynum("/archive.arc")==-1); assert(DVDReadPrio(&info, data.data(), 4, 0, 2) == DVD_RESULT_FATAL_ERROR); });
        wrong.join();
        assert(DVDOpenDir("/",&directory));
        std::array<DVDFileInfo,DvdProvider::fileCapacity> retained{};
        // One original file remains live. Fill the other slots, check the
        // exact bound, then verify released slots can be reused.
        for(unsigned n=0;n<retained.size()-1;++n)assert(DVDFastOpen(1,&retained[n]));
        assert(!DVDFastOpen(1,&retained.back())&&provider.lastStatus()==DvdStatus::exhausted);
        for(unsigned n=0;n<retained.size()-1;++n)assert(DVDClose(&retained[n]));
        assert(DVDFastOpen(1,&retained.back())&&DVDClose(&retained.back()));
        assert(disc.open(path.c_str()) == DiscStatus::ready);
        assert(!DVDReadDir(&directory,&entry) && DVDCloseDir(&directory));
        assert(!DVDGetCurrentDir(current,sizeof(current)) && !current[0]);
        assert(DVDConvertPathToEntrynum("archive.arc")==-1 && DVDChangeDir("/"));
        assert(DVDGetFileInfoStatus(&info)==DVD_STATE_FATAL_ERROR && DVDGetCommandBlockStatus(&info.cb)==DVD_STATE_FATAL_ERROR);
        assert(DVDReadPrio(&info, data.data(), 4, 0, 2) == DVD_RESULT_FATAL_ERROR);
        assert(!DVDConvertEntrynumToPath(1, name, sizeof(name)));
        assert(DVDClose(&info) && !DVDClose(&info));
        ArchiveBootstrap archive; ResourceSpan resource;
        assert(archive.load(disc, "/archive.arc", limits) == ArchiveStatus::ready);
        assert(archive.findBasename("screen.blo", resource) == ArchiveStatus::ready && resource.size == 20);
        assert(disc.open(nullptr)==DiscStatus::missing);
        std::thread missing([&]{assert(!DVDCheckDisk()&&DVDGetDriveStatus()==DVD_STATE_NO_DISK);});missing.join();
        // Change only this disposable synthetic image. Validation must publish
        // wrong-disc and malformed states rather than leave a stale ready bit.
        f=std::fopen(path.c_str(),"r+b");assert(f);std::fputc('X',f);assert(!std::fclose(f));
        assert(disc.open(path.c_str())==DiscStatus::unsupported);
        std::thread unsupported([&]{assert(!DVDCheckDisk()&&DVDGetDriveStatus()==DVD_STATE_WRONG_DISK);});unsupported.join();
        f=std::fopen(path.c_str(),"wb");assert(f);assert(std::fwrite(image.data(),1,32,f)==32);assert(!std::fclose(f));
        assert(disc.open(path.c_str())==DiscStatus::malformed);
        std::thread malformed([&]{assert(!DVDCheckDisk()&&DVDGetDriveStatus()==DVD_STATE_FATAL_ERROR);});malformed.join();
        f=std::fopen(path.c_str(),"wb");assert(f);assert(std::fwrite(image.data(),1,image.size(),f)==image.size());assert(!std::fclose(f));
        assert(disc.open(path.c_str())==DiscStatus::ready&&DVDCheckDisk());
    }
    assert(!DVDCheckDisk()&&DVDGetDriveStatus()==DVD_STATE_NO_DISK);
    assert(DVDReadPrio(&info, nullptr, 0, 0, 2) == DVD_RESULT_FATAL_ERROR);
    assert(DVDConvertPathToEntrynum("/archive.arc")==-1);
    assert(std::remove(path.c_str()) == 0);
    std::puts("PASS DVD: bounded reads, synchronous counts, stale/foreign/negative rejection, binding, actual ready/missing/wrong/malformed snapshots across threads, directory hierarchy/rewind/current path/capacity, original file cache recursive lookup and archive load");
}
int privateDisc(const char* path) {
    Disc disc;
    if (disc.open(path) != DiscStatus::ready) return 2;
    {
        DvdProvider provider(disc);
        if(!provider.bound())return 5;
        dusk::version::init();
        if(dusk::version::getGameVersion()!=dusk::version::GameVersion::GcnUsa)return 8;
        const char* paths[]{"/res/Object/LogoUs.arc","/res/Layout/Title2D.arc",
                            "/res/Stage/F_SP103/R00_00.arc","/res/Stage/F_SP103/STG_00.arc"};
        for(unsigned i=0;i<4;++i) {
            const auto entry=DVDConvertPathToEntrynum(paths[i]);
            if(entry<0)return 6;
            JKRDvdFile original(entry);
            alignas(32) unsigned char header[32];
            if(!original.isAvailable() || original.readData(header,sizeof(header),0)!=32 ||
               (std::memcmp(header,"RARC",4) && std::memcmp(header,"Yaz0",4)))return 7;
            std::printf("private original DVD file %u: bounded header read complete; bytes=%d\n",i,original.getFileSize());
        }
    }
    const char* archives[]{"/res/Object/LogoUs.arc", "/res/Layout/Title2D.arc"};
    const char* resources[]{"nintendo_376x104.bti", "zelda_press_start.blo"};
    for (unsigned i = 0; i < 2; ++i) {
        ArchiveBootstrap archive; ResourceSpan output;
        const auto status = archive.load(disc, archives[i], limits);
        std::printf("private archive %u: %s; bytes=%zu\n", i, archiveStatusText(status), archive.archiveBytes());
        if (status != ArchiveStatus::ready) return 3;
        const auto found = archive.findBasename(resources[i], output);
        std::printf("private resource %u: %s; bytes=%zu\n", i, archiveStatusText(found), output.size);
        if (found != ArchiveStatus::ready) return 4;
    }
    std::puts("PASS private: original JKRDvdFile entry open/read/callback plus bounded original JKR resource lookup; no payload output");
    return 0;
}
}
int main(int argc, char** argv) {
    alignas(32) static std::array<unsigned char, 2 * 1024 * 1024> memory;
    assert(initializeKernel(memory.data(), memory.size()));
    int result = 0;
    std::thread loadingWorker([&] {
        JKRHeap::setCurrentHeap(JKRHeap::getSystemHeap());
        if (argc == 3 && !std::strcmp(argv[1], "--private-disc")) result = privateDisc(argv[2]);
        else { assert(argc == 2); testArchive(); testDisc(argv[1]); }
        JKRHeap::setCurrentHeap(nullptr);
    });
    loadingWorker.join();
    assert(kernelStats().healthy);
    return result;
}
