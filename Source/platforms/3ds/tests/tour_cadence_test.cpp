// Emit a trace for the host reader test, including a cross-step, two-second stall.
#include "tour_cadence.hpp"
#include "frame_telemetry.hpp"
#include "clock.hpp"
using namespace dusk::native;

int main(int argc,char** argv){
    if(argc!=2)return 1;
    FrameTelemetryInput sample{};
    sample.stage="F_SP104";sample.room=1;sample.submitTick=123;
    beginTourCadence(argv[1],1,sample.submitTick);
    for(unsigned n=0;n<1025;++n){
        ++sample.frame;sample.submitTick+=nativeTickFrequency/28;
        recordTourCadence(sample,1,0);
    }
    // Work done after the last picture (including checkpoint I/O) belongs
    // to the next interval even when the step changes halfway through it.
    beginTourCadence(argv[1],2,sample.submitTick+nativeTickFrequency);
    ++sample.frame;sample.submitTick+=2*nativeTickFrequency;
    sample.demoFrame=123;sample.sceneFlags=3;
    recordTourCadence(sample,255,1);
    return endTourCadence()?0:1;
}
