# Embedded audio component

OpenDSP by Cellenseres, https://github.com/Cellenseres/OpenDSP, commit 781c8a3bf22f3d661b304f309526fb19120caf80. Copied firmware bytes unmodified from firmware/opendsp.cdc (202,196 bytes; SHA-256 1d4f126106e1715eed63393f9787d4fa796412bc60292a23b31b4b6650ca1ab4). BSD-2-Clause license and bundled minimp3 notice/license are retained alongside the binary and must accompany distribution. This is open-source replacement firmware, not an owner-console dump.

Dusklight uses the ordinary libctru NDSP PCM interface and supplies these persistent read-only bytes with ndspUseComponent before initialization. No OpenDSP codec/effect extension API is used. CMake verifies the full firmware hash before embedding. No separate SD firmware file is required by this path. Physical output remains a separate acceptance gate.
