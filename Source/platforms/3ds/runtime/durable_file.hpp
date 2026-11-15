#pragma once
#include <cstddef>
#include <cstdint>
// SD writes that survive the console being switched off right after them.
// Each call opens the file through the FS service, writes with FS_WRITE_FLUSH
// and closes it: no C library buffer is left in RAM. Paths are SD-relative,
// e.g. "/3ds/DuskLight/gpu-journal.txt". Any thread; no GSP or GPU call.
namespace dusk::native {
bool durableReplace(const char* path,const void* data,std::size_t bytes);
bool durableAppend(const char* path,const void* data,std::size_t bytes);
bool durableReplaceText(const char* path,const char* text);
bool durableAppendText(const char* path,const char* text);
// Whole small file (up to capacity-1 bytes) as text; false when missing.
bool durableReadText(const char* path,char* out,std::size_t capacity);
// An open file kept for repeated appends (the native log). Each append is
// flushed by the FS service before it returns.
struct DurableLog {std::uint32_t handle{};std::uint64_t offset{};bool open{};};
bool durableLogOpen(DurableLog& log,const char* path);
bool durableLogAppend(DurableLog& log,const void* data,std::size_t bytes);
// A small fixed-size record kept open and rewritten in place: one FS write,
// flushed, no size change. The cheapest durable state on the SD card (a
// replace of a file costs an open, a truncate, a write and a close).
struct DurableSlot {std::uint32_t handle{},bytes{};bool open{};};
bool durableSlotOpen(DurableSlot& slot,const char* path,std::uint32_t bytes);
// The text followed by newlines up to the slot size (longer text is cut).
bool durableSlotWrite(DurableSlot& slot,const char* text);
// Keep earlier copies: path -> path-1 -> path-2 ... up to `keep` copies.
void durableRotate(const char* stem,const char* extension,unsigned keep);
}
