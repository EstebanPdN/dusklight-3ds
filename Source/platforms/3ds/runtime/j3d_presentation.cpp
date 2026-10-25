#include "dusk/interp/frame_interpolation.h"
#include "dusk/interp/vertex.h"
#include "dusk/interp/material.h"
class JPABaseEmitter;
// Native presentation currently renders each 30 Hz simulation pose directly.
// These optional desktop interpolation hooks retain no history and never replace
// current animation, deformation or material data. Actual J3D calculations run.
namespace dusk::interp {
bool is_enabled(){return false;}
bool is_presentation_active(){return false;}
bool lookup_replacement(const void*,Mtx){return false;}
bool lookup_concat_replacement(const void*,const void*,Mtx){return false;}
void record_final_mtx(Mtx){}
void record_final_mtx(Mtx,const void*){}
void forget_mtx(const void*){}
namespace vertex {
void capture(J3DVertexBuffer*,const J3DDeformData*){}
void reset(const J3DVertexBuffer*){}
void invalidate(const J3DDeformData*){}
void* positions(const J3DVertexBuffer*,void* current){return current;}
void* normals(const J3DVertexBuffer*,void* current){return current;}
}
namespace material {
void record_model(J3DModel*){}
// There is no native presentation history to rebase or erase. The environment
// has already transformed each original light for the current view; recording
// it here must not transform that current state a second time.
void record_light_view(J3DMaterial*){}
void reset(const J3DAnmBase*){}
void reset(const J3DFrameCtrl*){}
}
namespace particle {
void reset(const JPABaseEmitter*){}
}
}
