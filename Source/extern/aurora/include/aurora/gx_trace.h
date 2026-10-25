#pragma once
#if DUSK_GX_TRACE
#ifdef __cplusplus
extern "C" {
#endif
/* Metadata only. Call on the game thread inside an active frame. */
void dusk_gx_trace_scene(const char* stage, int room);
void dusk_gx_trace_cpu(unsigned stage, double milliseconds); /* 0 update, 1 draw preparation */
#ifdef __cplusplus
}
#endif
#endif
