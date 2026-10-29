cmake_minimum_required(VERSION 3.25)
project(dusklight_native_3ds LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(DUSK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(WORKSPACE "${DUSK_ROOT}/.." ABSOLUTE)
set(DEVKITPRO "/opt/devkitpro" CACHE PATH "Existing devkitPro installation")
find_program(THREEDSXTOOL 3dsxtool HINTS "${DEVKITPRO}/tools/bin" REQUIRED)
find_program(SMDHTOOL smdhtool HINTS "${DEVKITPRO}/tools/bin" REQUIRED)
find_program(PICASSO picasso HINTS "${DEVKITPRO}/tools/bin" REQUIRED)

# Independent native renderer. The game-model integration option below links
# this library and redirects real packet entrypoints; asset/input migration is
# incomplete. Building this target alone never creates a game executable.
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_scene.shbin"
    COMMAND "${PICASSO}" -o "${CMAKE_CURRENT_BINARY_DIR}/native_scene.shbin" "${CMAKE_CURRENT_SOURCE_DIR}/native/scene.v.pica"
    DEPENDS native/scene.v.pica VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_scene_shader.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/native_scene.shbin"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/native_scene_shader.cpp" -DSYMBOL=native_scene_shader -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/native_scene.shbin" embed_shader.cmake VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_scene_rigid.shbin"
    COMMAND "${PICASSO}" -o "${CMAKE_CURRENT_BINARY_DIR}/native_scene_rigid.shbin" "${CMAKE_CURRENT_SOURCE_DIR}/native/scene_rigid.v.pica"
    DEPENDS native/scene_rigid.v.pica VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_scene_rigid_shader.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/native_scene_rigid.shbin"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/native_scene_rigid_shader.cpp" -DSYMBOL=native_scene_rigid_shader -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/native_scene_rigid.shbin" embed_shader.cmake VERBATIM)
foreach(native_variant IN ITEMS uv rigid_uv projective rigid_projective)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_scene_${native_variant}.shbin"
        COMMAND "${PICASSO}" -o "${CMAKE_CURRENT_BINARY_DIR}/native_scene_${native_variant}.shbin"
            "${CMAKE_CURRENT_SOURCE_DIR}/native/scene_${native_variant}.v.pica"
        DEPENDS "native/scene_${native_variant}.v.pica" VERBATIM)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_scene_${native_variant}_shader.cpp"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/native_scene_${native_variant}.shbin"
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/native_scene_${native_variant}_shader.cpp"
            "-DSYMBOL=native_scene_${native_variant}_shader" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake"
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/native_scene_${native_variant}.shbin" embed_shader.cmake VERBATIM)
endforeach()
add_library(dusk_pica_engine STATIC EXCLUDE_FROM_ALL
    native/contracts.cpp native/renderer.cpp native/quad_batch.cpp native/pack.cpp native/texture_pack.cpp native/material_pack.cpp native/model.cpp native/model_plan.cpp native/scene.cpp native/pose.cpp native/game_draw.cpp native/game_source.cpp native/asset_pack.cpp native/game_assets.cpp native/camera.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/native_scene_shader.cpp" "${CMAKE_CURRENT_BINARY_DIR}/native_scene_rigid_shader.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/native_scene_uv_shader.cpp" "${CMAKE_CURRENT_BINARY_DIR}/native_scene_rigid_uv_shader.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/native_scene_projective_shader.cpp" "${CMAKE_CURRENT_BINARY_DIR}/native_scene_rigid_projective_shader.cpp")
target_include_directories(dusk_pica_engine PRIVATE "${DEVKITPRO}/libctru/include")
target_compile_options(dusk_pica_engine PRIVATE -O2 -ffunction-sections -fdata-sections
    -Wall -Wextra -Werror -fno-exceptions -fno-rtti -fstack-usage)

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/preview.shbin"
    COMMAND "${PICASSO}" -o "${CMAKE_CURRENT_BINARY_DIR}/preview.shbin" "${CMAKE_CURRENT_SOURCE_DIR}/preview.v.pica"
    DEPENDS preview.v.pica VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/preview_shader.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/preview.shbin"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/preview_shader.cpp" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/preview.shbin" embed_shader.cmake VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gx_color.shbin"
    COMMAND "${PICASSO}" -o "${CMAKE_CURRENT_BINARY_DIR}/gx_color.shbin" "${CMAKE_CURRENT_SOURCE_DIR}/gx_color.v.pica"
    DEPENDS gx_color.v.pica VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gx_color_shader.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/gx_color.shbin"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/gx_color_shader.cpp" -DSYMBOL=gx_color_shader -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/gx_color.shbin" embed_shader.cmake VERBATIM)

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid.shbin"
    COMMAND "${PICASSO}" -o "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid.shbin" "${CMAKE_CURRENT_SOURCE_DIR}/gx_rigid.v.pica"
    DEPENDS gx_rigid.v.pica VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_shader.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/gx_rigid.shbin"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_shader.cpp" -DSYMBOL=gx_rigid_shader -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid.shbin" embed_shader.cmake VERBATIM)

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_mt.shbin"
    COMMAND "${PICASSO}" -o "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_mt.shbin" "${CMAKE_CURRENT_SOURCE_DIR}/gx_rigid_mt.v.pica"
    DEPENDS gx_rigid_mt.v.pica VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_mt_shader.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_mt.shbin"
        "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_mt_shader.cpp" -DSYMBOL=gx_rigid_mt_shader -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_shader.cmake"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_mt.shbin" embed_shader.cmake VERBATIM)

add_library(dusk_native_kernel OBJECT
    runtime/video_retrace.cpp
    runtime/bootstrap.cpp
    runtime/archive_bootstrap.cpp runtime/dvd_provider.cpp
    runtime/jas_critical_section.cpp
    runtime/audio_control.cpp
    runtime/pad_mapping.cpp runtime/pad_clamp.cpp runtime/pad_native.cpp runtime/pad_bootstrap.cpp
    "${DUSK_ROOT}/libs/JSystem/src/JUtility/JUTGamePad.cpp"
    "${DUSK_ROOT}/src/m_Do/m_Do_controller_pad.cpp"
    runtime/game_clock_native.cpp runtime/stage_data.cpp "${DUSK_ROOT}/src/dusk/game_clock.cpp"
    "${DUSK_ROOT}/src/helpers/offset_ptr.cpp"
    gx_api.cpp j2d_probe.cpp j2d_picture_probe.cpp
    "${DUSK_ROOT}/libs/JSystem/src/J2DGraph/J2DGrafContext.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/J2DGraph/J2DOrthoGraph.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/J2DGraph/J2DPicture.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/J2DGraph/J2DPane.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/J2DGraph/J2DScreen.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/J2DGraph/J2DManage.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JUtility/JUTTexture.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DTexture.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DGD.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JUtility/JUTPalette.cpp"
    "${DUSK_ROOT}/src/helpers/string.cpp"
    "${DUSK_ROOT}/src/dusk/version.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JSupport/JSUList.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRHeap.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRExpHeap.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRSolidHeap.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRArchivePri.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRArchivePub.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRMemArchive.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRFileLoader.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRFileFinder.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRDvdFile.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRDecomp.cpp"
    "${DUSK_ROOT}/libs/JSystem/src/JKernel/JKRDisposer.cpp")
target_include_directories(dusk_native_kernel PRIVATE
    "${DUSK_ROOT}/include" "${DUSK_ROOT}/libs/JSystem/include"
    "${DUSK_ROOT}/extern/aurora/include" "${DUSK_ROOT}/extern/aurora/include/dolphin"
    "${DUSK_ROOT}/sdk/include" "${DUSK_ROOT}/src" "${DEVKITPRO}/libctru/include")
target_compile_definitions(dusk_native_kernel PRIVATE TARGET_PC=1 DUSK_TARGET_3DS=1
    DUSK_BUILDING_GAME=1 VERSION=0 PARTIAL_DEBUG=1 AVOID_UB=1 MTX_USE_PS=1)
target_compile_options(dusk_native_kernel PRIVATE
    "SHELL:-include \"${DUSK_ROOT}/extern/aurora/include/dolphin/types.h\""
    "SHELL:-include \"${DUSK_ROOT}/include/global.h\""
    -Wno-multichar -Wno-deprecated-declarations)


# Private BG instance admission experiment; no budget defaults.
option(DUSK_BG_BUDGET_EXPERIMENT_76 "Experimental bounded BG instance admission" OFF)
target_compile_definitions(dusk_native_kernel PRIVATE
    DUSK_BG_BUDGET_EXPERIMENT_76=$<BOOL:${DUSK_BG_BUDGET_EXPERIMENT_76}>)
if(DUSK_BG_BUDGET_EXPERIMENT_76)
    target_sources(dusk_native_kernel PRIVATE
        runtime/bg_budget_experiment.cpp runtime/bg_budget_experiment_sd.cpp)
endif()
option(DUSK_SCENE_CREATE_PROBE "Private bounded scene creation probe" OFF)
target_compile_definitions(dusk_native_kernel PRIVATE
    DUSK_SCENE_CREATE_PROBE=$<BOOL:${DUSK_SCENE_CREATE_PROBE}>)
if(DUSK_SCENE_CREATE_PROBE)
    target_sources(dusk_native_kernel PRIVATE
        runtime/scene_create_probe.cpp runtime/scene_create_probe_sd.cpp)
endif()

option(DUSK_GROUND_CREATE_PROBE "Private bounded ground creation probe" OFF)
target_compile_definitions(dusk_native_kernel PRIVATE
    DUSK_GROUND_CREATE_PROBE=$<BOOL:${DUSK_GROUND_CREATE_PROBE}>)
if(DUSK_GROUND_CREATE_PROBE)
    target_sources(dusk_native_kernel PRIVATE
        runtime/ground_create_probe.cpp runtime/ground_create_probe_sd.cpp)
endif()

add_library(dusk_native_math OBJECT
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/mtx.c"
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/mtx44.c"
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/mtxvec.c"
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/vec.c"
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/quat.c")
target_include_directories(dusk_native_math PRIVATE "${DUSK_ROOT}/extern/aurora/include")
target_compile_definitions(dusk_native_math PRIVATE TARGET_PC=1 DUSK_TARGET_3DS=1 MTX_USE_PS=1)

# Original Aurora command parsing/batching, without desktop GPU dependencies.
add_library(dusk_native_display_list OBJECT
    runtime/gd_native.cpp
    runtime/gx_rigid_decode.cpp runtime/gx_vertex_decode.cpp
    runtime/gx_vertex_transform.cpp
    runtime/gx_geometry.cpp
    runtime/gx_gpu_geometry.cpp
    runtime/gx_geometry_pack.cpp
    runtime/gx_texture_commands.cpp
    runtime/gx_texture_cache.cpp
    runtime/gx_raster_plan.cpp
    runtime/gx_raster_color.cpp
    gx_command_draw.cpp gx_command_probe.cpp
    runtime/gx_commands.cpp
    runtime/gx_stream.cpp
    runtime/gx_transform_commands.cpp
    runtime/gx_vertex_commands.cpp
    runtime/gx_lighting_commands.cpp
    runtime/gx_light_objects.cpp
    runtime/gx_queries.cpp
    runtime/gx_tev_commands.cpp
    runtime/gx_pixel_commands.cpp
    runtime/gx_initial_state.cpp
    runtime/gx_gpu_sync.cpp
    runtime/gx_framebuffer_scale.cpp
    runtime/gx_framebuffer.cpp runtime/gx_framebuffer_api.cpp
    runtime/gx_copy_pixels.cpp runtime/fused_color.cpp runtime/gx_copy_execution.cpp runtime/copy_lifecycle_probe.cpp
    runtime/gx_misc.cpp
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/thp/THPDec.cpp"
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/thp/THPAudio.cpp"
    runtime/gx_copy_commands.cpp
    runtime/gx_material.cpp
    runtime/gx_pica_material.cpp
    gx_command_api.cpp
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/gd/GDGeometry.cpp"
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/gd/GDTev.cpp"
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/gd/GDIndirect.cpp"
    "${DUSK_ROOT}/extern/aurora/lib/dolphin/gx/GXDraw.cpp"
    "${DUSK_ROOT}/extern/aurora/lib/gx/dl.cpp"
    "${DUSK_ROOT}/extern/aurora/lib/gx/attr_fmt.cpp")
target_include_directories(dusk_native_display_list PRIVATE "${DUSK_ROOT}/extern/aurora/include"
    "${DUSK_ROOT}/extern/aurora/include/dolphin" "${DUSK_ROOT}/libs/JSystem/include")
target_compile_definitions(dusk_native_display_list PRIVATE TARGET_PC=1 DUSK_TARGET_3DS=1)

add_executable(dusklight-native
    main.cpp diagnostics.cpp preview.cpp gx_pica.cpp gx_texture_pica.cpp gx_material_pica.cpp gx_geometry_pica.cpp
    runtime/gx_state.cpp runtime/gx_pica_math.cpp runtime/gx_pica_tev.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/gx_color_shader.cpp" "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_shader.cpp" "${CMAKE_CURRENT_BINARY_DIR}/gx_rigid_mt_shader.cpp" runtime/disc.cpp runtime/dump_io.cpp runtime/developer_script.cpp
    runtime/resource_archive.cpp runtime/texture_ia8.cpp runtime/texture_i4.cpp runtime/texture_gx.cpp "${CMAKE_CURRENT_BINARY_DIR}/preview_shader.cpp"
    runtime/os_sync.cpp runtime/os_thread.cpp runtime/thread_backend_ctru.cpp
    runtime/os_time.cpp runtime/os_arena.cpp runtime/primitives_ctru.cpp runtime/video_retrace_ctru.cpp runtime/gpu_sync_ctru.cpp
    runtime/gx_framebuffer_ctru.cpp runtime/font_pages.cpp runtime/font_batch.cpp runtime/texture_resources.cpp
    runtime/os_system.cpp runtime/os_system_ctru.cpp runtime/video_mode.cpp
    $<TARGET_OBJECTS:dusk_native_kernel> $<TARGET_OBJECTS:dusk_native_math>
    $<TARGET_OBJECTS:dusk_native_display_list>)
set_target_properties(dusklight-native PROPERTIES SUFFIX ".elf")
target_include_directories(dusklight-native PRIVATE
    "${DUSK_ROOT}/include" "${DUSK_ROOT}/extern/aurora/include" "${DUSK_ROOT}/src" "${DEVKITPRO}/libctru/include")
target_compile_definitions(dusklight-native PRIVATE TARGET_PC=1 DUSK_TARGET_3DS=1 DUSK_NATIVE_THREAD_RUNTIME=1)
foreach(target dusk_native_kernel dusk_native_math dusk_native_display_list dusklight-native)
    target_compile_options(${target} PRIVATE -O2 -g -mword-relocations
        -ffunction-sections -fdata-sections -fno-strict-aliasing)
endforeach()
# The native GX translation runs per draw and per vertex; small fixed loops
# must unroll on the ARM11. Original game sources keep their -O2 build.
foreach(target dusk_native_math dusk_native_display_list dusklight-native)
    target_compile_options(${target} PRIVATE -O3 -fno-math-errno)
endforeach()
target_link_directories(dusklight-native PRIVATE "${DEVKITPRO}/libctru/lib")
target_link_options(dusklight-native PRIVATE -specs=3dsx.specs
    "-Wl,--wrap=GX_BindQueue"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/dusklight-native.map")
# Keep the pinned queue body unchanged. A strong public IRQ forwarding gate is
# necessary: --wrap cannot intercept the event TU's co-located weak default.
set_source_files_properties(runtime/ctru_gxqueue.c PROPERTIES
    COMPILE_DEFINITIONS "gxCmdQueueInterrupt=duskOriginalGxCmdQueueInterrupt")
target_sources(dusklight-native PRIVATE runtime/ctru_gxqueue.c)
target_link_libraries(dusklight-native PRIVATE citro3d ctru m)
# Packaging consumes only the owner-supplied icon. No ISO, cache or ROMFS content.
set(ICON "${WORKSPACE}/assets/3ds/icon-48.png" CACHE FILEPATH "Owner-supplied icon")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dusklight-native.smdh"
    COMMAND "${SMDHTOOL}" --create "Dusklight New3DS" "Native diagnostic bootstrap - not gameplay"
        "Twilit Realm / local port" "${ICON}" "${CMAKE_CURRENT_BINARY_DIR}/dusklight-native.smdh"
    DEPENDS "${ICON}" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dusklight-native.3dsx"
    COMMAND "${THREEDSXTOOL}" "$<TARGET_FILE:dusklight-native>"
        "${CMAKE_CURRENT_BINARY_DIR}/dusklight-native.3dsx"
        "--smdh=${CMAKE_CURRENT_BINARY_DIR}/dusklight-native.smdh"
    DEPENDS dusklight-native "${CMAKE_CURRENT_BINARY_DIR}/dusklight-native.smdh" VERBATIM)
add_custom_target(native_3dsx ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/dusklight-native.3dsx")

option(DUSK_COMMAND_PROBE_STARTUP "Start in synthetic native command/GPU diagnostic" OFF)
if(DUSK_COMMAND_PROBE_STARTUP)
    target_compile_definitions(dusklight-native PRIVATE DUSK_COMMAND_PROBE_STARTUP=1)
endif()

# Optional bounded admission run exercises normal GPU/worker destruction.
# The interactive diagnostic and original game retain their normal lifetime.
set(DUSK_DIAGNOSTIC_EXIT_FRAMES "0" CACHE STRING "Diagnostic frames before normal exit; 0 disables")
if(NOT DUSK_DIAGNOSTIC_EXIT_FRAMES MATCHES "^[0-9]+$")
    message(FATAL_ERROR "DUSK_DIAGNOSTIC_EXIT_FRAMES must be an integer")
endif()
if(DUSK_DIAGNOSTIC_EXIT_FRAMES GREATER 0)
    if(DUSK_DIAGNOSTIC_EXIT_FRAMES LESS 600 OR DUSK_DIAGNOSTIC_EXIT_FRAMES GREATER 3600)
        message(FATAL_ERROR "Bounded diagnostic admission requires 600..3600 frames")
    endif()
    if(NOT DUSK_COMMAND_PROBE_STARTUP)
        message(FATAL_ERROR "Bounded diagnostic requires the alternating command/J2D probe")
    endif()
    target_compile_definitions(dusklight-native PRIVATE DUSK_DIAGNOSTIC_EXIT_FRAMES=${DUSK_DIAGNOSTIC_EXIT_FRAMES})
endif()

option(DUSK_SCENE_TEXTURES "Render original Ordon material commands" ON)
option(DUSK_SCENE_STARTUP "Load original Ordon scene on native target" OFF)
if(DUSK_SCENE_STARTUP AND DUSK_DIAGNOSTIC_EXIT_FRAMES GREATER 0)
    message(FATAL_ERROR "Bounded diagnostic admission cannot replace original scene lifetime")
endif()
if(DUSK_SCENE_STARTUP)
    target_compile_definitions(dusklight-native PRIVATE DUSK_SCENE_STARTUP=1)
    if(DUSK_SCENE_TEXTURES)
        target_compile_definitions(dusk_native_kernel PRIVATE DUSK_SCENE_TEXTURES=1)
    endif()
    target_sources(dusk_native_kernel PRIVATE scene_bootstrap.cpp runtime/j3d_presentation.cpp
        "${DUSK_ROOT}/libs/JSystem/src/JUtility/JUTNameTab.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JMath/JMath.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JMath/JMATrigonometric.cpp"
        "${DUSK_ROOT}/extern/aurora/lib/dolphin/gd/GDLight.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DDrawBuffer.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DMatBlock.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DMaterial.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DPacket.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DShape.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DShapeDraw.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DShapeMtx.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DStruct.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DSys.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DTevs.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DTransform.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphBase/J3DVertex.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DAnimation.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DCluster.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DJoint.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DJointTree.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DMaterialAnm.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DMaterialAttach.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DModel.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DModelData.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DMtxBuffer.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DShapeTable.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphAnimator/J3DSkinDeform.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphLoader/J3DAnmLoader.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphLoader/J3DClusterLoader.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphLoader/J3DJointFactory.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphLoader/J3DMaterialFactory.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphLoader/J3DMaterialFactory_v21.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphLoader/J3DModelLoader.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphLoader/J3DModelLoaderCalcSize.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DGraphLoader/J3DShapeFactory.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DU/J3DUClipper.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DU/J3DUDL.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DU/J3DUFur.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DU/J3DUMotion.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/J3DU/J3DUShadow.cpp"
)
endif()

option(DUSK_SCENE_ENVIRONMENT_STARTUP "Link original environment owners and initialize room light state" OFF)
if(DUSK_SCENE_ENVIRONMENT_STARTUP)
    if(NOT DUSK_SCENE_STARTUP OR NOT DUSK_SCENE_TEXTURES)
        message(FATAL_ERROR "Original environment startup requires the material scene build")
    endif()
    get_target_property(kernel_sources dusk_native_kernel SOURCES)
    get_target_property(math_sources dusk_native_math SOURCES)
    get_target_property(display_sources dusk_native_display_list SOURCES)
    set(existing_sources ${kernel_sources} ${math_sources} ${display_sources})
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/compile/environment_sources.json" environment_manifest)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/compile/environment_sources.json")
    string(JSON environment_count LENGTH "${environment_manifest}" sources)
    math(EXPR environment_last "${environment_count} - 1")
    set(environment_sources)
    foreach(index RANGE 0 ${environment_last})
        string(JSON relative GET "${environment_manifest}" sources ${index} path)
        set(source "${DUSK_ROOT}/${relative}")
        if(NOT source IN_LIST existing_sources)
            list(APPEND environment_sources "${source}")
        endif()
    endforeach()
    add_library(dusk_original_environment STATIC ${environment_sources})
    foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
        get_target_property(value dusk_native_kernel ${property})
        set_target_properties(dusk_original_environment PROPERTIES ${property} "${value}")
    endforeach()
    target_include_directories(dusk_original_environment PRIVATE "${DUSK_ROOT}/assets/GZ2E01")
    target_compile_definitions(dusk_native_kernel PRIVATE DUSK_SCENE_ENVIRONMENT_STARTUP=1)
    # Retain the actual environment/material providers and their constructors.
    # This option does not claim complete world or player startup.
    target_link_options(dusklight-native PRIVATE
        "-Wl,-u,_Z15dKy_tevstr_initP12dKy_tevstr_cah"
        "-Wl,-u,_ZN18dScnKy_env_light_c11setLight_bgEP12dKy_tevstr_cP10GXColorS10S3_PfS4_"
        "-Wl,-u,_ZN18dScnKy_env_light_c25setLightTevColorType_MAJIEP12J3DModelDataP12dKy_tevstr_c")
    target_link_libraries(dusklight-native PRIVATE dusk_original_environment citro3d ctru m)
endif()


option(DUSK_SCENE_FOG_INSPECTION "Inspect original scene textures with fog explicitly omitted; not gameplay" OFF)
if(DUSK_SCENE_FOG_INSPECTION)
    if(NOT DUSK_SCENE_ENVIRONMENT_STARTUP)
        message(FATAL_ERROR "Fog inspection requires the original scene environment")
    endif()
    target_compile_definitions(dusk_native_kernel PRIVATE DUSK_SCENE_FOG_INSPECTION=1)
endif()

# This gate requires an actual final executable link of the original process,
# player, camera and play-scene roots. Missing actors/services are linker errors;
# no profile substitutions or successful no-op game callbacks are supplied.
option(DUSK_GAMEPLAY_LINK_GATE "Require original gameplay roots at executable link; not gameplay acceptance" OFF)
option(DUSK_GAME_STARTUP "Run original machine/process startup and game loop" OFF)
set(DUSK_PROFILE_LINK_SELECTION "" CACHE FILEPATH "Private profile selection for bounded native LINK probe; empty retains full game")
if(DUSK_PROFILE_LINK_SELECTION AND NOT DUSK_GAMEPLAY_LINK_GATE)
    message(FATAL_ERROR "Bounded original-profile link probe requires the gameplay link gate")
endif()
if(DUSK_GAMEPLAY_LINK_GATE)
    # Full original owners replace the diagnostic-only JUT console/panic adapters.
    set_property(SOURCE runtime/bootstrap.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS DUSK_NATIVE_ORIGINAL_JUT=1)
    if(NOT DUSK_SCENE_ENVIRONMENT_STARTUP)
        message(FATAL_ERROR "Gameplay link gate requires the original scene/environment owners")
    endif()
    get_target_property(environment_sources dusk_original_environment SOURCES)
    list(APPEND existing_sources ${environment_sources})
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/compile/gameplay_sources.json" gameplay_manifest)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/compile/gameplay_sources.json")
    string(JSON gameplay_count LENGTH "${gameplay_manifest}" sources)
    math(EXPR gameplay_last "${gameplay_count} - 1")
    set(gameplay_sources)
    foreach(index RANGE 0 ${gameplay_last})
        string(JSON relative GET "${gameplay_manifest}" sources ${index} path)
        set(source "${DUSK_ROOT}/${relative}")
        if(NOT source IN_LIST existing_sources)
            list(APPEND gameplay_sources "${source}")
        endif()
    endforeach()
    if(DUSK_PROFILE_LINK_SELECTION)
        find_program(DUSK_HOST_PYTHON python3 REQUIRED)
        set(profile_generator "${WORKSPACE}/Tools/3ds/generate_profile_link_probe.py")
        set(profile_list "${DUSK_ROOT}/src/f_pc/f_pc_profile_lst.cpp")
        set(probe_source "${CMAKE_CURRENT_BINARY_DIR}/bounded_original_profiles.cpp")
        execute_process(COMMAND "${DUSK_HOST_PYTHON}" "${profile_generator}"
            --selection "${DUSK_PROFILE_LINK_SELECTION}" --output "${probe_source}"
            RESULT_VARIABLE profile_result OUTPUT_VARIABLE profile_output ERROR_VARIABLE profile_error)
        if(NOT profile_result EQUAL 0)
            message(FATAL_ERROR "Original-profile probe generation failed: ${profile_error}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${profile_generator}" "${profile_list}" "${DUSK_PROFILE_LINK_SELECTION}")
        list(REMOVE_ITEM gameplay_sources "${profile_list}")
        list(APPEND gameplay_sources "${probe_source}")
    endif()
    add_library(dusk_original_gameplay STATIC ${gameplay_sources})
    foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
        get_target_property(value dusk_native_kernel ${property})
        set_target_properties(dusk_original_gameplay PROPERTIES ${property} "${value}")
    endforeach()
    if(DUSK_PROFILE_LINK_SELECTION)
        set_source_files_properties("${probe_source}" "${DUSK_ROOT}/src/f_pc/f_pc_profile.cpp"
            PROPERTIES COMPILE_DEFINITIONS DUSK_PROFILE_LINK_PROBE=1)
    endif()
    target_include_directories(dusk_original_gameplay PRIVATE "${DUSK_ROOT}/assets/GZ2E01")
    target_link_options(dusklight-native PRIVATE
        "-Wl,-u,g_profile_ALINK" "-Wl,-u,g_profile_MIDNA"
        "-Wl,-u,g_profile_CAMERA" "-Wl,-u,g_profile_PLAY_SCENE"
        "-Wl,-u,_Z12fapGm_Createv" "-Wl,-u,_Z13fapGm_Executev")
    target_link_libraries(dusklight-native PRIVATE
        "-Wl,--start-group" dusk_original_gameplay dusk_original_environment
        "-Wl,--end-group" citro3d ctru m)
endif()
if(DUSK_GAME_STARTUP)
    set(DUSK_DUMP_BUILD_ID "unavailable-unsealed-native-integration" CACHE STRING
        "Private candidate/source fingerprint recorded in application RAM dumps")
    target_sources(dusk_native_kernel PRIVATE
        runtime/application_dump.cpp runtime/application_dump_ctru.cpp)
    set_property(SOURCE runtime/application_dump_ctru.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS DUSK_DUMP_BUILD_ID="${DUSK_DUMP_BUILD_ID}")
    if(NOT DUSK_GAMEPLAY_LINK_GATE OR DUSK_PROFILE_LINK_SELECTION)
        message(FATAL_ERROR "Original game startup requires the complete original profile list")
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/opendsp_component.cpp"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/vendor/opendsp/opendsp.cdc"
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/opendsp_component.cpp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_audio.cmake"
        DEPENDS vendor/opendsp/opendsp.cdc embed_audio.cmake embed_shader.cmake VERBATIM)
    target_sources(dusk_native_kernel PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/opendsp_component.cpp")
    target_sources(dusk_native_kernel PRIVATE runtime/game_startup.cpp runtime/fused_color_config.cpp runtime/aram.cpp runtime/gx_fifo.cpp runtime/card.cpp runtime/audio_ndsp.cpp
        "${DUSK_ROOT}/src/dusk/audio/DuskDsp.cpp"
        "${DUSK_ROOT}/src/dusk/audio/Adpcm.cpp"
        "${DUSK_ROOT}/libs/freeverb/revmodel.cpp"
        "${DUSK_ROOT}/libs/freeverb/comb.cpp"
        "${DUSK_ROOT}/libs/freeverb/allpass.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JAUInitializer.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JAUAudioArcInterpreter.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JAUAudioArcLoader.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JAUStreamFileTable.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JAUSeqCollection.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JAUBankTable.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JASWSParser.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JASBNKParser.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JASVoiceBank.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JASSimpleWaveBank.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JASBasicBank.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JASBasicInst.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JASDrumSet.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JASProbe.cpp"
        "${DUSK_ROOT}/libs/JSystem/src/JAudio2/JASCmdStack.cpp"
        "${DUSK_ROOT}/src/Z2AudioLib/Z2AudioArcLoader.cpp")
    target_include_directories(dusk_native_kernel PRIVATE "${DUSK_ROOT}/libs/freeverb/include" "${DUSK_ROOT}/libs/freeverb/include/freeverb")
    target_compile_definitions(dusklight-native PRIVATE DUSK_GAME_STARTUP=1)
    target_compile_definitions(dusk_native_kernel PRIVATE DUSK_GAME_STARTUP=1)
endif()
option(DUSK_SCENE_MATERIAL_INSPECTION "Inspect admitted scene meshes with unsupported materials omitted; not gameplay" OFF)
if(DUSK_SCENE_MATERIAL_INSPECTION)
    if(NOT DUSK_SCENE_FOG_INSPECTION)
        message(FATAL_ERROR "Partial material inspection requires the explicitly marked fog inspection")
    endif()
    target_compile_definitions(dusk_native_kernel PRIVATE DUSK_SCENE_MATERIAL_INSPECTION=1)
endif()

# Current native 3D integration: real game model packets dispatch prepared native
# resources. UI/effects migration is still unfinished; missing model bindings fail.
option(DUSK_PICA_GAME_DRAW "Use native PICA draws for original game model packets" ON)
if(DUSK_GAME_STARTUP AND DUSK_PICA_GAME_DRAW)
    foreach(native_game_target IN ITEMS dusklight-native dusk_native_kernel dusk_native_math
            dusk_native_display_list dusk_original_gameplay dusk_original_environment)
        if(TARGET ${native_game_target})
            target_compile_definitions(${native_game_target} PRIVATE DUSK_PICA_GAME_DRAW=1)
        endif()
    endforeach()
    target_link_libraries(dusklight-native PRIVATE dusk_pica_engine citro3d ctru m)
endif()
