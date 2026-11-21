# Complete native tool index

The public build and test commands are documented in [Tools/3ds](../Tools/3ds/README.md).
The remaining entries are retained development tools. Their first source docstring
describes intent, not proof of execution or clean-checkout compatibility. Inspect
arguments and prerequisites before running historical workflows.

| Tool source | Declared purpose |
| --- | --- |
| [analyze_audio_pcm.py](../Tools/3ds/analyze_audio_pcm.py) | Verify bounded native PCM evidence; signal metrics do not certify playback. |
| [analyze_cadence.py](../Tools/3ds/analyze_cadence.py) | Report every submitted picture of an automatic-test folder (`tour/NNN`), including transitions, SD work and diagnostic frames. Its timing gate needs a complete story or Test everything run; a pass never establishes device provenance, story coverage or visual correctness. |
| [analyze_depth_audit.py](../Tools/3ds/analyze_depth_audit.py) | Verify private PICA depth snapshots, queued queries, and previous-frame retention. |
| [analyze_gpu_lists.py](../Tools/3ds/analyze_gpu_lists.py) | Validate a DLPC001 GPU sidecar (`gpu-camera.bin`, `gpu-still.bin`) and report its P3D jobs and the ordered state of every draw. |
| [analyze_gx_trace.py](../Tools/3ds/analyze_gx_trace.py) | Validate private metadata-only GX captures and report workload, never 3DS FPS. |
| [analyze_story.py](../Tools/3ds/analyze_story.py) | Evaluate a story benchmark folder (`story-plan.csv`, `story.csv`) against its ordered plan and the observed gameplay. |
| [analyze_texture_dump.py](../Tools/3ds/analyze_texture_dump.py) | Analyze every registered CPU GX image in a private DSKTEX01 snapshot. |
| [analyze_tour.py](../Tools/3ds/analyze_tour.py) | Summarize an automatic-test folder (`/3ds/DuskLight/tour/NNN`) per zone and phase. |
| [arm32_inventory.py](../Tools/3ds/arm32_inventory.py) | Compile every bootstrap TU; retain failures, dependencies and categorized evidence. |
| [audit_cmpr_half_candidates.py](../Tools/3ds/audit_cmpr_half_candidates.py) | Bounded private GX14 census; metadata only, never approves startup sources. |
| [audit_scenario_coverage.py](../Tools/3ds/audit_scenario_coverage.py) | Separate executed stage probes, visible scenes, and verified story coverage. |
| [audit_shared_state.py](../Tools/3ds/audit_shared_state.py) | Build check of the second-core worker: from the image's object files, every writable variable the game thread and the drawing side can both reach; one without a verdict in `shared_state_judged.tsv` fails the build (`shared_state_roots.tsv` names the drawing side's entry points and the functions that sync first). |
| [audit_texture_half_candidates.py](../Tools/3ds/audit_texture_half_candidates.py) | Bounded private RGB565 provenance census. Metadata only; never approves content. |
| [azahar_debug.py](../Tools/3ds/azahar_debug.py) | Bounded local guest diagnostics over Azahar's GDB stub. No emulator launch. |
| [azahar_rpc_probe.py](../Tools/3ds/azahar_rpc_probe.py) | Read process/performance data and a screenshot from a local Azahar RPC server. |
| [azahar_run_report.py](../Tools/3ds/azahar_run_report.py) | Classify native Azahar evidence without confusing process exit with game success. |
| [build_color_lookup_probe.py](../Tools/3ds/build_color_lookup_probe.py) | Build the asset-free dual-raster RGBA GPU probe; execution and completed pixel metrics are separate. |
| [build_public.py](../Tools/3ds/build_public.py) | Build the existing native target locally; never tag, upload or publish. |
| [capture_to_png.py](../Tools/3ds/capture_to_png.py) | Convert native DSKCAP01 framebuffer captures to PNG in LCD orientation. |
| [check_ctru_abi.py](../Tools/3ds/check_ctru_abi.py) | Build gate: fail when a platform source reads an SDK member or inline across the enum-size boundary. |
| [check_eh_globals.py](../Tools/3ds/check_eh_globals.py) | Build gate: fail when C++ exception state can be shared between threads (devkitARM's libstdc++ keeps one block per process; `runtime/eh_globals.cpp` gives each thread its own). |
| [check_pica_shader.py](../Tools/3ds/check_pica_shader.py) | Build gate: every output component of every PICA vertex program is written exactly once. |
| [check_hot_inlining.py](../Tools/3ds/check_hot_inlining.py) | Build gate: the linked image holds no per-vertex reader or field pass of the native vertex decoder as a function of its own. |
| [check_publication.py](../Tools/3ds/check_publication.py) | Check tracked public content, maintained guide links and open firmware integrity. |
| [check_story_demo_banks.py](../Tools/3ds/check_story_demo_banks.py) | Check named story events against the room, stage and selected layer's disc resources. |
| [check_svc_access.py](../Tools/3ds/check_svc_access.py) | Fail when the ELF uses an SVC that the CXI exheader does not grant. |
| [compare_native_profiles.py](../Tools/3ds/compare_native_profiles.py) | Compare sparse native guest profiles; these are not hardware FPS results. |
| [complete_pinned_checkout.py](../Tools/3ds/complete_pinned_checkout.py) | Complete exact gitlink revisions without creating remotes, branches or commits. |
| [decode_rejected_material.py](../Tools/3ds/decode_rejected_material.py) | Decode the last native material rejection from a private Azahar text log. |
| [decode_snapshot_texture.cpp](../Tools/3ds/decode_snapshot_texture.cpp) | Native development fixture or configuration; inspect the source before use. |
| [depth_retained_probe_host.cpp](../Tools/3ds/depth_retained_probe_host.cpp) | Native development fixture or configuration; inspect the source before use. |
| [full_game_coverage.py](../Tools/3ds/full_game_coverage.py) | Private metadata inventory and conservative full-game evidence ledger. |
| [gdb_sampler.py](../Tools/3ds/gdb_sampler.py) | Bounded guest thread-context observations; Azahar RSP is not a CPU profiler. |
| [generate_profile_link_probe.py](../Tools/3ds/generate_profile_link_probe.py) | Select original profile pointers for a bounded LINK diagnostic, not gameplay. |
| [generate_zone_table.py](../Tools/3ds/generate_zone_table.py) | Generate the developer-console zone table (`runtime/zone_table.inc`) from start-point metadata. |
| [gpu_hang_report.py](../Tools/3ds/gpu_hang_report.py) | Report a console GPU guard hang record: job, shadow registers, uniforms, last GPU jobs and draw data. |
| [gpu_jobs_report.py](../Tools/3ds/gpu_jobs_report.py) | Where the console PICA spends a picture, from a dump extracted with `verify_application_dump.py --extract`: the last GSP jobs with their submission and completion ticks, and the command lists of the last complete picture decoded draw by draw. |
| [inspect_first_room.py](../Tools/3ds/inspect_first_room.py) | Read original Ordon actor-name metadata for a private bounded LINK probe. |
| [inspect_startup_textures.py](../Tools/3ds/inspect_startup_textures.py) | Bounded private startup texture metadata inventory; no asset payload output. |
| [inventory_game_core.py](../Tools/3ds/inventory_game_core.py) | Inventory ARM32 game objects without modifying or linking the native target. |
| [list_stage_spawns.py](../Tools/3ds/list_stage_spawns.py) | List original player start points per stage room from the private disc. |
| [location_matrix.json](../Tools/3ds/location_matrix.json) | Native development fixture or configuration; inspect the source before use. |
| [make_azahar_input_fixture.py](../Tools/3ds/make_azahar_input_fixture.py) | Modify only button fields in a real local CTM recording; preserve poll order. |
| [measure_report.py](../Tools/3ds/measure_report.py) | Where a picture's time goes (Alpha 21 and later), from a measured full dump, a zone's `measure.txt` or an automatic-test folder: time by process and named section, GPU time by owner and painter phase, and what the improvements of `runtime/perf_features.hpp` are worth together and one by one over the zones. `--extracted` adds the per-draw GPU view of a dump. |
| [mip_inventory.cpp](../Tools/3ds/mip_inventory.cpp) | Mac-only survey behind `mip_inventory.py`: every disc texture counted once, classified by the materials that sample it (MAT3), to decide which may get generated levels (`worldMips`). Never linked on the 3DS. |
| [mip_inventory.py](../Tools/3ds/mip_inventory.py) | Mac only, private disc: which textures are authored without mip levels, their PICA bytes and how the game samples them (builds and runs [mip_inventory.cpp](../Tools/3ds/mip_inventory.cpp)). Reads the disc only; prints the report. |
| [mip_register_check.py](../Tools/3ds/mip_register_check.py) | World mip check on an extracted dump: ties the texture cache to the texture registers of the last complete picture; exit 1 when images with generated levels are bound and fewer than 90 % of their draw-units sample them. |
| [model_lod.py](../Tools/3ds/model_lod.py) | Mac only, private data: builds [model_lod_build.cpp](../Tools/3ds/model_lod_build.cpp), reduces every J3D model of the owner's `zelda.twilight` and writes `zelda.lod` (copy it to `sdmc:/3ds/DuskLight/`), `report.json` and `shapes.txt`. `--test` adds the proof against the runtime's command processor and reads the store back with the runtime's reader; `--unit` runs the disc-free unit test. Never overwrites a file. |
| [model_lod_batch_test.cpp](../Tools/3ds/model_lod_batch_test.cpp) | Mac-only proof for `model_lod.py --test`: every display list of every J3D model of the disc runs through `runtime/gx_commands.cpp` as the 3DS build configures it, and each draw must equal the tool's: same count, same vertex records byte for byte, same indices. |
| [model_lod_build.cpp](../Tools/3ds/model_lod_build.cpp) | Mac-only level-of-detail converter run by `model_lod.py`: cuts every display list into the runtime's draws ([model_lod_batches.hpp](../Tools/3ds/model_lod_batches.hpp)) and reduces each with half-edge collapses over its own vertex records ([model_lod_simplify.hpp](../Tools/3ds/model_lod_simplify.hpp)); writes the store `runtime/model_lod_store.hpp` reads. |
| [model_lod_unit_test.cpp](../Tools/3ds/model_lod_unit_test.cpp) | Mac-only unit test for `model_lod.py --unit`: the simplifier on small meshes with known answers, and the runtime's record parser against malformed records. |
| [model_prep.py](../Tools/3ds/model_prep.py) | Mac-only, private disc: what offline model preparation would save in runtime draws (room shape merges, batch caps, skinned matrix palettes), with a proof on the runtime's own GX command processor over every room display list. |
| [model_prep_batch_test.cpp](../Tools/3ds/model_prep_batch_test.cpp) | Mac-only proof for `model_prep.py`: every room display list through the runtime's own GX command processor, with the 3DS caps and with 16384-vertex caps. |
| [model_prep_skin.cpp](../Tools/3ds/model_prep_skin.cpp) | Mac-only, `model_prep.py --skin`: skinned shapes of every model on the disc and the draws a wider matrix palette leaves of them, planned by `runtime/skin_palette.hpp`. |
| [model_prep_survey.cpp](../Tools/3ds/model_prep_survey.cpp) | Mac-only survey behind `model_prep.py`: room shape merges, batch caps and their draw savings, counted in the draws the runtime makes ([model_prep_bmd.hpp](../Tools/3ds/model_prep_bmd.hpp), [model_prep_walk.hpp](../Tools/3ds/model_prep_walk.hpp)). |
| [package_diagnostic.py](../Tools/3ds/package_diagnostic.py) | Build a local diagnostic CIA from native code and owner-supplied artwork only. |
| [package_startup_candidate.py](../Tools/3ds/package_startup_candidate.py) | Package an immutable local original-startup CXI; no ROMFS or game data. |
| [pica_list_decode.py](../Tools/3ds/pica_list_decode.py) | Decode a saved PICA command list into named register writes. |
| [pica_shader_disasm.py](../Tools/3ds/pica_shader_disasm.py) | Disassemble a Picasso `.shbin` vertex program with resolved operands and outputs. |
| [pica_vertex_interp.py](../Tools/3ds/pica_vertex_interp.py) | Host interpreter for assembled picasso vertex programs (`.shbin`): runs one DVLE over given inputs and uniforms and counts the instructions executed (float32, as the emulator's interpreter). |
| [platform_audit.py](../Tools/3ds/platform_audit.py) | Fingerprint exact conditional blocks; carry forward only matching manual reviews. |
| [pretexture_build.cpp](../Tools/3ds/pretexture_build.cpp) | Mac-only converter behind `pretexture_pack.py`: every BTI of the disc ([pretexture_walk.hpp](../Tools/3ds/pretexture_walk.hpp)) to pre-converted PICA images in the `runtime/pretexture_store.hpp` layout. Never linked on the 3DS. |
| [pretexture_pack.py](../Tools/3ds/pretexture_pack.py) | Mac only, private data: pre-converted PICA textures for `zelda.twilight` (builds [pretexture_build.cpp](../Tools/3ds/pretexture_build.cpp), which links the runtime's disc reader, archive reader and texture decoder) and optionally appends the section to a bundle. Measured in Alpha 12 and not used: the console would load the larger data slower. Never overwrites a file. |
| [pretexture_store_test.cpp](../Tools/3ds/pretexture_store_test.cpp) | Mac-only test: every disc texture read back through `runtime/pretexture_store.cpp` must equal the runtime decoder's output byte for byte. |
| [probe_environment_link.py](../Tools/3ds/probe_environment_link.py) | Resolve retained original environment dependencies; never claim runtime from this probe. |
| [probe_j3d_link.py](../Tools/3ds/probe_j3d_link.py) | Inspect retained original model dependencies; not a final executable link. |
| [run_azahar_smoke.py](../Tools/3ds/run_azahar_smoke.py) | Bounded emulator smoke test; private ISO read-only, workspace-only writes. |
| [run_location_matrix.py](../Tools/3ds/run_location_matrix.py) | Run one CXI across the multi-location matrix in Azahar and summarize each location. |
| [run_private_pc.py](../Tools/3ds/run_private_pc.py) | Bounded local PC run: immutable input, confined output, no network access. |
| [serve_private_cia.py](../Tools/3ds/serve_private_cia.py) | Bounded local-only installer server: one explicitly selected CIA, no directory listing. |
| [survey_material_refusals.py](../Tools/3ds/survey_material_refusals.py) | Mac only: runs the PICA material planner (`tests/material_refusal_survey.cpp`) over material survey files with the Alpha 12 operation rules off and on, counts refusals per error and rule, and checks every added plan against the GX reference evaluator. |
| [test_alpha_interval.py](../Tools/3ds/test_alpha_interval.py) | Host-only exact alpha interval oracle, real backend emission, and adapter regressions. |
| [test_alpha_interval_ownership.py](../Tools/3ds/test_alpha_interval_ownership.py) | Host-only actual alpha backend + framebuffer owner integration and regressions. |
| [test_analyze_texture_dump.py](../Tools/3ds/test_analyze_texture_dump.py) | Offline export/parser/metric regressions; synthetic data, no emulator or ISO. |
| [test_application_dump_verifier.py](../Tools/3ds/test_application_dump_verifier.py) | Native development fixture or configuration; inspect the source before use. |
| [test_audio_analysis.py](../Tools/3ds/test_audio_analysis.py) | Host checks that native PCM evidence corruption cannot pass validation. |
| [test_audio_mixer.py](../Tools/3ds/test_audio_mixer.py) | Host-only: the software mixer's ADPCM decoder and fixed-point mix against the float path (`tests/audio_mixer_test.cpp`). Benchmark output is host time, a ratio only. |
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
| [test_eh_globals.py](../Tools/3ds/test_eh_globals.py) | Per-thread C++ exception state (`runtime/eh_globals.cpp`) with host threads under ASan/UBSan and TSan; with `--image` and `--image-before`, `check_eh_globals.py` must pass on the new image and fail on the old one. |
| [test_environment_probe.py](../Tools/3ds/test_environment_probe.py) | Check ARM archive admission and allocated-relocation filtering in the probe. |
| [test_file_warning_animation.py](../Tools/3ds/test_file_warning_animation.py) | Compile actual warning/menu methods against bounded host presentation stubs. |
| [test_font_batch.py](../Tools/3ds/test_font_batch.py) | Capture native font batches and compare exact original quad output. |
| [test_font_pages.py](../Tools/3ds/test_font_pages.py) | Verify actual native font page storage/registration with host sanitizers. |
| [test_font_pane_lifecycle.py](../Tools/3ds/test_font_pane_lifecycle.py) | Exercise the original font owner on the host with the Alpha 17 pane budget. |
| [test_framebuffer_execution.py](../Tools/3ds/test_framebuffer_execution.py) | Host sanitizer checks for actual native copy, framebuffer and SD save logic. |
| [test_framebuffer_viewport.py](../Tools/3ds/test_framebuffer_viewport.py) | Host-only ASan/UBSan regression for native framebuffer raster-state scope. |
| [test_fused_color_config.py](../Tools/3ds/test_fused_color_config.py) | Host-only exact opt-in parser, one-shot SD reader and startup-call checks. |
| [test_gdb_sampler_fidelity.py](../Tools/3ds/test_gdb_sampler_fidelity.py) | Focused offline fidelity regressions. Never attach to or launch Azahar. |
| [test_gpu_canary.py](../Tools/3ds/test_gpu_canary.py) | Host test of the console GPU guard (`runtime/gpu_canary_ctru.cpp`) against stub SDK headers and host stand-ins for GSP, the GX queue and the SD. |
| [test_gpu_queue_lifecycle.py](../Tools/3ds/test_gpu_queue_lifecycle.py) | Compile unchanged wrapper and pinned gxqueue.c against bounded host events. |
| [test_gx_command_draw.py](../Tools/3ds/test_gx_command_draw.py) | Verify the original-command draw adapter with bounded host GPU substitutes. |
| [test_gx_commands.py](../Tools/3ds/test_gx_commands.py) | Verify CPU GX command processing with original GD encoders and bounded arrays. |
| [test_gx_copy_commands.py](../Tools/3ds/test_gx_copy_commands.py) | Compare native copy setup to original GX owners; real transfers remain separate. |
| [test_gx_geometry_layout.py](../Tools/3ds/test_gx_geometry_layout.py) | Check shared projection regression and measure indexed geometry ARM storage. |
| [test_gx_indexed_geometry.py](../Tools/3ds/test_gx_indexed_geometry.py) | Verify indexed GPU arena, prepared packing, native compilation and shader assembly. |
| [test_gx_indirect.py](../Tools/3ds/test_gx_indirect.py) | Indirect warp sampler and its constant-offset GPU plan: hookup on the real draw adapter, host ASan/UBSan, ARM compile, picasso assembly with the output-write gate. |
| [test_gx_initial_state.py](../Tools/3ds/test_gx_initial_state.py) | Exercise actual GX initialization and the first untextured original fader. |
| [test_gx_lighting_commands.py](../Tools/3ds/test_gx_lighting_commands.py) | Verify native channel/texgen configuration against original GD. |
| [test_gx_lookup.py](../Tools/3ds/test_gx_lookup.py) | Check bounded live texture lookup hints under full-table collision and reuse. |
| [test_gx_material.py](../Tools/3ds/test_gx_material.py) | Verify material decoding and banked TEV colors with original GD encoders. |
| [test_gx_material_backend.py](../Tools/3ds/test_gx_material_backend.py) | Verify Citro3D material register packing and legacy backend equivalence. |
| [test_gx_pica_material.py](../Tools/3ds/test_gx_pica_material.py) | Verify common PICA material algebra independently from the translator. |
| [test_gx_pica_tev.py](../Tools/3ds/test_gx_pica_tev.py) | Verify bounded GX->PICA TEV planning under host sanitizers; compile for ARM. |
| [test_gx_queries.py](../Tools/3ds/test_gx_queries.py) | Verify actual native state queries/restoration and raw-list changes. |
| [test_gx_raster_lighting.py](../Tools/3ds/test_gx_raster_lighting.py) | Verify GX raster lighting and selected normal matrices. |
| [test_gx_rigid_texgen.py](../Tools/3ds/test_gx_rigid_texgen.py) | Multi-texture GPU texgen (`gx_rigid_mt.v.pica`) against the CPU vertex path on real register banks; shader mutations must be detected. |
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
| [test_gx_worker.py](../Tools/3ds/test_gx_worker.py) | Second-core worker's portable parts: register bank deltas against a real command processor, and the shape queue deferred and with a consumer thread (also under ThreadSanitizer). |
| [test_invisible_model_entry.py](../Tools/3ds/test_invisible_model_entry.py) | Exercise the native model and dependent Z-sort packet entry with host draw-list substitutes. |
| [test_j3d_skin_matrix_runs.py](../Tools/3ds/test_j3d_skin_matrix_runs.py) | J3D skin matrix runs and the worker's bank parity on the host. |
| [test_material_memo.py](../Tools/3ds/test_material_memo.py) | J2D material memo against the real native typed GX setters. |
| [test_memcard_permission.py](../Tools/3ds/test_memcard_permission.py) | Execute production card-controller and file-select permission recovery with real synthetic native CARD storage. |
| [test_native_archive.py](../Tools/3ds/test_native_archive.py) | Original JKR memory archive integration under host ASan/UBSan; no extracted payloads. |
| [test_native_bootstrap.py](../Tools/3ds/test_native_bootstrap.py) | Host integration checks. Synthetic input only; no hardware claims. |
| [test_native_contracts.py](../Tools/3ds/test_native_contracts.py) | Exercise actual JKR code with native policy and test-only host OS primitives. |
| [test_native_game_clock.py](../Tools/3ds/test_native_game_clock.py) | Exercise original game-clock phase logic with the actual native Aurora time ABI. |
| [test_native_gd.py](../Tools/3ds/test_native_gd.py) | Test native command storage with the actual J3D GD and display-list objects. |
| [test_native_j3d_texture.py](../Tools/3ds/test_native_j3d_texture.py) | Link original J3DTexture/JKR to bounded TEX1 registration under sanitizers. |
| [test_native_logic.py](../Tools/3ds/test_native_logic.py) | `logic.txt` rules: process id table and shared skin products, with the pinned Aurora matrix code built without fused multiply-add (as the 3DS VFPv2), under ASan/UBSan. |
| [test_native_material_identity.py](../Tools/3ds/test_native_material_identity.py) | PICA material planner against a reference planner: with the sum rules off identical on every material; with them on, reference plans unchanged and every added plan matching the GX evaluator. |
| [test_native_material_operation_rules.py](../Tools/3ds/test_native_material_operation_rules.py) | The Alpha 12 operation rules of the material planner against the Alpha 11 planner, the same way; refusals must name their rule. The default reference is a private local snapshot. |
| [test_native_math.py](../Tools/3ds/test_native_math.py) | Numerically test pinned Aurora C math with native compile policy on the host. |
| [test_native_original_jut.py](../Tools/3ds/test_native_original_jut.py) | Check actual original JUT console and panic owners with host OS backends. |
| [test_native_packets.py](../Tools/3ds/test_native_packets.py) | Native packets (`runtime/native_packets.hpp`) against the original path: every shape drawn in order under the same bank and bindings, including the verify modes. |
| [test_native_pad.py](../Tools/3ds/test_native_pad.py) | Exercise original JUT/JKR with native mapping and synthetic HID snapshots. |
| [test_native_picture.py](../Tools/3ds/test_native_picture.py) | Link original J2DPicture/JUTTexture to genuine native providers and capture GX under sanitizers. |
| [test_native_platform.py](../Tools/3ds/test_native_platform.py) | Test production arena and clock adapters with synthetic host inputs. |
| [test_native_sync.py](../Tools/3ds/test_native_sync.py) | Run the production native OS synchronization algorithm with host primitives. |
| [test_native_threads.py](../Tools/3ds/test_native_threads.py) | Exercise native OS/real JKR worker lifecycle with host threads and compile ARM production backends. |
| [test_original_video_retrace.py](../Tools/3ds/test_original_video_retrace.py) | Check actual JUTVideo lifetime and native retrace delivery with synthetic hardware. |
| [test_pica_blocks.py](../Tools/3ds/test_pica_blocks.py) | Native PICA blocks (`runtime/pica_blocks.hpp`): a block replayed on another bank must equal, byte for byte, the words the normal replay path emits whenever its checks pass. |
| [test_process_profile.py](../Tools/3ds/test_process_profile.py) | Check original native static-profile lookup without simulating actors. |
| [test_public_host.py](../Tools/3ds/test_public_host.py) | Run enumerated synthetic production fixtures without a disc, SDK or emulator. |
| [test_renderer_safety.py](../Tools/3ds/test_renderer_safety.py) | Run owned renderer-safety host regressions with ASan/UBSan, without ARM/emulator work. |
| [test_resource_archive.py](../Tools/3ds/test_resource_archive.py) | Test bounded archive parsers without extracting or saving private resource payloads. |
| [test_run_azahar_smoke.py](../Tools/3ds/test_run_azahar_smoke.py) | Offline lifecycle tests: no real emulator, fork, signal, or debugger socket. |
| [test_scene_blockers_s44.py](../Tools/3ds/test_scene_blockers_s44.py) | Host-only session44 clip/alpha/safety regressions; no native build or emulator. |
| [test_scene_blockers_s44_continuation08.py](../Tools/3ds/test_scene_blockers_s44_continuation08.py) | Host-only session44 clip/alpha/safety regressions; no native build or emulator. |
| [test_skin_palette.py](../Tools/3ds/test_skin_palette.py) | Merged matrix groups of skinned shapes (`skin` switch): palette planner against GX's slot rules on hand-made and random shapes, merged lists byte for byte, refusals and uniform rows. |
| [test_stage_data.py](../Tools/3ds/test_stage_data.py) | Bounded DZS/DZR test plus optional read-only private metadata inspection. |
| [test_static_compact_cache.py](../Tools/3ds/test_static_compact_cache.py) | Native static geometry cache: compact-palette entries equal the uncached decode byte for byte; replay placement and the stage-change clear. |
| [test_story_benchmark.py](../Tools/3ds/test_story_benchmark.py) | Host regression tests for the story route rules and the receipt evaluator. |
| [test_story_chapters.py](../Tools/3ds/test_story_chapters.py) | Host checks for the chapter itinerary, proof rules and result evaluator. |
| [test_texture_compact.py](../Tools/3ds/test_texture_compact.py) | Alpha 12 compact 16-bit texture plans must hold the RGBA8 picture (`runtime/texture_gx.cpp` under ASan/UBSan). |
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
| [test_tour_cadence.py](../Tools/3ds/test_tour_cadence.py) | Cadence file interchange, every-picture accounting, the strict gate and truncation. |
| [test_trace.py](../Tools/3ds/test_trace.py) | Synthetic collector regression checks; never reads game input. |
| [test_twilight_pack.py](../Tools/3ds/test_twilight_pack.py) | Synthetic USA/PAL bundle identity, payload round trips and unsupported revision rejection. |
| [test_vertex_decode.py](../Tools/3ds/test_vertex_decode.py) | Check bounded native vertex decoding on host sanitizers and ARM compiler. |
| [test_vertex_variants.py](../Tools/3ds/test_vertex_variants.py) | Vertex program variants (`vertex` switch) against the general multi-texture program, output bit for output bit, by executing the assembled binary; also the 512-word and operand-table limits. |
| [test_video_retrace.py](../Tools/3ds/test_video_retrace.py) | Test native retrace/sleep contracts with a synthetic event source and real queues. |
| [test_wolf_door.py](../Tools/3ds/test_wolf_door.py) | Exercise the Fishing Pond door cut on the host, with optional disc verification. |
| [test_worker_shutdown.py](../Tools/3ds/test_worker_shutdown.py) | Test production worker shutdown with real host threads; never builds ARM or controls an emulator. |
| [test_world_mips.py](../Tools/3ds/test_world_mips.py) | Generated world mip levels against an independent box average, alpha-tested coverage per level, refusals and the register rules of `runtime/gx_world_mips.hpp`. |
| [test_zone_harness.py](../Tools/3ds/test_zone_harness.py) | Automatic-test tooling: start-point choice, start-point fields, zone statuses, log line lengths and the elapsed-time rule of a restarted test; `--spawns` also regenerates the zone table and compares it. |
| [texture_compression_lab.py](../Tools/3ds/texture_compression_lab.py) | Bounded PRIVATE host texture lab. No game assets, source or installed tools modified. |
| [texture_lab_etc_host.cpp](../Tools/3ds/texture_lab_etc_host.cpp) | Native development fixture or configuration; inspect the source before use. |
| [texture_lab_formats.py](../Tools/3ds/texture_lab_formats.py) | Host lab formats. Independent ETC1 decoder follows the Khronos bit grammar. |
| [twilight_pack.py](../Tools/3ds/twilight_pack.py) | Private, bounded GameCube USA/PAL r0 -> DuskLight indexed asset container v1. |
| [validate_texture_compression_lab.py](../Tools/3ds/validate_texture_compression_lab.py) | Reopen a lab run, validate every actual PICA payload, and retain receipts. |
| [verify_application_dump.py](../Tools/3ds/verify_application_dump.py) | Read-only verification of a private application dump: a DUMP_1 directory or a packed DUMP_2 `.dpk` file (Python 3.14+); `--extract` writes its screens and memory regions. |
| [verify_diagnostic_package.py](../Tools/3ds/verify_diagnostic_package.py) | Validate this diagnostic CIA's structure and hashes, not signatures or hardware. |
| [verify_gpu_queue_gate.py](../Tools/3ds/verify_gpu_queue_gate.py) | Verify the actual linked ARM queue/callback gate; never infer interception from flags. |
| [vertex_variant_cost.py](../Tools/3ds/vertex_variant_cost.py) | Instructions per vertex of the rigid vertex programs, path by path, by running the assembled program (`pica_vertex_interp.py`); with census logs (`Native vertex plans`) also per picture. |

## Native renderer development tools

Start with [Native renderer](NATIVE_RENDERER.md) for data dependencies and
[Testing](TESTING.md) for executable host commands. These tools do not establish
PICA output or full-game acceptance. Generated outputs remain local.

| Tool | Responsibility |
| --- | --- |
| [audit_native_engine.py](../Tools/3ds/audit_native_engine.py) | Seal the standalone ARM native engine and check its dependency boundary. |
| [compile_native_texture.cpp](../Tools/3ds/compile_native_texture.cpp) | Native bank/compiler development helper; see the native renderer guide and source contract. |
| [inspect_native_asset_pack.cpp](../Tools/3ds/inspect_native_asset_pack.cpp) | Native bank/compiler development helper; see the native renderer guide and source contract. |
| [inspect_native_material_pack.cpp](../Tools/3ds/inspect_native_material_pack.cpp) | Native bank/compiler development helper; see the native renderer guide and source contract. |
| [inspect_native_mesh_pack.cpp](../Tools/3ds/inspect_native_mesh_pack.cpp) | Native bank/compiler development helper; see the native renderer guide and source contract. |
| [inspect_native_model_plan.cpp](../Tools/3ds/inspect_native_model_plan.cpp) | Native bank/compiler development helper; see the native renderer guide and source contract. |
| [inspect_native_pose.cpp](../Tools/3ds/inspect_native_pose.cpp) | Native bank/compiler development helper; see the native renderer guide and source contract. |
| [inspect_native_texture_pack.cpp](../Tools/3ds/inspect_native_texture_pack.cpp) | Native bank/compiler development helper; see the native renderer guide and source contract. |
| [j3d_material_source.py](../Tools/3ds/j3d_material_source.py) | Bounded Mac-only MAT3 declaration reader, independent of runtime GX state. |
| [j3d_native_geometry.py](../Tools/3ds/j3d_native_geometry.py) | Offline J3D geometry -> native meshes. Never used by the 3DS renderer. |
| [j3d_native_materials.py](../Tools/3ds/j3d_native_materials.py) | Mac-only MAT3 fragment compiler. No source interpreter on 3DS. |
| [j3d_native_model.py](../Tools/3ds/j3d_native_model.py) | Mac-only original model association -> native model-plan assembly. |
| [j3d_native_pose.py](../Tools/3ds/j3d_native_pose.py) | Mac-only rest hierarchy/envelope compiler -> native affine graph. |
| [j3d_native_samplers.py](../Tools/3ds/j3d_native_samplers.py) | Mac-only source sampler controls -> native PICA descriptor constants. |
| [j3d_native_textures.py](../Tools/3ds/j3d_native_textures.py) | Mac-only TEX1 image preparation. Samplers/materials/texgen remain separate. |
| [native_asset_pack.py](../Tools/3ds/native_asset_pack.py) | Mac-only assembly of source-identified native resource banks; no GX output. |
| [native_camera_fixture.py](../Tools/3ds/native_camera_fixture.py) | Local source math oracles and real game publication helper for host checks. |
| [native_game_packet_fixture.py](../Tools/3ds/native_game_packet_fixture.py) | Host fixture for the actual source packet entry bodies; never a game run. |
| [native_game_source_fixture.py](../Tools/3ds/native_game_source_fixture.py) | Extract actual game source identity/pose bodies for host SDK substitutes. |
| [native_material_pack.py](../Tools/3ds/native_material_pack.py) | Mac-only native fragment-state writer. Never emits source command packets. |
| [native_mesh_pack.py](../Tools/3ds/native_mesh_pack.py) | Prepare version1 native mesh packs on the Mac; no source-command payloads. |
| [native_model_plan.py](../Tools/3ds/native_model_plan.py) | Mac native model-plan writer; no original hierarchy/source enums in output. |
| [native_pose_pack.py](../Tools/3ds/native_pose_pack.py) | Native affine pose graph writer. No source file or renderer commands. |
| [native_texture_compression.py](../Tools/3ds/native_texture_compression.py) | DEFAULT-OFF Mac asset experiment: native ETC with explicit byte-quality gates. |
| [native_texture_pack.py](../Tools/3ds/native_texture_pack.py) | Offline native image pack writer. No source-console formats in the output. |
| [test_j3d_native_geometry.py](../Tools/3ds/test_j3d_native_geometry.py) | Independent binary fixtures for the Mac-only J3D native geometry compiler. |
| [test_j3d_native_materials.py](../Tools/3ds/test_j3d_native_materials.py) | Independent source fixtures and normalized fragment-equation checks. |
| [test_j3d_native_textures.py](../Tools/3ds/test_j3d_native_textures.py) | Mac compiler checks against independent constant/coordinate image fixtures. |
| [test_native_engine.py](../Tools/3ds/test_native_engine.py) | Compile/run native contracts and backend with SDK substitutes; never Azahar. |
| [test_native_material_pack.py](../Tools/3ds/test_native_material_pack.py) | Native material binary contracts with actual parser under sanitizers. |
| [test_native_material_parameters.py](../Tools/3ds/test_native_material_parameters.py) | Native parameter wiring, versioned pack and offline source specialization. |
| [test_native_material_temporaries.py](../Tools/3ds/test_native_material_temporaries.py) | Offline native buffer allocation, normalized equations and refusal controls. |
| [test_native_model_plan.py](../Tools/3ds/test_native_model_plan.py) | Source hierarchy/sampler model and native metadata contracts; no graphics. |
| [test_native_pack.py](../Tools/3ds/test_native_pack.py) | Cross-language native pack verification; synthetic assets only, no emulator. |
| [test_native_pose.py](../Tools/3ds/test_native_pose.py) | Native pose contracts and offline matrices, never emulator/GPU execution. |
| [test_native_projective_shader.py](../Tools/3ds/test_native_projective_shader.py) | Native position->STQ source algebra and binary ABI. Never GPU execution. |
| [test_native_shader.py](../Tools/3ds/test_native_shader.py) | Native shader algebra and assembled ABI checks. No GPU/emulator execution. |
| [test_native_texture_compression.py](../Tools/3ds/test_native_texture_compression.py) | Offline native ETC admission and exact-alpha regression checks. |
| [test_native_texture_pack.py](../Tools/3ds/test_native_texture_pack.py) | Native image contract checks. No emulator or GPU execution. |
| [test_native_uv_shader.py](../Tools/3ds/test_native_uv_shader.py) | Native affine UV shader algebra/ABI. No GPU or emulator execution. |
| [test_private_fixture_git.py](../Tools/3ds/test_private_fixture_git.py) | Private fixture protection after the public repository moved to ROOT. |
