#include "frame_telemetry.hpp"
#include "clock.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace dusk::native;

int main() {
    FrameTelemetryRow row{};
    row.frame = UINT32_MAX;
    std::memcpy(row.stage, "12345678", 8);
    row.room = -128;
    row.phase = 255;
    row.loop = 1;
    row.paint = 199;
    row.pace = 27000;
    row.gpuLists = row.gpuCopies = UINT16_MAX;
    row.gpuIdle = 5000;
    row.gpuJobs = 190;
    const auto file = std::tmpfile();
    assert(file && writeFrameTelemetryRows(file, std::span(&row, 1)));
    std::rewind(file);
    std::array<char, 400> header{}, line{};
    assert(std::fgets(header.data(), header.size(), file));
    assert(std::fgets(line.data(), line.size(), file));
    assert(!std::strcmp(line.data(),
        "4294967295,12345678,-128,0,255,0,0.01,0.00,1.99,0.00,0.00,270.00,0.00,0.00,"
        "0.00,0.00,0.00,0.00,0,0,0,0,0,1310.70,655.35,655.35,50.00,0.00,190\n"));
    assert(std::fgetc(file) == EOF);
    std::fclose(file);

    initializeFrameTelemetry();
    FrameTelemetryInput sample{};
    sample.frame = 11; sample.stage = "Ordon"; sample.room = -1;
    recordFrameTelemetry(sample);
    std::array<char, 640> output{};
    auto length = formatRecentFrameTelemetry(output.data(), output.size(), 1);
    assert(length == std::strlen(output.data()) && length > std::strlen(header.data()));
    assert(std::strncmp(output.data(), header.data(), std::strlen(header.data())) == 0);
    // A short buffer must never expose the prefix of a truncated row.
    assert(formatRecentFrameTelemetry(output.data(), length, 1) == std::strlen(header.data()));
    assert(!std::strcmp(output.data(), header.data()));

    char directory[] = "/tmp/dusklight-zone-frames-XXXXXX";
    assert(mkdtemp(directory));
    auto path = std::filesystem::path(directory) / "frames.csv";
    setFrameTelemetryTag(2, 0);
    sample.frame = 12;
    sample.loopTicks = (nativeTickFrequency + 9) / 10;
    constexpr auto collection = telemetryCaptureOwner | telemetryCaptureWriter | telemetryFullDump;
    flagNextFrameTelemetry(collection);
    recordFrameTelemetry(sample);
    FrameTelemetryRow captured{};
    assert(copyRecentFrameTelemetry(std::span(&captured, 1)) == 1);
    assert(captured.flags == collection && captured.loop == 10000);
    TelemetryStats withCollection{};
    assert(frameTelemetryStats(2, -1, withCollection));
    assert(withCollection.frames == 1 && withCollection.flagged == 0 && withCollection.over50 == 1);
    TelemetryStats clean{};
    assert(!frameTelemetryStats(2, 0, clean, 0, telemetryDiagnosticFrame | collection));
    assert(clean.frames == 0 && clean.flagged == 1);
    assert(queueZoneFrameCsv(path.c_str(), 2));
    assert(!queueZoneFrameCsv(path.c_str(), 2));
    setFrameTelemetryTag(3, 0);
    for (unsigned frame = 13; frame <= 4112; ++frame) {
        sample.frame = frame;
        recordFrameTelemetry(sample);
    }
    std::array<FrameTelemetryRow,32> latest;
    assert(copyRecentFrameTelemetry(latest) == latest.size());
    for (unsigned i = 0; i < latest.size(); ++i) assert(latest[i].frame == 4081 + i);
    assert(writePendingZoneFrameCsv());
    std::ifstream csv(path);
    std::string text;
    assert(std::getline(csv,text) && text.starts_with("frame,"));
    assert(std::getline(csv,text) && text.starts_with("12,Ordon,-1,2,"));
    assert(!std::getline(csv,text));
    assert(queueZoneFrameCsv(directory, 2)); // Publishing over a directory fails.
    assert(!writePendingZoneFrameCsv() && writePendingZoneFrameCsv());
    std::filesystem::remove(std::string(directory) + ".tmp");
    std::filesystem::remove_all(directory);
}
