# Complete native tool index

The public build and test commands are documented in [Tools/3ds](../Tools/3ds/README.md).
The remaining entries are retained development tools. Their first source docstring
describes intent, not proof of execution or clean-checkout compatibility. Inspect
arguments and prerequisites before running historical workflows.

| Tool source | Declared purpose |
| --- | --- |
| [analyze_audio_pcm.py](../Tools/3ds/analyze_audio_pcm.py) | Verify bounded native PCM evidence; signal metrics do not certify playback. |
| [analyze_depth_audit.py](../Tools/3ds/analyze_depth_audit.py) | Verify private PICA depth snapshots, queued queries, and previous-frame retention. |
| [analyze_gx_trace.py](../Tools/3ds/analyze_gx_trace.py) | Validate private metadata-only GX captures and report workload, never 3DS FPS. |
| [analyze_texture_dump.py](../Tools/3ds/analyze_texture_dump.py) | Analyze every registered CPU GX image in a private DSKTEX01 snapshot. |
| [arm32_inventory.py](../Tools/3ds/arm32_inventory.py) | Compile every bootstrap TU; retain failures, dependencies and categorized evidence. |
| [audit_cmpr_half_candidates.py](../Tools/3ds/audit_cmpr_half_candidates.py) | Bounded private GX14 census; metadata only, never approves startup sources. |
| [audit_scenario_coverage.py](../Tools/3ds/audit_scenario_coverage.py) | Separate executed stage probes, visible scenes, and verified story coverage. |
| [audit_texture_half_candidates.py](../Tools/3ds/audit_texture_half_candidates.py) | Bounded private RGB565 provenance census. Metadata only; never approves content. |
| [azahar_debug.py](../Tools/3ds/azahar_debug.py) | Bounded local guest diagnostics over Azahar's GDB stub. No emulator launch. |
| [azahar_rpc_probe.py](../Tools/3ds/azahar_rpc_probe.py) | Read process/performance data and a screenshot from a local Azahar RPC server. |
| [azahar_run_report.py](../Tools/3ds/azahar_run_report.py) | Classify native Azahar evidence without confusing process exit with game success. |
| [build_public.py](../Tools/3ds/build_public.py) | Build the existing native target locally; never tag, upload or publish. |
| [capture_to_png.py](../Tools/3ds/capture_to_png.py) | Convert native DSKCAP01 framebuffer captures to PNG in LCD orientation. |
| [check_publication.py](../Tools/3ds/check_publication.py) | Check tracked public content, maintained guide links and open firmware integrity. |
| [check_svc_access.py](../Tools/3ds/check_svc_access.py) | Fail when the ELF uses an SVC that the CXI exheader does not grant. |
| [compare_native_profiles.py](../Tools/3ds/compare_native_profiles.py) | Compare sparse native guest profiles; these are not hardware FPS results. |
| [complete_pinned_checkout.py](../Tools/3ds/complete_pinned_checkout.py) | Complete exact gitlink revisions without creating remotes, branches or commits. |
| [decode_rejected_material.py](../Tools/3ds/decode_rejected_material.py) | Decode the last native material rejection from a private Azahar text log. |
| [decode_snapshot_texture.cpp](../Tools/3ds/decode_snapshot_texture.cpp) | Native development fixture or configuration; inspect the source before use. |
| [depth_retained_probe_host.cpp](../Tools/3ds/depth_retained_probe_host.cpp) | Native development fixture or configuration; inspect the source before use. |
| [full_game_coverage.py](../Tools/3ds/full_game_coverage.py) | Private metadata inventory and conservative full-game evidence ledger. |
| [gdb_sampler.py](../Tools/3ds/gdb_sampler.py) | Bounded guest thread-context observations; Azahar RSP is not a CPU profiler. |
| [generate_profile_link_probe.py](../Tools/3ds/generate_profile_link_probe.py) | Select original profile pointers for a bounded LINK diagnostic, not gameplay. |
| [inspect_first_room.py](../Tools/3ds/inspect_first_room.py) | Read original Ordon actor-name metadata for a private bounded LINK probe. |
| [inspect_startup_textures.py](../Tools/3ds/inspect_startup_textures.py) | Bounded private startup texture metadata inventory; no asset payload output. |
| [inventory_game_core.py](../Tools/3ds/inventory_game_core.py) | Inventory ARM32 game objects without modifying or linking the native target. |
| [list_stage_spawns.py](../Tools/3ds/list_stage_spawns.py) | List original player start points per stage room from the private disc. |
| [location_matrix.json](../Tools/3ds/location_matrix.json) | Native development fixture or configuration; inspect the source before use. |
| [make_azahar_input_fixture.py](../Tools/3ds/make_azahar_input_fixture.py) | Modify only button fields in a real local CTM recording; preserve poll order. |
| [package_diagnostic.py](../Tools/3ds/package_diagnostic.py) | Build a local diagnostic CIA from native code and owner-supplied artwork only. |
| [package_startup_candidate.py](../Tools/3ds/package_startup_candidate.py) | Package an immutable local original-startup CXI; no ROMFS or game data. |
| [platform_audit.py](../Tools/3ds/platform_audit.py) | Fingerprint exact conditional blocks; carry forward only matching manual reviews. |
| [probe_environment_link.py](../Tools/3ds/probe_environment_link.py) | Resolve retained original environment dependencies; never claim runtime from this probe. |
| [probe_j3d_link.py](../Tools/3ds/probe_j3d_link.py) | Inspect retained original model dependencies; not a final executable link. |
| [run_azahar_smoke.py](../Tools/3ds/run_azahar_smoke.py) | Bounded emulator smoke test; private ISO read-only, workspace-only writes. |
| [run_location_matrix.py](../Tools/3ds/run_location_matrix.py) | Run one CXI across the multi-location matrix in Azahar and summarize each location. |
| [run_private_pc.py](../Tools/3ds/run_private_pc.py) | Bounded local PC run: immutable input, confined output, no network access. |
| [serve_private_cia.py](../Tools/3ds/serve_private_cia.py) | Bounded local-only installer server: one explicitly selected CIA, no directory listing. |
| [test_alpha_interval.py](../Tools/3ds/test_alpha_interval.py) | Host-only exact alpha interval oracle, real backend emission, and adapter regressions. |
| [test_alpha_interval_ownership.py](../Tools/3ds/test_alpha_interval_ownership.py) | Host-only actual alpha backend + framebuffer owner integration and regressions. |
| [test_analyze_texture_dump.py](../Tools/3ds/test_analyze_texture_dump.py) | Offline export/parser/metric regressions; synthetic data, no emulator or ISO. |
| [test_application_dump_verifier.py](../Tools/3ds/test_application_dump_verifier.py) | Native development fixture or configuration; inspect the source before use. |
| [test_audio_analysis.py](../Tools/3ds/test_audio_analysis.py) | Host checks that native PCM evidence corruption cannot pass validation. |
| [test_audio_pcm.py](../Tools/3ds/test_audio_pcm.py) | Host-only tests for bounded native PCM capture. Never builds ARM or starts an emulator. |
| [test_azahar_debug.py](../Tools/3ds/test_azahar_debug.py) | Offline RSP/diagnostic tests; stdlib only, no emulator or native build. |
| [test_azahar_run_report.py](../Tools/3ds/test_azahar_run_report.py) | Native development fixture or configuration; inspect the source before use. |
| [test_color_copy_orientation.py](../Tools/3ds/test_color_copy_orientation.py) | Host-only color-copy oracle anchored to preserved R2/R3 native probe captures. |
| [test_compare_native_profiles.py](../Tools/3ds/test_compare_native_profiles.py) | Regression checks for timing evidence alignment and exclusions. |
| [test_corner_clip.py](../Tools/3ds/test_corner_clip.py) | Corner non-coverage proof versus exact integer feasibility and old outcodes. |
| [test_depth_audit.py](../Tools/3ds/test_depth_audit.py) | Native development fixture or configuration; inspect the source before use. |
| [test_depth_retained_probe.py](../Tools/3ds/test_depth_retained_probe.py) | Host ASan/UBSan: actual CTRU backend, extracted developer probe, real dump I/O. |
| [test_diagnostic_package.py](../Tools/3ds/test_diagnostic_package.py) | Reject damaged or incorrectly labeled copies of the generated diagnostic CIA. |
| [test_disc_files.py](../Tools/3ds/test_disc_files.py) | Bounded FST access checks; synthetic data by default, optional local read-only probe. |
| [test_display_list.py](../Tools/3ds/test_display_list.py) | Execute the pinned Aurora display-list processor with native dependencies only. |
| [test_environment_probe.py](../Tools/3ds/test_environment_probe.py) | Check ARM archive admission and allocated-relocation filtering in the probe. |
| [test_file_warning_animation.py](../Tools/3ds/test_file_warning_animation.py) | Compile actual warning/menu methods against bounded host presentation stubs. |
| [test_font_batch.py](../Tools/3ds/test_font_batch.py) | Capture native font batches and compare exact original quad output. |
| [test_font_pages.py](../Tools/3ds/test_font_pages.py) | Verify actual native font page storage/registration with host sanitizers. |
| [test_framebuffer_execution.py](../Tools/3ds/test_framebuffer_execution.py) | Host sanitizer checks for actual native copy, framebuffer and SD save logic. |
| [test_framebuffer_viewport.py](../Tools/3ds/test_framebuffer_viewport.py) | Host-only ASan/UBSan regression for native framebuffer raster-state scope. |
| [test_fused_color_config.py](../Tools/3ds/test_fused_color_config.py) | Host-only exact opt-in parser, one-shot SD reader and startup-call checks. |
| [test_gdb_sampler_fidelity.py](../Tools/3ds/test_gdb_sampler_fidelity.py) | Focused offline fidelity regressions. Never attach to or launch Azahar. |
| [test_gpu_queue_lifecycle.py](../Tools/3ds/test_gpu_queue_lifecycle.py) | Compile unchanged wrapper and pinned gxqueue.c against bounded host events. |
| [test_gx_command_draw.py](../Tools/3ds/test_gx_command_draw.py) | Verify the original-command draw adapter with bounded host GPU substitutes. |
| [test_gx_commands.py](../Tools/3ds/test_gx_commands.py) | Verify CPU GX command processing with original GD encoders and bounded arrays. |
| [test_gx_copy_commands.py](../Tools/3ds/test_gx_copy_commands.py) | Compare native copy setup to original GX owners; real transfers remain separate. |
| [test_gx_geometry_layout.py](../Tools/3ds/test_gx_geometry_layout.py) | Check shared projection regression and measure indexed geometry ARM storage. |
| [test_gx_indexed_geometry.py](../Tools/3ds/test_gx_indexed_geometry.py) | Verify indexed GPU arena, prepared packing, native compilation and shader assembly. |
| [test_gx_initial_state.py](../Tools/3ds/test_gx_initial_state.py) | Exercise actual GX initialization and the first untextured original fader. |
| [test_gx_lighting_commands.py](../Tools/3ds/test_gx_lighting_commands.py) | Verify native channel/texgen configuration against original GD. |
| [test_gx_lookup.py](../Tools/3ds/test_gx_lookup.py) | Check bounded live texture lookup hints under full-table collision and reuse. |
| [test_gx_material.py](../Tools/3ds/test_gx_material.py) | Verify material decoding and banked TEV colors with original GD encoders. |
| [test_gx_material_backend.py](../Tools/3ds/test_gx_material_backend.py) | Verify Citro3D material register packing and legacy backend equivalence. |
| [test_gx_pica_material.py](../Tools/3ds/test_gx_pica_material.py) | Verify common PICA material algebra independently from the translator. |
| [test_gx_pica_tev.py](../Tools/3ds/test_gx_pica_tev.py) | Verify bounded GX->PICA TEV planning under host sanitizers; compile for ARM. |
| [test_gx_queries.py](../Tools/3ds/test_gx_queries.py) | Verify actual native state queries/restoration and raw-list changes. |
| [test_gx_raster_lighting.py](../Tools/3ds/test_gx_raster_lighting.py) | Verify GX raster lighting and selected normal matrices. |
| [test_gx_state.py](../Tools/3ds/test_gx_state.py) | Run actual pinned J2D primitive classes against the bounded native GX capture. |
| [test_gx_stream.py](../Tools/3ds/test_gx_stream.py) | Verify original J3D calls through native GX scalar and display-list entry points. |
| [test_gx_tev_commands.py](../Tools/3ds/test_gx_tev_commands.py) | Verify native typed TEV/indirect commands against original GD owners. |
| [test_gx_texture_backend.py](../Tools/3ds/test_gx_texture_backend.py) | Verify native texture cache lifecycle and independent per-unit samplers. |
| [test_gx_texture_commands.py](../Tools/3ds/test_gx_texture_commands.py) | Verify original J3D texture lists resolve native registered image tokens. |
| [test_gx_transforms.py](../Tools/3ds/test_gx_transforms.py) | Verify typed GX transforms and original J3D commands share register state. |
| [test_gx_typed_draw_pixel.py](../Tools/3ds/test_gx_typed_draw_pixel.py) | Verify actual native typed GX draws and pixel commands against original GD. |
| [test_gx_uniforms.py](../Tools/3ds/test_gx_uniforms.py) | Verify exact Citro3D uniform shadow updates; no GPU/hardware timing claim. |
| [test_gx_vertex_commands.py](../Tools/3ds/test_gx_vertex_commands.py) | Verify typed vertex state against original GD and decode registered arrays. |
| [test_gx_vertex_transform.py](../Tools/3ds/test_gx_vertex_transform.py) | Verify GX vertex transforms against independent double-precision equations. |
| [test_native_archive.py](../Tools/3ds/test_native_archive.py) | Original JKR memory archive integration under host ASan/UBSan; no extracted payloads. |
| [test_native_bootstrap.py](../Tools/3ds/test_native_bootstrap.py) | Host integration checks. Synthetic input only; no hardware claims. |
| [test_native_contracts.py](../Tools/3ds/test_native_contracts.py) | Exercise actual JKR code with native policy and test-only host OS primitives. |
| [test_native_game_clock.py](../Tools/3ds/test_native_game_clock.py) | Exercise original game-clock phase logic with the actual native Aurora time ABI. |
| [test_native_gd.py](../Tools/3ds/test_native_gd.py) | Test native command storage with the actual J3D GD and display-list objects. |
| [test_native_j3d_texture.py](../Tools/3ds/test_native_j3d_texture.py) | Link original J3DTexture/JKR to bounded TEX1 registration under sanitizers. |
| [test_native_math.py](../Tools/3ds/test_native_math.py) | Numerically test pinned Aurora C math with native compile policy on the host. |
| [test_native_original_jut.py](../Tools/3ds/test_native_original_jut.py) | Check actual original JUT console and panic owners with host OS backends. |
| [test_native_pad.py](../Tools/3ds/test_native_pad.py) | Exercise original JUT/JKR with native mapping and synthetic HID snapshots. |
| [test_native_picture.py](../Tools/3ds/test_native_picture.py) | Link original J2DPicture/JUTTexture to genuine native providers and capture GX under sanitizers. |
| [test_native_platform.py](../Tools/3ds/test_native_platform.py) | Test production arena and clock adapters with synthetic host inputs. |
| [test_native_sync.py](../Tools/3ds/test_native_sync.py) | Run the production native OS synchronization algorithm with host primitives. |
| [test_native_threads.py](../Tools/3ds/test_native_threads.py) | Exercise native OS/real JKR worker lifecycle with host threads and compile ARM production backends. |
| [test_original_video_retrace.py](../Tools/3ds/test_original_video_retrace.py) | Check actual JUTVideo lifetime and native retrace delivery with synthetic hardware. |
| [test_process_profile.py](../Tools/3ds/test_process_profile.py) | Check original native static-profile lookup without simulating actors. |
| [test_public_host.py](../Tools/3ds/test_public_host.py) | Run enumerated synthetic production fixtures without a disc, SDK or emulator. |
| [test_renderer_safety.py](../Tools/3ds/test_renderer_safety.py) | Run owned renderer-safety host regressions with ASan/UBSan, without ARM/emulator work. |
| [test_resource_archive.py](../Tools/3ds/test_resource_archive.py) | Test bounded archive parsers without extracting or saving private resource payloads. |
| [test_run_azahar_smoke.py](../Tools/3ds/test_run_azahar_smoke.py) | Offline lifecycle tests: no real emulator, fork, signal, or debugger socket. |
| [test_scene_blockers_s44.py](../Tools/3ds/test_scene_blockers_s44.py) | Host-only session44 clip/alpha/safety regressions; no native build or emulator. |
| [test_scene_blockers_s44_continuation08.py](../Tools/3ds/test_scene_blockers_s44_continuation08.py) | Host-only session44 clip/alpha/safety regressions; no native build or emulator. |
| [test_stage_data.py](../Tools/3ds/test_stage_data.py) | Bounded DZS/DZR test plus optional read-only private metadata inspection. |
| [test_texture_compression_lab.py](../Tools/3ds/test_texture_compression_lab.py) | Independent byte/layout oracles for the private texture lab, no game changes. |
| [test_texture_gx.py](../Tools/3ds/test_texture_gx.py) | Compare native PICA texture storage with unchanged pinned Aurora decoder functions. |
| [test_texture_half.py](../Tools/3ds/test_texture_half.py) | Host-only authored-mip half policy validation. No ARM build or emulator. |
| [test_texture_half_cmpr.py](../Tools/3ds/test_texture_half_cmpr.py) | Host ASan/UBSan half-CMPR validation with independent block/pixel/hash oracle. |
| [test_texture_i4.py](../Tools/3ds/test_texture_i4.py) | Run the synthetic I4 conversion fixture under host ASan/UBSan. |
| [test_texture_ia8.py](../Tools/3ds/test_texture_ia8.py) | Run the synthetic IA8 conversion fixture under host ASan/UBSan. |
| [test_texture_resources.py](../Tools/3ds/test_texture_resources.py) | Verify actual native archive/texture lifetime code with host sanitizers. |
| [test_texture_rgb565.py](../Tools/3ds/test_texture_rgb565.py) | Host-only native RGB565 admission checks. No ARM build, emulator, ISO or installed-tool changes. |
| [test_title_commands.py](../Tools/3ds/test_title_commands.py) | Verify the native CPU command pipeline with the owner's title model in memory. |
| [test_title_display_list.py](../Tools/3ds/test_title_display_list.py) | Read the owner's local title model in memory; verify original DL optimization. |
| [test_title_draw_state.py](../Tools/3ds/test_title_draw_state.py) | Verify the native material metadata with the owner's title model in memory. |
| [test_title_geometry.py](../Tools/3ds/test_title_geometry.py) | Verify the native typed vertex state and list ABI with the owner's title model in memory. |
| [test_title_material.py](../Tools/3ds/test_title_material.py) | Verify the native material metadata with the owner's title model in memory. |
| [test_title_stream.py](../Tools/3ds/test_title_stream.py) | Verify the native GX scalar/list ABI with the owner's title model in memory. |
| [test_title_typed_vertex.py](../Tools/3ds/test_title_typed_vertex.py) | Verify the native typed vertex state and list ABI with the owner's title model in memory. |
| [test_title_vertex.py](../Tools/3ds/test_title_vertex.py) | Read the owner's local title model in memory; verify original DL optimization and native vertex decoding. |
| [test_trace.py](../Tools/3ds/test_trace.py) | Synthetic collector regression checks; never reads game input. |
| [test_vertex_decode.py](../Tools/3ds/test_vertex_decode.py) | Check bounded native vertex decoding on host sanitizers and ARM compiler. |
| [test_video_retrace.py](../Tools/3ds/test_video_retrace.py) | Test native retrace/sleep contracts with a synthetic event source and real queues. |
| [test_worker_shutdown.py](../Tools/3ds/test_worker_shutdown.py) | Test production worker shutdown with real host threads; never builds ARM or controls an emulator. |
| [texture_compression_lab.py](../Tools/3ds/texture_compression_lab.py) | Bounded PRIVATE host texture lab. No game assets, source or installed tools modified. |
| [texture_lab_etc_host.cpp](../Tools/3ds/texture_lab_etc_host.cpp) | Native development fixture or configuration; inspect the source before use. |
| [texture_lab_formats.py](../Tools/3ds/texture_lab_formats.py) | Host lab formats. Independent ETC1 decoder follows the Khronos bit grammar. |
| [twilight_pack.py](../Tools/3ds/twilight_pack.py) | Private, bounded GameCube USA r0 -> DuskLight indexed asset container v1. |
| [validate_texture_compression_lab.py](../Tools/3ds/validate_texture_compression_lab.py) | Reopen a lab run, validate every actual PICA payload, and retain receipts. |
| [verify_application_dump.py](../Tools/3ds/verify_application_dump.py) | Read-only verification of a private DUSKLIGHT_APPLICATION_DUMP_1 directory. |
| [verify_diagnostic_package.py](../Tools/3ds/verify_diagnostic_package.py) | Validate this diagnostic CIA's structure and hashes, not signatures or hardware. |
| [verify_gpu_queue_gate.py](../Tools/3ds/verify_gpu_queue_gate.py) | Verify the actual linked ARM queue/callback gate; never infer interception from flags. |
