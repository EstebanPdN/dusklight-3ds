# Native runtime source map

This directory implements the original engine's platform contracts. GPU-facing
Citro3D submission remains one level up in the platform directory.

| Responsibility | File families |
| --- | --- |
| Original startup and bootstrap | `game_startup`, `bootstrap`, `archive_bootstrap`, `boot_stage` |
| Disc and DVD ownership | `disc`, `dvd_provider`, `resource_archive`, `stage_data` |
| Input | `pad_native`, `pad_mapping`, `pad_clamp`, `pad_bootstrap`, `developer_script` |
| Audio and card persistence | `audio_ndsp`, `audio_control`, `aram`, `card`, `jas_critical_section` |
| Threads and synchronization | `os_thread`, `os_sync`, `thread_backend*`, `primitives*` |
| Clock, arena and display lifecycle | `os_time`, `os_arena`, `os_system*`, `game_clock_native`, `video_*` |
| Command retention and GX state | `gx_commands`, `gx_stream`, `gx_state`, `gd_native`, `gx_*_commands` |
| Geometry and transform | `gx_vertex*`, `gx_rigid*`, `gx_geometry*`, `gx_gpu_geometry*` |
| Material/raster planning | `gx_material`, `gx_pica_material`, `gx_raster*`, `gx_pica_math`, `gx_pica_tev` |
| Textures and fonts | `texture_*`, `gx_texture_*`, `font_pages`, `font_batch` |
| Framebuffers, copies and GPU completion | `gx_framebuffer*`, `gx_copy*`, `gx_gpu_sync*`, `ctru_gxqueue`, `fused_color*` |
| Application diagnostics | `application_dump*`, `dump_io`, probe and profile headers |

Headers describe ownership and unsupported states at the API boundary. The
[architecture guide](../../../../docs/ARCHITECTURE.md) connects these families
to the original game. Source tests are under `../tests`; host substitutes do not
stand in for hardware acceptance. Keep data lifetimes, thread ownership, queue
completion and unsupported-state reporting intact when changing these adapters.
