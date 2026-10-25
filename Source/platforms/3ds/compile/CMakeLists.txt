cmake_minimum_required(VERSION 3.25)
project(dusklight_arm32_inventory LANGUAGES C CXX)

# Compile-only bootstrap candidates. This deliberately does not link an application.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(DUSK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
file(READ "${CMAKE_CURRENT_LIST_DIR}/sources.json" SOURCE_MANIFEST)
string(JSON SOURCE_COUNT LENGTH "${SOURCE_MANIFEST}" sources)
math(EXPR SOURCE_LAST "${SOURCE_COUNT} - 1")
foreach(INDEX RANGE ${SOURCE_LAST})
    string(JSON UNIT GET "${SOURCE_MANIFEST}" sources ${INDEX} path)
    list(APPEND BOOTSTRAP_SOURCES "${DUSK_ROOT}/${UNIT}")
endforeach()
add_library(dusk_arm32_inventory OBJECT ${BOOTSTRAP_SOURCES} abi_probe.cpp)
target_include_directories(dusk_arm32_inventory PRIVATE
    "${DUSK_ROOT}/include" "${DUSK_ROOT}/libs/JSystem/include"
    "${DUSK_ROOT}/extern/aurora/include" "${DUSK_ROOT}/extern/aurora/include/dolphin"
    "${DUSK_ROOT}/sdk/include" "${DUSK_ROOT}/src")
# Retain the host-resource ABI for inventory, not a production platform decision.
# DUSK_BUILDING_GAME preserves mod coupling so the inventory cannot conceal it.
target_compile_definitions(dusk_arm32_inventory PRIVATE
    DUSK_TARGET_3DS=1 TARGET_PC=1 DUSK_BUILDING_GAME=1 VERSION=0
    PARTIAL_DEBUG=1 AVOID_UB=1 MTX_USE_PS=1)
target_compile_options(dusk_arm32_inventory PRIVATE
    -O2 -g -ffunction-sections -fdata-sections -fno-strict-aliasing
    -Wno-multichar -Wno-deprecated-declarations -fmax-errors=12
    "SHELL:-include \"${DUSK_ROOT}/extern/aurora/include/dolphin/types.h\""
    "SHELL:-include \"${DUSK_ROOT}/include/global.h\"")

# Portable Aurora matrix/vector code: C ABI, no desktop renderer dependency.
file(READ "${CMAKE_CURRENT_LIST_DIR}/math_sources.json" MATH_MANIFEST)
string(JSON MATH_COUNT LENGTH "${MATH_MANIFEST}" sources)
math(EXPR MATH_LAST "${MATH_COUNT} - 1")
foreach(INDEX RANGE ${MATH_LAST})
    string(JSON UNIT GET "${MATH_MANIFEST}" sources ${INDEX} path)
    list(APPEND MATH_SOURCES "${DUSK_ROOT}/${UNIT}")
endforeach()
add_library(dusk_arm32_math_inventory OBJECT ${MATH_SOURCES})
set_target_properties(dusk_arm32_math_inventory PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(dusk_arm32_math_inventory PRIVATE "${DUSK_ROOT}/extern/aurora/include")
target_compile_definitions(dusk_arm32_math_inventory PRIVATE TARGET_PC=1 DUSK_TARGET_3DS=1 MTX_USE_PS=1)
target_compile_options(dusk_arm32_math_inventory PRIVATE -O2 -g -ffunction-sections -fdata-sections -fno-strict-aliasing)

add_library(dusk_arm32_platform_inventory OBJECT
    ../runtime/os_sync.cpp ../runtime/primitives_ctru.cpp ../runtime/os_arena.cpp ../runtime/os_time.cpp)
target_include_directories(dusk_arm32_platform_inventory PRIVATE
    "${DUSK_ROOT}/extern/aurora/include" "/opt/devkitpro/libctru/include")
target_compile_definitions(dusk_arm32_platform_inventory PRIVATE TARGET_PC=1 DUSK_TARGET_3DS=1)
target_compile_options(dusk_arm32_platform_inventory PRIVATE -O2 -g -ffunction-sections -fdata-sections)
