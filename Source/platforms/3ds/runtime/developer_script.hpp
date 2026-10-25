#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <span>
namespace dusk::native {
namespace gx {struct TargetExtent;}
struct FramebufferDepthQuery;
// Private development automation, read once from the SD card like
// boot-stage.txt. Absent files keep ordinary input, presentation and exit.
//   input-script.txt   FIRST LAST BUTTONS [CIRCLE_X CIRCLE_Y]  (frames inclusive)
//   capture-frames.txt frame numbers whose main framebuffer is written to captures/
//   dump-frames.txt    frame numbers whose game arena is written to dumps/
//   exit-frame.txt     frame number after which the game loop ends cleanly
struct ScriptedInput {std::uint64_t first{},last{};std::uint32_t keys{};bool circle{};std::int16_t x{},y{};};
bool parseInputScriptLine(std::string_view line,ScriptedInput& out);
void loadDeveloperScripts();
// calibrate.txt: report guest ticks for loops of known instruction counts.
void runDeveloperCalibration();
void setDeveloperFrame(std::uint64_t frame);
void setDeveloperDumpRegion(const void* data,std::size_t bytes);
void applyScriptedInput(std::uint32_t& keys,std::int16_t& circleX,std::int16_t& circleY);
bool developerCaptureRequested(std::uint64_t frame);
bool developerDumpRequested(std::uint64_t frame);
// Optional private depth-order evidence; absent frame list performs no readback.
bool developerDepthAuditRequested(std::uint64_t frame);
void captureDeveloperDepthBoundary(bool afterRender);
// depth-retained-frames.txt is a separate explicit opt-in SUBSET of the audit
// frames. No file means no retained copies, allocations or checksum work.
enum class RetainedDepthStage {afterColor,afterFrameEnd,beforeConsole,afterConsoleWrites,afterConsoleFlush,afterReporting};
void observeDeveloperRetainedDepth(RetainedDepthStage);
void finishDeveloperRetainedDepth(bool afterNextBegin);
bool developerRetainedDepthDiagnosticFrame(std::uint64_t frame);
void captureDeveloperDepthQueries(const gx::TargetExtent&,std::span<const std::byte>,
                                 std::span<const FramebufferDepthQuery>);
bool developerExitRequested(std::uint64_t frame);
bool dumpDeveloperMemory(std::uint64_t frame);
// Writes the completed main framebuffer as an RGBA8 dump for host conversion.
bool captureMainFramebuffer(std::uint64_t frame);
}
