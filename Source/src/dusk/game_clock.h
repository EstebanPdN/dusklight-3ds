#pragma once

#include <cstdint>

namespace dusk::game_clock {

// Amount of time that a simulation tick advances
constexpr float kSimPeriod = 1.0f / 30.0f;
constexpr float kUiMaximumDt = 0.05f;
constexpr float kUiInitialDt = 1.0f / 60.0f;

float original_frames();

struct FrameTiming {
    // Amount of time elapsed in seconds since the last advance
    float dt = 0;
    // Whether interpolation is active
    bool interpolating = false;
    // Run simulation and presentation separately (for interpolation or time scaling)
    bool separatePresentation = false;
    // Number of simulation ticks to run
    int numSimTicks = 0;
    // Changes whenever presentation history must be discarded and re-anchored.
    uint64_t presentationEpoch = 0;
};
extern FrameTiming g_frameTiming;

void initialize();
void reset();
const FrameTiming& advance();
void begin_sim_tick();
void commit_sim_tick();
float sample_interpolation_step();

bool is_sim_frame();
bool is_presentation_frame();

double sample_time();
float consume_interval(double& lastSample);

// Sets the effective simulation rate through the game clock time scale.
void set_sim_rate(float hz);
float get_sim_rate();

}  // namespace dusk::game_clock
