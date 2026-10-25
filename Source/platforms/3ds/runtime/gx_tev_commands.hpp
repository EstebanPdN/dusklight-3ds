#pragma once
#include "gx_state.hpp"
namespace dusk::native::gx {
// Typed TEV setters share the BP bank with original GD/display lists. Return
// false only when the older immediate diagnostic path owns GX. Never allocate.
bool commandTevCount(unsigned count,bool indirect);
bool commandTevDirect(unsigned stage);
bool commandTevOperation(unsigned stage,unsigned operation);
bool commandTevOrder(unsigned stage,unsigned coordinate,unsigned texture,unsigned channel);
bool commandTevInputs(unsigned stage,bool alpha,unsigned a,unsigned b,unsigned c,unsigned d);
bool commandTevOperator(unsigned stage,bool alpha,unsigned operation,unsigned bias,unsigned scale,unsigned clamp,unsigned output);
bool commandTevRegister(unsigned id,int r,int g,int b,int a,bool konst);
bool commandTevKonstSelection(unsigned stage,unsigned selection,bool alpha);
}
