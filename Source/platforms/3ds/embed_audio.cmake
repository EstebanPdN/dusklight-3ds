# Reproducible, immutable OpenDSP component. See vendor/opendsp/README.md.
file(SHA256 "${INPUT}" audio_sha256)
if(NOT audio_sha256 STREQUAL "1d4f126106e1715eed63393f9787d4fa796412bc60292a23b31b4b6650ca1ab4")
    message(FATAL_ERROR "Embedded OpenDSP input does not match the reviewed firmware")
endif()
set(SYMBOL dusk_opendsp_component)
include("${CMAKE_CURRENT_LIST_DIR}/embed_shader.cmake")
